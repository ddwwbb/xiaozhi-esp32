#ifndef _DEVICE_CONFIG_H_
#define _DEVICE_CONFIG_H_

#include <cJSON.h>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct DeviceConfigData {
    std::string listening_mode = "auto";
    bool wake_barge_in = true;
    int dim_timeout_sec = 60;
    int dim_brightness = 5;
    bool dim_on_usb = true;
    int battery_off_min = 15;
    int brightness = 80;
    int volume = 70;
    std::string theme = "light";
    bool skip_boot_ota = true;
    int config_version = 0;
};

/**
 * Singleton store for BOX2 runtime configuration backed by NVS ("device_cfg").
 * Thread-safe across OTA, WS receive, HTTP config server, MCP tools, and UI tasks.
 */
class DeviceConfig {
public:
    static DeviceConfig& GetInstance() {
        static DeviceConfig instance;
        return instance;
    }

    DeviceConfig(const DeviceConfig&) = delete;
    DeviceConfig& operator=(const DeviceConfig&) = delete;

    void LoadFromNvs();
    DeviceConfigData Snapshot() const;

    /**
     * Apply a partial or full JSON config object (e.g. from OTA, WS hello/config, HTTP, MCP).
     * If version >= 0 is provided, config_version is updated in NVS as well.
     * Returns true if any config field or version changed.
     */
    bool ApplyJson(const cJSON* root, int version = -1, bool notify_listeners = true);

    std::string ToJsonString(bool include_ota_url = false) const;

    std::string GetEffectiveOtaUrl() const;
    void SetOtaUrl(const std::string& url);

    void AddChangeListener(std::function<void()> listener);

private:
    DeviceConfig() = default;

    mutable std::mutex mutex_;
    DeviceConfigData data_;
    bool loaded_ = false;
    std::vector<std::function<void()>> listeners_;
};

#endif  // _DEVICE_CONFIG_H_
