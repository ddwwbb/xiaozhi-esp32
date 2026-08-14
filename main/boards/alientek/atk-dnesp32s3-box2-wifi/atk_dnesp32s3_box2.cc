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
#include "sdkconfig.h"
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>

#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include "esp_io_expander_tca95xx_16bit.h"

#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <string>
#include <strings.h>
#include <vector>
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

    struct CodexAccount {
        std::string auth_index;
        std::string account_id;
    };

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

    // 查询 CLIProxyAPI (GET /v0/management/auth-files) 的账号套餐用量并在屏幕显示
    void StartChatGptUsageQuery() {
        if (usage_fetching_.exchange(true)) {
            return;
        }
        if (xTaskCreate([](void* arg) {
                auto self = static_cast<atk_dnesp32s3_box2_wifi*>(arg);
                self->QueryChatGptUsage();
                self->usage_fetching_ = false;
                vTaskDelete(nullptr);
            }, "usage_query", 6144, this, 4, nullptr) != pdPASS) {
            usage_fetching_ = false;
        }
    }

    static int GetJsonInt(cJSON* obj, const char* key) {
        cJSON* item = cJSON_GetObjectItem(obj, key);
        return cJSON_IsNumber(item) ? item->valueint : 0;
    }

    static bool GetJsonBool(cJSON* obj, const char* key) {
        cJSON* item = cJSON_GetObjectItem(obj, key);
        return cJSON_IsTrue(item);
    }

    void QueryChatGptUsage() {
        auto display = GetDisplay();
        const char* base_url = CONFIG_CLIPROXY_USAGE_BASE_URL;
        const char* management_key = CONFIG_CLIPROXY_USAGE_MANAGEMENT_KEY;
        if (base_url[0] == '\0' || management_key[0] == '\0') {
            display->ShowNotification("未配置CLIProxyAPI用量查询", 3000);
            return;
        }

        display->ShowNotification("查询套餐用量...", 30000);

        std::string url = std::string(base_url) + "/v0/management/auth-files";
        auto http = GetNetwork()->CreateHttp(0);
        http->SetTimeout(10000);
        http->SetHeader("Authorization", std::string("Bearer ") + management_key);
        http->SetHeader("Accept", "application/json");
        if (!http->Open("GET", url)) {
            ESP_LOGE(TAG, "Usage query: failed to open %s, err=0x%x", url.c_str(), http->GetLastError());
            display->ShowNotification("用量查询失败:无法连接", 3000);
            return;
        }
        int status = http->GetStatusCode();
        std::string body = status == 200 ? http->ReadAll() : "";
        http->Close();

        if (status != 200) {
            ESP_LOGE(TAG, "Usage query: HTTP %d", status);
            std::string reason = "查询失败:HTTP " + std::to_string(status);
            if (status == 401) {
                reason = "查询失败:密钥错误(401)";
            } else if (status == 404) {
                reason = "查询失败:管理接口未启用(404)";
            }
            display->ShowNotification(reason, 3000);
            return;
        }

        cJSON* root = cJSON_Parse(body.c_str());
        if (root == nullptr) {
            display->ShowNotification("用量查询失败:响应解析错误", 3000);
            return;
        }

        const char* provider_filter = CONFIG_CLIPROXY_USAGE_PROVIDER;
        int accounts = 0;
        int window_success = 0;
        int window_failed = 0;
        int cooling = 0;
        std::vector<std::string> plans;
        std::vector<CodexAccount> codex_accounts;

        cJSON* files = cJSON_GetObjectItem(root, "files");
        cJSON* file = nullptr;
        cJSON_ArrayForEach(file, files) {
            if (!cJSON_IsObject(file)) {
                continue;
            }
            cJSON* provider_item = cJSON_GetObjectItem(file, "provider");
            const char* provider = cJSON_IsString(provider_item) ? provider_item->valuestring : "";
            if (provider_filter[0] != '\0' && strcasecmp(provider, provider_filter) != 0) {
                continue;
            }
            accounts++;

            // recent_requests: 20 个 10 分钟桶，约 200 分钟窗口（provider 非 codex 时的回退显示）
            cJSON* bucket = nullptr;
            cJSON_ArrayForEach(bucket, cJSON_GetObjectItem(file, "recent_requests")) {
                window_success += GetJsonInt(bucket, "success");
                window_failed += GetJsonInt(bucket, "failed");
            }

            bool unavailable = GetJsonBool(file, "unavailable") || GetJsonBool(file, "disabled");
            if (unavailable) {
                cooling++;
            }

            cJSON* id_token = cJSON_GetObjectItem(file, "id_token");
            cJSON* plan_item = id_token ? cJSON_GetObjectItem(id_token, "plan_type") : nullptr;
            const char* plan = cJSON_IsString(plan_item) ? plan_item->valuestring : nullptr;
            if (plan != nullptr && plan[0] != '\0') {
                bool exists = false;
                for (auto& p : plans) {
                    if (strcasecmp(p.c_str(), plan) == 0) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    plans.push_back(plan);
                }
            }

            cJSON* email_item = cJSON_GetObjectItem(file, "email");
            ESP_LOGI(TAG, "Usage: provider=%s email=%s plan=%s unavailable=%d",
                     provider,
                     cJSON_IsString(email_item) ? email_item->valuestring : "-",
                     plan ? plan : "-",
                     unavailable ? 1 : 0);

            if (strcasecmp(provider, "codex") == 0) {
                CodexAccount acc;
                cJSON* index_item = cJSON_GetObjectItem(file, "auth_index");
                acc.auth_index = cJSON_IsString(index_item) ? index_item->valuestring : "";
                cJSON* account_item = id_token ? cJSON_GetObjectItem(id_token, "chatgpt_account_id") : nullptr;
                acc.account_id = cJSON_IsString(account_item) ? account_item->valuestring : "";
                if (!acc.auth_index.empty() && !acc.account_id.empty()) {
                    codex_accounts.push_back(acc);
                }
            }
        }
        cJSON_Delete(root);

        std::string label = "账号";
        if (provider_filter[0] != '\0') {
            label = strcasecmp(provider_filter, "codex") == 0 ? "ChatGPT" : provider_filter;
        }
        if (accounts == 0) {
            display->ShowNotification("无" + label + "账号凭证", 3000);
            return;
        }

        if (strcasecmp(provider_filter, "codex") == 0 && !codex_accounts.empty()) {
            ShowCodexRemaining(base_url, management_key, codex_accounts, plans, cooling);
            return;
        }

        std::string text = label + "账号:" + std::to_string(accounts) + "个";
        if (!plans.empty()) {
            text += "(";
            for (size_t i = 0; i < plans.size(); i++) {
                if (i > 0) text += ",";
                text += plans[i];
            }
            text += ")";
        }
        text += "\n近200分钟 成功" + std::to_string(window_success) + " 失败" + std::to_string(window_failed);
        if (cooling > 0) {
            text += "\n限额冷却:" + std::to_string(cooling) + "个";
        }
        display->ShowNotification(text, 8000);
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

    // 通过 POST /v0/management/api-call 用账号令牌代呼
    // https://chatgpt.com/backend-api/wham/usage，返回 5h/每周剩余百分比
    static bool FetchCodexRemaining(const std::string& base_url, const std::string& management_key,
                                    const std::string& auth_index, const std::string& account_id,
                                    int& remaining_5h, int& remaining_weekly, int& reset_5h_seconds) {
        remaining_5h = -1;
        remaining_weekly = -1;
        reset_5h_seconds = -1;

        cJSON* header = cJSON_CreateObject();
        cJSON_AddStringToObject(header, "Authorization", "Bearer $TOKEN$");
        cJSON_AddStringToObject(header, "Content-Type", "application/json");
        cJSON_AddStringToObject(header, "User-Agent", "codex_cli_rs/0.76.0 (Debian 13.0.0; x86_64) WindowsTerminal");
        cJSON_AddStringToObject(header, "Chatgpt-Account-Id", account_id.c_str());
        cJSON* request = cJSON_CreateObject();
        cJSON_AddStringToObject(request, "auth_index", auth_index.c_str());
        cJSON_AddStringToObject(request, "method", "GET");
        cJSON_AddStringToObject(request, "url", "https://chatgpt.com/backend-api/wham/usage");
        cJSON_AddItemToObject(request, "header", header);
        char* printed = cJSON_PrintUnformatted(request);
        std::string payload = printed != nullptr ? printed : "";
        cJSON_free(printed);
        cJSON_Delete(request);
        if (payload.empty()) {
            return false;
        }

        auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
        http->SetTimeout(8000);
        http->SetHeader("Authorization", "Bearer " + management_key);
        http->SetHeader("Content-Type", "application/json");
        http->SetContent(std::move(payload));
        std::string url = base_url + "/v0/management/api-call";
        if (!http->Open("POST", url)) {
            ESP_LOGE(TAG, "api-call: failed to open %s, err=0x%x", url.c_str(), http->GetLastError());
            return false;
        }
        int status = http->GetStatusCode();
        std::string body = status == 200 ? http->ReadAll() : "";
        http->Close();
        if (status != 200) {
            ESP_LOGE(TAG, "api-call: HTTP %d (auth_index=%s)", status, auth_index.c_str());
            return false;
        }

        cJSON* root = cJSON_Parse(body.c_str());
        if (root == nullptr) {
            return false;
        }
        cJSON* upstream_status = cJSON_GetObjectItem(root, "status_code");
        cJSON* usage_body = cJSON_GetObjectItem(root, "body");
        cJSON* usage = (cJSON_IsNumber(upstream_status) && upstream_status->valueint == 200 &&
                        cJSON_IsString(usage_body))
                           ? cJSON_Parse(usage_body->valuestring)
                           : nullptr;
        cJSON_Delete(root);
        if (usage == nullptr) {
            return false;
        }

        cJSON* rate_limit = cJSON_GetObjectItem(usage, "rate_limit");
        cJSON* primary = rate_limit ? cJSON_GetObjectItem(rate_limit, "primary_window") : nullptr;
        cJSON* secondary = rate_limit ? cJSON_GetObjectItem(rate_limit, "secondary_window") : nullptr;
        remaining_5h = WindowRemainingPct(primary);
        remaining_weekly = WindowRemainingPct(secondary);
        reset_5h_seconds = WindowResetSeconds(primary);
        ESP_LOGI(TAG, "Codex usage: auth_index=%s 5h=%d%% weekly=%d%% reset=%ds",
                 auth_index.c_str(), remaining_5h, remaining_weekly, reset_5h_seconds);
        cJSON_Delete(usage);
        return remaining_5h >= 0 || remaining_weekly >= 0;
    }

    void ShowCodexRemaining(const char* base_url, const std::string& management_key,
                            const std::vector<CodexAccount>& accounts, std::vector<std::string>& plans,
                            int cooling) {
        // 逐账号出站查询较慢，最多查 4 个账号
        const size_t kMaxQueried = 4;
        std::string line_5h = "5h余:";
        std::string line_weekly = "周余:";
        int min_remaining = 101;  // 最紧张账号的剩余与重置时间
        int min_reset = -1;
        int queried = 0;
        int failed = 0;

        for (size_t i = 0; i < accounts.size() && i < kMaxQueried; i++) {
            int remaining_5h = -1;
            int remaining_weekly = -1;
            int reset_5h = -1;
            if (!FetchCodexRemaining(base_url, management_key, accounts[i].auth_index,
                                     accounts[i].account_id, remaining_5h, remaining_weekly, reset_5h)) {
                failed++;
            }
            if (queried > 0) {
                line_5h += "|";
                line_weekly += "|";
            }
            line_5h += remaining_5h >= 0 ? std::to_string(remaining_5h) + "%" : "?";
            line_weekly += remaining_weekly >= 0 ? std::to_string(remaining_weekly) + "%" : "?";
            if (remaining_5h >= 0 && remaining_5h < min_remaining) {
                min_remaining = remaining_5h;
                min_reset = reset_5h;
            }
            queried++;
        }

        if (queried == 0 || (failed == queried)) {
            GetDisplay()->ShowNotification("剩余用量查询失败:api-call不可用", 3000);
            return;
        }

        std::string text = "ChatGPT:" + std::to_string(accounts.size()) + "个";
        if (!plans.empty()) {
            text += "(";
            for (size_t i = 0; i < plans.size(); i++) {
                if (i > 0) text += ",";
                text += plans[i];
            }
            text += ")";
        }
        text += "\n" + line_5h;
        if (min_reset > 0) {
            char reset_label[24];
            if (min_reset >= 3600) {
                snprintf(reset_label, sizeof(reset_label), " %.1f时重置", min_reset / 3600.0);
            } else {
                snprintf(reset_label, sizeof(reset_label), " %d分重置", min_reset / 60);
            }
            text += reset_label;
        }
        text += "\n" + line_weekly;
        if (cooling > 0) {
            text += "\n限额冷却:" + std::to_string(cooling) + "个";
        }
        GetDisplay()->ShowNotification(text, 8000);
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

        // 返回键（XIO_KEY_Q）：短按打断对话/查询用量，长按强制查询用量
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
            auto& app = Application::GetInstance();
            auto state = app.GetDeviceState();
            if (state == kDeviceStateListening || state == kDeviceStateSpeaking) {
                app.ToggleChatState();
            } else if (state == kDeviceStateIdle) {
                self->StartChatGptUsageQuery();
            }
        }, this);

        iot_button_register_cb(q_btn_handle, BUTTON_LONG_PRESS_START, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<atk_dnesp32s3_box2_wifi*>(usr_data);
            self->power_save_timer_->WakeUp();
            self->StartChatGptUsageQuery();
        }, this);

        iot_button_register_cb(r_btn_handle, BUTTON_PRESS_DOWN, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<atk_dnesp32s3_box2_wifi*>(usr_data);
            self->power_save_timer_->WakeUp();
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
