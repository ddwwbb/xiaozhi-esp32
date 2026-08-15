#include "wifi_board.h"
#include "codecs/es8389_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
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
    // XIO_KEY_Q 有效电平无原理图依据（L 键低有效、M 键高有效），上电按空闲电平推断
    bool q_key_active_high_ = true;
    // 长按 Q 进入的配置模式：临时 HTTP 页面把用量查询配置写入 NVS
    bool usage_config_mode_ = false;
    httpd_handle_t usage_config_server_ = nullptr;
    esp_timer_handle_t usage_config_exit_timer_ = nullptr;
    // 单账号用量数据：remaining/reset 为 -1 表示查询失败
    struct AccountDetail {
        std::string email;
        std::string plan;
        std::string name;                // 邮箱前缀，无邮箱时为 账号N
        std::string subscription_until;  // 订阅有效期（显示子串）
        std::string last_refresh;        // Token 最近刷新（显示子串）
        bool unavailable = false;  // 401/403 等令牌异常标记
        // 官方 wham/profiles/me 与 rate-limit-reset-credits 数据
        std::vector<std::pair<std::string, long long>> daily_buckets;  // 每日 token 用量
        long long lifetime_tokens = -1;
        long long peak_daily_tokens = -1;
        long current_streak_days = -1;
        int reset_credits = -1;  // 可用限额重置积分
        // api-call wham/usage 查询结果
        int remaining_5h = -1;
        int remaining_weekly = -1;
        int remaining_cr = -1;  // 代码审查窗口
        int reset_5h = -1;
        int reset_weekly = -1;
        int reset_cr = -1;
        // 查询时需要
        std::string account_id;
    };

    // Q 键查询结果的全屏用量面板（LVGL，用时创建、关闭即删除）
    lv_obj_t* usage_panel_ = nullptr;
    esp_timer_handle_t usage_panel_timer_ = nullptr;
    // 面板数据缓存：列表页与详情页共用；-1 表示当前是列表页
    std::vector<AccountDetail> usage_details_;
    int usage_detail_index_ = -1;
    // 面板显示期间的自动刷新（0=关闭），以及自动刷新后恢复到原详情页
    esp_timer_handle_t usage_refresh_timer_ = nullptr;
    esp_timer_handle_t usage_watch_timer_ = nullptr;
    esp_timer_handle_t usage_loading_timer_ = nullptr;
    lv_obj_t* loading_label_ = nullptr;
    int loading_dots_ = 0;
    int pending_detail_restore_ = -1;
    time_t usage_updated_at_ = 0;


    // 直连模式的账号令牌（NVS，vendor 命名空间），内容为 codex auth.json 格式
    static std::string CodexAuthKey(int index) {
        return "codex_auth_" + std::to_string(index + 1);
    }

    std::vector<std::string> LoadDirectAuthJsons() {
        std::vector<std::string> jsons;
        Settings settings("vendor");
        int count = settings.GetInt("codex_auth_count", 0);
        for (int i = 0; i < count && i < 8; i++) {
            std::string json = settings.GetString(CodexAuthKey(i), "");
            if (!json.empty()) {
                jsons.push_back(std::move(json));
            }
        }
        return jsons;
    }

    void SaveDirectAuthJsons(const std::vector<std::string>& jsons) {
        Settings settings("vendor", true);
        int old_count = settings.GetInt("codex_auth_count", 0);
        for (int i = (int)jsons.size(); i < old_count; i++) {
            settings.EraseKey(CodexAuthKey(i));
        }
        for (size_t i = 0; i < jsons.size(); i++) {
            settings.SetString(CodexAuthKey((int)i), jsons[i]);
        }
        settings.SetInt("codex_auth_count", (int)jsons.size());
    }

    int GetUsageRefreshMinutes() {
        Settings settings("vendor");
        return settings.GetInt("usage_refresh_minutes", 0);
    }

    // 直连模式可选代理（socks5 或 http CONNECT，均支持账号密码认证）
    struct Socks5Config {
        std::string type;  // "socks5" | "http"
        std::string host;
        int port = 0;
        std::string user;
        std::string pass;
        bool enabled() const { return !host.empty() && port > 0; }
    };

    Socks5Config GetSocks5Config() {
        Socks5Config proxy;
        Settings settings("vendor");
        proxy.type = settings.GetString("usage_proxy_type", "socks5");
        proxy.host = settings.GetString("usage_proxy_host", "");
        proxy.port = settings.GetInt("usage_proxy_port", 0);
        proxy.user = settings.GetString("usage_proxy_user", "");
        proxy.pass = settings.GetString("usage_proxy_pass", "");
        return proxy;
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

    static std::string GetConfigPage() {
        auto cfg_proxy = instance_->GetSocks5Config();
        int refresh_minutes = instance_->GetUsageRefreshMinutes();

        std::string page =
            "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>用量查询配置</title></head>"
            "<body style=\"font-family:sans-serif;max-width:520px;margin:24px auto\">"
            "<h2>ChatGPT 用量查询配置（官方接口直连）</h2>"
            "<h3>直连账号</h3>";

        auto jsons = instance_->LoadDirectAuthJsons();
        if (jsons.empty()) {
            page += "<p><small>尚未导入账号令牌。把 Codex 令牌文件（auth.json）内容粘贴到下面导入，"
                    "设备将直连官方接口查询用量并自动刷新令牌。</small></p>";
        } else {
            for (size_t i = 0; i < jsons.size(); i++) {
                cJSON* root = cJSON_Parse(jsons[i].c_str());
                std::string email = "未知账号";
                if (root != nullptr) {
                    cJSON* email_item = cJSON_GetObjectItem(root, "email");
                    if (cJSON_IsString(email_item)) {
                        email = email_item->valuestring;
                    }
                    cJSON_Delete(root);
                }
                page += "<form method=\"POST\" action=\"/delete\" style=\"margin:2px 0\">"
                        "<input type=\"hidden\" name=\"index\" value=\"" + std::to_string(i) + "\">"
                        "<button type=\"submit\" style=\"width:100%;text-align:left\">" +
                        std::to_string(i + 1) + ". " + email + "  [删除]</button></form>";
            }
        }

        page += "<form method=\"POST\" action=\"/import\">"
                "<textarea name=\"auth_json\" rows=\"5\" style=\"width:100%\" "
                "placeholder=\"粘贴 Codex 令牌文件内容（auth.json，需含 refresh_token）\"></textarea>"
                "<p><button type=\"submit\">导入令牌</button> <small>同一账号重复导入会覆盖，最多 8 个</small></p>"
                "</form>"

                "<form method=\"POST\" action=\"/save\">"

                "<h3>网络代理（可选，访问官方接口需要）</h3>"
                "<p>类型 "
                "<select name=\"proxy_type\">"
                "<option value=\"socks5\"" + std::string(cfg_proxy.type == "socks5" ? " selected" : "") + ">SOCKS5</option>"
                "<option value=\"http\"" + std::string(cfg_proxy.type == "http" ? " selected" : "") + ">HTTP</option>"
                "</select> "
                "地址 <input name=\"proxy_host\" value=\"" + cfg_proxy.host +
                "\" style=\"width:130px\" placeholder=\"192.168.1.2\"> "
                "端口 <input name=\"proxy_port\" type=\"number\" value=\"" +
                (cfg_proxy.port > 0 ? std::to_string(cfg_proxy.port) : "") +
                "\" style=\"width:70px\" placeholder=\"7890\"></p>"
                "<p>账号 <input name=\"proxy_user\" value=\"" + cfg_proxy.user +
                "\" style=\"width:120px\"> "
                "密码 <input name=\"proxy_pass\" type=\"password\" value=\"" + cfg_proxy.pass +
                "\" style=\"width:120px\"> <small>无认证可留空；Clash/v2ray 混合端口两种类型都支持</small></p>"

                "<h3>自动刷新</h3>"
                "<p>间隔分钟（0=关闭，面板 15 秒自动关闭；大于 0 时面板常驻并按此间隔自动刷新）<br>"
                "<input name=\"refresh_minutes\" type=\"number\" min=\"0\" max=\"1440\" value=\"" +
                std::to_string(refresh_minutes) + "\" style=\"width:120px\"></p>"
                "<p><button type=\"submit\">保存</button> "
                "<button type=\"submit\" formaction=\"/clear_proxy\" formmethod=\"post\">清除代理</button></p>"
                "<p><small>保存后设备自动退出配置模式；导入/删除账号不退出，可连续操作。</small></p>"
                "</form></body></html>";
        return page;
    }

    static esp_err_t UsageConfigGetHandler(httpd_req_t* req) {
        std::string page = GetConfigPage();
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req, page.c_str(), HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    static void SendSimplePage(httpd_req_t* req, const std::string& body, const char* back_text) {
        std::string page = "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                           "<meta http-equiv=\"refresh\" content=\"2;url=/\"></head><body>" +
                           body + "<p><a href=\"/\">" + back_text + "</a></p></body></html>";
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req, page.c_str(), HTTPD_RESP_USE_STRLEN);
    }

    // 从已存令牌 JSON 提取 account_id（去重用）
    static std::string ExtractAccountId(const std::string& json) {
        cJSON* root = cJSON_Parse(json.c_str());
        if (root == nullptr) {
            return "";
        }
        cJSON* item = cJSON_GetObjectItem(root, "account_id");
        std::string account_id = cJSON_IsString(item) ? item->valuestring : "";
        if (account_id.empty()) {
            cJSON* id_token = cJSON_GetObjectItem(root, "id_token");
            if (cJSON_IsString(id_token)) {
                cJSON* payload = DecodeJwtPayload(id_token->valuestring);
                cJSON* auth = payload ? cJSON_GetObjectItem(payload, "https://api.openai.com/auth") : nullptr;
                cJSON* account_item = auth ? cJSON_GetObjectItem(auth, "chatgpt_account_id") : nullptr;
                if (cJSON_IsString(account_item)) {
                    account_id = account_item->valuestring;
                }
                cJSON_Delete(payload);
            }
        }
        cJSON_Delete(root);
        return account_id;
    }

    static esp_err_t UsageConfigImportHandler(httpd_req_t* req) {
        if (req->content_len > 16384) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
            return ESP_FAIL;
        }
        std::vector<char> buf(req->content_len + 1, 0);
        int received = httpd_req_recv(req, buf.data(), req->content_len);
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
        std::string account_id = ExtractAccountId(auth_json);
        if (account_id.empty()) {
            cJSON_Delete(root);
            SendSimplePage(req, "<h3>导入失败：无法确定 account_id</h3>", "返回");
            return ESP_OK;
        }
        cJSON* account_item = cJSON_GetObjectItem(root, "account_id");
        if (cJSON_IsString(account_item)) {
            cJSON_ReplaceItemInObject(root, "account_id", cJSON_CreateString(account_id.c_str()));
        } else {
            cJSON_AddStringToObject(root, "account_id", account_id.c_str());
        }

        auto jsons = instance_->LoadDirectAuthJsons();
        int replace_index = -1;
        for (size_t i = 0; i < jsons.size(); i++) {
            if (ExtractAccountId(jsons[i]) == account_id) {
                replace_index = (int)i;
                break;
            }
        }
        if (replace_index < 0 && jsons.size() >= 8) {
            cJSON_Delete(root);
            SendSimplePage(req, "<h3>导入失败：最多 8 个账号</h3>", "返回");
            return ESP_OK;
        }

        char* printed = cJSON_PrintUnformatted(root);
        if (printed == nullptr) {
            cJSON_Delete(root);
            SendSimplePage(req, "<h3>导入失败：序列化错误</h3>", "返回");
            return ESP_OK;
        }
        if (replace_index >= 0) {
            jsons[replace_index] = printed;
        } else {
            jsons.push_back(printed);
        }
        cJSON_free(printed);
        cJSON_Delete(root);
        instance_->SaveDirectAuthJsons(jsons);

        SendSimplePage(req, "<h3>已导入，当前共 " + std::to_string(jsons.size()) + " 个账号</h3>", "返回");
        return ESP_OK;
    }

    static esp_err_t UsageConfigClearProxyHandler(httpd_req_t* req) {
        Settings settings("vendor", true);
        settings.SetString("usage_proxy_type", "socks5");
        settings.EraseKey("usage_proxy_host");
        settings.EraseKey("usage_proxy_port");
        settings.EraseKey("usage_proxy_user");
        settings.EraseKey("usage_proxy_pass");
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
        instance_->SaveDirectAuthJsons(jsons);
        SendSimplePage(req, "<h3>已删除，剩余 " + std::to_string(jsons.size()) + " 个账号</h3>", "返回");
        return ESP_OK;
    }

    static esp_err_t UsageConfigSaveHandler(httpd_req_t* req) {
        char body[768] = {0};
        size_t total = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1;
        int received = httpd_req_recv(req, body, total);
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

        Settings settings("vendor", true);
        if (!proxy_type.empty()) {
            settings.SetString("usage_proxy_type", proxy_type == "http" ? "http" : "socks5");
        }
        if (!proxy_host.empty()) {
            settings.SetString("usage_proxy_host", proxy_host);
        }
        if (!proxy_port.empty()) {
            int port = atoi(proxy_port.c_str());
            if (port > 0 && port < 65536) {
                settings.SetInt("usage_proxy_port", port);
            }
        }
        if (!proxy_user.empty()) {
            settings.SetString("usage_proxy_user", proxy_user);
        }
        if (!proxy_pass.empty()) {
            settings.SetString("usage_proxy_pass", proxy_pass);
        }
        if (!refresh_minutes.empty()) {
            int minutes = atoi(refresh_minutes.c_str());
            if (minutes < 0) minutes = 0;
            if (minutes > 1440) minutes = 1440;
            settings.SetInt("usage_refresh_minutes", minutes);
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

        httpd_uri_t clear_proxy_uri = {};
        clear_proxy_uri.uri = "/clear_proxy";
        clear_proxy_uri.method = HTTP_POST;
        clear_proxy_uri.handler = UsageConfigClearProxyHandler;
        httpd_register_uri_handler(usage_config_server_, &clear_proxy_uri);

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

    // 用导入的账号令牌直连官方接口查询用量并展示
    void StartChatGptUsageQuery() {
        if (usage_fetching_.exchange(true)) {
            return;
        }
        if (xTaskCreate([](void* arg) {
                auto self = static_cast<atk_dnesp32s3_box2_wifi*>(arg);
                self->QueryChatGptUsage();
                self->usage_fetching_ = false;
                vTaskDelete(nullptr);
            }, "usage_query", 10240, this, 4, nullptr) != pdPASS) {
            usage_fetching_ = false;
        }
    }

    // 直连官方接口模式：使用导入的 codex 令牌；401 时自动刷新并重试
    void QueryDirectUsage() {
        std::vector<std::string> jsons = LoadDirectAuthJsons();
        std::vector<AccountDetail> details;
        int ok_count = 0;

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
            FillDetailFromIdToken(acc, get_str("id_token"));
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
                status = DirectFetchUsage(access_token, acc.account_id, acc);
                if (status == 200) {
                    final_token = access_token;
                }
            }
            if (status == 401) {
                // access_token 过期：OAuth 刷新（新令牌已回写 NVS）后重试一次
                std::string new_token = RefreshCodexToken(root, (int)i);
                if (!new_token.empty()) {
                    status = DirectFetchUsage(new_token, acc.account_id, acc);
                    if (status == 200) {
                        final_token = new_token;
                    }
                }
            }
            if (status == 200 && !final_token.empty()) {
                // 每日用量/统计/重置积分；失败不致命，字段保持 -1
                FetchOfficialExtras(final_token, acc.account_id, acc);
            }
            if (status == 200) {
                ok_count++;
            } else if (status == 400 || status == 401 || status == 403) {
                acc.unavailable = true;
            }
            cJSON_Delete(root);
            details.push_back(std::move(acc));
        }

        if (details.empty() || ok_count == 0) {
            HideUsagePanel();
            GetDisplay()->ShowNotification("直连查询失败:令牌失效或无法访问chatgpt.com", 4000);
            return;
        }
        ShowUsageListPanel(std::move(details));
    }

    void QueryChatGptUsage() {
        pending_detail_restore_ = usage_detail_index_;
        if (LoadDirectAuthJsons().empty()) {
            GetDisplay()->ShowNotification("未导入账号令牌,长按Q键配置", 3000);
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

    // 窗口剩余百分比：优先 used_percent，退回 remaining_count/total_count；未知返回 -1
    static int WindowRemainingPct(cJSON* window) {
        if (window == nullptr) {
            return -1;
        }
        cJSON* used = cJSON_GetObjectItem(window, "used_percent");
        if (cJSON_IsNumber(used)) {
            int remaining = 100 - used->valueint;
            return remaining < 0 ? 0 : (remaining > 100 ? 100 : remaining);
        }
        cJSON* remaining = cJSON_GetObjectItem(window, "remaining_count");
        cJSON* total = cJSON_GetObjectItem(window, "total_count");
        if (cJSON_IsNumber(remaining) && cJSON_IsNumber(total) && total->valueint > 0) {
            return remaining->valueint * 100 / total->valueint;
        }
        return -1;
    }

    static int WindowResetSeconds(cJSON* window) {
        if (window == nullptr) {
            return -1;
        }
        cJSON* after = cJSON_GetObjectItem(window, "reset_after_seconds");
        if (cJSON_IsNumber(after) && after->valueint > 0) {
            return after->valueint;
        }
        cJSON* at = cJSON_GetObjectItem(window, "reset_at");
        if (cJSON_IsNumber(at) && at->valueint > 0) {
            return at->valueint - (int)time(nullptr);
        }
        return -1;
    }

    // ---------------- 直连 HTTPS 客户端：可选 SOCKS5 代理（RFC1928/1929）+ mbedtls TLS ----------------

    // 带超时的 TCP 连接（非阻塞 connect + select）
    static int TcpConnect(const std::string& host, int port, int timeout_ms) {
        struct addrinfo hints = {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* result = nullptr;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0 || result == nullptr) {
            ESP_LOGE(TAG, "TCP: resolve %s failed", host.c_str());
            return -1;
        }
        int fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (fd < 0) {
            freeaddrinfo(result);
            return -1;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        int ret = connect(fd, result->ai_addr, result->ai_addrlen);
        (void)ret;
        freeaddrinfo(result);

        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(fd, &write_set);
        struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
        if (select(fd + 1, nullptr, &write_set, nullptr, &tv) <= 0) {
            close(fd);
            ESP_LOGE(TAG, "TCP: connect %s:%d timeout", host.c_str(), port);
            return -1;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {
            close(fd);
            ESP_LOGE(TAG, "TCP: connect %s:%d failed, errno=%d", host.c_str(), port, err);
            return -1;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
        struct timeval io_tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io_tv, sizeof(io_tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io_tv, sizeof(io_tv));
        return fd;
    }

    static bool Socks5Handshake(int fd, const Socks5Config& proxy, const std::string& host, int port) {
        auto socks_recv = [fd](uint8_t* buf, size_t len) -> bool {
            size_t got = 0;
            while (got < len) {
                int n = recv(fd, buf + got, len - got, 0);
                if (n <= 0) {
                    return false;
                }
                got += n;
            }
            return true;
        };

        // 方法协商：无认证 + 账号密码
        bool has_auth = !proxy.user.empty();
        uint8_t greeting[4] = {0x05, (uint8_t)(has_auth ? 2 : 1), 0x00, 0x02};
        if (send(fd, greeting, has_auth ? 4 : 3, 0) < 0) {
            return false;
        }
        uint8_t reply[2] = {0};
        if (!socks_recv(reply, 2) || reply[0] != 0x05 || reply[1] == 0xFF) {
            ESP_LOGE(TAG, "SOCKS5: greeting rejected");
            return false;
        }
        if (reply[1] == 0x02) {
            // RFC1929 账号密码认证
            if (proxy.user.size() > 255 || proxy.pass.size() > 255) {
                return false;
            }
            std::vector<uint8_t> auth(3 + proxy.user.size() + proxy.pass.size());
            size_t pos = 0;
            auth[pos++] = 0x01;
            auth[pos++] = (uint8_t)proxy.user.size();
            memcpy(auth.data() + pos, proxy.user.data(), proxy.user.size());
            pos += proxy.user.size();
            auth[pos++] = (uint8_t)proxy.pass.size();
            memcpy(auth.data() + pos, proxy.pass.data(), proxy.pass.size());
            if (send(fd, auth.data(), auth.size(), 0) < 0) {
                return false;
            }
            uint8_t auth_reply[2] = {0};
            if (!socks_recv(auth_reply, 2) || auth_reply[1] != 0x00) {
                ESP_LOGE(TAG, "SOCKS5: auth failed");
                return false;
            }
        } else if (reply[1] != 0x00) {
            return false;
        }

        // CONNECT（域名由代理解析）
        if (host.size() > 255) {
            return false;
        }
        std::vector<uint8_t> request(7 + host.size());
        size_t pos = 0;
        request[pos++] = 0x05;
        request[pos++] = 0x01;
        request[pos++] = 0x00;
        request[pos++] = 0x03;  // 域名
        request[pos++] = (uint8_t)host.size();
        memcpy(request.data() + pos, host.data(), host.size());
        pos += host.size();
        request[pos++] = (uint8_t)(port >> 8);
        request[pos++] = (uint8_t)(port & 0xFF);
        if (send(fd, request.data(), request.size(), 0) < 0) {
            return false;
        }
        uint8_t head[4] = {0};
        if (!socks_recv(head, 4) || head[0] != 0x05 || head[1] != 0x00) {
            ESP_LOGE(TAG, "SOCKS5: connect %s:%d rejected, rep=0x%02x", host.c_str(), port, head[1]);
            return false;
        }
        // 跳过绑定地址
        size_t addr_len = head[3] == 0x01 ? 4 : (head[3] == 0x04 ? 16 : 0);
        if (addr_len == 0) {
            uint8_t len8 = 0;
            if (!socks_recv(&len8, 1)) {
                return false;
            }
            addr_len = len8;
        }
        std::vector<uint8_t> skip(addr_len + 2);
        return socks_recv(skip.data(), skip.size());
    }

    static std::string Base64Encode(const std::string& in) {
        static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((in.size() + 2) / 3 * 4);
        int val = 0, valb = -6;
        for (uint8_t c : in) {
            val = (val << 8) + c;
            valb += 8;
            while (valb >= 0) {
                out.push_back(kAlphabet[(val >> valb) & 0x3F]);
                valb -= 6;
            }
        }
        if (valb > -6) {
            out.push_back(kAlphabet[((val << 8) >> (valb + 8)) & 0x3F]);
        }
        while (out.size() % 4 != 0) {
            out.push_back('=');
        }
        return out;
    }

    // HTTP CONNECT 隧道（支持 Basic 账号密码认证），成功后该 TCP 直接承载 TLS
    static bool HttpProxyHandshake(int fd, const Socks5Config& proxy, const std::string& host, int port) {
        std::string req = "CONNECT " + host + ":" + std::to_string(port) + " HTTP/1.1\r\n";
        req += "Host: " + host + ":" + std::to_string(port) + "\r\n";
        if (!proxy.user.empty()) {
            req += "Proxy-Authorization: Basic " + Base64Encode(proxy.user + ":" + proxy.pass) + "\r\n";
        }
        req += "\r\n";
        if (send(fd, req.c_str(), req.size(), 0) < 0) {
            return false;
        }

        // 读到响应头结束，判断状态码
        std::string raw;
        raw.reserve(512);
        char buf[256];
        while (raw.find("\r\n\r\n") == std::string::npos && raw.size() < 1024) {
            int n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                ESP_LOGE(TAG, "HTTP proxy: no response");
                return false;
            }
            raw.append(buf, n);
        }
        if (raw.compare(0, 5, "HTTP/") != 0) {
            ESP_LOGE(TAG, "HTTP proxy: bad response");
            return false;
        }
        size_t sp = raw.find(' ');
        int status = sp == std::string::npos ? 0 : atoi(raw.c_str() + sp + 1);
        if (status != 200) {
            ESP_LOGE(TAG, "HTTP proxy: CONNECT rejected, status=%d", status);
            return false;
        }
        return true;
    }

    struct TlsClient {
        mbedtls_ssl_context ssl{};
        mbedtls_ssl_config conf{};
        bool valid = false;

        ~TlsClient() {
            if (valid) {
                mbedtls_ssl_close_notify(&ssl);
            }
            mbedtls_ssl_free(&ssl);
            mbedtls_ssl_config_free(&conf);
        }


        static int SendCb(void* ctx, const unsigned char* buf, size_t len) {
            int fd = (int)(intptr_t)ctx;
            int n = send(fd, buf, len, 0);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                return MBEDTLS_ERR_SSL_WANT_WRITE;
            }
            if (n < 0) {
                return MBEDTLS_ERR_NET_SEND_FAILED;
            }
            return n;
        }

        static int RecvCb(void* ctx, unsigned char* buf, size_t len) {
            int fd = (int)(intptr_t)ctx;
            int n = recv(fd, buf, len, 0);
            if (n == 0) {
                return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                return MBEDTLS_ERR_SSL_WANT_READ;
            }
            if (n < 0) {
                return MBEDTLS_ERR_NET_RECV_FAILED;
            }
            return n;
        }

        bool Connect(int fd, const std::string& host, int timeout_ms) {
            mbedtls_ssl_init(&ssl);
            mbedtls_ssl_config_init(&conf);
            if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                            MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
                return false;
            }
            mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
            if (esp_crt_bundle_attach(&conf) != ESP_OK) {
                ESP_LOGE(TAG, "TLS: crt bundle attach failed");
                return false;
            }
            if (mbedtls_ssl_setup(&ssl, &conf) != 0) {
                return false;
            }
            if (mbedtls_ssl_set_hostname(&ssl, host.c_str()) != 0) {
                return false;
            }
            mbedtls_ssl_set_bio(&ssl, (void*)(intptr_t)fd, SendCb, RecvCb, nullptr);

            int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
            int ret = 0;
            while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
                if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                    char err[64];
                    mbedtls_strerror(ret, err, sizeof(err));
                    ESP_LOGE(TAG, "TLS: handshake with %s failed: %s", host.c_str(), err);
                    return false;
                }
                if (esp_timer_get_time() > deadline) {
                    ESP_LOGE(TAG, "TLS: handshake timeout");
                    return false;
                }
            }
            valid = true;
            return true;
        }

        bool WriteAll(const std::string& data, int timeout_ms) {
            size_t sent = 0;
            int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
            while (sent < data.size()) {
                int n = mbedtls_ssl_write(&ssl, (const unsigned char*)data.data() + sent, data.size() - sent);
                if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
                    if (esp_timer_get_time() > deadline) {
                        return false;
                    }
                    continue;
                }
                if (n <= 0) {
                    return false;
                }
                sent += n;
            }
            return true;
        }

        // 读到对端关闭或超时；max_bytes 上限保护
        bool ReadAll(std::string& out, size_t max_bytes, int timeout_ms) {
            char buf[1024];
            int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
            while (out.size() < max_bytes) {
                int n = mbedtls_ssl_read(&ssl, (unsigned char*)buf, sizeof(buf));
                if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == 0) {
                    break;
                }
                if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
                    if (esp_timer_get_time() > deadline) {
                        break;
                    }
                    continue;
                }
                if (n < 0) {
                    ESP_LOGE(TAG, "TLS: read error -0x%x", -n);
                    return false;
                }
                out.append(buf, n);
            }
            return true;
        }
    };

    static bool HttpsDechunk(std::string& body) {
        std::string out;
        out.reserve(body.size());
        size_t pos = 0;
        while (pos + 2 <= body.size()) {
            size_t eol = body.find("\r\n", pos);
            if (eol == std::string::npos) {
                break;
            }
            unsigned long size = strtoul(body.substr(pos, eol - pos).c_str(), nullptr, 16);
            pos = eol + 2;
            if (size == 0) {
                break;
            }
            size_t take = std::min<size_t>(size, body.size() - pos);
            out.append(body, pos, take);
            pos += take + 2;
        }
        body = std::move(out);
        return true;
    }

    // HTTPS 请求（走/不走 SOCKS5 均可）：Connection: close，一次一连接
    static bool HttpsRequest(const std::string& host, const std::string& path, const char* method,
                             const std::vector<std::pair<std::string, std::string>>& headers,
                             const std::string& body, int& status_code, std::string& resp_body) {
        auto proxy = instance_->GetSocks5Config();
        std::string connect_host = proxy.enabled() ? proxy.host : host;
        int connect_port = proxy.enabled() ? proxy.port : 443;

        int fd = TcpConnect(connect_host, connect_port, 10000);
        if (fd < 0) {
            return false;
        }
        bool ok = true;
        if (proxy.enabled()) {
            if (proxy.type == "http") {
                ok = HttpProxyHandshake(fd, proxy, host, 443);
            } else {
                ok = Socks5Handshake(fd, proxy, host, 443);
            }
        }

        TlsClient tls;
        if (ok) {
            ok = tls.Connect(fd, host, 10000);
        }
        if (ok) {
            std::string req = std::string(method) + " " + path + " HTTP/1.1\r\n";
            req += "Host: " + host + "\r\nConnection: close\r\nAccept: application/json\r\n";
            for (auto& header : headers) {
                req += header.first + ": " + header.second + "\r\n";
            }
            if (!body.empty()) {
                req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
            }
            req += "\r\n" + body;
            ok = tls.WriteAll(req, 10000);
        }
        std::string raw;
        if (ok) {
            ok = tls.ReadAll(raw, 20 * 1024, 10000);
        }
        close(fd);
        if (!ok || raw.empty()) {
            return false;
        }

        // 解析状态行
        if (raw.compare(0, 5, "HTTP/") != 0) {
            return false;
        }
        size_t sp = raw.find(' ');
        if (sp == std::string::npos) {
            return false;
        }
        status_code = atoi(raw.c_str() + sp + 1);

        size_t header_end = raw.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            resp_body.clear();
            return true;
        }
        std::string header_block = raw.substr(0, header_end);
        resp_body = raw.substr(header_end + 4);
        std::string lower;
        lower.reserve(header_block.size());
        for (char c : header_block) {
            lower.push_back((char)tolower((unsigned char)c));
        }
        if (lower.find("transfer-encoding: chunked") != std::string::npos) {
            HttpsDechunk(resp_body);
        }
        return true;
    }

    static bool Base64UrlDecode(const std::string& in, std::string& out) {
        auto value_of = [](char c) -> int {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '-' || c == '+') return 62;
            if (c == '_' || c == '/') return 63;
            return -1;
        };
        out.clear();
        out.reserve(in.size() * 3 / 4 + 3);
        int val = 0, valb = -8;
        for (char c : in) {
            int v = value_of(c);
            if (v < 0) {
                continue;  // 跳过 '=' 填充与非法字符
            }
            val = (val << 6) + v;
            valb += 6;
            if (valb >= 0) {
                out.push_back((char)((val >> valb) & 0xFF));
                valb -= 8;
            }
        }
        return !out.empty();
    }

    // 解开 id_token(JWT) payload，提取 email/exp 与 https://api.openai.com/auth 嵌套声明
    static cJSON* DecodeJwtPayload(const std::string& jwt) {
        size_t p1 = jwt.find('.');
        size_t p2 = jwt.find('.', p1 == std::string::npos ? 0 : p1 + 1);
        if (p1 == std::string::npos || p2 == std::string::npos || p2 <= p1 + 1) {
            return nullptr;
        }
        std::string payload;
        if (!Base64UrlDecode(jwt.substr(p1 + 1, p2 - p1 - 1), payload)) {
            return nullptr;
        }
        return cJSON_Parse(payload.c_str());
    }

    static std::string JwtString(cJSON* payload, const char* key) {
        cJSON* item = payload ? cJSON_GetObjectItem(payload, key) : nullptr;
        return cJSON_IsString(item) ? item->valuestring : "";
    }

    // 用 id_token 填充账号展示信息（套餐/订阅/账号ID），字段名与 codex 官方 JWT 声明一致
    void FillDetailFromIdToken(AccountDetail& acc, const std::string& id_token) {
        cJSON* payload = DecodeJwtPayload(id_token);
        if (payload == nullptr) {
            return;
        }
        if (acc.email.empty()) {
            acc.email = JwtString(payload, "email");
        }
        cJSON* auth = cJSON_GetObjectItem(payload, "https://api.openai.com/auth");
        if (auth != nullptr) {
            cJSON* account_item = cJSON_GetObjectItem(auth, "chatgpt_account_id");
            if (acc.account_id.empty() && cJSON_IsString(account_item)) {
                acc.account_id = account_item->valuestring;
            }
            cJSON* plan_item = cJSON_GetObjectItem(auth, "chatgpt_plan_type");
            if (acc.plan.empty() && cJSON_IsString(plan_item)) {
                acc.plan = plan_item->valuestring;
            }
            cJSON* until_item = cJSON_GetObjectItem(auth, "chatgpt_subscription_active_until");
            if (cJSON_IsString(until_item)) {
                acc.subscription_until = std::string(until_item->valuestring).substr(0, 10);
            }
        }
        cJSON_Delete(payload);
    }

    // 直连官方接口 GET chatgpt.com/backend-api/wham/usage；返回 HTTP 状态码，200 时填充 acc
    static int DirectFetchUsage(const std::string& access_token, const std::string& account_id,
                                AccountDetail& acc) {
        std::vector<std::pair<std::string, std::string>> headers = {
            {"Authorization", "Bearer " + access_token},
            {"Content-Type", "application/json"},
            {"User-Agent", "codex_cli_rs/0.76.0 (Debian 13.0.0; x86_64) WindowsTerminal"},
            {"Chatgpt-Account-Id", account_id},
        };
        int status = 0;
        std::string body;
        if (!HttpsRequest("chatgpt.com", "/backend-api/wham/usage", "GET", headers, "", status, body)) {
            ESP_LOGE(TAG, "Direct usage: request failed (%s)", acc.name.c_str());
            return -1;
        }
        if (status != 200) {
            ESP_LOGE(TAG, "Direct usage: HTTP %d (%s)", status, acc.name.c_str());
            return status;
        }

        cJSON* usage = cJSON_Parse(body.c_str());
        if (usage == nullptr) {
            return -2;
        }
        cJSON* plan_item = cJSON_GetObjectItem(usage, "plan_type");
        if (cJSON_IsString(plan_item) && acc.plan.empty()) {
            acc.plan = plan_item->valuestring;
        }
        cJSON* rate_limit = cJSON_GetObjectItem(usage, "rate_limit");
        cJSON* primary = rate_limit ? cJSON_GetObjectItem(rate_limit, "primary_window") : nullptr;
        cJSON* secondary = rate_limit ? cJSON_GetObjectItem(rate_limit, "secondary_window") : nullptr;
        cJSON* cr_limit = cJSON_GetObjectItem(usage, "code_review_rate_limit");
        cJSON* cr_primary = cr_limit ? cJSON_GetObjectItem(cr_limit, "primary_window") : nullptr;
        acc.remaining_5h = WindowRemainingPct(primary);
        acc.remaining_weekly = WindowRemainingPct(secondary);
        acc.remaining_cr = WindowRemainingPct(cr_primary);
        acc.reset_5h = WindowResetSeconds(primary);
        acc.reset_weekly = WindowResetSeconds(secondary);
        acc.reset_cr = WindowResetSeconds(cr_primary);
        cJSON_Delete(usage);
        ESP_LOGI(TAG, "Direct usage: %s 5h=%d%% weekly=%d%% cr=%d%%",
                 acc.name.c_str(), acc.remaining_5h, acc.remaining_weekly, acc.remaining_cr);
        return 200;
    }

    // 直连官方扩展数据：每日 token 用量/统计（profiles/me）+ 可用重置积分
    static void FetchOfficialExtras(const std::string& access_token, const std::string& account_id,
                                    AccountDetail& acc) {
        std::vector<std::pair<std::string, std::string>> headers = {
            {"Authorization", "Bearer " + access_token},
            {"Content-Type", "application/json"},
            {"User-Agent", "codex_cli_rs/0.76.0 (Debian 13.0.0; x86_64) WindowsTerminal"},
            {"Chatgpt-Account-Id", account_id},
        };

        int status = 0;
        std::string body;
        if (HttpsRequest("chatgpt.com", "/backend-api/wham/profiles/me", "GET", headers, "", status, body) &&
            status == 200) {
            cJSON* root = cJSON_Parse(body.c_str());
            cJSON* stats = root != nullptr ? cJSON_GetObjectItem(root, "stats") : nullptr;
            if (stats != nullptr) {
                auto number_value = [](cJSON* parent, const char* key) -> long long {
                    cJSON* item = cJSON_GetObjectItem(parent, key);
                    return cJSON_IsNumber(item) ? (long long)item->valuedouble : -1;
                };
                acc.lifetime_tokens = number_value(stats, "lifetime_tokens");
                acc.peak_daily_tokens = number_value(stats, "peak_daily_tokens");
                acc.current_streak_days = (long)number_value(stats, "current_streak_days");
                cJSON* bucket = nullptr;
                cJSON_ArrayForEach(bucket, cJSON_GetObjectItem(stats, "daily_usage_buckets")) {
                    cJSON* date_item = cJSON_GetObjectItem(bucket, "start_date");
                    cJSON* tokens_item = cJSON_GetObjectItem(bucket, "tokens");
                    if (cJSON_IsString(date_item) && cJSON_IsNumber(tokens_item)) {
                        acc.daily_buckets.emplace_back(date_item->valuestring,
                                                       (long long)tokens_item->valuedouble);
                    }
                }
            }
            cJSON_Delete(root);
        }

        status = 0;
        if (HttpsRequest("chatgpt.com", "/backend-api/wham/rate-limit-reset-credits", "GET", headers, "",
                         status, body) &&
            status == 200) {
            cJSON* root = cJSON_Parse(body.c_str());
            cJSON* count = root != nullptr ? cJSON_GetObjectItem(root, "available_count") : nullptr;
            if (cJSON_IsNumber(count)) {
                acc.reset_credits = count->valueint;
            }
            cJSON_Delete(root);
        }
    }

    // OAuth 刷新令牌（codex 官方端点与参数），成功后把新令牌回写 NVS 并返回新 access_token
    static std::string RefreshCodexToken(cJSON* auth_json, int slot_index) {
        cJSON* refresh_item = cJSON_GetObjectItem(auth_json, "refresh_token");
        if (!cJSON_IsString(refresh_item) || refresh_item->valuestring[0] == '\0') {
            return "";
        }
        std::string body = "grant_type=refresh_token"
                           "&client_id=app_EMoamEEZ73f0CkXaXp7hrann"
                           "&refresh_token=" + std::string(refresh_item->valuestring) +
                           "&scope=openid%20profile%20email";

        std::vector<std::pair<std::string, std::string>> headers = {
            {"Content-Type", "application/x-www-form-urlencoded"},
        };
        int status = 0;
        std::string resp;
        if (!HttpsRequest("auth.openai.com", "/oauth/token", "POST", headers, body, status, resp)) {
            ESP_LOGE(TAG, "Token refresh: request failed");
            return "";
        }
        if (status != 200) {
            ESP_LOGE(TAG, "Token refresh: HTTP %d", status);
            return "";
        }

        cJSON* token = cJSON_Parse(resp.c_str());
        if (token == nullptr) {
            return "";
        }
        cJSON* access_item = cJSON_GetObjectItem(token, "access_token");
        if (!cJSON_IsString(access_item)) {
            cJSON_Delete(token);
            return "";
        }

        // 回写：access/refresh/id token 都可能轮换，必须全部更新并持久化
        auto replace_string = [auth_json](const char* key, cJSON* value_root) {
            cJSON* value = value_root ? cJSON_GetObjectItem(value_root, key) : nullptr;
            if (cJSON_IsString(value)) {
                cJSON_ReplaceItemInObject(auth_json, key, cJSON_CreateString(value->valuestring));
            }
        };
        replace_string("access_token", token);
        replace_string("refresh_token", token);
        replace_string("id_token", token);

        time_t now = time(nullptr);
        char time_str[24];
        strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
        cJSON_ReplaceItemInObject(auth_json, "last_refresh", cJSON_CreateString(time_str));
        cJSON* expires_item = cJSON_GetObjectItem(token, "expires_in");
        if (cJSON_IsNumber(expires_item)) {
            time_t expired_at = now + expires_item->valueint;
            strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", gmtime(&expired_at));
            cJSON_ReplaceItemInObject(auth_json, "expired", cJSON_CreateString(time_str));
        }
        cJSON_Delete(token);

        char* printed = cJSON_PrintUnformatted(auth_json);
        if (printed != nullptr) {
            Settings settings("vendor", true);
            settings.SetString(CodexAuthKey(slot_index), printed);
            cJSON_free(printed);
        }
        ESP_LOGI(TAG, "Token refreshed for slot %d", slot_index + 1);
        return access_item->valuestring;
    }

    static void PlanBadgeColors(const std::string& plan, uint32_t& text, uint32_t& bg) {
        if (strcasecmp(plan.c_str(), "pro") == 0) {
            text = 0xD7A9FF;
            bg = 0x3A2A4D;
        } else if (strcasecmp(plan.c_str(), "team") == 0 || strcasecmp(plan.c_str(), "business") == 0) {
            text = 0xFDD663;
            bg = 0x443B1E;
        } else if (strcasecmp(plan.c_str(), "free") == 0) {
            text = 0x9AA0A6;
            bg = 0x26292E;
        } else {  // plus 及未知
            text = 0x8AB4F8;
            bg = 0x283142;
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

    static lv_color_t UsageBarColor(int remaining_pct) {
        if (remaining_pct < 0) {
            return lv_color_hex(0x5F6368);
        }
        if (remaining_pct < 25) {
            return lv_color_hex(0xEA4335);
        }
        if (remaining_pct < 50) {
            return lv_color_hex(0xFBBC04);
        }
        return lv_color_hex(0x34A853);
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
        usage_detail_index_ = -1;
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
                        instance_->StartChatGptUsageQuery();
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

    lv_obj_t* CreateUsagePanelBase(const char* title, const char* nav_hint) {
        HideUsagePanel();
        auto display = GetDisplay();
        if (display->GetTheme() == nullptr) {
            return nullptr;
        }
        DisplayLockGuard lock(display);
        auto font = static_cast<LvglTheme*>(display->GetTheme())->text_font()->font();

        usage_panel_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(usage_panel_, LV_HOR_RES, LV_VER_RES);
        lv_obj_add_flag(usage_panel_, LV_OBJ_FLAG_FLOATING);
        lv_obj_move_foreground(usage_panel_);
        lv_obj_set_style_bg_color(usage_panel_, lv_color_hex(0x101418), 0);
        lv_obj_set_style_bg_opa(usage_panel_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(usage_panel_, 0, 0);
        lv_obj_set_style_border_width(usage_panel_, 0, 0);
        lv_obj_set_style_pad_all(usage_panel_, 10, 0);
        lv_obj_set_flex_flow(usage_panel_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(usage_panel_, 6, 0);
        lv_obj_clear_flag(usage_panel_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_text_font(usage_panel_, font, 0);

        // 标题行：左标题，右数据时间戳
        lv_obj_t* header_row = lv_obj_create(usage_panel_);
        lv_obj_set_width(header_row, lv_pct(100));
        lv_obj_set_height(header_row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(header_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(header_row, 0, 0);
        lv_obj_set_style_pad_all(header_row, 0, 0);
        lv_obj_set_flex_flow(header_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(header_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(header_row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* header = lv_label_create(header_row);
        lv_obj_set_style_text_color(header, lv_color_hex(0xE8EAED), 0);
        lv_label_set_text(header, title);

        std::string updated = FormatUpdatedAt(usage_updated_at_);
        if (!updated.empty()) {
            lv_obj_t* updated_label = lv_label_create(header_row);
            lv_obj_set_style_text_color(updated_label, lv_color_hex(0x9AA0A6), 0);
            lv_label_set_text(updated_label, updated.c_str());
        }

        if (nav_hint != nullptr && nav_hint[0] != '\0') {
            lv_obj_t* nav = lv_label_create(usage_panel_);
            lv_obj_set_style_text_color(nav, lv_color_hex(0x9AA0A6), 0);
            lv_label_set_text(nav, nav_hint);
        }

        lv_obj_t* content = lv_obj_create(usage_panel_);
        lv_obj_set_width(content, lv_pct(100));
        lv_obj_set_flex_grow(content, 1);
        lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(content, 0, 0);
        lv_obj_set_style_pad_all(content, 0, 0);
        lv_obj_set_style_pad_row(content, 6, 0);
        lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
        return content;
    }

    // 一行进度条：标签 + 彩条 + 百分比
    void AddUsageBarRow(lv_obj_t* parent, const char* tag, int remaining_pct) {
        auto display = GetDisplay();
        auto font = static_cast<LvglTheme*>(display->GetTheme())->text_font()->font();

        lv_obj_t* row = lv_obj_create(parent);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_column(row, 6, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* tag_label = lv_label_create(row);
        lv_obj_set_style_text_font(tag_label, font, 0);
        lv_obj_set_style_text_color(tag_label, lv_color_hex(0x9AA0A6), 0);
        lv_obj_set_width(tag_label, 26);
        lv_label_set_text(tag_label, tag);

        lv_obj_t* bar = lv_bar_create(row);
        lv_obj_set_height(bar, 10);
        lv_obj_set_flex_grow(bar, 1);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_value(bar, remaining_pct >= 0 ? remaining_pct : 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x2A2F36), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, UsageBarColor(remaining_pct), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);

        lv_obj_t* pct_label = lv_label_create(row);
        lv_obj_set_style_text_font(pct_label, font, 0);
        lv_obj_set_style_text_color(pct_label, lv_color_hex(0xE8EAED), 0);
        lv_obj_set_width(pct_label, 42);
        lv_obj_set_style_text_align(pct_label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(pct_label, remaining_pct >= 0 ? (std::to_string(remaining_pct) + "%").c_str() : "?");
    }

    void AddUsageCard(lv_obj_t* parent, const AccountDetail& acc) {
        auto display = GetDisplay();
        auto font = static_cast<LvglTheme*>(display->GetTheme())->text_font()->font();

        lv_obj_t* card = lv_obj_create(parent);
        lv_obj_set_width(card, lv_pct(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x1A2028), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 8, 0);
        lv_obj_set_style_pad_row(card, 6, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* title_row = lv_obj_create(card);
        lv_obj_set_width(title_row, lv_pct(100));
        lv_obj_set_height(title_row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(title_row, 0, 0);
        lv_obj_set_style_pad_all(title_row, 0, 0);
        lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);

        if (acc.unavailable) {
            lv_obj_t* alert = lv_label_create(title_row);
            lv_obj_set_style_text_font(alert, font, 0);
            lv_obj_set_style_text_color(alert, lv_color_hex(0xEA4335), 0);
            lv_label_set_text(alert, "!");
        }

        lv_obj_t* name_label = lv_label_create(title_row);
        lv_obj_set_style_text_font(name_label, font, 0);
        lv_obj_set_style_text_color(name_label, lv_color_hex(0xE8EAED), 0);
        lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(name_label, 1);
        lv_label_set_text(name_label, acc.name.c_str());

        std::string reset = FormatResetShort(acc.reset_5h);
        if (!reset.empty()) {
            lv_obj_t* reset_label = lv_label_create(title_row);
            lv_obj_set_style_text_font(reset_label, font, 0);
            lv_obj_set_style_text_color(reset_label, lv_color_hex(0x9AA0A6), 0);
            lv_label_set_text(reset_label, reset.c_str());
        }

        if (!acc.plan.empty()) {
            uint32_t text_color = 0;
            uint32_t bg_color = 0;
            PlanBadgeColors(acc.plan, text_color, bg_color);
            lv_obj_t* plan_label = lv_label_create(title_row);
            lv_obj_set_style_text_font(plan_label, font, 0);
            lv_obj_set_style_text_color(plan_label, lv_color_hex(text_color), 0);
            lv_obj_set_style_bg_color(plan_label, lv_color_hex(bg_color), 0);
            lv_obj_set_style_bg_opa(plan_label, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(plan_label, 6, 0);
            lv_obj_set_style_pad_hor(plan_label, 8, 0);
            lv_obj_set_style_pad_ver(plan_label, 2, 0);
            lv_label_set_text(plan_label, acc.plan.c_str());
        }

        AddUsageBarRow(card, "5h", acc.remaining_5h);
        AddUsageBarRow(card, "周", acc.remaining_weekly);
    }

    void ShowUsagePanelLoading() {
        lv_obj_t* content = CreateUsagePanelBase("ChatGPT 用量", nullptr);
        if (content == nullptr) {
            GetDisplay()->ShowNotification("查询套餐用量...", 8000);
            return;
        }
        DisplayLockGuard lock(GetDisplay());
        auto font = static_cast<LvglTheme*>(GetDisplay()->GetTheme())->text_font()->font();

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

    // 详情页窗口块：标签行（名称+重置 左，剩余百分比 右）+ 大进度条，共两行
    void AddDetailBarBlock(lv_obj_t* parent, const char* title, int remaining_pct, int reset_seconds) {
        auto display = GetDisplay();
        auto font = static_cast<LvglTheme*>(display->GetTheme())->text_font()->font();

        lv_obj_t* head = lv_obj_create(parent);
        lv_obj_set_width(head, lv_pct(100));
        lv_obj_set_height(head, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(head, 0, 0);
        lv_obj_set_style_pad_all(head, 0, 0);
        lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

        std::string left = title;
        std::string reset = FormatResetShort(reset_seconds);
        if (!reset.empty()) {
            left += "  " + reset + "后重置";
        }
        lv_obj_t* title_label = lv_label_create(head);
        lv_obj_set_style_text_font(title_label, font, 0);
        lv_obj_set_style_text_color(title_label, lv_color_hex(0x9AA0A6), 0);
        lv_label_set_text(title_label, left.c_str());

        lv_obj_t* pct_label = lv_label_create(head);
        lv_obj_set_style_text_font(pct_label, font, 0);
        lv_obj_set_style_text_color(pct_label, UsageBarColor(remaining_pct), 0);
        lv_label_set_text(pct_label, remaining_pct >= 0 ? (std::to_string(remaining_pct) + "% 剩余").c_str() : "查询失败");

        lv_obj_t* bar = lv_bar_create(parent);
        lv_obj_set_width(bar, lv_pct(100));
        lv_obj_set_height(bar, 14);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_value(bar, remaining_pct >= 0 ? remaining_pct : 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x2A2F36), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, UsageBarColor(remaining_pct), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
    }

    static std::string FormatTokens(long long value) {
        char label[24];
        if (value < 0) {
            return "-";
        }
        if (value >= 100000000LL) {
            snprintf(label, sizeof(label), "%.2f亿", value / 100000000.0);
        } else if (value >= 10000LL) {
            snprintf(label, sizeof(label), "%.1f万", value / 10000.0);
        } else {
            snprintf(label, sizeof(label), "%lld", value);
        }
        return label;
    }

    // 每日 token 用量柱状图（官方 daily_usage_buckets，取末尾最多 14 天）
    void AddDailyTokensChart(lv_obj_t* parent, const std::vector<std::pair<std::string, long long>>& buckets) {
        auto display = GetDisplay();
        auto font = static_cast<LvglTheme*>(display->GetTheme())->text_font()->font();

        lv_obj_t* chart = lv_obj_create(parent);
        lv_obj_set_width(chart, lv_pct(100));
        lv_obj_set_height(chart, 40);
        lv_obj_set_style_bg_color(chart, lv_color_hex(0x1A2028), 0);
        lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(chart, 8, 0);
        lv_obj_set_style_border_width(chart, 0, 0);
        lv_obj_set_style_pad_hor(chart, 4, 0);
        lv_obj_set_style_pad_ver(chart, 4, 0);
        lv_obj_set_style_pad_column(chart, 2, 0);
        lv_obj_set_flex_flow(chart, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(chart, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_text_font(chart, font, 0);

        size_t count = buckets.size() > 14 ? 14 : buckets.size();
        size_t offset = buckets.size() - count;
        long long max_tokens = 1;
        for (size_t i = offset; i < buckets.size(); i++) {
            max_tokens = std::max(max_tokens, buckets[i].second);
        }
        for (size_t i = offset; i < buckets.size(); i++) {
            long long tokens = buckets[i].second;
            int height = tokens > 0 ? (int)(tokens * 32 / max_tokens) : 0;
            if (height < 2) {
                height = 2;
            }
            lv_color_t color = tokens * 100 > max_tokens * 80
                                   ? lv_color_hex(0xFBBC04)  // 高峰日黄色提示
                                   : lv_color_hex(0x8AB4F8);
            lv_obj_t* bar = lv_obj_create(chart);
            lv_obj_set_size(bar, 6, height);
            lv_obj_set_style_bg_color(bar, color, 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(bar, 1, 0);
            lv_obj_set_style_border_width(bar, 0, 0);
            lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        }
    }

    void AddDetailInfoLine(lv_obj_t* parent, const std::string& text, uint32_t color) {
        auto display = GetDisplay();
        auto font = static_cast<LvglTheme*>(display->GetTheme())->text_font()->font();
        lv_obj_t* label = lv_label_create(parent);
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_text(label, text.c_str());
    }

    // M 键从列表进入的单账号详情页；数据来自 usage_details_ 缓存
    void ShowUsageDetailPanel(int index) {
        if (usage_details_.empty()) {
            return;
        }
        index = ((index % (int)usage_details_.size()) + (int)usage_details_.size()) % (int)usage_details_.size();
        usage_detail_index_ = index;
        const AccountDetail& acc = usage_details_[index];

        std::string nav = "< " + std::to_string(index + 1) + "/" + std::to_string(usage_details_.size()) +
                         " >  音量键切换 · M键返回";
        lv_obj_t* content = CreateUsagePanelBase("账号详情", nav.c_str());
        if (content == nullptr) {
            GetDisplay()->ShowNotification("详情页创建失败", 3000);
            return;
        }

        DisplayLockGuard lock(GetDisplay());
        auto font = static_cast<LvglTheme*>(GetDisplay()->GetTheme())->text_font()->font();

        // 邮箱 + 套餐徽章
        lv_obj_t* title_row = lv_obj_create(content);
        lv_obj_set_width(title_row, lv_pct(100));
        lv_obj_set_height(title_row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(title_row, 0, 0);
        lv_obj_set_style_pad_all(title_row, 0, 0);
        lv_obj_set_style_pad_column(title_row, 6, 0);
        lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* email_label = lv_label_create(title_row);
        lv_obj_set_style_text_font(email_label, font, 0);
        lv_obj_set_style_text_color(email_label, lv_color_hex(0xE8EAED), 0);
        lv_label_set_long_mode(email_label, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(email_label, 1);
        lv_label_set_text(email_label, acc.email.empty() ? acc.name.c_str() : acc.email.c_str());

        if (!acc.plan.empty()) {
            uint32_t plan_text = 0;
            uint32_t plan_bg = 0;
            PlanBadgeColors(acc.plan, plan_text, plan_bg);
            lv_obj_t* plan_label = lv_label_create(title_row);
            lv_obj_set_style_text_font(plan_label, font, 0);
            lv_obj_set_style_text_color(plan_label, lv_color_hex(plan_text), 0);
            lv_obj_set_style_bg_color(plan_label, lv_color_hex(plan_bg), 0);
            lv_obj_set_style_bg_opa(plan_label, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(plan_label, 6, 0);
            lv_obj_set_style_pad_hor(plan_label, 8, 0);
            lv_obj_set_style_pad_ver(plan_label, 2, 0);
            lv_label_set_text(plan_label, acc.plan.c_str());
        }

        if (!acc.subscription_until.empty()) {
            AddDetailInfoLine(content, "订阅有效至 " + acc.subscription_until, 0x9AA0A6);
        }

        // 三个限额窗口
        AddDetailBarBlock(content, "5小时窗口", acc.remaining_5h, acc.reset_5h);
        AddDetailBarBlock(content, "每周窗口", acc.remaining_weekly, acc.reset_weekly);
        if (acc.remaining_cr >= 0) {
            AddDetailBarBlock(content, "代码审查", acc.remaining_cr, acc.reset_cr);
        }

        // 官方数据（重置积分 / 每日 token / 统计）
        if (acc.reset_credits > 0) {
            AddDetailInfoLine(content, "限额重置积分:" + std::to_string(acc.reset_credits) + "个可用", 0x8AB4F8);
        }
        if (!acc.daily_buckets.empty()) {
            size_t days = acc.daily_buckets.size() > 14 ? 14 : acc.daily_buckets.size();
            AddDetailInfoLine(content, "近" + std::to_string(days) + "天token用量(高峰黄色)", 0x9AA0A6);
            AddDailyTokensChart(content, acc.daily_buckets);
        }
        std::string stats_line;
        if (acc.current_streak_days >= 0) {
            stats_line += "连续" + std::to_string(acc.current_streak_days) + "天";
        }
        if (acc.peak_daily_tokens >= 0) {
            stats_line += (stats_line.empty() ? std::string() : std::string(" · ")) + "日峰值" +
                          FormatTokens(acc.peak_daily_tokens);
        }
        if (!stats_line.empty()) {
            AddDetailInfoLine(content, stats_line, 0x9AA0A6);
        }
        if (acc.lifetime_tokens >= 0) {
            AddDetailInfoLine(content, "累计token " + FormatTokens(acc.lifetime_tokens), 0x9AA0A6);
        }

        if (!acc.last_refresh.empty()) {
            AddDetailInfoLine(content, "Token刷新 " + acc.last_refresh, 0x9AA0A6);
        }
        if (acc.unavailable) {
            AddDetailInfoLine(content, "状态: 令牌异常,建议重新导入", 0xEA4335);
        }

        RestartUsagePanelTimer(GetUsagePanelHideTimeoutUs(30000000LL));
        StartUsageAutoRefresh();
        StartUsagePanelWatchdog();
    }

    // M 键在详情页再按时回到列表
    void BackToUsageList() {
        if (usage_details_.empty()) {
            HideUsagePanel();
            return;
        }
        lv_obj_t* content = CreateUsagePanelBase("ChatGPT 用量", nullptr);
        if (content == nullptr) {
            HideUsagePanel();
            return;
        }
        usage_detail_index_ = -1;
        DisplayLockGuard lock(GetDisplay());
        for (auto& acc : usage_details_) {
            AddUsageCard(content, acc);
        }
        RestartUsagePanelTimer(GetUsagePanelHideTimeoutUs(15000000LL));
        StartUsageAutoRefresh();
        StartUsagePanelWatchdog();
    }


    // 列表页；自动刷新后恢复原详情页
    void ShowUsageListPanel(std::vector<AccountDetail> details) {
        usage_details_ = std::move(details);
        usage_updated_at_ = time(nullptr);

        int restore = pending_detail_restore_;
        pending_detail_restore_ = -1;
        if (restore >= 0 && restore < (int)usage_details_.size()) {
            ShowUsageDetailPanel(restore);
            return;
        }

        lv_obj_t* content = CreateUsagePanelBase("ChatGPT 用量", nullptr);
        if (content == nullptr) {
            GetDisplay()->ShowNotification("用量面板创建失败", 3000);
            return;
        }
        usage_detail_index_ = -1;

        DisplayLockGuard lock(GetDisplay());
        for (auto& acc : usage_details_) {
            AddUsageCard(content, acc);
        }
        RestartUsagePanelTimer(GetUsagePanelHideTimeoutUs(15000000LL));
        StartUsageAutoRefresh();
        StartUsagePanelWatchdog();
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
            if (self->usage_detail_index_ >= 0) {
                self->ShowUsageDetailPanel(self->usage_detail_index_ - 1);
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
                if (self->usage_detail_index_ >= 0) {
                    self->BackToUsageList();
                } else {
                    self->ShowUsageDetailPanel(0);
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
                    self->HideUsagePanel();
                } else {
                    self->StartChatGptUsageQuery();
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
            if (self->usage_detail_index_ >= 0) {
                self->ShowUsageDetailPanel(self->usage_detail_index_ + 1);
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
        InitializeI2c();
        InitializeIoExpander();
        InitializePowerSaveTimer();
        InitializePowerManager();
        InitializeSt7789Display();
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
