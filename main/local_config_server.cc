#include "local_config_server.h"

#include "application.h"
#include "device_config.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>
#include <string>

#define TAG "LocalCfgWeb"

namespace {
constexpr size_t kMaxPostBodyBytes = 4096;

const char kIndexHtml[] = R"HTML(<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>BOX2 本地设备配置</title>
<style>
body{font-family:system-ui,-apple-system,sans-serif;background:#f5f7fb;color:#1e293b;margin:0;padding:16px}
.wrap{max-width:560px;margin:0 auto;background:#fff;border:1px solid #e2e8f0;border-radius:12px;padding:20px;box-shadow:0 2px 8px rgba(0,0,0,.05)}
h1{font-size:18px;margin:0 0 6px}
.sub{font-size:12px;color:#64748b;margin-bottom:16px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.full{grid-column:1/-1}
label{display:block;font-size:12px;font-weight:600;margin-bottom:4px}
input,select{width:100%;box-sizing:border-box;padding:8px;border:1px solid #cbd5e1;border-radius:6px;font-size:14px}
.chk{display:flex;align-items:center;gap:8px;font-size:13px;padding:6px 0}
.chk input{width:auto}
.btns{display:flex;flex-wrap:wrap;gap:8px;margin-top:18px}
button{flex:1;min-width:110px;padding:10px;border:1px solid #cbd5e1;border-radius:8px;background:#fff;font-size:14px;font-weight:600;cursor:pointer}
button.pri{background:#2563eb;color:#fff;border-color:#2563eb}
button.warn{color:#b91c1c;border-color:#fecaca}
#msg{margin-top:12px;font-size:13px;min-height:18px;color:#0f766e}
</style></head><body><div class="wrap">
<h1>BOX2 设备参数配置</h1>
<div class="sub">MAC: <code id="mac">-</code></div>
<div class="grid">
  <div><label>对话模式</label>
    <select id="listening_mode"><option value="auto">auto (自由连续)</option><option value="manual">manual (按键说话)</option></select>
  </div>
  <div><label>主题</label>
    <select id="theme"><option value="light">light (浅色)</option><option value="dark">dark (深色)</option></select>
  </div>
  <div><label>闲置微亮等待 (秒, 0=关闭)</label><input id="dim_timeout_sec" type="number" min="0" max="3600"></div>
  <div><label>微亮亮度 (1-50)</label><input id="dim_brightness" type="number" min="1" max="50"></div>
  <div><label>电池自动关机 (分钟, 0=永不)</label><input id="battery_off_min" type="number" min="0" max="1440"></div>
  <div><label>工作亮度 (1-100)</label><input id="brightness" type="number" min="1" max="100"></div>
  <div><label>音量 (0-100)</label><input id="volume" type="number" min="0" max="100"></div>
  <div class="full"><label>网关 OTA 引导地址 (ota_url)</label><input id="ota_url" type="text"></div>
  <div class="full chk"><input id="wake_barge_in" type="checkbox"><label for="wake_barge_in" style="margin:0">唤醒词随时打断说话</label></div>
  <div class="full chk"><input id="dim_on_usb" type="checkbox"><label for="dim_on_usb" style="margin:0">USB 供电也启用屏幕微亮</label></div>
  <div class="full chk"><input id="skip_boot_ota" type="checkbox"><label for="skip_boot_ota" style="margin:0">开机跳过阻塞升级并秒连后端</label></div>
</div>
<div class="btns">
  <button class="pri" onclick="saveCfg()">保存并立即生效</button>
  <button onclick="rebootDev()">重启设备</button>
  <button class="warn" onclick="wifiCfg()">进入热点配网</button>
</div>
<div id="msg"></div>
</div>
<script>
const $ = id => document.getElementById(id);
async function load(){
  const r = await fetch('/api/config'); const d = await r.json();
  $('mac').textContent = d.mac || '-';
  $('listening_mode').value = d.listening_mode || 'auto';
  $('theme').value = d.theme || 'light';
  $('dim_timeout_sec').value = d.dim_timeout_sec ?? 60;
  $('dim_brightness').value = d.dim_brightness ?? 5;
  $('battery_off_min').value = d.battery_off_min ?? 15;
  $('brightness').value = d.brightness ?? 80;
  $('volume').value = d.volume ?? 70;
  $('ota_url').value = d.ota_url || '';
  $('wake_barge_in').checked = d.wake_barge_in !== false;
  $('dim_on_usb').checked = d.dim_on_usb !== false;
  $('skip_boot_ota').checked = d.skip_boot_ota !== false;
}
async function saveCfg(){
  const body = {
    listening_mode: $('listening_mode').value,
    theme: $('theme').value,
    dim_timeout_sec: parseInt($('dim_timeout_sec').value, 10),
    dim_brightness: parseInt($('dim_brightness').value, 10),
    battery_off_min: parseInt($('battery_off_min').value, 10),
    brightness: parseInt($('brightness').value, 10),
    volume: parseInt($('volume').value, 10),
    ota_url: $('ota_url').value.trim(),
    wake_barge_in: $('wake_barge_in').checked,
    dim_on_usb: $('dim_on_usb').checked,
    skip_boot_ota: $('skip_boot_ota').checked
  };
  const r = await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  $('msg').textContent = r.ok ? '已保存并应用！' : '保存失败';
}
async function rebootDev(){
  await fetch('/api/reboot',{method:'POST'});
  $('msg').textContent = '设备正在重启...';
}
async function wifiCfg(){
  if(!confirm('设备将断开当前 Wi-Fi 并开启配网热点，确认？')) return;
  await fetch('/api/wifi_config',{method:'POST'});
  $('msg').textContent = '已切换到配网热点模式';
}
load();
</script></body></html>)HTML";

void SendJson(httpd_req_t* req, const char* status, const std::string& json) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.size());
}
}  // namespace

void LocalConfigServer::Start(std::function<void()> on_enter_wifi_config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (on_enter_wifi_config) {
        on_enter_wifi_config_ = std::move(on_enter_wifi_config);
    }
    if (server_ != nullptr) {
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32769;  // Avoid clashing with esp-wifi-connect control port
    config.max_uri_handlers = 8;
    config.stack_size = 4096;
    config.lru_purge_enable = true;

    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start local HTTP config server on port 80");
        server_ = nullptr;
        return;
    }

    const httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = HandleRootGet, .user_ctx = this};
    const httpd_uri_t cfg_get = {.uri = "/api/config", .method = HTTP_GET, .handler = HandleConfigGet, .user_ctx = this};
    const httpd_uri_t cfg_post = {.uri = "/api/config", .method = HTTP_POST, .handler = HandleConfigPost, .user_ctx = this};
    const httpd_uri_t reboot_post = {.uri = "/api/reboot", .method = HTTP_POST, .handler = HandleRebootPost, .user_ctx = this};
    const httpd_uri_t wifi_post = {.uri = "/api/wifi_config", .method = HTTP_POST, .handler = HandleWifiConfigPost, .user_ctx = this};

    httpd_register_uri_handler(server_, &root_uri);
    httpd_register_uri_handler(server_, &cfg_get);
    httpd_register_uri_handler(server_, &cfg_post);
    httpd_register_uri_handler(server_, &reboot_post);
    httpd_register_uri_handler(server_, &wifi_post);
    ESP_LOGI(TAG, "Local config web server started on port 80");
}

void LocalConfigServer::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
        ESP_LOGI(TAG, "Local config web server stopped");
    }
}

bool LocalConfigServer::IsRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return server_ != nullptr;
}

esp_err_t LocalConfigServer::HandleRootGet(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, kIndexHtml, strlen(kIndexHtml));
    return ESP_OK;
}

esp_err_t LocalConfigServer::HandleConfigGet(httpd_req_t* req) {
    std::string cfg_json = DeviceConfig::GetInstance().ToJsonString(true);
    cJSON* obj = cJSON_Parse(cfg_json.c_str());
    if (obj == nullptr) {
        SendJson(req, "200 OK", cfg_json);
        return ESP_OK;
    }
    cJSON_AddStringToObject(obj, "mac", SystemInfo::GetMacAddress().c_str());
    char* out = cJSON_PrintUnformatted(obj);
    std::string body(out ? out : cfg_json);
    if (out) {
        cJSON_free(out);
    }
    cJSON_Delete(obj);
    SendJson(req, "200 OK", body);
    return ESP_OK;
}

esp_err_t LocalConfigServer::HandleConfigPost(httpd_req_t* req) {
    int total = req->content_len;
    if (total <= 0 || total > static_cast<int>(kMaxPostBodyBytes)) {
        SendJson(req, "400 Bad Request", R"({"error":"invalid content length"})");
        return ESP_OK;
    }
    std::string body(total, '\0');
    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, body.data() + received, total - received);
        if (ret <= 0) {
            SendJson(req, "400 Bad Request", R"({"error":"incomplete body"})");
            return ESP_OK;
        }
        received += ret;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (!cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        SendJson(req, "400 Bad Request", R"({"error":"invalid json"})");
        return ESP_OK;
    }
    DeviceConfig::GetInstance().ApplyJson(root, -1, true);
    cJSON_Delete(root);
    SendJson(req, "200 OK", R"({"status":"ok"})");
    return ESP_OK;
}

esp_err_t LocalConfigServer::HandleRebootPost(httpd_req_t* req) {
    SendJson(req, "200 OK", R"({"status":"rebooting"})");
    Application::GetInstance().Schedule([]() {
        vTaskDelay(pdMS_TO_TICKS(300));
        Application::GetInstance().Reboot();
    });
    return ESP_OK;
}

esp_err_t LocalConfigServer::HandleWifiConfigPost(httpd_req_t* req) {
    auto* self = static_cast<LocalConfigServer*>(req->user_ctx);
    SendJson(req, "200 OK", R"({"status":"entering_wifi_config"})");
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        cb = self->on_enter_wifi_config_;
    }
    if (cb) {
        Application::GetInstance().Schedule(std::move(cb));
    }
    return ESP_OK;
}
