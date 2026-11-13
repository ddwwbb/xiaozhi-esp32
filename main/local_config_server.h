#ifndef _LOCAL_CONFIG_SERVER_H_
#define _LOCAL_CONFIG_SERVER_H_

#include <esp_http_server.h>

#include <functional>
#include <mutex>

class LocalConfigServer {
public:
    static LocalConfigServer& GetInstance() {
        static LocalConfigServer instance;
        return instance;
    }

    LocalConfigServer(const LocalConfigServer&) = delete;
    LocalConfigServer& operator=(const LocalConfigServer&) = delete;

    void Start(std::function<void()> on_enter_wifi_config = nullptr);
    void Stop();
    bool IsRunning() const;

private:
    LocalConfigServer() = default;

    static esp_err_t HandleRootGet(httpd_req_t* req);
    static esp_err_t HandleConfigGet(httpd_req_t* req);
    static esp_err_t HandleConfigPost(httpd_req_t* req);
    static esp_err_t HandleRebootPost(httpd_req_t* req);
    static esp_err_t HandleWifiConfigPost(httpd_req_t* req);

    mutable std::mutex mutex_;
    httpd_handle_t server_ = nullptr;
    std::function<void()> on_enter_wifi_config_;
};

#endif  // _LOCAL_CONFIG_SERVER_H_
