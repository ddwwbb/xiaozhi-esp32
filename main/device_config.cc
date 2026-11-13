#include "device_config.h"
#include "settings.h"

#include <esp_log.h>

#include <algorithm>
#include <cstring>

#define TAG "DeviceConfig"

namespace {
constexpr const char* kCfgNs = "device_cfg";
// All NVS keys MUST stay <= 15 characters (ESP-IDF NVS limit).
constexpr const char* kKeyListenMode = "listen_mode";
constexpr const char* kKeyWakeBarge = "wake_barge_in";
constexpr const char* kKeyDimTimeout = "dim_timeout";
constexpr const char* kKeyDimBright = "dim_bright";
constexpr const char* kKeyDimUsb = "dim_on_usb";
constexpr const char* kKeyBatOffMin = "bat_off_min";
constexpr const char* kKeyBright = "brightness";
constexpr const char* kKeyVolume = "volume";
constexpr const char* kKeyTheme = "theme";
constexpr const char* kKeySkipOta = "skip_boot_ota";
constexpr const char* kKeyCfgVersion = "cfg_version";
}  // namespace

void DeviceConfig::LoadFromNvs() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (loaded_) {
        return;
    }

    Settings cfg(kCfgNs, false);
    std::string mode = cfg.GetString(kKeyListenMode, "auto");
    data_.listening_mode = (mode == "manual") ? "manual" : "auto";
    data_.wake_barge_in = cfg.GetBool(kKeyWakeBarge, true);
    data_.dim_timeout_sec = std::clamp<int>(cfg.GetInt(kKeyDimTimeout, 60), 0, 3600);
    data_.dim_brightness = std::clamp<int>(cfg.GetInt(kKeyDimBright, 5), 1, 50);
    data_.dim_on_usb = cfg.GetBool(kKeyDimUsb, true);
    data_.battery_off_min = std::clamp<int>(cfg.GetInt(kKeyBatOffMin, 15), 0, 1440);
#ifdef CONFIG_BOX2_SKIP_BOOT_OTA
    bool default_skip_ota = true;
#else
    bool default_skip_ota = false;
#endif
    data_.skip_boot_ota = cfg.GetBool(kKeySkipOta, default_skip_ota);
    data_.config_version = cfg.GetInt(kKeyCfgVersion, 0);

    Settings display_cfg("display", false);
    int disp_bright = display_cfg.GetInt("brightness", -1);
    data_.brightness = std::clamp<int>(
        cfg.GetInt(kKeyBright, disp_bright >= 1 ? disp_bright : 80), 1, 100);
    std::string disp_theme = display_cfg.GetString("theme", "");
    std::string stored_theme = cfg.GetString(kKeyTheme, disp_theme.empty() ? "light" : disp_theme);
    data_.theme = (stored_theme == "dark") ? "dark" : "light";

    Settings audio_cfg("audio", false);
    int audio_vol = audio_cfg.GetInt("output_volume", -1);
    data_.volume = std::clamp<int>(
        cfg.GetInt(kKeyVolume, audio_vol >= 0 ? audio_vol : 70), 0, 100);

    loaded_ = true;
    ESP_LOGI(TAG,
             "Loaded: mode=%s barge=%d dim=%ds(%d%%) usb_dim=%d bat_off=%dm skip_ota=%d ver=%d",
             data_.listening_mode.c_str(), data_.wake_barge_in, data_.dim_timeout_sec,
             data_.dim_brightness, data_.dim_on_usb, data_.battery_off_min, data_.skip_boot_ota,
             data_.config_version);
}

DeviceConfigData DeviceConfig::Snapshot() const {
    const_cast<DeviceConfig*>(this)->LoadFromNvs();
    std::lock_guard<std::mutex> lock(mutex_);
    return data_;
}

bool DeviceConfig::ApplyJson(const cJSON* root, int version, bool notify_listeners) {
    if (!cJSON_IsObject(root)) {
        return false;
    }
    LoadFromNvs();

    bool changed = false;
    bool version_updated = false;
    std::vector<std::function<void()>> listeners_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Settings cfg(kCfgNs, true);

        auto item = cJSON_GetObjectItem(root, "listening_mode");
        if (cJSON_IsString(item)) {
            std::string val = (strcmp(item->valuestring, "manual") == 0) ? "manual" : "auto";
            if (val != data_.listening_mode) {
                data_.listening_mode = val;
                cfg.SetString(kKeyListenMode, val);
                changed = true;
            }
        }

        item = cJSON_GetObjectItem(root, "wake_barge_in");
        if (cJSON_IsBool(item)) {
            bool val = cJSON_IsTrue(item);
            if (val != data_.wake_barge_in) {
                data_.wake_barge_in = val;
                cfg.SetBool(kKeyWakeBarge, val);
                changed = true;
            }
        }

        item = cJSON_GetObjectItem(root, "dim_timeout_sec");
        if (cJSON_IsNumber(item)) {
            int val = std::clamp<int>(item->valueint, 0, 3600);
            if (val != data_.dim_timeout_sec) {
                data_.dim_timeout_sec = val;
                cfg.SetInt(kKeyDimTimeout, val);
                changed = true;
            }
        }

        item = cJSON_GetObjectItem(root, "dim_brightness");
        if (cJSON_IsNumber(item)) {
            int val = std::clamp<int>(item->valueint, 1, 50);
            if (val != data_.dim_brightness) {
                data_.dim_brightness = val;
                cfg.SetInt(kKeyDimBright, val);
                changed = true;
            }
        }

        item = cJSON_GetObjectItem(root, "dim_on_usb");
        if (cJSON_IsBool(item)) {
            bool val = cJSON_IsTrue(item);
            if (val != data_.dim_on_usb) {
                data_.dim_on_usb = val;
                cfg.SetBool(kKeyDimUsb, val);
                changed = true;
            }
        }

        item = cJSON_GetObjectItem(root, "battery_off_min");
        if (cJSON_IsNumber(item)) {
            int val = std::clamp<int>(item->valueint, 0, 1440);
            if (val != data_.battery_off_min) {
                data_.battery_off_min = val;
                cfg.SetInt(kKeyBatOffMin, val);
                changed = true;
            }
        }

        item = cJSON_GetObjectItem(root, "brightness");
        if (cJSON_IsNumber(item)) {
            int val = std::clamp<int>(item->valueint, 1, 100);
            if (val != data_.brightness) {
                data_.brightness = val;
                cfg.SetInt(kKeyBright, val);
                changed = true;
            }
        }

        item = cJSON_GetObjectItem(root, "volume");
        if (cJSON_IsNumber(item)) {
            int val = std::clamp<int>(item->valueint, 0, 100);
            if (val != data_.volume) {
                data_.volume = val;
                cfg.SetInt(kKeyVolume, val);
                changed = true;
            }
        }

        item = cJSON_GetObjectItem(root, "theme");
        if (cJSON_IsString(item)) {
            std::string val = (strcmp(item->valuestring, "dark") == 0) ? "dark" : "light";
            if (val != data_.theme) {
                data_.theme = val;
                cfg.SetString(kKeyTheme, val);
                changed = true;
            }
        }

        item = cJSON_GetObjectItem(root, "skip_boot_ota");
        if (cJSON_IsBool(item)) {
            bool val = cJSON_IsTrue(item);
            if (val != data_.skip_boot_ota) {
                data_.skip_boot_ota = val;
                cfg.SetBool(kKeySkipOta, val);
                changed = true;
            }
        }

        item = cJSON_GetObjectItem(root, "ota_url");
        if (cJSON_IsString(item) && item->valuestring[0] != '\0') {
            Settings wifi_cfg("wifi", true);
            if (wifi_cfg.GetString("ota_url") != item->valuestring) {
                wifi_cfg.SetString("ota_url", item->valuestring);
                changed = true;
            }
        }

        if (version >= 0 && version != data_.config_version) {
            data_.config_version = version;
            cfg.SetInt(kKeyCfgVersion, version);
            version_updated = true;
        }

        if (changed) {
            listeners_copy = listeners_;
        }
    }

    if (changed && notify_listeners) {
        for (auto& fn : listeners_copy) {
            if (fn) {
                fn();
            }
        }
    }
    return changed || version_updated;
}

std::string DeviceConfig::ToJsonString(bool include_ota_url) const {
    auto snap = Snapshot();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "listening_mode", snap.listening_mode.c_str());
    cJSON_AddBoolToObject(root, "wake_barge_in", snap.wake_barge_in);
    cJSON_AddNumberToObject(root, "dim_timeout_sec", snap.dim_timeout_sec);
    cJSON_AddNumberToObject(root, "dim_brightness", snap.dim_brightness);
    cJSON_AddBoolToObject(root, "dim_on_usb", snap.dim_on_usb);
    cJSON_AddNumberToObject(root, "battery_off_min", snap.battery_off_min);
    cJSON_AddNumberToObject(root, "brightness", snap.brightness);
    cJSON_AddNumberToObject(root, "volume", snap.volume);
    cJSON_AddStringToObject(root, "theme", snap.theme.c_str());
    cJSON_AddBoolToObject(root, "skip_boot_ota", snap.skip_boot_ota);
    cJSON_AddNumberToObject(root, "config_version", snap.config_version);
    if (include_ota_url) {
        cJSON_AddStringToObject(root, "ota_url", GetEffectiveOtaUrl().c_str());
    }
    char* printed = cJSON_PrintUnformatted(root);
    std::string out(printed ? printed : "{}");
    if (printed) {
        cJSON_free(printed);
    }
    cJSON_Delete(root);
    return out;
}

std::string DeviceConfig::GetEffectiveOtaUrl() const {
    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty()) {
        url = CONFIG_OTA_URL;
    }
    return url;
}

void DeviceConfig::SetOtaUrl(const std::string& url) {
    Settings settings("wifi", true);
    if (url.empty()) {
        settings.EraseKey("ota_url");
    } else {
        settings.SetString("ota_url", url);
    }
}

void DeviceConfig::AddChangeListener(std::function<void()> listener) {
    std::lock_guard<std::mutex> lock(mutex_);
    listeners_.push_back(std::move(listener));
}
