#include "wifi_board.h"
#include "codecs/es8389_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "usage_api.h"
#include "power_save_timer.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "power_manager.h"

#include "i2c_device.h"
#include "settings.h"
#include "lvgl_theme.h"
#include <lvgl.h>
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_http_server.h>
#include <esp_netif.h>
#include <nvs.h>

#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include "esp_io_expander_tca95xx_16bit.h"

#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_crt_bundle.h>
#include <mbedtls/ssl.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <fcntl.h>
#include <errno.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <utility>
#include <string>
#include <strings.h>
#include <vector>
#include <cctype>
#include <ctime>

#define TAG "atk_dnesp32s3_box2_wifi"

// 用量面板补字字体：内置 noto 子集缺 周/日/分/智/谱 等字，由 simhei 子集补齐（板目录内随构建编入）
LV_FONT_DECLARE(font_usage_extra_20);
LV_FONT_DECLARE(font_usage_small_14);
LV_FONT_DECLARE(font_usage_mid_16);

class atk_dnesp32s3_box2_wifi : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;   
    LcdDisplay* display_;
    esp_io_expander_handle_t io_exp_handle;
    button_handle_t btns;
    button_driver_t* btn_driver_ = nullptr;
    static atk_dnesp32s3_box2_wifi* instance_;
    PowerSaveTimer* power_save_timer_;
    PowerManager* power_manager_;
    PowerSupply power_status_;
    esp_timer_handle_t wake_timer_handle_;
    esp_lcd_panel_io_handle_t panel_io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    int ticks_ = 0;
    const int kChgCtrlInterval = 5;
    std::atomic<bool> usage_fetching_{false};
    // StartUsageQuery 参数经成员传入查询任务（xTaskCreate 无参捕获）
    bool usage_query_keep_source_ = false;
    // XIO_KEY_Q 有效电平无原理图依据（L 键低有效、M 键高有效），上电按空闲电平推断
    bool q_key_active_high_ = true;
    // 长按 Q 进入的配置模式：临时 HTTP 页面把用量查询配置写入 NVS
    bool usage_config_mode_ = false;
    httpd_handle_t usage_config_server_ = nullptr;
    esp_timer_handle_t usage_config_exit_timer_ = nullptr;

    // 用量查询的数据结构/网络/数据源层在 usage_api.*，此处只引别名
    using AccountDetail = usage::AccountDetail;
    using Socks5Config = usage::Socks5Config;
    using LocalUsageConfig = usage::LocalUsageConfig;

    // vendor 数据存板专属 64KB NVS 分区（partitions/v2/16m.csv 的 nvs_vendor）；
    // 旧固件分区表无此分区时回落默认 "nvs"（容量小，导入大令牌受限但不崩溃）
    static bool vendor_partition_ok_;
    // ESP-IDF NVS key 最长 15 字符，配置键必须全部保持在该限制内。
    static constexpr const char* kCodexCountKey = "codex_count";
    static constexpr const char* kLocalCountKey = "local_count";
    static constexpr const char* kRefreshMinutesKey = "refresh_min";
    static constexpr const char* kProxyTypeKey = "proxy_type";
    static constexpr const char* kProxyHostKey = "proxy_host";
    static constexpr const char* kProxyPortKey = "proxy_port";
    static constexpr const char* kProxyUserKey = "proxy_user";
    static constexpr const char* kProxyPassKey = "proxy_pass";
    static constexpr const char* kChatGptProxyKey = "proxy_chatgpt";
    static constexpr const char* kZhipuProxyKey = "proxy_zhipu";

    static const char* VendorPartition() {
        return vendor_partition_ok_ ? "nvs_vendor" : "nvs";
    }

    // 初始化 nvs_vendor 分区，并把默认分区里的旧 vendor 数据一次性迁移过来
    // （令牌经 OAuth 轮换后 PC 原文件已失效，NVS 里是最新值，必须迁移）
    static void InitializeVendorNvs() {
        esp_err_t err = nvs_flash_init_partition("nvs_vendor");
        if (err != ESP_OK && err != ESP_ERR_NVS_NO_FREE_PAGES && err != ESP_ERR_NVS_NEW_VERSION_FOUND) {
            vendor_partition_ok_ = false;
            ESP_LOGW(TAG, "nvs_vendor partition unavailable (%s), fallback to default nvs",
                     esp_err_to_name(err));
            return;
        }
        vendor_partition_ok_ = true;

        Settings dst("vendor", true, "nvs_vendor");
        if (dst.GetInt("vendor_migrated", 0) != 0) {
            return;  // 已迁移
        }
        Settings src("vendor");  // 默认分区
        // 已知 key 集合迁移（本文件定义的全部 vendor 键，string 与 int 分列）
        for (int i = 0; i < 8; i++) {
            std::string value = src.GetString("codex_auth_" + std::to_string(i + 1), "");
            if (!value.empty()) {
                dst.SetString("codex_auth_" + std::to_string(i + 1), value);
            }
        }
        for (int i = 0; i < 4; i++) {
            std::string value = src.GetString("zhipu_key_" + std::to_string(i + 1), "");
            if (!value.empty()) {
                dst.SetString("zhipu_key_" + std::to_string(i + 1), value);
            }
        }
        for (int i = 0; i < 4; i++) {
            std::string prefix = "local_" + std::to_string(i + 1);
            std::string host = src.GetString(prefix + "_host", "");
            if (!host.empty()) {
                dst.SetString(prefix + "_host", host);
                dst.SetInt(prefix + "_port", src.GetInt(prefix + "_port", 3939));
                std::string key = src.GetString(prefix + "_key", "");
                if (!key.empty()) {
                    dst.SetString(prefix + "_key", key);
                }
            }
        }
        const char* string_keys[] = {kProxyTypeKey, kProxyHostKey, kProxyUserKey, kProxyPassKey};
        for (const char* key : string_keys) {
            std::string value = src.GetString(key, "");
            if (!value.empty()) {
                dst.SetString(key, value);
            }
        }
        const char* int_keys[] = {kCodexCountKey, kLocalCountKey, kRefreshMinutesKey,
                                  kProxyPortKey, kChatGptProxyKey, kZhipuProxyKey,
                                  "zhipu_key_count"};
        for (const char* key : int_keys) {
            int32_t value = src.GetInt(key, -1);
            if (value >= 0) {
                dst.SetInt(key, value);
            }
        }
        dst.SetInt("vendor_migrated", 1);
        ESP_LOGI(TAG, "vendor settings migrated to nvs_vendor partition");
    }

    // Q 键查询结果的全屏用量面板（LVGL，用时创建、关闭即删除）
    lv_obj_t* usage_panel_ = nullptr;
    esp_timer_handle_t usage_panel_timer_ = nullptr;
    // 面板数据缓存：按来源分组，各自独立的列表页与详情页。
    // usage_source_: 0=官方 API 账号（ChatGPT+智谱），1=本机统计；Q 键在两组面板间循环切换
    static constexpr int kUsageSourceApi = 0;
    static constexpr int kUsageSourceLocal = 1;
    int usage_source_ = kUsageSourceApi;
    std::vector<AccountDetail> api_details_;
    std::vector<AccountDetail> local_details_;
    int api_detail_index_ = -1;   // 当前详情页账号（-1=列表页）
    int local_detail_index_ = -1;
    // 240x320 竖屏统一每页 2 张卡片，避免内容拥挤或被裁切
    int api_list_page_ = 0;
    int local_list_page_ = 0;
    static constexpr int kUsageCardsPerPage = 2;
    static constexpr int kLocalCardsPerPage = 2;
    // 面板显示期间的自动刷新（0=关闭），以及自动刷新后恢复到原详情页
    esp_timer_handle_t usage_refresh_timer_ = nullptr;
    esp_timer_handle_t usage_watch_timer_ = nullptr;
    esp_timer_handle_t usage_loading_timer_ = nullptr;
    lv_obj_t* loading_label_ = nullptr;
    int loading_dots_ = 0;
    // 自动刷新后恢复位置（源 + 详情索引，-1=列表页）
    int pending_restore_source_ = -1;
    int pending_detail_restore_ = -1;
    time_t usage_updated_at_ = 0;

    // ChatGPT 令牌（NVS，vendor 命名空间），内容为 Codex auth.json 格式
    static constexpr int kMaxDirectAccounts = 4;

    static std::string CodexAuthKey(int index) {
        return "codex_auth_" + std::to_string(index + 1);
    }

    static std::string AuthJsonString(const std::string& json, const char* key) {
        cJSON* root = cJSON_Parse(json.c_str());
        if (root == nullptr) {
            return "";
        }
        cJSON* item = cJSON_GetObjectItem(root, key);
        std::string value = cJSON_IsString(item) ? item->valuestring : "";
        cJSON_Delete(root);
        return value;
    }

    std::vector<std::string> LoadDirectAuthJsons() {
        std::vector<std::string> jsons;
        Settings settings("vendor", false, VendorPartition());
        int count = settings.GetInt(kCodexCountKey, 0);
        for (int i = 0; i < count && i < kMaxDirectAccounts; i++) {
            std::string json = settings.GetString(CodexAuthKey(i), "");
            if (!json.empty()) {
                jsons.push_back(std::move(json));
            }
        }
        return jsons;
    }

    // 账号令牌属于不可信的大输入，不能走 Settings::SetString 的 ESP_ERROR_CHECK 路径；
    // 任一 NVS 错误都返回给网页，绝不能让设备 abort 重启。
    static bool SaveDirectAuthJsons(const std::vector<std::string>& jsons) {
        if (jsons.size() > kMaxDirectAccounts) {
            return false;
        }
        nvs_handle_t handle = 0;
        esp_err_t err = nvs_open_from_partition(VendorPartition(), "vendor", NVS_READWRITE, &handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Open token storage failed: %s", esp_err_to_name(err));
            return false;
        }
        int32_t old_count = 0;
        err = nvs_get_i32(handle, kCodexCountKey, &old_count);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            old_count = 0;
            err = ESP_OK;
        }
        for (int i = (int)jsons.size(); i < old_count; i++) {
            esp_err_t erase_err = nvs_erase_key(handle, CodexAuthKey(i).c_str());
            if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
                err = erase_err;
                break;
            }
        }
        for (size_t i = 0; err == ESP_OK && i < jsons.size(); i++) {
            err = nvs_set_str(handle, CodexAuthKey((int)i).c_str(), jsons[i].c_str());
        }
        if (err == ESP_OK) {
            err = nvs_set_i32(handle, kCodexCountKey, (int32_t)jsons.size());
        }
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Save token storage failed: %s", esp_err_to_name(err));
            return false;
        }
        return true;
    }

    static bool SaveDirectAuthJson(int index, const std::string& json) {
        nvs_handle_t handle = 0;
        esp_err_t err = nvs_open_from_partition(VendorPartition(), "vendor", NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            err = nvs_set_str(handle, CodexAuthKey(index).c_str(), json.c_str());
        }
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        if (handle != 0) {
            nvs_close(handle);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Update token storage failed: %s", esp_err_to_name(err));
            return false;
        }
        return true;
    }

    // 智谱官方 API Key（NVS，vendor 命名空间），最多 4 个
    static std::string ZhipuKeyKey(int index) {
        return "zhipu_key_" + std::to_string(index + 1);
    }

    std::vector<std::string> LoadZhipuKeys() {
        std::vector<std::string> keys;
        Settings settings("vendor", false, VendorPartition());
        int count = settings.GetInt("zhipu_key_count", 0);
        for (int i = 0; i < count && i < 4; i++) {
            std::string key = settings.GetString(ZhipuKeyKey(i), "");
            if (!key.empty()) {
                keys.push_back(std::move(key));
            }
        }
        return keys;
    }

    void SaveZhipuKeys(const std::vector<std::string>& keys) {
        Settings settings("vendor", true, VendorPartition());
        int old_count = settings.GetInt("zhipu_key_count", 0);
        for (int i = (int)keys.size(); i < old_count; i++) {
            settings.EraseKey(ZhipuKeyKey(i));
        }
        for (size_t i = 0; i < keys.size(); i++) {
            settings.SetString(ZhipuKeyKey((int)i), keys[i]);
        }
        settings.SetInt("zhipu_key_count", (int)keys.size());
    }

    int GetUsageRefreshMinutes() {
        Settings settings("vendor", false, VendorPartition());
        return settings.GetInt(kRefreshMinutesKey, 0);
    }

    // 本机统计服务（PC 端 box2-usage-server）地址，最多 4 台
    static std::string LocalUsageKey(int index, const char* field) {
        return "local_" + std::to_string(index + 1) + "_" + field;
    }

    std::vector<LocalUsageConfig> LoadLocalUsages() {
        std::vector<LocalUsageConfig> out;
        Settings settings("vendor", false, VendorPartition());
        int count = settings.GetInt(kLocalCountKey, 0);
        for (int i = 0; i < count && i < 4; i++) {
            LocalUsageConfig cfg;
            cfg.host = settings.GetString(LocalUsageKey(i, "host"), "");
            if (cfg.host.empty() || !ValidLocalHost(cfg.host)) {
                continue;
            }
            cfg.port = settings.GetInt(LocalUsageKey(i, "port"), 3939);
            if (cfg.port <= 0 || cfg.port >= 65536) {
                cfg.port = 3939;
            }
            cfg.key = settings.GetString(LocalUsageKey(i, "key"), "");
            out.push_back(std::move(cfg));
        }
        return out;
    }

    void SaveLocalUsages(const std::vector<LocalUsageConfig>& configs) {
        Settings settings("vendor", true, VendorPartition());
        int old_count = settings.GetInt(kLocalCountKey, 0);
        for (int i = (int)configs.size(); i < old_count && i < 4; i++) {
            settings.EraseKey(LocalUsageKey(i, "host"));
            settings.EraseKey(LocalUsageKey(i, "port"));
            settings.EraseKey(LocalUsageKey(i, "key"));
        }
        for (size_t i = 0; i < configs.size() && i < 4; i++) {
            settings.SetString(LocalUsageKey((int)i, "host"), configs[i].host);
            settings.SetInt(LocalUsageKey((int)i, "port"), configs[i].port);
            if (!configs[i].key.empty()) {
                settings.SetString(LocalUsageKey((int)i, "key"), configs[i].key);
            } else {
                settings.EraseKey(LocalUsageKey((int)i, "key"));
            }
        }
        settings.SetInt(kLocalCountKey, (int)configs.size());
    }

    // 直连模式可选代理（socks5 或 http CONNECT，均支持账号密码认证）

    Socks5Config GetSocks5Config() {
        Socks5Config proxy;
        Settings settings("vendor", false, VendorPartition());
        proxy.type = settings.GetString(kProxyTypeKey, "socks5");
        proxy.host = settings.GetString(kProxyHostKey, "");
        proxy.port = settings.GetInt(kProxyPortKey, 0);
        proxy.user = settings.GetString(kProxyUserKey, "");
        proxy.pass = settings.GetString(kProxyPassKey, "");
        return proxy;
    }

    // 代理适用范围：ChatGPT 默认走代理（国内访问官方接口必需），智谱默认直连（国内站）
    bool ChatGptUseProxy() {
        Settings settings("vendor", false, VendorPartition());
        return settings.GetInt(kChatGptProxyKey, 1) != 0;
    }

    bool ZhipuUseProxy() {
        Settings settings("vendor", false, VendorPartition());
        return settings.GetInt(kZhipuProxyKey, 0) != 0;
    }

    void InitializeBoardPowerManager() {
        instance_ = this;

        if (IoExpanderGetLevel(XIO_CHRG) == 0) {
            power_status_ = kDeviceTypecSupply;
        } else {
            power_status_ = kDeviceBatterySupply;
        }

        esp_timer_create_args_t wake_display_timer_args = {
            .callback = [](void *arg) {
                atk_dnesp32s3_box2_wifi* self = static_cast<atk_dnesp32s3_box2_wifi*>(arg);

                self->ticks_ ++;
                if (self->ticks_ % self->kChgCtrlInterval == 0) {
                    if (self->IoExpanderGetLevel(XIO_CHRG) == 0) {
                        self->power_status_ = kDeviceTypecSupply;
                    } else {
                        self->power_status_ = kDeviceBatterySupply;
                    }

                    /* 低于某个电量，会自动关机 */
                    if (self->power_manager_->low_voltage_ < 2630 && self->power_status_ == kDeviceBatterySupply) {
                        esp_timer_stop(self->power_manager_->timer_handle_);

                        esp_io_expander_set_dir(self->io_exp_handle, XIO_CHG_CTRL, IO_EXPANDER_OUTPUT);
                        esp_io_expander_set_level(self->io_exp_handle, XIO_CHG_CTRL, 0);
                        vTaskDelay(pdMS_TO_TICKS(100));

                        esp_io_expander_set_dir(self->io_exp_handle, XIO_CHG_CTRL, IO_EXPANDER_INPUT);
                        esp_io_expander_set_level(self->io_exp_handle, XIO_CHG_CTRL, 0);
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                }
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "wake_update_timer",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&wake_display_timer_args, &wake_timer_handle_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(wake_timer_handle_, 100000));
    }

    void InitializePowerManager() {
        power_manager_ = new PowerManager(io_exp_handle);
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
            } else {
                power_save_timer_->SetEnabled(true);
            }
        });
    }

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(1);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            if (power_status_ == kDeviceBatterySupply) {
                GetBacklight()->SetBrightness(0);
                esp_timer_stop(power_manager_->timer_handle_);
                esp_io_expander_set_dir( io_exp_handle, XIO_CHG_CTRL, IO_EXPANDER_OUTPUT);
                esp_io_expander_set_level(io_exp_handle, XIO_CHG_CTRL, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_io_expander_set_level(io_exp_handle, XIO_SYS_POW, 0);
            }
        });

        power_save_timer_->SetEnabled(true);
    }

    void audio_volume_change(bool direction) {
        auto codec = GetAudioCodec();
        auto volume = codec->output_volume();

        if (direction) {
            volume += 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
        } else {
            volume -= 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
        }
        GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
    }

    void audio_volume_minimum(){
        GetAudioCodec()->SetOutputVolume(0);
        GetDisplay()->ShowNotification(Lang::Strings::MUTED);
    }

    void audio_volume_maxmum(){
        GetAudioCodec()->SetOutputVolume(100);
        GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
    }

    static std::string UrlDecode(const std::string& in) {
        std::string out;
        out.reserve(in.size());
        for (size_t i = 0; i < in.size(); i++) {
            if (in[i] == '+') {
                out += ' ';
            } else if (in[i] == '%' && i + 2 < in.size() &&
                       isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2])) {
                out += (char)strtol(in.substr(i + 1, 2).c_str(), nullptr, 16);
                i += 2;
            } else {
                out += in[i];
            }
        }
        return out;
    }

    static std::string GetFormField(const std::string& body, const std::string& name) {
        std::string key = name + "=";
        size_t pos = body.find(key);
        if (pos == std::string::npos) {
            return "";
        }
        pos += key.size();
        size_t end = body.find('&', pos);
        return UrlDecode(body.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
    }

    // 动态值（email/代理地址等来自导入文件或用户输入）插入 HTML 前统一转义，
    // 防止 < > & 引号破坏标签结构与属性
    static std::string HtmlEscape(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                case '"': out += "&quot;"; break;
                case '\'': out += "&#39;"; break;
                default: out += c;
            }
        }
        return out;
    }

    static std::string GetConfigPage() {
        auto cfg_proxy = instance_->GetSocks5Config();
        int refresh_minutes = instance_->GetUsageRefreshMinutes();
        auto local_usages = instance_->LoadLocalUsages();
        auto jsons = instance_->LoadDirectAuthJsons();
        auto zhipu_keys = instance_->LoadZhipuKeys();

        // 卡片式单页。表单只能平级摆放：HTML 禁止 form 嵌套，内层 form 标签会被
        // 浏览器忽略导致按钮误提交外层表单（旧版"本机统计"新增/删除因此失效过）。
        std::string page = R"HTML(<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>用量查询配置</title>
<style>
:root{color-scheme:dark}
*{box-sizing:border-box}
body{margin:0;padding:16px;background:#0e1216;color:#e6e9ed;
 font-family:system-ui,-apple-system,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif}
main{max-width:560px;margin:0 auto;display:flex;flex-direction:column;gap:12px}
h1{font-size:20px;margin:0}
.sub{color:#8b95a1;font-size:13px;margin:4px 0 0}
.card{background:#161b21;border:1px solid #242c35;border-radius:12px;padding:14px}
h2{font-size:15px;margin:0;display:flex;align-items:center;gap:8px}
.chip{font-size:12px;font-weight:400;color:#8b95a1;background:#1f2731;border-radius:999px;padding:1px 8px}
.hint{color:#8b95a1;font-size:13px;margin:8px 0 10px;line-height:1.5}
.rows{display:flex;flex-direction:column;gap:6px;margin-bottom:10px}
.row{display:flex;align-items:center;justify-content:space-between;gap:8px;
 background:#0e1216;border:1px solid #242c35;border-radius:8px;padding:8px 10px}
.row-main{min-width:0;display:flex;flex-direction:column;gap:2px}
.name{font-size:14px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.meta{font-size:12px;color:#8b95a1}
.tag{display:inline-block;font-size:11px;color:#7ab0ff;background:#16233a;border-radius:4px;padding:0 5px}
textarea,input,select{width:100%;background:#0e1216;color:#e6e9ed;border:1px solid #2a323d;
 border-radius:8px;padding:8px;font-size:14px}
textarea{resize:vertical;margin-top:6px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.grid label{display:flex;flex-direction:column;gap:4px;font-size:13px;color:#8b95a1}
.span2{grid-column:1/-1}
.btn{border:none;border-radius:8px;padding:8px 16px;font-size:14px;cursor:pointer}
.primary{background:#3b76e0;color:#fff}
.danger{background:transparent;color:#e5484d;border:1px solid #5a2a2e;padding:5px 14px}
.ghost{background:transparent;color:#8b95a1;border:1px solid #2a323d}
.actions{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin-top:10px}
form.stack{display:flex;flex-direction:column;gap:12px;margin:0}
form.inline{margin:0}
.foot{color:#8b95a1;font-size:12px;margin:8px 0 0}
</style></head><body><main>
<header><h1>AI 用量查询配置</h1>
<p class="sub">官方接口直连 · 修改后短按 Q 立即查询 · 配置模式 5 分钟后自动退出</p></header>
)HTML";

        page += "<section class=\"card\"><h2>ChatGPT 令牌 <span class=\"chip\">" + std::to_string(jsons.size()) +
                "/4</span></h2>";
        if (jsons.empty()) {
            page += "<p class=\"hint\">尚未导入 ChatGPT 令牌。把 Codex 令牌文件（auth.json）内容粘贴到下方导入，"
                    "设备将直连官方接口查询用量并自动刷新令牌。</p>";
        } else {
            page += "<div class=\"rows\">";
            for (size_t i = 0; i < jsons.size(); i++) {
                std::string email = HtmlEscape(AuthJsonString(jsons[i], "email"));
                if (email.empty()) {
                    email = "未知账号";
                }
                std::string plan = HtmlEscape(AuthJsonString(jsons[i], "plan"));
                std::string until = HtmlEscape(AuthJsonString(jsons[i], "subscription_until"));
                page += "<div class=\"row\"><div class=\"row-main\"><span class=\"name\">" + std::to_string(i + 1) +
                        ". " + email + "</span>";
                if (!plan.empty() || !until.empty()) {
                    page += "<span class=\"meta\">";
                    if (!plan.empty()) {
                        page += "<span class=\"tag\">" + plan + "</span>";
                    }
                    if (!until.empty()) {
                        page += "有效期至 " + until;
                    }
                    page += "</span>";
                }
                page += "</div><form class=\"inline\" method=\"POST\" action=\"/delete\""
                        " onsubmit=\"return confirm('删除令牌 " + email + " ?')\">"
                        "<input type=\"hidden\" name=\"index\" value=\"" + std::to_string(i) + "\">"
                        "<button class=\"btn danger\" type=\"submit\">删除</button></form></div>";
            }
            page += "</div>";
        }
        page += "<form method=\"POST\" action=\"/import\">"
                "<textarea name=\"auth_json\" rows=\"5\""
                " placeholder=\"粘贴 Codex 令牌文件内容（auth.json，需含 refresh_token）\"></textarea>"
                "<div class=\"actions\"><button class=\"btn primary\" type=\"submit\">导入令牌</button>"
                "<span class=\"meta\">不同令牌分别保存，相同覆盖</span></div></form></section>";

        page += "<section class=\"card\"><h2>智谱 AI 令牌 <span class=\"chip\">" + std::to_string(zhipu_keys.size()) +
                "/4</span></h2>"
                "<p class=\"hint\">粘贴智谱开放平台（open.bigmodel.cn）的 API Key，查询 GLM 套餐 token 用量"
                "（5 小时/每周窗口）。不能使用 Codex auth.json；最多 4 个。</p>";
        if (!zhipu_keys.empty()) {
            page += "<div class=\"rows\">";
            for (size_t i = 0; i < zhipu_keys.size(); i++) {
                std::string tail = zhipu_keys[i].size() > 6 ? zhipu_keys[i].substr(zhipu_keys[i].size() - 6)
                                                            : zhipu_keys[i];
                page += "<div class=\"row\"><div class=\"row-main\"><span class=\"name\">" + std::to_string(i + 1) +
                        ". 智谱 ****" + HtmlEscape(tail) + "</span></div>"
                        "<form class=\"inline\" method=\"POST\" action=\"/zhipu_delete\""
                        " onsubmit=\"return confirm('删除智谱令牌 ****" + HtmlEscape(tail) + " ?')\">"
                        "<input type=\"hidden\" name=\"index\" value=\"" + std::to_string(i) + "\">"
                        "<button class=\"btn danger\" type=\"submit\">删除</button></form></div>";
            }
            page += "</div>";
        }
        page += "<form method=\"POST\" action=\"/zhipu_import\">"
                "<textarea name=\"zhipu_key\" rows=\"2\""
                " placeholder=\"粘贴智谱 API Key，形如 xxxxxxxxxx.xxxxxxxx\"></textarea>"
                "<div class=\"actions\"><button class=\"btn primary\" type=\"submit\">导入智谱令牌</button>"
                "<span class=\"meta\">同一 Key 重复导入覆盖</span></div></form></section>";

        page += "<section class=\"card\"><h2>本机统计 <span class=\"chip\">" + std::to_string(local_usages.size()) +
                "/4</span></h2>"
                "<p class=\"hint\">PC 上运行 box2-usage-server（Claude Code + Codex 用量/费用统计）时添加其地址，"
                "「本地用量」面板为每台 PC 生成 当天/本周/累计 卡片。全部删除即关闭。</p>";
        if (!local_usages.empty()) {
            page += "<div class=\"rows\">";
            for (size_t i = 0; i < local_usages.size(); i++) {
                std::string label = HtmlEscape(local_usages[i].host + ":" + std::to_string(local_usages[i].port)) +
                                    (local_usages[i].key.empty() ? "" : "（密钥）");
                page += "<div class=\"row\"><div class=\"row-main\"><span class=\"name\">" + std::to_string(i + 1) +
                        ". " + label + "</span></div>"
                        "<form class=\"inline\" method=\"POST\" action=\"/local_delete\""
                        " onsubmit=\"return confirm('删除 " + label + " ?')\">"
                        "<input type=\"hidden\" name=\"index\" value=\"" + std::to_string(i) + "\">"
                        "<button class=\"btn danger\" type=\"submit\">删除</button></form></div>";
            }
            page += "</div>";
        }
        page += "<form method=\"POST\" action=\"/local_add\"><div class=\"grid\">"
                "<label class=\"span2\">地址<input name=\"host\" placeholder=\"192.168.1.2\"></label>"
                "<label>端口<input name=\"port\" type=\"number\" value=\"3939\"></label>"
                "<label>密钥（可留空）<input name=\"key\" placeholder=\"可选\"></label>"
                "</div><div class=\"actions\"><button class=\"btn primary\" type=\"submit\">添加</button>"
                "<span class=\"meta\">同一地址重复添加覆盖</span></div></form></section>";

        // 代理与自动刷新共用一个 /save 表单：外层 form 只包住这两张卡片，保持平级无嵌套
        page += "<form class=\"stack\" method=\"POST\" action=\"/save\">"
                "<section class=\"card\"><h2>网络代理</h2>"
                "<p class=\"hint\">可选，访问 ChatGPT 官方接口通常需要；Clash/v2ray 混合端口两种类型均支持，"
                "无认证可留空账号密码。</p><div class=\"grid\">"
                "<label>类型<select name=\"proxy_type\">"
                "<option value=\"socks5\"" + std::string(cfg_proxy.type == "socks5" ? " selected" : "") + ">SOCKS5</option>"
                "<option value=\"http\"" + std::string(cfg_proxy.type == "http" ? " selected" : "") + ">HTTP</option>"
                "</select></label>"
                "<label>端口<input name=\"proxy_port\" type=\"number\" placeholder=\"7890\" value=\"" +
                (cfg_proxy.port > 0 ? std::to_string(cfg_proxy.port) : "") + "\"></label>"
                "<label class=\"span2\">地址<input name=\"proxy_host\" placeholder=\"192.168.1.2\" value=\"" +
                HtmlEscape(cfg_proxy.host) + "\"></label>"
                "<label>账号<input name=\"proxy_user\" autocomplete=\"off\" value=\"" + HtmlEscape(cfg_proxy.user) +
                "\"></label>"
                "<label>密码<input name=\"proxy_pass\" type=\"password\" autocomplete=\"new-password\" value=\"" +
                HtmlEscape(cfg_proxy.pass) + "\"></label>"
                "<label>ChatGPT 接入<select name=\"chatgpt_proxy\">"
                "<option value=\"1\"" + std::string(instance_->ChatGptUseProxy() ? " selected" : "") + ">走代理</option>"
                "<option value=\"0\"" + std::string(!instance_->ChatGptUseProxy() ? " selected" : "") + ">直连</option>"
                "</select></label>"
                "<label>智谱接入<select name=\"zhipu_proxy\">"
                "<option value=\"0\"" + std::string(!instance_->ZhipuUseProxy() ? " selected" : "") + ">直连（推荐）</option>"
                "<option value=\"1\"" + std::string(instance_->ZhipuUseProxy() ? " selected" : "") + ">走代理</option>"
                "</select></label></div>"
                "<p class=\"foot\">代理范围按目标分别指定；本机统计始终明文直连，不经代理。</p></section>"

                "<section class=\"card\"><h2>自动刷新</h2>"
                "<p class=\"hint\">间隔分钟（0=关闭，面板 15 秒自动关闭；大于 0 时面板常驻并按此间隔自动刷新）</p>"
                "<div class=\"grid\"><label class=\"span2\">间隔（分钟）<input name=\"refresh_minutes\" type=\"number\""
                " min=\"0\" max=\"1440\" value=\"" + std::to_string(refresh_minutes) + "\"></label></div>"
                "<div class=\"actions\"><button class=\"btn primary\" type=\"submit\">保存</button>"
                "<button class=\"btn ghost\" type=\"submit\" formaction=\"/clear_proxy\" formmethod=\"post\">清除代理</button></div>"
                "<p class=\"foot\">保存后设备自动退出配置模式；导入/删除账号不退出，可连续操作。</p>"
                "</section></form>";

        page += "</main></body></html>";
        return page;
    }

    static esp_err_t UsageConfigGetHandler(httpd_req_t* req) {
        std::string page = GetConfigPage();
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req, page.c_str(), HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    static void SendSimplePage(httpd_req_t* req, const std::string& body, const char* back_text) {
        std::string page = "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
                           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                           "<meta http-equiv=\"refresh\" content=\"2;url=/\">"
                           "<style>body{margin:0;padding:24px 16px;background:#0e1216;color:#e6e9ed;"
                           "font-family:system-ui,-apple-system,'Segoe UI','PingFang SC','Microsoft YaHei',sans-serif}"
                           ".msg{max-width:560px;margin:0 auto;background:#161b21;border:1px solid #242c35;"
                           "border-radius:12px;padding:14px}.msg h3{margin:0}"
                           ".msg a{color:#7ab0ff}.msg p{margin:12px 0 0}</style></head><body>"
                           "<div class=\"msg\">" + body + "<p><a href=\"/\">" + back_text + "</a></p></div></body></html>";
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req, page.c_str(), HTTPD_RESP_USE_STRLEN);
    }


    // 当前 vendor NVS 分区剩余空间是否够写 length 字节的字符串值。
    // 空间不足时 Settings::SetString 内部的 ESP_ERROR_CHECK 会直接 abort 重启，必须预检
    static bool NvsHasSpaceFor(size_t length) {
        nvs_stats_t stats = {};
        if (nvs_get_stats(VendorPartition(), &stats) != ESP_OK) {
            return false;  // 无法确认容量时拒绝写入，避免走到 NVS 错误路径
        }
        size_t need_entries = (length + 31) / 32 + 2;      // 值 entry + key 开销
        return stats.free_entries > need_entries + 8;      // 余量防页内碎片
    }

    static esp_err_t UsageConfigImportHandler(httpd_req_t* req) {
        if (req->content_len > 16384) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
            return ESP_FAIL;
        }
        std::vector<char> buf(req->content_len + 1, 0);
        // TCP 分段时一次 recv 可能只收到部分 body，循环读满
        int received = 0;
        while (received < (int)req->content_len) {
            int n = httpd_req_recv(req, buf.data() + received, req->content_len - received);
            if (n <= 0) {
                break;
            }
            received += n;
        }
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
            return ESP_FAIL;
        }
        std::string auth_json = GetFormField(std::string(buf.data(), received), "auth_json");

        cJSON* root = cJSON_Parse(auth_json.c_str());
        if (root == nullptr) {
            SendSimplePage(req, "<h3>导入失败：不是有效的 JSON</h3>", "返回");
            return ESP_OK;
        }
        cJSON* refresh_item = cJSON_GetObjectItem(root, "refresh_token");
        if (!cJSON_IsString(refresh_item) || refresh_item->valuestring[0] == '\0') {
            cJSON_Delete(root);
            SendSimplePage(req, "<h3>导入失败：缺少 refresh_token 字段</h3>", "返回");
            return ESP_OK;
        }

        // account_id 缺失时从 id_token JWT 提取
        std::string account_id = usage::ExtractAccountId(auth_json);
        if (account_id.empty()) {
            cJSON_Delete(root);
            SendSimplePage(req, "<h3>导入失败：无法确定 account_id</h3>", "返回");
            return ESP_OK;
        }
        cJSON_DeleteItemFromObject(root, "account_id");
        cJSON_AddStringToObject(root, "account_id", account_id.c_str());

        char* printed = cJSON_PrintUnformatted(root);
        if (printed == nullptr) {
            cJSON_Delete(root);
            SendSimplePage(req, "<h3>导入失败：序列化错误</h3>", "返回");
            return ESP_OK;
        }
        // auth.json 常同时带两个长 JWT；id_token 只用于展示元数据，先提取再丢弃，
        // 避免原样写入触发 NVS 单字符串约 4KB 的上限。
        std::string compact_json = usage::CompactCodexAuthJson(printed);
        cJSON_free(printed);
        cJSON_Delete(root);
        if (compact_json.empty()) {
            SendSimplePage(req, "<h3>导入失败：令牌整理失败</h3>", "返回");
            return ESP_OK;
        }
        // 一个 ChatGPT 账号可能签发多个独立 refresh_token；仅完全相同的令牌视为重复导入。
        // 不能只按 account_id 去重，否则第二个令牌会错误覆盖第一个槽位。
        auto jsons = instance_->LoadDirectAuthJsons();
        std::string refresh_token = AuthJsonString(compact_json, "refresh_token");
        int replace_index = -1;
        for (size_t i = 0; i < jsons.size(); i++) {
            if (!refresh_token.empty() && AuthJsonString(jsons[i], "refresh_token") == refresh_token) {
                replace_index = (int)i;
                break;
            }
        }
        if (replace_index < 0 && jsons.size() >= kMaxDirectAccounts) {
            SendSimplePage(req, "<h3>导入失败：最多 4 个令牌</h3>", "返回");
            return ESP_OK;
        }
        // 超限或空间不足时不写入（SetString 失败会 abort 重启）
        size_t printed_len = compact_json.size();
        if (printed_len > 3600 || !NvsHasSpaceFor(printed_len)) {
            SendSimplePage(req, "<h3>导入失败：令牌过大或设备存储空间不足，"
                                "可删除不用的账号后重试</h3>", "返回");
            return ESP_OK;
        }
        if (replace_index >= 0) {
            jsons[replace_index] = compact_json;
        } else {
            jsons.push_back(std::move(compact_json));
        }
        if (!instance_->SaveDirectAuthJsons(jsons)) {
            SendSimplePage(req, "<h3>导入失败：设备存储写入失败，请完整烧录新版分区表后重试</h3>", "返回");
            return ESP_OK;
        }

        SendSimplePage(req, "<h3>已导入，当前共 " + std::to_string(jsons.size()) + " 个令牌</h3>", "返回");
        return ESP_OK;
    }

    static esp_err_t UsageConfigClearProxyHandler(httpd_req_t* req) {
        Settings settings("vendor", true, VendorPartition());
        settings.SetString(kProxyTypeKey, "socks5");
        settings.EraseKey(kProxyHostKey);
        settings.EraseKey(kProxyPortKey);
        settings.EraseKey(kProxyUserKey);
        settings.EraseKey(kProxyPassKey);
        // 范围一并回到默认：ChatGPT 走代理、智谱直连
        settings.SetInt(kChatGptProxyKey, 1);
        settings.SetInt(kZhipuProxyKey, 0);
        SendSimplePage(req, "<h3>已清除代理设置</h3>", "返回");
        return ESP_OK;
    }

    static esp_err_t UsageConfigDeleteHandler(httpd_req_t* req) {
        char body[128] = {0};
        size_t total = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1;
        int received = httpd_req_recv(req, body, total);
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
            return ESP_FAIL;
        }
        int index = atoi(GetFormField(body, "index").c_str());
        auto jsons = instance_->LoadDirectAuthJsons();
        if (index < 0 || index >= (int)jsons.size()) {
            SendSimplePage(req, "<h3>删除失败：索引无效</h3>", "返回");
            return ESP_OK;
        }
        jsons.erase(jsons.begin() + index);
        if (!instance_->SaveDirectAuthJsons(jsons)) {
            SendSimplePage(req, "<h3>删除失败：设备存储写入失败</h3>", "返回");
            return ESP_OK;
        }
        SendSimplePage(req, "<h3>已删除，剩余 " + std::to_string(jsons.size()) + " 个账号</h3>", "返回");
        return ESP_OK;
    }

    // 智谱 API Key 会拼进 Authorization 头，必须拒绝空白与控制字符（防头注入）
    static bool ValidZhipuKey(const std::string& key) {
        if (key.size() < 20 || key.size() > 128) {
            return false;
        }
        size_t dot = key.find('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 == key.size()) {
            return false;
        }
        for (unsigned char c : key) {
            if (c <= 0x20 || c >= 0x7F) {
                return false;
            }
        }
        return true;
    }

    // 本机统计地址拼进 Host 头与 TcpConnect，只允许 IPv4/域名字符（防注入）
    static bool ValidLocalHost(const std::string& host) {
        if (host.empty() || host.size() > 63) {
            return false;
        }
        for (char c : host) {
            if (!isalnum((unsigned char)c) && c != '.' && c != '-') {
                return false;
            }
        }
        return true;
    }

    // 本机统计密钥拼进 URL query 与 HTML value 属性，只允许无歧义 ASCII 可见字符
    // （排除空白/控制/& # % 与引号尖括号，防 query 截断、请求行注入与属性闭合）
    static bool ValidLocalKey(const std::string& key) {
        if (key.empty() || key.size() > 64) {
            return false;
        }
        for (char c : key) {
            if (c <= 0x20 || c >= 0x7F) {
                return false;
            }
            if (c == '&' || c == '#' || c == '%' || c == '"' || c == '\'' || c == '<' || c == '>') {
                return false;
            }
        }
        return true;
    }

    // 去除首尾空白（空格/制表/换行）——从网页复制粘贴的 Key/地址常带尾随换行，
    // 会被字符校验误判无效
    static void TrimSpaces(std::string& s) {
        size_t begin = s.find_first_not_of(" \t\r\n");
        size_t end = s.find_last_not_of(" \t\r\n");
        if (begin == std::string::npos) {
            s.clear();
            return;
        }
        s = s.substr(begin, end - begin + 1);
    }

    static esp_err_t UsageConfigZhipuImportHandler(httpd_req_t* req) {
        char body[1024] = {0};
        size_t total = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1;
        // TCP 分段时一次 recv 可能只收到部分 body，循环读满
        int received = 0;
        while (received < (int)total) {
            int n = httpd_req_recv(req, body + received, total - received);
            if (n <= 0) {
                break;
            }
            received += n;
        }
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
            return ESP_FAIL;
        }
        std::string key = GetFormField(std::string(body, received), "zhipu_key");
        TrimSpaces(key);

        // 纯文本 Key 常以数字开头。cJSON_Parse("123.xxx") 会只消费数字前缀并返回成功，
        // 因此仅对明显的 JSON 输入做严格（必须消费完整字符串）解析。
        cJSON* root = nullptr;
        if (!key.empty() && (key.front() == '{' || key.front() == '"')) {
            root = cJSON_ParseWithOpts(key.c_str(), nullptr, true);
            if (root == nullptr) {
                SendSimplePage(req, "<h3>导入失败：不是有效的智谱 API Key（形如 xxxxxxxxxx.xxxxxxxx）</h3>", "返回");
                return ESP_OK;
            }
            // 容错：直接粘贴了 {"api_key":"..."} 之类的 JSON 时取字符串字段
            const char* fields[] = {"api_key", "apiKey", "key", "token"};
            std::string extracted;
            if (cJSON_IsString(root)) {
                extracted = root->valuestring;
            } else if (cJSON_IsObject(root)) {
                for (const char* field : fields) {
                    cJSON* item = cJSON_GetObjectItem(root, field);
                    if (cJSON_IsString(item)) {
                        extracted = item->valuestring;
                        break;
                    }
                }
            }
            bool is_codex_auth = cJSON_IsObject(root) &&
                                 (cJSON_GetObjectItem(root, "access_token") != nullptr ||
                                  cJSON_GetObjectItem(root, "refresh_token") != nullptr);
            cJSON_Delete(root);
            // 可解析为 JSON 却不含已知令牌字段：粘贴的是错误内容，整体判无效，
            // 防止 JSON 文本本身恰好通过 ValidZhipuKey 被存入 NVS
            if (extracted.empty()) {
                if (is_codex_auth) {
                    SendSimplePage(req, "<h3>导入失败：这是 Codex/OpenAI 账号令牌，请粘贴到“ChatGPT 令牌”输入框</h3>", "返回");
                    return ESP_OK;
                }
                SendSimplePage(req, "<h3>导入失败：不是有效的智谱 API Key（形如 xxxxxxxxxx.xxxxxxxx）</h3>", "返回");
                return ESP_OK;
            }
            key = extracted;
            TrimSpaces(key);
        }

        if (!ValidZhipuKey(key)) {
            SendSimplePage(req, "<h3>导入失败：不是有效的智谱 API Key（形如 xxxxxxxxxx.xxxxxxxx）</h3>", "返回");
            return ESP_OK;
        }

        auto keys = instance_->LoadZhipuKeys();
        int replace_index = -1;
        for (size_t i = 0; i < keys.size(); i++) {
            if (keys[i] == key) {
                replace_index = (int)i;
                break;
            }
        }
        if (replace_index < 0 && keys.size() >= 4) {
            SendSimplePage(req, "<h3>导入失败：最多 4 个智谱令牌</h3>", "返回");
            return ESP_OK;
        }
        if (replace_index >= 0) {
            keys[replace_index] = key;
        } else {
            keys.push_back(key);
        }
        instance_->SaveZhipuKeys(keys);

        SendSimplePage(req, "<h3>已导入智谱令牌，当前共 " + std::to_string(keys.size()) + " 个</h3>", "返回");
        return ESP_OK;
    }

    static esp_err_t UsageConfigZhipuDeleteHandler(httpd_req_t* req) {
        char body[128] = {0};
        size_t total = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1;
        int received = httpd_req_recv(req, body, total);
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
            return ESP_FAIL;
        }
        int index = atoi(GetFormField(body, "index").c_str());
        auto keys = instance_->LoadZhipuKeys();
        if (index < 0 || index >= (int)keys.size()) {
            SendSimplePage(req, "<h3>删除失败：索引无效</h3>", "返回");
            return ESP_OK;
        }
        keys.erase(keys.begin() + index);
        instance_->SaveZhipuKeys(keys);
        SendSimplePage(req, "<h3>已删除，剩余 " + std::to_string(keys.size()) + " 个智谱令牌</h3>", "返回");
        return ESP_OK;
    }

    static esp_err_t UsageConfigLocalAddHandler(httpd_req_t* req) {
        char body[384] = {0};
        size_t total = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1;
        int received = 0;
        while (received < (int)total) {
            int n = httpd_req_recv(req, body + received, total - received);
            if (n <= 0) {
                break;
            }
            received += n;
        }
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
            return ESP_FAIL;
        }
        std::string host = GetFormField(std::string(body, received), "host");
        int port = atoi(GetFormField(std::string(body, received), "port").c_str());
        std::string key = GetFormField(std::string(body, received), "key");
        TrimSpaces(host);
        TrimSpaces(key);
        if (key.empty() || ValidLocalKey(key)) {
            if (port <= 0 || port >= 65536) {
                port = 3939;
            }
            if (!ValidLocalHost(host)) {
                SendSimplePage(req, "<h3>添加失败：地址无效（仅 IPv4/域名）</h3>", "返回");
                return ESP_OK;
            }
            auto configs = instance_->LoadLocalUsages();
            int replace_index = -1;
            for (size_t i = 0; i < configs.size(); i++) {
                if (configs[i].host == host) {
                    replace_index = (int)i;
                    break;
                }
            }
            if (replace_index < 0 && configs.size() >= 4) {
                SendSimplePage(req, "<h3>添加失败：最多 4 台 PC</h3>", "返回");
                return ESP_OK;
            }
            LocalUsageConfig cfg;
            cfg.host = host;
            cfg.port = port;
            cfg.key = key;
            if (replace_index >= 0) {
                configs[replace_index] = std::move(cfg);
            } else {
                configs.push_back(std::move(cfg));
            }
            instance_->SaveLocalUsages(configs);
            SendSimplePage(req, "<h3>已添加，当前共 " + std::to_string(configs.size()) + " 台 PC</h3>", "返回");
        } else {
            // 非法密钥拒绝：该值会拼进 URL query 与 HTML 属性
            SendSimplePage(req, "<h3>添加失败：密钥仅限 ASCII 可见字符（不能含 &amp; # % 引号尖括号）</h3>", "返回");
        }
        return ESP_OK;
    }

    static esp_err_t UsageConfigLocalDeleteHandler(httpd_req_t* req) {
        char body[128] = {0};
        size_t total = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1;
        int received = httpd_req_recv(req, body, total);
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
            return ESP_FAIL;
        }
        int index = atoi(GetFormField(body, "index").c_str());
        auto configs = instance_->LoadLocalUsages();
        if (index < 0 || index >= (int)configs.size()) {
            SendSimplePage(req, "<h3>删除失败：索引无效</h3>", "返回");
            return ESP_OK;
        }
        configs.erase(configs.begin() + index);
        instance_->SaveLocalUsages(configs);
        SendSimplePage(req, "<h3>已删除，剩余 " + std::to_string(configs.size()) + " 台 PC</h3>", "返回");
        return ESP_OK;
    }

    static esp_err_t UsageConfigSaveHandler(httpd_req_t* req) {
        char body[2048] = {0};
        size_t total = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1;
        // TCP 分段时一次 recv 可能只收到部分 body，循环读满
        int received = 0;
        while (received < (int)total) {
            int n = httpd_req_recv(req, body + received, total - received);
            if (n <= 0) {
                break;
            }
            received += n;
        }
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
            return ESP_FAIL;
        }

        auto refresh_minutes = GetFormField(body, "refresh_minutes");
        auto proxy_type = GetFormField(body, "proxy_type");
        auto proxy_host = GetFormField(body, "proxy_host");
        auto proxy_port = GetFormField(body, "proxy_port");
        auto proxy_user = GetFormField(body, "proxy_user");
        auto proxy_pass = GetFormField(body, "proxy_pass");
        auto chatgpt_proxy = GetFormField(body, "chatgpt_proxy");
        auto zhipu_proxy = GetFormField(body, "zhipu_proxy");

        Settings settings("vendor", true, VendorPartition());
        if (!proxy_type.empty()) {
            settings.SetString(kProxyTypeKey, proxy_type == "http" ? "http" : "socks5");
        }
        if (!chatgpt_proxy.empty()) {
            settings.SetInt(kChatGptProxyKey, atoi(chatgpt_proxy.c_str()) != 0 ? 1 : 0);
        }
        if (!zhipu_proxy.empty()) {
            settings.SetInt(kZhipuProxyKey, atoi(zhipu_proxy.c_str()) != 0 ? 1 : 0);
        }
        if (!proxy_host.empty()) {
            settings.SetString(kProxyHostKey, proxy_host);
        }
        if (!proxy_port.empty()) {
            int port = atoi(proxy_port.c_str());
            if (port > 0 && port < 65536) {
                settings.SetInt(kProxyPortKey, port);
            }
        }
        if (!proxy_user.empty()) {
            settings.SetString(kProxyUserKey, proxy_user);
        }
        if (!proxy_pass.empty()) {
            settings.SetString(kProxyPassKey, proxy_pass);
        }
        if (!refresh_minutes.empty()) {
            int minutes = atoi(refresh_minutes.c_str());
            if (minutes < 0) minutes = 0;
            if (minutes > 1440) minutes = 1440;
            settings.SetInt(kRefreshMinutesKey, minutes);
        }
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req, "<!DOCTYPE html><html><head><meta charset=\"utf-8\"></head>"
                             "<body><h3>已保存，设备正在退出配置模式</h3></body></html>",
                        HTTPD_RESP_USE_STRLEN);

        // httpd_stop 不能在处理器上下文里调用，延迟退出
        esp_timer_stop(instance_->usage_config_exit_timer_);
        esp_timer_start_once(instance_->usage_config_exit_timer_, 1500000);
        return ESP_OK;
    }

    std::string GetLocalIpAddress() {
        esp_netif_t* netif = nullptr;
        while ((netif = esp_netif_next_unsafe(netif)) != nullptr) {
            if (!esp_netif_is_netif_up(netif)) {
                continue;
            }
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
                char ip[16];
                snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.ip));
                return ip;
            }
        }
        return "";
    }

    void ExitUsageConfigMode() {
        if (!usage_config_mode_) {
            return;
        }
        usage_config_mode_ = false;
        esp_timer_stop(usage_config_exit_timer_);
        if (usage_config_server_ != nullptr) {
            httpd_stop(usage_config_server_);
            usage_config_server_ = nullptr;
        }
        GetDisplay()->ShowNotification("已退出配置模式", 2000);
    }

    void EnterUsageConfigMode() {
        if (usage_config_mode_) {
            return;
        }
        HideUsagePanel();
        std::string ip = GetLocalIpAddress();
        if (ip.empty()) {
            GetDisplay()->ShowNotification("网络未连接,无法配置", 3000);
            return;
        }

        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.stack_size = 8192;
        if (httpd_start(&usage_config_server_, &config) != ESP_OK) {
            usage_config_server_ = nullptr;
            GetDisplay()->ShowNotification("配置模式启动失败", 3000);
            return;
        }

        httpd_uri_t get_uri = {};
        get_uri.uri = "/";
        get_uri.method = HTTP_GET;
        get_uri.handler = UsageConfigGetHandler;
        httpd_register_uri_handler(usage_config_server_, &get_uri);

        httpd_uri_t save_uri = {};
        save_uri.uri = "/save";
        save_uri.method = HTTP_POST;
        save_uri.handler = UsageConfigSaveHandler;
        httpd_register_uri_handler(usage_config_server_, &save_uri);

        httpd_uri_t import_uri = {};
        import_uri.uri = "/import";
        import_uri.method = HTTP_POST;
        import_uri.handler = UsageConfigImportHandler;
        httpd_register_uri_handler(usage_config_server_, &import_uri);

        httpd_uri_t delete_uri = {};
        delete_uri.uri = "/delete";
        delete_uri.method = HTTP_POST;
        delete_uri.handler = UsageConfigDeleteHandler;
        httpd_register_uri_handler(usage_config_server_, &delete_uri);

        httpd_uri_t zhipu_import_uri = {};
        zhipu_import_uri.uri = "/zhipu_import";
        zhipu_import_uri.method = HTTP_POST;
        zhipu_import_uri.handler = UsageConfigZhipuImportHandler;
        httpd_register_uri_handler(usage_config_server_, &zhipu_import_uri);

        httpd_uri_t zhipu_delete_uri = {};
        zhipu_delete_uri.uri = "/zhipu_delete";
        zhipu_delete_uri.method = HTTP_POST;
        zhipu_delete_uri.handler = UsageConfigZhipuDeleteHandler;
        httpd_register_uri_handler(usage_config_server_, &zhipu_delete_uri);

        httpd_uri_t clear_proxy_uri = {};
        clear_proxy_uri.uri = "/clear_proxy";
        clear_proxy_uri.method = HTTP_POST;
        clear_proxy_uri.handler = UsageConfigClearProxyHandler;
        httpd_register_uri_handler(usage_config_server_, &clear_proxy_uri);

        httpd_uri_t local_add_uri = {};
        local_add_uri.uri = "/local_add";
        local_add_uri.method = HTTP_POST;
        local_add_uri.handler = UsageConfigLocalAddHandler;
        httpd_register_uri_handler(usage_config_server_, &local_add_uri);

        httpd_uri_t local_delete_uri = {};
        local_delete_uri.uri = "/local_delete";
        local_delete_uri.method = HTTP_POST;
        local_delete_uri.handler = UsageConfigLocalDeleteHandler;
        httpd_register_uri_handler(usage_config_server_, &local_delete_uri);

        if (usage_config_exit_timer_ == nullptr) {
            esp_timer_create_args_t timer_args = {
                .callback = [](void* arg) { instance_->ExitUsageConfigMode(); },
                .arg = nullptr,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "usage_cfg_exit",
                .skip_unhandled_events = true,
            };
            ESP_ERROR_CHECK(esp_timer_create(&timer_args, &usage_config_exit_timer_));
        }

        usage_config_mode_ = true;
        // 5 分钟未操作自动退出，避免配置页面长期暴露在局域网
        esp_timer_start_once(usage_config_exit_timer_, 300000000ULL);
        GetDisplay()->ShowNotification("配置模式\nhttp://" + ip + "/", 300000);
        ESP_LOGI(TAG, "Usage config mode: http://%s/", ip.c_str());
    }

    // 用导入的令牌直连官方接口查询用量并展示（ChatGPT 账号 + 智谱 API Key）。
    // keep_source=true（自动刷新）回到刷新前的源与页面；false（用户主动按 Q）
    // 走初始源（官方查询成功优先官方面板），避免被上一次所在面板锁死
    void StartUsageQuery(bool keep_source = false) {
        if (usage_fetching_.exchange(true)) {
            return;
        }
        usage_query_keep_source_ = keep_source;
        if (xTaskCreate([](void* arg) {
                auto self = static_cast<atk_dnesp32s3_box2_wifi*>(arg);
                self->QueryUsage(self->usage_query_keep_source_);
                self->usage_fetching_ = false;
                vTaskDelete(nullptr);
            }, "usage_query", 10240, this, 4, nullptr) != pdPASS) {
            usage_fetching_ = false;
        }
    }

    // 卡片名后缀：IPv4 取末段（192.168.1.134 → 134），域名截前 8 字符
    static std::string HostSuffix(const std::string& host) {
        if (host.find_first_not_of("0123456789.") == std::string::npos) {
            size_t dot = host.rfind('.');
            if (dot != std::string::npos) {
                return host.substr(dot + 1);
            }
        }
        return host.size() > 8 ? host.substr(0, 8) : host;
    }

    // 直连官方接口模式：使用导入的 codex 令牌；401 时自动刷新并重试。
    // 结果按来源分组（官方 API / 本机统计），两组各自有列表页与详情页
    void QueryDirectUsage() {
        std::vector<std::string> jsons = LoadDirectAuthJsons();
        std::vector<AccountDetail> api_details;
        std::vector<AccountDetail> local_details;
        bool api_ok = false;
        bool local_ok = false;
        // 代理配置按范围组装一次：ChatGPT/智谱各自可配走代理或直连，本机统计始终直连
        Socks5Config proxy_cfg = GetSocks5Config();
        const Socks5Config* chatgpt_proxy = ChatGptUseProxy() ? &proxy_cfg : nullptr;
        const Socks5Config* zhipu_proxy = ZhipuUseProxy() ? &proxy_cfg : nullptr;

        for (size_t i = 0; i < jsons.size(); i++) {
            cJSON* root = cJSON_Parse(jsons[i].c_str());
            if (root == nullptr) {
                continue;
            }
            auto get_str = [&root](const char* key) -> std::string {
                cJSON* item = cJSON_GetObjectItem(root, key);
                return cJSON_IsString(item) ? item->valuestring : "";
            };

            AccountDetail acc;
            acc.email = get_str("email");
            acc.account_id = get_str("account_id");
            acc.plan = get_str("plan");
            acc.subscription_until = get_str("subscription_until");
            usage::FillDetailFromIdToken(acc, get_str("id_token"));
            std::string last_refresh = get_str("last_refresh");
            if (last_refresh.size() > 5) {
                acc.last_refresh = last_refresh.substr(5, 11);
            }
            size_t at = acc.email.find('@');
            acc.name = at != std::string::npos ? acc.email.substr(0, at) : acc.email;
            if (acc.name.empty()) {
                acc.name = "账号" + std::to_string(i + 1);
            }
            if (acc.name.size() > 16) {
                acc.name.resize(16);
            }

            std::string access_token = get_str("access_token");
            std::string final_token;
            int status = -1;
            if (!access_token.empty() && !acc.account_id.empty()) {
                status = usage::DirectFetchUsage(access_token, acc.account_id, acc, chatgpt_proxy);
                if (status == 200) {
                    final_token = access_token;
                }
            }
            if (status == 401) {
                // access_token 过期：OAuth 刷新后重试一次；新令牌在此回写 NVS
                auto [new_token, updated_json] = usage::RefreshCodexToken(jsons[i], chatgpt_proxy);
                if (!new_token.empty()) {
                    jsons[i] = updated_json;
                    if (!SaveDirectAuthJson((int)i, updated_json)) {
                        ESP_LOGE(TAG, "Refreshed token could not be persisted for account %u",
                                 (unsigned)i);
                    }
                    status = usage::DirectFetchUsage(new_token, acc.account_id, acc, chatgpt_proxy);
                    if (status == 200) {
                        final_token = new_token;
                    }
                }
            }
            if (status == 200 && !final_token.empty()) {
                // 每日用量/统计/重置积分；失败不致命，字段保持 -1
                usage::FetchOfficialExtras(final_token, acc.account_id, acc, chatgpt_proxy);
            }
            if (status == 200) {
                api_ok = true;
            } else if (status == 400 || status == 401 || status == 403) {
                acc.unavailable = true;
            }
            cJSON_Delete(root);
            api_details.push_back(std::move(acc));
        }

        // 智谱官方 API Key：独立于 ChatGPT 账号，查 GLM 套餐 token 用量
        auto zhipu_keys = LoadZhipuKeys();
        for (size_t i = 0; i < zhipu_keys.size(); i++) {
            AccountDetail acc;
            std::string tail = zhipu_keys[i].size() > 6 ? zhipu_keys[i].substr(zhipu_keys[i].size() - 6)
                                                        : zhipu_keys[i];
            acc.name = "智谱*" + tail;
            if (zhipu_keys.size() > 1) {
                acc.name = "智谱" + std::to_string(i + 1) + " *" + tail;
            }
            acc.email = "智谱开放平台 ****" + tail;
            int status = usage::FetchZhipuUsage(zhipu_keys[i], acc, zhipu_proxy);
            if (status == 200) {
                api_ok = true;
            } else if (status == 400 || status == 401 || status == 403 || status == -2) {
                acc.unavailable = true;
            }
            api_details.push_back(std::move(acc));
        }

        // 本机统计：每台 PC（box2-usage-server）独立查询，各生成 当天/本周/累计 三张卡片。
        // 多 PC 时卡片名带地址尾段区分；某台不可达只标记该 PC 的卡片 unavailable
        auto local_usages = LoadLocalUsages();
        for (size_t i = 0; i < local_usages.size(); i++) {
            const auto& cfg = local_usages[i];
            std::string suffix = local_usages.size() > 1 ? HostSuffix(cfg.host) : "";
            int status = usage::FetchLocalUsage(cfg.host, cfg.port, cfg.key, suffix, local_details);
            if (status == 200) {
                local_ok = true;
            } else {
                AccountDetail acc;
                acc.name = suffix.empty() ? "本地" : "本地@" + suffix;
                acc.email = "Claude Code @ " + cfg.host;
                acc.local_stats = true;
                acc.unavailable = true;
                local_details.push_back(std::move(acc));
            }
        }

        if (!api_ok && !local_ok) {
            HideUsagePanel();
            // 只有本机卡片时，失败原因一定是本地服务而非官方令牌
            GetDisplay()->ShowNotification(api_details.empty() && !local_details.empty()
                                               ? "本机统计服务不可达,检查PC端"
                                               : "直连查询失败:令牌失效或网络不可达",
                                           4000);
            return;
        }
        ShowUsageListPanel(std::move(api_details), std::move(local_details), api_ok, local_ok);
    }

    void QueryUsage(bool keep_source = false) {
        if (keep_source) {
            pending_restore_source_ = usage_source_;
            pending_detail_restore_ = CurrentDetailIndex();
        } else {
            pending_restore_source_ = -1;
            pending_detail_restore_ = -1;
        }
        if (LoadDirectAuthJsons().empty() && LoadZhipuKeys().empty() && LoadLocalUsages().empty()) {
            GetDisplay()->ShowNotification("未导入令牌,长按Q键配置", 3000);
            return;
        }
        // 自动刷新时面板已在显示，跳过 loading 重建避免闪烁，数据到达后直接替换
        if (usage_panel_ == nullptr) {
            ShowUsagePanelLoading();
        } else {
            power_save_timer_->WakeUp();
        }
        QueryDirectUsage();
    }


    // v2 面板配色：底/卡/描边/强调（青），语义色与徽章色沿用
    static constexpr uint32_t kPanelBg = 0x0C0F13;
    static constexpr uint32_t kPanelCard = 0x171C24;
    static constexpr uint32_t kPanelLine = 0x252C37;
    static constexpr uint32_t kPanelCyan = 0x35D0E8;
    static constexpr uint32_t kPanelCyanDark = 0x2E6E7E;
    static constexpr uint32_t kPanelCyanLight = 0x7FE8F5;

    // 套餐徽章三色（描边标签风格）：亮字 + 深底 + 同色 1px 描边，深色卡上边缘清晰
    static void PlanBadgeColors(const std::string& plan, uint32_t& text, uint32_t& bg, uint32_t& border) {
        if (strcasecmp(plan.c_str(), "pro") == 0) {
            text = 0xE5CEFF;
            bg = 0x2A1F38;
            border = 0x9B6BD4;
        } else if (strcasecmp(plan.c_str(), "team") == 0 || strcasecmp(plan.c_str(), "business") == 0 ||
                   strcasecmp(plan.c_str(), "lite") == 0) {
            text = 0xFFE38A;
            bg = 0x3A3117;
            border = 0xC9A227;
        } else if (plan == "本地") {
            text = 0x8FE9F7;
            bg = 0x0E2E36;
            border = 0x2E9DB2;
        } else if (strcasecmp(plan.c_str(), "free") == 0) {
            text = 0xC7CDD6;
            bg = 0x232830;
            border = 0x6B7684;
        } else {  // plus 及未知
            text = 0xB7D0FF;
            bg = 0x1F2C42;
            border = 0x5F8BDC;
        }
    }

    static std::string FormatResetShort(int seconds) {
        if (seconds <= 0) {
            return "";
        }
        char label[20];
        if (seconds >= 86400) {
            snprintf(label, sizeof(label), "%d日%d时", seconds / 86400, (seconds % 86400) / 3600);
        } else if (seconds >= 3600) {
            snprintf(label, sizeof(label), "%.1f时", seconds / 3600.0);
        } else {
            snprintf(label, sizeof(label), "%d分", seconds / 60);
        }
        return label;
    }

    // 进度条颜色按"已用百分比"分级（cc-switch 同款阈值）：<70 绿、70-89 橙、≥90 红
    static lv_color_t UsageBarColor(int used_pct) {
        if (used_pct < 0) {
            return lv_color_hex(0x5F6368);
        }
        if (used_pct >= 90) {
            return lv_color_hex(0xEA4335);
        }
        if (used_pct >= 70) {
            return lv_color_hex(0xFBBC04);
        }
        return lv_color_hex(0x34A853);
    }

    // 当前源的数据组与页面状态（官方 API / 本机统计各自独立）
    const std::vector<AccountDetail>& CurrentDetails() const {
        return usage_source_ == kUsageSourceLocal ? local_details_ : api_details_;
    }

    int CurrentDetailIndex() const {
        return usage_source_ == kUsageSourceLocal ? local_detail_index_ : api_detail_index_;
    }

    void SetCurrentDetailIndex(int index) {
        if (usage_source_ == kUsageSourceLocal) {
            local_detail_index_ = index;
        } else {
            api_detail_index_ = index;
        }
    }

    int CurrentListPage() const {
        return usage_source_ == kUsageSourceLocal ? local_list_page_ : api_list_page_;
    }

    void SetCurrentListPage(int page) {
        if (usage_source_ == kUsageSourceLocal) {
            local_list_page_ = page;
        } else {
            api_list_page_ = page;
        }
    }

    void HideUsagePanel() {
        if (usage_panel_timer_ != nullptr) {
            esp_timer_stop(usage_panel_timer_);
        }
        if (usage_refresh_timer_ != nullptr) {
            esp_timer_stop(usage_refresh_timer_);
        }
        if (usage_watch_timer_ != nullptr) {
            esp_timer_stop(usage_watch_timer_);
            power_save_timer_->SetEnabled(true);
        }
        if (usage_loading_timer_ != nullptr) {
            esp_timer_stop(usage_loading_timer_);
        }
        loading_label_ = nullptr;
        api_detail_index_ = -1;
        local_detail_index_ = -1;
        if (usage_panel_ == nullptr) {
            return;
        }
        DisplayLockGuard lock(GetDisplay());
        lv_obj_delete(usage_panel_);
        usage_panel_ = nullptr;
    }

    // 配置了自动刷新间隔时面板常驻（12 小时兜底），否则用默认的自动关闭时长
    int64_t GetUsagePanelHideTimeoutUs(int64_t default_us) {
        return GetUsageRefreshMinutes() > 0 ? int64_t(12) * 3600 * 1000000LL : default_us;
    }

    // 面板显示期间按配置间隔（分钟）自动重新查询
    void StartUsageAutoRefresh() {
        int minutes = GetUsageRefreshMinutes();
        if (minutes <= 0 || usage_refresh_timer_ == nullptr) {
            return;
        }
        esp_timer_stop(usage_refresh_timer_);
        esp_timer_start_once(usage_refresh_timer_, (int64_t)minutes * 60000000LL);
    }

    void RestartUsagePanelTimer(int64_t timeout_us) {
        if (usage_panel_timer_ == nullptr) {
            esp_timer_create_args_t timer_args = {
                .callback = [](void* arg) { instance_->HideUsagePanel(); },
                .arg = nullptr,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "usage_panel_hide",
                .skip_unhandled_events = true,
            };
            ESP_ERROR_CHECK(esp_timer_create(&timer_args, &usage_panel_timer_));
        }
        if (usage_refresh_timer_ == nullptr) {
            esp_timer_create_args_t refresh_args = {
                .callback = [](void* arg) {
                    if (instance_->usage_panel_ != nullptr) {
                        instance_->StartUsageQuery(true);
                    }
                },
                .arg = nullptr,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "usage_auto_refresh",
                .skip_unhandled_events = true,
            };
            ESP_ERROR_CHECK(esp_timer_create(&refresh_args, &usage_refresh_timer_));
        }
        esp_timer_stop(usage_panel_timer_);
        esp_timer_start_once(usage_panel_timer_, timeout_us);
    }

    // 创建面板骨架（深色全屏 + 标题），返回内容容器；UI 未就绪时返回 nullptr
    // 标题行右侧的数据时间戳
    std::string FormatUpdatedAt(time_t at) {
        if (at <= 0) {
            return "";
        }
        char label[16];
        strftime(label, sizeof(label), "更新%H:%M", localtime(&at));
        return label;
    }

    // 面板显示期间：对话开始自动收起 + 屏幕保持亮
    void StartUsagePanelWatchdog() {
        if (usage_watch_timer_ == nullptr) {
            esp_timer_create_args_t timer_args = {
                .callback = [](void* arg) {
                    auto state = Application::GetInstance().GetDeviceState();
                    if (state != kDeviceStateIdle && state != kDeviceStateStarting) {
                        instance_->HideUsagePanel();
                        return;
                    }
                    instance_->power_save_timer_->WakeUp();
                },
                .arg = nullptr,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "usage_watch",
                .skip_unhandled_events = true,
            };
            ESP_ERROR_CHECK(esp_timer_create(&timer_args, &usage_watch_timer_));
        }
        esp_timer_stop(usage_watch_timer_);
        esp_timer_start_periodic(usage_watch_timer_, 2000000);  // 2 秒
        power_save_timer_->SetEnabled(false);
        power_save_timer_->WakeUp();
    }

    // 面板主字体：主题字体副本串补字 fallback（周/日/分/智/谱等不在内置字体子集）。
    // 用副本是为了不被字幕动态字形缓存对主题字体 SetFallback 的覆盖影响
    const lv_font_t* PanelTextFont() {
        static lv_font_t font = [] {
            lv_font_t f = *static_cast<LvglTheme*>(instance_->GetDisplay()->GetTheme())->text_font()->font();
            f.fallback = &font_usage_extra_20;
            return f;
        }();
        return &font;
    }

    // 卡片进度条行/导航行的小号字体（ASCII + 面板用汉字全集）
    static const lv_font_t* PanelSmallFont() {
        return &font_usage_small_14;
    }

    // 16px 中间档（LVGL 嵌入式 14/16/20 阶梯）：信息模块的账号身份行，
    // 与 14px 元数据拉开层次
    static const lv_font_t* PanelMidFont() {
        return &font_usage_mid_16;
    }

    // 创建面板骨架（深色全屏 + 标题行）。页码（"1/2"）并入标题行右侧，不再单列按键说明行
    lv_obj_t* CreateUsagePanelBase(const char* title, const char* page_no) {
        HideUsagePanel();
        auto display = GetDisplay();
        if (display->GetTheme() == nullptr) {
            return nullptr;
        }
        DisplayLockGuard lock(display);
        auto font = PanelTextFont();

        usage_panel_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(usage_panel_, LV_HOR_RES, LV_VER_RES);
        lv_obj_add_flag(usage_panel_, LV_OBJ_FLAG_FLOATING);
        lv_obj_move_foreground(usage_panel_);
        lv_obj_set_style_bg_color(usage_panel_, lv_color_hex(kPanelBg), 0);
        lv_obj_set_style_bg_opa(usage_panel_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(usage_panel_, 0, 0);
        lv_obj_set_style_border_width(usage_panel_, 0, 0);
        lv_obj_set_style_pad_all(usage_panel_, 6, 0);
        lv_obj_set_flex_flow(usage_panel_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(usage_panel_, 4, 0);
        lv_obj_clear_flag(usage_panel_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_text_font(usage_panel_, font, 0);

        // 标题行：青色状态灯 + 左标题，右侧页码 + 数据时间戳
        lv_obj_t* header_row = lv_obj_create(usage_panel_);
        lv_obj_set_width(header_row, lv_pct(100));
        lv_obj_set_height(header_row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(header_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(header_row, 0, 0);
        lv_obj_set_style_pad_all(header_row, 0, 0);
        lv_obj_set_flex_flow(header_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(header_row, 6, 0);
        lv_obj_set_flex_align(header_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(header_row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* lamp = lv_obj_create(header_row);
        lv_obj_set_size(lamp, 8, 8);
        lv_obj_set_style_bg_color(lamp, lv_color_hex(kPanelCyan), 0);
        lv_obj_set_style_bg_opa(lamp, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(lamp, 2, 0);
        lv_obj_set_style_border_width(lamp, 0, 0);
        lv_obj_set_style_pad_all(lamp, 0, 0);
        lv_obj_clear_flag(lamp, LV_OBJ_FLAG_SCROLLABLE);

        // 标题行：标题 + 页码紧跟其后（内容宽），弹簧占位把刷新时间推到右侧
        lv_obj_t* header = lv_label_create(header_row);
        lv_obj_set_style_text_color(header, lv_color_hex(0xE8EAED), 0);
        lv_label_set_text(header, title);

        if (page_no != nullptr && page_no[0] != '\0') {
            lv_obj_t* page_label = lv_label_create(header_row);
            lv_obj_set_style_text_font(page_label, PanelSmallFont(), 0);
            lv_obj_set_style_text_color(page_label, lv_color_hex(0x9AA0A6), 0);
            lv_label_set_text(page_label, page_no);
        }

        lv_obj_t* spacer = lv_obj_create(header_row);
        lv_obj_set_width(spacer, 0);
        lv_obj_set_flex_grow(spacer, 1);
        lv_obj_set_height(spacer, 1);
        lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(spacer, 0, 0);
        lv_obj_set_style_pad_all(spacer, 0, 0);

        std::string updated = FormatUpdatedAt(usage_updated_at_);
        if (!updated.empty()) {
            lv_obj_t* updated_label = lv_label_create(header_row);
            lv_obj_set_style_text_font(updated_label, PanelSmallFont(), 0);
            lv_obj_set_style_text_color(updated_label, lv_color_hex(0x9AA0A6), 0);
            lv_obj_set_width(updated_label, 74);
            lv_obj_set_style_text_align(updated_label, LV_TEXT_ALIGN_RIGHT, 0);
            lv_label_set_long_mode(updated_label, LV_LABEL_LONG_DOT);
            lv_label_set_text(updated_label, updated.c_str());
        }

        lv_obj_t* content = lv_obj_create(usage_panel_);
        lv_obj_set_width(content, lv_pct(100));
        lv_obj_set_flex_grow(content, 1);
        lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(content, 0, 0);
        lv_obj_set_style_pad_all(content, 0, 0);
        lv_obj_set_style_pad_row(content, 4, 0);
        lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
        return content;
    }

    // 重置时刻（时钟）：当天显示 hh:mm，跨天带月日（如 8月21日 14:35）；依赖联网时间同步
    static std::string FormatResetClock(int seconds) {
        if (seconds <= 0) {
            return "";
        }
        time_t now = time(nullptr);
        time_t reset_at = now + seconds;
        struct tm tm_reset;
        struct tm tm_now;
        localtime_r(&reset_at, &tm_reset);
        localtime_r(&now, &tm_now);
        char time_buf[56];
        if (tm_reset.tm_yday == tm_now.tm_yday && tm_reset.tm_year == tm_now.tm_year) {
            strftime(time_buf, sizeof(time_buf), "%H:%M", &tm_reset);
        } else {
            snprintf(time_buf, sizeof(time_buf), "%d月%d日 %02d:%02d",
                     tm_reset.tm_mon + 1, tm_reset.tm_mday, tm_reset.tm_hour, tm_reset.tm_min);
        }
        return time_buf;
    }

    // 分段刻度条：n 段等宽，点亮段数四舍五入，颜色按已用百分比分级（cc-switch 阈值）。
    // grow=true 用于横向 flex 父容器（与百分比同行分享宽度）；
    // grow=false 用于纵向 flex 父容器（双卡），宽度撑满——否则 flex_grow 会变成纵向伸展
    void AddSegBar(lv_obj_t* parent, int used_pct, int nsegs, bool grow) {
        lv_obj_t* bar = lv_obj_create(parent);
        if (grow) {
            lv_obj_set_width(bar, 0);
            lv_obj_set_flex_grow(bar, 1);
        } else {
            lv_obj_set_width(bar, lv_pct(100));
        }
        lv_obj_set_height(bar, 12);
        lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_set_style_pad_column(bar, 2, 0);
        lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_color_t on_color = UsageBarColor(used_pct);
        int lit = used_pct >= 0 ? (used_pct * nsegs + 50) / 100 : 0;
        for (int i = 0; i < nsegs; i++) {
            lv_obj_t* seg = lv_obj_create(bar);
            lv_obj_set_width(seg, 0);
            lv_obj_set_flex_grow(seg, 1);
            lv_obj_set_height(seg, 12);
            lv_obj_set_style_bg_color(seg, i < lit ? on_color : lv_color_hex(kPanelLine), 0);
            lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(seg, 2, 0);
            lv_obj_set_style_border_width(seg, 0, 0);
            lv_obj_set_style_pad_all(seg, 0, 0);
            lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        }
    }

    // 列表卡窗口块（两行）：上 = 类型 20px + 重置时刻 14px 灰（右），
    // 下 = 10 段刻度条 + 20px 语义色大百分比（右对齐，卡片第一视觉层级）。
    // 查询失败显示 "--" 与空轨道
    void AddUsageWindowRow(lv_obj_t* parent, const std::string& type, int used_pct, int reset_seconds) {
        lv_obj_t* win = lv_obj_create(parent);
        lv_obj_set_width(win, lv_pct(100));
        lv_obj_set_height(win, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(win, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(win, 0, 0);
        lv_obj_set_style_pad_all(win, 0, 0);
        lv_obj_set_style_pad_row(win, 3, 0);
        lv_obj_set_flex_flow(win, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(win, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* top = lv_obj_create(win);
        lv_obj_set_width(top, lv_pct(100));
        lv_obj_set_height(top, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(top, 0, 0);
        lv_obj_set_style_pad_all(top, 0, 0);
        lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* type_label = lv_label_create(top);
        lv_obj_set_style_text_font(type_label, PanelTextFont(), 0);
        lv_obj_set_style_text_color(type_label, lv_color_hex(0xE8EAED), 0);
        lv_label_set_text(type_label, type.c_str());

        std::string clock = FormatResetClock(reset_seconds);
        if (!clock.empty()) {
            lv_obj_t* spacer = lv_obj_create(top);
            lv_obj_set_width(spacer, 0);
            lv_obj_set_flex_grow(spacer, 1);
            lv_obj_set_height(spacer, 1);
            lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(spacer, 0, 0);
            lv_obj_set_style_pad_all(spacer, 0, 0);
            lv_obj_t* clock_label = lv_label_create(top);
            lv_obj_set_style_text_font(clock_label, PanelSmallFont(), 0);
            lv_obj_set_style_text_color(clock_label, lv_color_hex(0x9AA0A6), 0);
            lv_label_set_text(clock_label, clock.c_str());
        }

        lv_obj_t* main = lv_obj_create(win);
        lv_obj_set_width(main, lv_pct(100));
        lv_obj_set_height(main, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(main, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(main, 0, 0);
        lv_obj_set_style_pad_all(main, 0, 0);
        lv_obj_set_style_pad_column(main, 6, 0);
        lv_obj_set_flex_flow(main, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(main, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(main, LV_OBJ_FLAG_SCROLLABLE);

        AddSegBar(main, used_pct, 10, true);

        lv_obj_t* pct_label = lv_label_create(main);
        lv_obj_set_style_text_font(pct_label, PanelTextFont(), 0);
        lv_obj_set_style_text_color(pct_label,
                                    used_pct >= 0 ? UsageBarColor(used_pct) : lv_color_hex(0x9AA0A6), 0);
        lv_obj_set_width(pct_label, 50);
        lv_obj_set_style_text_align(pct_label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(pct_label, used_pct >= 0 ? (std::to_string(used_pct) + "%").c_str() : "--");
    }

    // 列表卡（v2 仪表风）：左侧 3px 健康竖条一眼分级（最紧窗口语义色/失败红/本机青）。
    // 官方卡 = 名字+套餐徽章 + 窗口块（5小时 + 每周，免费套餐次窗口为 30日）；
    // 本机卡 = 名字+本地徽章 + 倒计时行 + 日/周/累计三行数值；失败卡显示提示与空轨道
    void AddUsageCard(lv_obj_t* parent, const AccountDetail& acc) {
        auto font = PanelTextFont();

        lv_obj_t* card = lv_obj_create(parent);
        lv_obj_set_width(card, lv_pct(100));
        // 卡片弹性填满整页可用高度（每页固定 2 张），内容纵向均匀分布不留大片空白
        lv_obj_set_height(card, 0);
        lv_obj_set_flex_grow(card, 1);
        lv_obj_set_style_bg_color(card, lv_color_hex(kPanelCard), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(kPanelLine), 0);
        lv_obj_set_style_pad_top(card, 6, 0);
        lv_obj_set_style_pad_bottom(card, 6, 0);
        lv_obj_set_style_pad_left(card, 11, 0);
        lv_obj_set_style_pad_right(card, 8, 0);
        lv_obj_set_style_pad_row(card, 4, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        // 健康竖条：负偏移抵消 pad_left 贴到卡片左缘，高度按内容区百分比伸缩。
        // 卡片是 flex 容器，必须 IGNORE_LAYOUT 脱离流式布局，否则 set_pos 无效
        lv_obj_t* health = lv_obj_create(card);
        lv_obj_add_flag(health, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_pos(health, -8, 8);
        lv_obj_set_width(health, 3);
        lv_obj_set_height(health, lv_pct(86));
        lv_color_t health_color = lv_color_hex(kPanelCyan);
        if (acc.unavailable) {
            health_color = lv_color_hex(0xEA4335);
        } else if (!acc.local_stats) {
            int worst = std::max(acc.used_5h, std::max(acc.used_weekly, acc.used_monthly));
            health_color = UsageBarColor(worst);
        }
        lv_obj_set_style_bg_color(health, health_color, 0);
        lv_obj_set_style_bg_opa(health, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(health, 2, 0);
        lv_obj_set_style_border_width(health, 0, 0);
        lv_obj_set_style_pad_all(health, 0, 0);
        lv_obj_clear_flag(health, LV_OBJ_FLAG_SCROLLABLE);

        // 标题行：名字 + 套餐/本地徽章
        lv_obj_t* title_row = lv_obj_create(card);
        lv_obj_set_width(title_row, lv_pct(100));
        lv_obj_set_height(title_row, 24);
        lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(title_row, 0, 0);
        lv_obj_set_style_pad_all(title_row, 0, 0);
        lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(title_row, 5, 0);
        lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* name_label = lv_label_create(title_row);
        lv_obj_set_style_text_font(name_label, font, 0);
        lv_obj_set_style_text_color(name_label, lv_color_hex(acc.unavailable ? 0xEA4335 : 0xE8EAED), 0);
        lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name_label, 0);
        lv_obj_set_flex_grow(name_label, 1);
        std::string display_name = acc.unavailable ? "! " + acc.name : acc.name;
        lv_label_set_text(name_label, display_name.c_str());

        if (!acc.plan.empty()) {
            uint32_t text_color = 0;
            uint32_t bg_color = 0;
            uint32_t border_color = 0;
            PlanBadgeColors(acc.plan, text_color, bg_color, border_color);
            lv_obj_t* plan_label = lv_label_create(title_row);
            lv_obj_set_style_text_font(plan_label, PanelSmallFont(), 0);
            lv_obj_set_style_text_color(plan_label, lv_color_hex(text_color), 0);
            lv_obj_set_style_bg_color(plan_label, lv_color_hex(bg_color), 0);
            lv_obj_set_style_bg_opa(plan_label, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(plan_label, 1, 0);
            lv_obj_set_style_border_color(plan_label, lv_color_hex(border_color), 0);
            lv_obj_set_style_radius(plan_label, 4, 0);
            lv_obj_set_style_pad_hor(plan_label, 5, 0);
            lv_obj_set_style_pad_ver(plan_label, 2, 0);
            lv_label_set_text(plan_label, acc.plan.c_str());
        }

        // 本机统计卡片：倒计时行 + 当天/本周/累计三行（左 tag 右数值，一页两台 PC）。
        // 数值 20px 主字体（万/亿 单位不在 14px 小字库），尾注 14px 灰
        if (acc.local_stats) {
            if (acc.unavailable) {
                lv_obj_t* dead_label = lv_label_create(card);
                lv_obj_set_width(dead_label, lv_pct(100));
                lv_obj_set_style_text_font(dead_label, PanelSmallFont(), 0);
                lv_obj_set_style_text_color(dead_label, lv_color_hex(0x9AA0A6), 0);
                lv_label_set_long_mode(dead_label, LV_LABEL_LONG_DOT);
                lv_label_set_text(dead_label, "服务不可达 · 检查PC端服务");
                return;
            }
            std::string reset = FormatResetShort(acc.reset_5h);
            if (!reset.empty()) {
                lv_obj_t* reset_label = lv_label_create(card);
                lv_obj_set_width(reset_label, lv_pct(100));
                lv_obj_set_height(reset_label, 16);
                lv_obj_set_style_text_font(reset_label, PanelSmallFont(), 0);
                lv_obj_set_style_text_color(reset_label, lv_color_hex(0x9AA0A6), 0);
                lv_label_set_long_mode(reset_label, LV_LABEL_LONG_DOT);
                lv_label_set_text(reset_label, (reset + " 后重置").c_str());
            }
            if (acc.today_tokens >= 0) {
                auto add_row = [&](const char* tag, const std::string& value, const std::string& tail, bool dim) {
                    lv_obj_t* row = lv_obj_create(card);
                    lv_obj_set_width(row, lv_pct(100));
                    lv_obj_set_height(row, LV_SIZE_CONTENT);
                    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
                    lv_obj_set_style_border_width(row, 0, 0);
                    lv_obj_set_style_pad_all(row, 0, 0);
                    lv_obj_set_style_pad_column(row, 6, 0);
                    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
                    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
                    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

                    // 仪表式行：tag 紧跟主值(20px)，尾注(次数/费用)吃剩余宽度右对齐，超宽自动省略
                    lv_obj_t* tag_label = lv_label_create(row);
                    lv_obj_set_style_text_font(tag_label, font, 0);
                    lv_obj_set_style_text_color(tag_label,
                                                lv_color_hex(dim ? 0x9AA0A6 : kPanelCyan), 0);
                    lv_label_set_text(tag_label, tag);

                    lv_obj_t* value_label = lv_label_create(row);
                    lv_obj_set_style_text_font(value_label, font, 0);
                    lv_obj_set_style_text_color(value_label,
                                                lv_color_hex(dim ? 0x9AA0A6 : 0xE8EAED), 0);
                    lv_label_set_text(value_label, value.c_str());

                    if (!tail.empty()) {
                        lv_obj_t* tail_label = lv_label_create(row);
                        lv_obj_set_style_text_font(tail_label, PanelSmallFont(), 0);
                        lv_obj_set_style_text_color(tail_label, lv_color_hex(0x9AA0A6), 0);
                        lv_obj_set_width(tail_label, 0);
                        lv_obj_set_flex_grow(tail_label, 1);
                        lv_obj_set_style_text_align(tail_label, LV_TEXT_ALIGN_RIGHT, 0);
                        lv_label_set_long_mode(tail_label, LV_LABEL_LONG_DOT);
                        lv_label_set_text(tail_label, tail.c_str());
                    }
                };
                auto tail_text = [](int requests, double cost) {
                    std::string cost_text = cost >= 0 ? usage::CostText(cost, true) : "";
                    std::string tail;
                    if (requests >= 0) {
                        tail = std::to_string(requests) + "次";
                    }
                    if (!cost_text.empty()) {
                        tail = tail.empty() ? cost_text : tail + " · " + cost_text;
                    }
                    return tail;
                };
                add_row("日", usage::FormatTokens(acc.today_tokens),
                        tail_text(acc.requests, acc.today_cost), false);
                add_row("周", usage::FormatTokens(acc.week_tokens),
                        tail_text(acc.week_requests, acc.week_cost), false);
                add_row("累", usage::FormatTokens(acc.lifetime_tokens),
                        acc.lifetime_cost >= 0 ? usage::CostText(acc.lifetime_cost, true) : "", true);
            }
        }

        if (acc.local_stats) {
            return;
        }

        // 官方账号：窗口块（5小时 + 每周；免费/prolite 套餐次窗口是 30 日）。
        // 失败卡显示提示行 + 空轨道占位，保持版式稳定
        if (acc.unavailable) {
            lv_obj_t* dead_label = lv_label_create(card);
            lv_obj_set_width(dead_label, lv_pct(100));
            lv_obj_set_style_text_font(dead_label, PanelSmallFont(), 0);
            lv_obj_set_style_text_color(dead_label, lv_color_hex(0x9AA0A6), 0);
            lv_label_set_long_mode(dead_label, LV_LABEL_LONG_DOT);
            lv_label_set_text(dead_label, "令牌异常 · 建议重新导入");
            AddUsageWindowRow(card, "5小时", -1, -1);
            return;
        }
        if (acc.used_5h >= 0) {
            AddUsageWindowRow(card, "5小时", acc.used_5h, acc.reset_5h);
        }
        if (acc.used_weekly >= 0) {
            AddUsageWindowRow(card, "每周", acc.used_weekly, acc.reset_weekly);
        } else if (acc.used_monthly >= 0) {
            AddUsageWindowRow(card, "30日", acc.used_monthly, acc.reset_monthly);
        }
    }

    void ShowUsagePanelLoading() {
        lv_obj_t* content = CreateUsagePanelBase("AI 用量", nullptr);
        if (content == nullptr) {
            GetDisplay()->ShowNotification("查询套餐用量...", 8000);
            return;
        }
        DisplayLockGuard lock(GetDisplay());
        auto font = PanelTextFont();

        lv_obj_t* row = lv_obj_create(content);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        loading_label_ = lv_label_create(row);
        lv_obj_set_style_text_font(loading_label_, font, 0);
        lv_obj_set_style_text_color(loading_label_, lv_color_hex(0x9AA0A6), 0);
        lv_label_set_text(loading_label_, "查询中");
        StartLoadingDotsAnimation();
        RestartUsagePanelTimer(25000000LL);
    }

    // LV_USE_SPINNER 未启用，用文字点动画做加载指示
    void StartLoadingDotsAnimation() {
        if (usage_loading_timer_ == nullptr) {
            esp_timer_create_args_t timer_args = {
                .callback = [](void* arg) {
                    DisplayLockGuard lock(instance_->GetDisplay());
                    if (instance_->loading_label_ != nullptr && lv_obj_is_valid(instance_->loading_label_)) {
                        instance_->loading_dots_ = (instance_->loading_dots_ + 1) % 4;
                        lv_label_set_text(instance_->loading_label_,
                                          ("查询中" + std::string(instance_->loading_dots_, '.')).c_str());
                    }
                },
                .arg = nullptr,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "usage_loading",
                .skip_unhandled_events = true,
            };
            ESP_ERROR_CHECK(esp_timer_create(&timer_args, &usage_loading_timer_));
        }
        loading_dots_ = 0;
        esp_timer_start_periodic(usage_loading_timer_, 500000);
    }

    // 近 14 天每日用量柱状图（官方 daily_usage_buckets，取末尾最多 14 天）：
    // 日期坐标直接嵌在每根柱底（16px 轨道区）；今日亮青、峰日（>80% 峰值）琥珀、其余暗青。
    // flex_grow 吃满详情页剩余高度，柱高按数据归一化
    void AddDailyTokensChart(lv_obj_t* parent, const std::vector<std::pair<std::string, long long>>& buckets) {
        lv_obj_t* chart = lv_obj_create(parent);
        lv_obj_set_width(chart, lv_pct(100));
        lv_obj_set_height(chart, 0);
        lv_obj_set_flex_grow(chart, 1);
        lv_obj_set_style_bg_color(chart, lv_color_hex(kPanelCard), 0);
        lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(chart, 8, 0);
        lv_obj_set_style_border_width(chart, 1, 0);
        lv_obj_set_style_border_color(chart, lv_color_hex(kPanelLine), 0);
        lv_obj_set_style_pad_all(chart, 5, 0);
        lv_obj_set_style_pad_column(chart, 1, 0);
        lv_obj_set_flex_flow(chart, LV_FLEX_FLOW_ROW);
        lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);

        size_t count = buckets.size() > 14 ? 14 : buckets.size();
        size_t offset = buckets.size() - count;
        long long max_tokens = 1;
        for (size_t i = offset; i < buckets.size(); i++) {
            max_tokens = std::max(max_tokens, buckets[i].second);
        }
        for (size_t i = offset; i < buckets.size(); i++) {
            long long tokens = buckets[i].second;
            bool is_today = (i + 1 == buckets.size());
            bool is_peak = tokens * 100 > max_tokens * 80;

            // 每根柱：上段用量填充（高度按峰值归一化，底对齐），下段 16px 日期坐标区
            lv_obj_t* column = lv_obj_create(chart);
            lv_obj_set_width(column, 0);
            lv_obj_set_flex_grow(column, 1);
            lv_obj_set_height(column, lv_pct(100));
            lv_obj_set_style_bg_opa(column, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(column, 0, 0);
            lv_obj_set_style_pad_all(column, 0, 0);
            lv_obj_set_style_pad_row(column, 1, 0);
            lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(column, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_clear_flag(column, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t* bar_area = lv_obj_create(column);
            lv_obj_set_width(bar_area, lv_pct(100));
            lv_obj_set_height(bar_area, 0);
            lv_obj_set_flex_grow(bar_area, 1);
            lv_obj_set_style_bg_opa(bar_area, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(bar_area, 0, 0);
            lv_obj_set_style_pad_all(bar_area, 0, 0);
            lv_obj_set_flex_flow(bar_area, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(bar_area, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_clear_flag(bar_area, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t* fill = lv_obj_create(bar_area);
            lv_obj_set_width(fill, lv_pct(100));
            int pct_height = tokens > 0 ? (int)(tokens * 100 / max_tokens) : 0;
            if (pct_height < 4) {
                pct_height = 4;
            }
            lv_obj_set_height(fill, lv_pct(pct_height));
            lv_obj_set_style_bg_color(fill, lv_color_hex(is_today ? kPanelCyanLight
                                                     : is_peak  ? 0xFBBB04
                                                                : kPanelCyanDark),
                                      0);
            lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(fill, 2, 0);
            lv_obj_set_style_border_width(fill, 0, 0);
            lv_obj_set_style_pad_all(fill, 0, 0);
            lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);

            // 日期坐标（柱底）：start_date 形如 2026-08-14，取两位日期
            std::string day = buckets[i].first.size() >= 10 ? buckets[i].first.substr(8, 2) : "";
            lv_obj_t* day_cell = lv_obj_create(column);
            lv_obj_set_width(day_cell, lv_pct(100));
            lv_obj_set_height(day_cell, 18);
            lv_obj_set_style_bg_color(day_cell, lv_color_hex(is_today ? 0x0E3A46 : kPanelLine), 0);
            lv_obj_set_style_bg_opa(day_cell, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(day_cell, 2, 0);
            lv_obj_set_style_border_width(day_cell, 0, 0);
            lv_obj_set_style_pad_all(day_cell, 0, 0);
            lv_obj_clear_flag(day_cell, LV_OBJ_FLAG_SCROLLABLE);
            if (!day.empty()) {
                lv_obj_t* day_label = lv_label_create(day_cell);
                lv_obj_set_style_text_font(day_label, PanelSmallFont(), 0);
                lv_obj_set_style_text_color(day_label,
                                            lv_color_hex(is_today ? kPanelCyanLight
                                                         : is_peak  ? 0xFBBB04
                                                                    : 0x9AA0A6),
                                            0);
                lv_obj_center(day_label);
                lv_label_set_text(day_label, day.c_str());
            }
        }
    }

    // 详情页账户卡（一屏固定布局，无滚动）：名称(16px)+类型徽章，
    // 下方数字小卡 = 官方(累计token/连续天数) 或 本机(累计token/累计费用)。
    // 无值字段整块隐藏：智谱账号退化为单行，卡片高度自适应
    void AddAccountCard(lv_obj_t* parent, const AccountDetail& acc) {
        lv_obj_t* card = lv_obj_create(parent);
        lv_obj_set_width(card, lv_pct(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, lv_color_hex(kPanelCard), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(kPanelLine), 0);
        lv_obj_set_style_pad_ver(card, 6, 0);
        lv_obj_set_style_pad_hor(card, 7, 0);
        lv_obj_set_style_pad_row(card, 4, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        // 名称行：邮箱(官方)/PC名(本机) 16px 中间档 + 类型徽章
        lv_obj_t* title_row = lv_obj_create(card);
        lv_obj_set_width(title_row, lv_pct(100));
        lv_obj_set_height(title_row, 20);
        lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(title_row, 0, 0);
        lv_obj_set_style_pad_all(title_row, 0, 0);
        lv_obj_set_style_pad_column(title_row, 5, 0);
        lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* name_label = lv_label_create(title_row);
        lv_obj_set_style_text_font(name_label, PanelMidFont(), 0);
        lv_obj_set_style_text_color(name_label, lv_color_hex(acc.unavailable ? 0xEA4335 : 0xE8EAED), 0);
        lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name_label, 0);
        lv_obj_set_flex_grow(name_label, 1);
        // 本机条目显示短名(本地@xx)：email 里的 "AI CLI @ <ip>" 过长必截断
        lv_label_set_text(name_label,
                          acc.local_stats ? acc.name.c_str()
                                          : (acc.email.empty() ? acc.name.c_str() : acc.email.c_str()));

        if (!acc.plan.empty()) {
            uint32_t plan_text = 0;
            uint32_t plan_bg = 0;
            uint32_t plan_border = 0;
            PlanBadgeColors(acc.plan, plan_text, plan_bg, plan_border);
            lv_obj_t* plan_label = lv_label_create(title_row);
            lv_obj_set_style_text_font(plan_label, PanelSmallFont(), 0);
            lv_obj_set_style_text_color(plan_label, lv_color_hex(plan_text), 0);
            lv_obj_set_style_bg_color(plan_label, lv_color_hex(plan_bg), 0);
            lv_obj_set_style_bg_opa(plan_label, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(plan_label, 1, 0);
            lv_obj_set_style_border_color(plan_label, lv_color_hex(plan_border), 0);
            lv_obj_set_style_radius(plan_label, 4, 0);
            lv_obj_set_style_pad_hor(plan_label, 5, 0);
            lv_obj_set_style_pad_ver(plan_label, 2, 0);
            lv_label_set_text(plan_label, acc.plan.c_str());
        }

        // 数字小卡对：只取有值的前两项
        std::vector<std::pair<std::string, std::string>> stats;
        if (acc.lifetime_tokens >= 0) {
            stats.emplace_back("累计token", usage::FormatTokens(acc.lifetime_tokens));
        }
        if (!acc.local_stats && acc.current_streak_days >= 0) {
            stats.emplace_back("连续天数", std::to_string(acc.current_streak_days) + "天");
        } else if (acc.local_stats && acc.lifetime_cost >= 0) {
            stats.emplace_back("累计费用", usage::CostText(acc.lifetime_cost, true));
        }
        if (stats.empty()) {
            return;
        }

        lv_obj_t* stats_row = lv_obj_create(card);
        lv_obj_set_width(stats_row, lv_pct(100));
        lv_obj_set_height(stats_row, 44);
        lv_obj_set_style_bg_opa(stats_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(stats_row, 0, 0);
        lv_obj_set_style_pad_all(stats_row, 0, 0);
        lv_obj_set_style_pad_column(stats_row, 4, 0);
        lv_obj_set_flex_flow(stats_row, LV_FLEX_FLOW_ROW);
        lv_obj_clear_flag(stats_row, LV_OBJ_FLAG_SCROLLABLE);
        for (const auto& kv : stats) {
            AddStatCard(stats_row, kv.first, kv.second);
        }
    }

    // 数字小卡：左标签(14px 灰) 右值(16px 白)，屏幕底色形成内嵌层次
    void AddStatCard(lv_obj_t* parent, const std::string& key, const std::string& value) {
        lv_obj_t* stat = lv_obj_create(parent);
        lv_obj_set_width(stat, 0);
        lv_obj_set_flex_grow(stat, 1);
        lv_obj_set_height(stat, lv_pct(100));
        lv_obj_set_style_bg_color(stat, lv_color_hex(kPanelBg), 0);
        lv_obj_set_style_bg_opa(stat, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(stat, 6, 0);
        lv_obj_set_style_border_width(stat, 0, 0);
        lv_obj_set_style_pad_ver(stat, 3, 0);
        lv_obj_set_style_pad_hor(stat, 7, 0);
        lv_obj_set_flex_flow(stat, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(stat, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(stat, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* key_label = lv_label_create(stat);
        lv_obj_set_style_text_font(key_label, PanelSmallFont(), 0);
        lv_obj_set_style_text_color(key_label, lv_color_hex(0x9AA0A6), 0);
        lv_label_set_text(key_label, key.c_str());

        lv_obj_t* value_label = lv_label_create(stat);
        lv_obj_set_style_text_font(value_label, PanelMidFont(), 0);
        lv_obj_set_style_text_color(value_label, lv_color_hex(0xE8EAED), 0);
        lv_obj_set_width(value_label, lv_pct(100));
        lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(value_label, value.c_str());
    }

    // 详情页左右双卡壳：flex1 圆角描边卡，内容纵向均匀分布
    lv_obj_t* DuoCardShell(lv_obj_t* parent) {
        lv_obj_t* card = lv_obj_create(parent);
        lv_obj_set_width(card, 0);
        lv_obj_set_flex_grow(card, 1);
        lv_obj_set_height(card, lv_pct(100));
        lv_obj_set_style_bg_color(card, lv_color_hex(kPanelCard), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(kPanelLine), 0);
        lv_obj_set_style_pad_ver(card, 6, 0);
        lv_obj_set_style_pad_hor(card, 7, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        return card;
    }

    // 双卡标签行：左标签 + 右注（14px 灰）
    void AddDuoLabelRow(lv_obj_t* card, const std::string& label, const std::string& note) {
        lv_obj_t* row = lv_obj_create(card);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* label_obj = lv_label_create(row);
        lv_obj_set_style_text_font(label_obj, PanelSmallFont(), 0);
        lv_obj_set_style_text_color(label_obj, lv_color_hex(0x9AA0A6), 0);
        lv_label_set_text(label_obj, label.c_str());

        lv_obj_t* spacer = lv_obj_create(row);
        lv_obj_set_width(spacer, 0);
        lv_obj_set_flex_grow(spacer, 1);
        lv_obj_set_height(spacer, 1);
        lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(spacer, 0, 0);
        lv_obj_set_style_pad_all(spacer, 0, 0);

        if (!note.empty()) {
            lv_obj_t* note_label = lv_label_create(row);
            lv_obj_set_style_text_font(note_label, PanelSmallFont(), 0);
            lv_obj_set_style_text_color(note_label, lv_color_hex(0x9AA0A6), 0);
            lv_label_set_text(note_label, note.c_str());
        }
    }

    // 双卡大数值行：20px 居中，卡片第一视觉层级
    void AddDuoBigText(lv_obj_t* card, const std::string& text, uint32_t color) {
        lv_obj_t* label = lv_label_create(card);
        lv_obj_set_style_text_font(label, PanelTextFont(), 0);
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
        lv_obj_set_width(label, lv_pct(100));
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(label, text.c_str());
    }

    // 详情页官方窗口双卡：标签行(窗口名+重置时刻) + 20px 语义色大百分比 + 8 段刻度条
    void AddPctDuoCard(lv_obj_t* parent, const std::string& label, int used_pct, int reset_seconds) {
        lv_obj_t* card = DuoCardShell(parent);
        AddDuoLabelRow(card, label, FormatResetClock(reset_seconds));
        uint32_t pct_color = 0x9AA0A6;
        if (used_pct >= 90) {
            pct_color = 0xEA4335;
        } else if (used_pct >= 70) {
            pct_color = 0xFBBB04;
        } else if (used_pct >= 0) {
            pct_color = 0x34A853;
        }
        AddDuoBigText(card, used_pct >= 0 ? std::to_string(used_pct) + "%" : "--", pct_color);
        AddSegBar(card, used_pct, 8, false);
    }

    // 详情页本机双卡：标签行(窗口名+次数) + 20px 青色大数值 + 底部费用行
    void AddLocalDuoCard(lv_obj_t* parent, const std::string& label, const std::string& big_text,
                         int requests, double cost) {
        lv_obj_t* card = DuoCardShell(parent);
        AddDuoLabelRow(card, label, requests >= 0 ? std::to_string(requests) + "次" : "");
        AddDuoBigText(card, big_text, kPanelCyan);
        // 底部费用行（14px 灰，右对齐；无值时省略）
        std::string cost_text = cost >= 0 ? usage::CostText(cost, true) : "";
        if (!cost_text.empty()) {
            AddDuoLabelRow(card, "", cost_text);
        }
    }

    // 本机模型分类块（14px 两列）：标题 + 前 2 项 + 其余汇总，吃满剩余空间均匀分布
    void AddModelSection(lv_obj_t* parent, const AccountDetail& acc) {
        auto add_row = [&](const std::string& name, const std::string& value) {
            lv_obj_t* row = lv_obj_create(parent);
            lv_obj_set_width(row, lv_pct(100));
            lv_obj_set_height(row, LV_SIZE_CONTENT);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_pad_all(row, 0, 0);
            lv_obj_set_style_pad_column(row, 6, 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t* name_label = lv_label_create(row);
            lv_obj_set_style_text_font(name_label, PanelSmallFont(), 0);
            lv_obj_set_style_text_color(name_label, lv_color_hex(0xE8EAED), 0);
            lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
            lv_obj_set_width(name_label, 0);
            lv_obj_set_flex_grow(name_label, 1);
            lv_label_set_text(name_label, name.c_str());

            lv_obj_t* value_label = lv_label_create(row);
            lv_obj_set_style_text_font(value_label, PanelSmallFont(), 0);
            lv_obj_set_style_text_color(value_label, lv_color_hex(0x9AA0A6), 0);
            lv_label_set_text(value_label, value.c_str());
        };

        AddDuoLabelRow(parent, "当天模型", "");
        size_t shown = acc.local_models.size() > 2 ? 2 : acc.local_models.size();
        for (size_t i = 0; i < shown; i++) {
            const auto& m = acc.local_models[i];
            add_row(m.name, usage::FormatTokens(m.tokens) + " " + usage::CostText(m.cost, m.priced));
        }
        if (acc.local_models.size() > shown) {
            long long rest_tokens = 0;
            double rest_cost = 0;
            bool rest_priced = true;
            for (size_t i = shown; i < acc.local_models.size(); i++) {
                rest_tokens += acc.local_models[i].tokens;
                rest_cost += acc.local_models[i].cost > 0 ? acc.local_models[i].cost : 0;
                rest_priced = rest_priced && acc.local_models[i].priced;
            }
            // 含未定价模型时费用被低估，显示 "-" 而非误导性的 $0.00
            add_row("其余" + std::to_string(acc.local_models.size() - shown) + "模型",
                    usage::FormatTokens(rest_tokens) + " " + usage::CostText(rest_cost, rest_priced));
        }
    }

    // 智谱账号不设详情页：仅 5h/周窗口百分比，与列表卡内容重复且无每日明细/元数据
    static bool HasDetailPage(const AccountDetail& acc) {
        return acc.name.rfind("智谱", 0) != 0;
    }

    // 详情页 L/R 切换：跳过无详情页的账号（智谱），循环回绕
    void SwitchUsageDetail(int step) {
        const auto& details = CurrentDetails();
        if (details.empty()) {
            return;
        }
        int index = CurrentDetailIndex();
        for (int i = 1; i <= (int)details.size(); i++) {
            int next = ((index + step * i) % (int)details.size() + (int)details.size()) % (int)details.size();
            if (HasDetailPage(details[next])) {
                ShowUsageDetailPanel(next);
                return;
            }
        }
    }

    // M 键从列表进入的单账号详情页（一屏固定布局，无滚动条）：
    // ① 账户卡（名称/类型徽章/累计token+连续天数数字小卡）
    // ② 左右双卡（官方 5小时+7天，免费套餐次窗口 30天；本机 当天+本周）
    // ③ 官方近 14 天柱状图 / 本机模型分类 / 均无数据时纵向均匀分布。
    // 数据来自当前源分组缓存；L/R 循环切账号（跳过智谱），M 返回列表
    void ShowUsageDetailPanel(int index) {
        if (CurrentDetails().empty()) {
            return;
        }
        index = ((index % (int)CurrentDetails().size()) + (int)CurrentDetails().size()) % (int)CurrentDetails().size();
        const AccountDetail& acc = CurrentDetails()[index];
        if (!HasDetailPage(acc)) {
            return;
        }

        std::string page_no = std::to_string(index + 1) + "/" + std::to_string(CurrentDetails().size());
        lv_obj_t* content = CreateUsagePanelBase("账号详情", page_no.c_str());
        if (content == nullptr) {
            GetDisplay()->ShowNotification("详情页创建失败", 3000);
            return;
        }
        // CreateUsagePanelBase 会先销毁旧页面并清理详情索引，必须在页面创建后恢复。
        // 之前在创建前赋值导致索引立刻变回 -1，L/R 因而被误判成列表翻页。
        SetCurrentDetailIndex(index);

        DisplayLockGuard lock(GetDisplay());
        ESP_LOGI(TAG, "Show usage detail %d/%d", index + 1, (int)CurrentDetails().size());

        // ① 账户卡
        AddAccountCard(content, acc);

        // ② 左右双卡（固定高 92）
        bool has_duo = acc.local_stats ? acc.today_tokens >= 0 || acc.week_tokens >= 0
                                       : acc.used_5h >= 0 || acc.used_weekly >= 0 || acc.used_monthly >= 0;
        if (has_duo) {
            lv_obj_t* duo = lv_obj_create(content);
            lv_obj_set_width(duo, lv_pct(100));
            lv_obj_set_height(duo, 92);
            lv_obj_set_style_bg_opa(duo, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(duo, 0, 0);
            lv_obj_set_style_pad_all(duo, 0, 0);
            lv_obj_set_style_pad_column(duo, 4, 0);
            lv_obj_set_flex_flow(duo, LV_FLEX_FLOW_ROW);
            lv_obj_clear_flag(duo, LV_OBJ_FLAG_SCROLLABLE);

            if (acc.local_stats) {
                AddLocalDuoCard(duo, "当天", usage::FormatTokens(acc.today_tokens),
                                acc.requests, acc.today_cost);
                AddLocalDuoCard(duo, "本周", usage::FormatTokens(acc.week_tokens),
                                acc.week_requests, acc.week_cost);
            } else if (acc.used_5h >= 0) {
                AddPctDuoCard(duo, "5小时", acc.used_5h, acc.reset_5h);
                if (acc.used_weekly >= 0) {
                    AddPctDuoCard(duo, "7天", acc.used_weekly, acc.reset_weekly);
                } else {
                    AddPctDuoCard(duo, "30天", acc.used_monthly, acc.reset_monthly);
                }
            } else if (acc.used_weekly >= 0) {
                AddPctDuoCard(duo, "7天", acc.used_weekly, acc.reset_weekly);
            } else {
                AddPctDuoCard(duo, "30天", acc.used_monthly, acc.reset_monthly);
            }
        }

        // ③ 官方近 14 天柱状图；本机改模型分类；两者皆无 → 内容纵向均匀分布
        if (!acc.local_stats && !acc.daily_buckets.empty()) {
            AddDailyTokensChart(content, acc.daily_buckets);
        } else {
            lv_obj_set_flex_align(content, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
            if (acc.local_stats && !acc.local_models.empty()) {
                AddModelSection(content, acc);
            }
        }

        RestartUsagePanelTimer(GetUsagePanelHideTimeoutUs(30000000LL));
        StartUsageAutoRefresh();
        StartUsagePanelWatchdog();
    }

    // 240x320 屏幕的两种数据源均固定每页 2 条
    int UsageCardsPerPage() const {
        return usage_source_ == kUsageSourceLocal ? kLocalCardsPerPage : kUsageCardsPerPage;
    }

    int UsageListPages() {
        return ((int)CurrentDetails().size() + UsageCardsPerPage() - 1) / UsageCardsPerPage();
    }

    // 列表页：当前页的 2 张卡片完整显示 + 页码在标题行（无按键说明行）
    void ShowUsageListPage() {
        int pages = UsageListPages();
        std::string page_no = std::to_string(CurrentListPage() + 1) + "/" + std::to_string(pages);
        const char* title = usage_source_ == kUsageSourceLocal ? "本地用量" : "AI 用量";
        lv_obj_t* content = CreateUsagePanelBase(title, pages > 1 ? page_no.c_str() : nullptr);
        if (content == nullptr) {
            GetDisplay()->ShowNotification("用量面板创建失败", 3000);
            return;
        }
        SetCurrentDetailIndex(-1);

        DisplayLockGuard lock(GetDisplay());
        lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
        int begin = CurrentListPage() * UsageCardsPerPage();
        int end = std::min(begin + UsageCardsPerPage(), (int)CurrentDetails().size());
        for (int i = begin; i < end; i++) {
            AddUsageCard(content, CurrentDetails()[i]);
        }
        RestartUsagePanelTimer(GetUsagePanelHideTimeoutUs(15000000LL));
        StartUsageAutoRefresh();
        StartUsagePanelWatchdog();
    }

    // 列表模式翻页（循环）
    void SwitchUsageListPage(int step) {
        int pages = UsageListPages();
        if (pages <= 1) {
            return;
        }
        SetCurrentListPage(((CurrentListPage() + step) % pages + pages) % pages);
        ShowUsageListPage();
    }

    // M 键在详情页再按时回到列表（保持当前页）
    void BackToUsageList() {
        if (CurrentDetails().empty()) {
            HideUsagePanel();
            return;
        }
        ShowUsageListPage();
        if (usage_panel_ == nullptr) {
            HideUsagePanel();
        }
    }

    // 新数据到达：更新两组缓存；自动刷新时恢复原源原页，否则打开初始源（官方 API 优先）
    void ShowUsageListPanel(std::vector<AccountDetail> api_details, std::vector<AccountDetail> local_details,
                            bool api_ok, bool local_ok) {
        api_details_ = std::move(api_details);
        local_details_ = std::move(local_details);
        usage_updated_at_ = time(nullptr);
        if (api_list_page_ >= ((int)api_details_.size() + kUsageCardsPerPage - 1) / kUsageCardsPerPage) {
            api_list_page_ = 0;
        }
        if (local_list_page_ >= ((int)local_details_.size() + kLocalCardsPerPage - 1) / kLocalCardsPerPage) {
            local_list_page_ = 0;
        }

        int restore_source = pending_restore_source_;
        int restore = pending_detail_restore_;
        pending_restore_source_ = -1;
        pending_detail_restore_ = -1;
        if (restore_source == kUsageSourceLocal && !local_details_.empty()) {
            usage_source_ = kUsageSourceLocal;
        } else if (restore_source == kUsageSourceApi && !api_details_.empty()) {
            usage_source_ = kUsageSourceApi;
        } else {
            // 初始源：官方 API 查询成功则优先（失败卡片仍保留在组内，可手动 Q 切入查看）
            usage_source_ = api_ok ? kUsageSourceApi : kUsageSourceLocal;
        }
        if (restore >= 0 && !CurrentDetails().empty()) {
            ShowUsageDetailPanel(restore);
            if (usage_panel_ != nullptr) {
                return;
            }
        }

        ShowUsageListPage();
    }

    esp_err_t IoExpanderSetLevel(uint16_t pin_mask, uint8_t level) {
        return esp_io_expander_set_level(io_exp_handle, pin_mask, level);
    }

    uint8_t IoExpanderGetLevel(uint16_t pin_mask) {
        uint32_t pin_val = 0;
        esp_io_expander_get_level(io_exp_handle, DRV_IO_EXP_INPUT_MASK, &pin_val);
        pin_mask &= DRV_IO_EXP_INPUT_MASK;
        return (uint8_t)((pin_val & pin_mask) ? 1 : 0);
    }

    void InitializeIoExpander() {
        esp_err_t ret = ESP_OK;
        esp_io_expander_new_i2c_tca95xx_16bit(i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_000, &io_exp_handle);

        ret |= esp_io_expander_set_dir(io_exp_handle, DRV_IO_EXP_OUTPUT_MASK, IO_EXPANDER_OUTPUT);
        ret |= esp_io_expander_set_dir(io_exp_handle, DRV_IO_EXP_INPUT_MASK, IO_EXPANDER_INPUT);

        ret |= esp_io_expander_set_level(io_exp_handle, XIO_SYS_POW, 1);
        ret |= esp_io_expander_set_level(io_exp_handle, XIO_EN_3V3A, 1);
        ret |= esp_io_expander_set_level(io_exp_handle, XIO_EN_4G, 1);
        ret |= esp_io_expander_set_level(io_exp_handle, XIO_SPK_EN, 1);
        ret |= esp_io_expander_set_level(io_exp_handle, XIO_USB_SEL, 1);
        ret |= esp_io_expander_set_level(io_exp_handle, XIO_VBUS_EN, 0);

        assert(ret == ESP_OK);
    }

    // Initialize I2C peripheral
    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeButtons() {
        instance_ = this;

        button_config_t l_btn_cfg = {
            .long_press_time = 800,
            .short_press_time = 500
        };

        button_config_t m_btn_cfg = {
            .long_press_time = 800,
            .short_press_time = 500
        };

        button_config_t r_btn_cfg = {
            .long_press_time = 800,
            .short_press_time = 500
        };

        button_driver_t* xio_l_btn_driver_ = nullptr;
        button_driver_t* xio_m_btn_driver_ = nullptr;

        button_handle_t l_btn_handle = NULL;
        button_handle_t m_btn_handle = NULL;
        button_handle_t r_btn_handle = NULL;

        xio_l_btn_driver_ = (button_driver_t*)calloc(1, sizeof(button_driver_t));
        xio_l_btn_driver_->enable_power_save = false;
        xio_l_btn_driver_->get_key_level = [](button_driver_t *button_driver) -> uint8_t {
            return !instance_->IoExpanderGetLevel(XIO_KEY_L);
        };
        ESP_ERROR_CHECK(iot_button_create(&l_btn_cfg, xio_l_btn_driver_, &l_btn_handle));

        xio_m_btn_driver_ = (button_driver_t*)calloc(1, sizeof(button_driver_t));
        xio_m_btn_driver_->enable_power_save = false;
        xio_m_btn_driver_->get_key_level = [](button_driver_t *button_driver) -> uint8_t {
            return instance_->IoExpanderGetLevel(XIO_KEY_M);
        };
        ESP_ERROR_CHECK(iot_button_create(&m_btn_cfg, xio_m_btn_driver_, &m_btn_handle));

        // 返回键（XIO_KEY_Q）：短按打断对话/查询用量，长按进入用量查询配置模式
        q_key_active_high_ = IoExpanderGetLevel(XIO_KEY_Q) == 0;
        button_driver_t* xio_q_btn_driver_ = (button_driver_t*)calloc(1, sizeof(button_driver_t));
        xio_q_btn_driver_->enable_power_save = false;
        xio_q_btn_driver_->get_key_level = [](button_driver_t *button_driver) -> uint8_t {
            if (instance_->q_key_active_high_) {
                return instance_->IoExpanderGetLevel(XIO_KEY_Q);
            }
            return !instance_->IoExpanderGetLevel(XIO_KEY_Q);
        };
        button_handle_t q_btn_handle = NULL;
        ESP_ERROR_CHECK(iot_button_create(&m_btn_cfg, xio_q_btn_driver_, &q_btn_handle));

        button_gpio_config_t r_cfg = {
            .gpio_num = R_BUTTON_GPIO,
            .active_level = BUTTON_INACTIVE,
            .enable_power_save = false,
            .disable_pull = false
        };
        ESP_ERROR_CHECK(iot_button_new_gpio_device(&r_btn_cfg, &r_cfg, &r_btn_handle));

        iot_button_register_cb(l_btn_handle, BUTTON_PRESS_DOWN, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<atk_dnesp32s3_box2_wifi*>(usr_data);
            self->power_save_timer_->WakeUp();
            if (self->CurrentDetailIndex() >= 0) {
                self->SwitchUsageDetail(-1);
                return;
            }
            if (self->usage_panel_ != nullptr && !self->usage_config_mode_) {
                self->SwitchUsageListPage(-1);
                return;
            }
            self->audio_volume_change(false);
        }, this);

        iot_button_register_cb(l_btn_handle, BUTTON_LONG_PRESS_START, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<atk_dnesp32s3_box2_wifi*>(usr_data);
            self->power_save_timer_->WakeUp();
            self->audio_volume_minimum();
        }, this);

        iot_button_register_cb(m_btn_handle, BUTTON_PRESS_DOWN, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<atk_dnesp32s3_box2_wifi*>(usr_data);
            self->power_save_timer_->WakeUp();
            if (!self->usage_config_mode_ && self->usage_panel_ != nullptr) {
                if (self->CurrentDetailIndex() >= 0) {
                    self->BackToUsageList();
                } else {
                    // 从当前页的第一个有详情账号进入（智谱无详情页）；整页均无则不动作
                    int begin = self->CurrentListPage() * self->UsageCardsPerPage();
                    int end = std::min(begin + self->UsageCardsPerPage(), (int)self->CurrentDetails().size());
                    for (int i = begin; i < end; i++) {
                        if (HasDetailPage(self->CurrentDetails()[i])) {
                            self->ShowUsageDetailPanel(i);
                            break;
                        }
                    }
                }
                return;
            }
            auto& app = Application::GetInstance();
            app.ToggleChatState();
        }, this);

        iot_button_register_cb(m_btn_handle, BUTTON_LONG_PRESS_START, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<atk_dnesp32s3_box2_wifi*>(usr_data);

            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                self->EnterWifiConfigMode();
                return;
            }

            if (self->power_status_ == kDeviceBatterySupply) {
                auto backlight = Board::GetInstance().GetBacklight();
                backlight->SetBrightness(0);
                esp_timer_stop(self->power_manager_->timer_handle_);
                esp_io_expander_set_dir(self->io_exp_handle, XIO_CHG_CTRL, IO_EXPANDER_OUTPUT);
                esp_io_expander_set_level(self->io_exp_handle, XIO_CHG_CTRL, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_io_expander_set_level(self->io_exp_handle, XIO_SYS_POW, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }, this);

        iot_button_register_cb(q_btn_handle, BUTTON_PRESS_DOWN, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<atk_dnesp32s3_box2_wifi*>(usr_data);
            self->power_save_timer_->WakeUp();
            if (self->usage_config_mode_) {
                self->GetDisplay()->ShowNotification("长按Q键退出配置", 2000);
                return;
            }
            auto& app = Application::GetInstance();
            auto state = app.GetDeviceState();
            if (state == kDeviceStateListening || state == kDeviceStateSpeaking) {
                app.ToggleChatState();
            } else if (state == kDeviceStateIdle) {
                if (self->usage_panel_ != nullptr) {
                    // Q 循环：官方面板 → 本机面板 → 关闭；目标组无数据（未配置/查询失败）时直接关闭
                    if (self->usage_source_ == self->kUsageSourceApi && !self->local_details_.empty()) {
                        self->usage_source_ = self->kUsageSourceLocal;
                        self->ShowUsageListPage();
                    } else {
                        self->HideUsagePanel();
                    }
                } else {
                    self->StartUsageQuery();
                }
            }
        }, this);

        iot_button_register_cb(q_btn_handle, BUTTON_LONG_PRESS_START, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<atk_dnesp32s3_box2_wifi*>(usr_data);
            self->power_save_timer_->WakeUp();
            if (self->usage_config_mode_) {
                self->ExitUsageConfigMode();
            } else {
                self->EnterUsageConfigMode();
            }
        }, this);

        iot_button_register_cb(r_btn_handle, BUTTON_PRESS_DOWN, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<atk_dnesp32s3_box2_wifi*>(usr_data);
            self->power_save_timer_->WakeUp();
            if (self->CurrentDetailIndex() >= 0) {
                self->SwitchUsageDetail(1);
                return;
            }
            if (self->usage_panel_ != nullptr && !self->usage_config_mode_) {
                self->SwitchUsageListPage(1);
                return;
            }
            self->audio_volume_change(true);
        }, this);

        iot_button_register_cb(r_btn_handle, BUTTON_LONG_PRESS_START, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<atk_dnesp32s3_box2_wifi*>(usr_data);
            self->power_save_timer_->WakeUp();
            self->audio_volume_maxmum();
        }, this);
    }

    void InitializeSt7789Display() {
        ESP_LOGI(TAG, "Install panel IO");

        /* RD PIN */
        gpio_config_t gpio_init_struct;
        gpio_init_struct.intr_type = GPIO_INTR_DISABLE;
        gpio_init_struct.mode = GPIO_MODE_INPUT_OUTPUT;
        gpio_init_struct.pin_bit_mask = 1ull << LCD_PIN_RD;
        gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;
        gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&gpio_init_struct);
        gpio_set_level(LCD_PIN_RD, 1);

        /* BL PIN */
        gpio_init_struct.pin_bit_mask = 1ull << DISPLAY_BACKLIGHT_PIN;
        gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;
        gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&gpio_init_struct);

        esp_lcd_i80_bus_handle_t i80_bus = NULL;
        esp_lcd_i80_bus_config_t bus_config = {
            .dc_gpio_num = LCD_PIN_DC,
            .wr_gpio_num = LCD_PIN_WR,
            .clk_src = LCD_CLK_SRC_DEFAULT,
            .data_gpio_nums = {
                LCD_PIN_D0,
                LCD_PIN_D1,
                LCD_PIN_D2,
                LCD_PIN_D3,
                LCD_PIN_D4,
                LCD_PIN_D5,
                LCD_PIN_D6,
                LCD_PIN_D7,
            },
            .bus_width = 8,
            .max_transfer_bytes = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
            .dma_burst_size = 64,
        };
        ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &i80_bus));

        esp_lcd_panel_io_i80_config_t io_config = {
            .cs_gpio_num = LCD_PIN_CS,
            .pclk_hz = (20 * 1000 * 1000),
            .trans_queue_depth = 7,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .dc_levels = {
                .dc_idle_level = 1,
                .dc_cmd_level = 0,
                .dc_dummy_level = 0,
                .dc_data_level = 1,
            },
            .flags = {
                .cs_active_high = 0,        
                .pclk_active_neg = 0,       
                .pclk_idle_low = 0,           
            },
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.reset_gpio_num = LCD_PIN_RST;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_set_gap(panel, 0, 0);
        esp_lcd_panel_io_tx_param(panel_io, 0xCF, (uint8_t[]) {0x00,0x83,0x30}, 3);
        esp_lcd_panel_io_tx_param(panel_io, 0xED, (uint8_t[]) {0x64,0x03,0x12,0x81}, 4);
        esp_lcd_panel_io_tx_param(panel_io, 0xE8, (uint8_t[]) {0x85,0x01,0x79}, 3);
        esp_lcd_panel_io_tx_param(panel_io, 0xCB, (uint8_t[]) {0x39,0x2C,0x00,0x34,0x02}, 5);
        esp_lcd_panel_io_tx_param(panel_io, 0xF7, (uint8_t[]) {0x20}, 1);
        esp_lcd_panel_io_tx_param(panel_io, 0xEA, (uint8_t[]) {0x00,0x00}, 2);
        esp_lcd_panel_io_tx_param(panel_io, 0xbb, (uint8_t[]) {0x20}, 1);
        esp_lcd_panel_io_tx_param(panel_io, 0xc3, (uint8_t[]) {0x00}, 1);
        esp_lcd_panel_io_tx_param(panel_io, 0xC4, (uint8_t[]) {0x20}, 1);
        esp_lcd_panel_io_tx_param(panel_io, 0xC5, (uint8_t[]) {0x20}, 1);
        esp_lcd_panel_io_tx_param(panel_io, 0xC6, (uint8_t[]) {0x10}, 1);
        esp_lcd_panel_io_tx_param(panel_io, 0xC7, (uint8_t[]) {0xB0}, 1);
        esp_lcd_panel_io_tx_param(panel_io, 0x36, (uint8_t[]) {0x60}, 1);
        esp_lcd_panel_io_tx_param(panel_io, 0x3A, (uint8_t[]) {0x55}, 1);
        esp_lcd_panel_io_tx_param(panel_io, 0xB1, (uint8_t[]) {0x00,0x1B}, 2);
        esp_lcd_panel_io_tx_param(panel_io, 0xF2, (uint8_t[]) {0x08}, 1);
        esp_lcd_panel_io_tx_param(panel_io, 0x26, (uint8_t[]) {0x01}, 1);
        esp_lcd_panel_io_tx_param(panel_io, 0xE0, (uint8_t[]) {0xD0,0x00,0x02,0x07,0x0A,0x28,0x32,0x44,0x42,0x06,0x0E,0x12,0x14,0x17}, 14);
        esp_lcd_panel_io_tx_param(panel_io, 0xE1, (uint8_t[]) {0xD0,0x00,0x02,0x07,0x0A,0x28,0x31,0x54,0x47,0x0E,0x1C,0x17,0x1B,0x1E}, 14);
        esp_lcd_panel_io_tx_param(panel_io, 0xB7, (uint8_t[]) {0x07}, 1);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

public:
    atk_dnesp32s3_box2_wifi()  {
        InitializeVendorNvs();
        InitializeI2c();
        InitializeIoExpander();
        InitializePowerSaveTimer();
        InitializePowerManager();
        InitializeSt7789Display();
        // 系统级文本（通知等）的主题字体串补字 fallback；字幕动态字形激活后会替换该
        // fallback，用量面板因使用独立字体副本（PanelTextFont）不受影响
        static_cast<LvglTheme*>(GetDisplay()->GetTheme())->text_font()->SetFallback(&font_usage_extra_20);
        InitializeButtons();
        GetBacklight()->RestoreBrightness();
        InitializeBoardPowerManager();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8389AudioCodec audio_codec(
            i2c_bus_, 
            I2C_NUM_0, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            GPIO_NUM_NC, 
            AUDIO_CODEC_ES8389_ADDR,
            AUDIO_CODEC_USE_MCLK);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = power_manager_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(atk_dnesp32s3_box2_wifi);

// 定义静态成员变量
atk_dnesp32s3_box2_wifi* atk_dnesp32s3_box2_wifi::instance_ = nullptr;
bool atk_dnesp32s3_box2_wifi::vendor_partition_ok_ = false;
