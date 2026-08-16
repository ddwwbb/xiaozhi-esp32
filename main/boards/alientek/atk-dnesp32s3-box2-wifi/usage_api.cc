// Box2 AI 用量查询：网络传输层与数据源适配（实现见 usage_api.h）。
// 从板实例解耦：代理配置经参数传入（空指针=直连），NVS 回写由调用方完成
#include "usage_api.h"

#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <mbedtls/ssl.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <fcntl.h>
#include <errno.h>

#include <algorithm>
#include <cJSON.h>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <strings.h>

#define TAG "usage_api"

namespace usage {

// 窗口剩余百分比：优先 used_percent，退回 remaining_count/total_count；未知返回 -1
int WindowRemainingPct(cJSON* window) {
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

int WindowResetSeconds(cJSON* window) {
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
int TcpConnect(const std::string& host, int port, int timeout_ms) {
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

bool Socks5Handshake(int fd, const Socks5Config& proxy, const std::string& host, int port) {
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

std::string Base64Encode(const std::string& in) {
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
bool HttpProxyHandshake(int fd, const Socks5Config& proxy, const std::string& host, int port) {
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

bool HttpsDechunk(std::string& body) {
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

// HTTPS 请求（proxy 为空指针时强制直连）：Connection: close，一次一连接
bool HttpsRequest(const std::string& host, const std::string& path, const char* method,
                         const std::vector<std::pair<std::string, std::string>>& headers,
                         const std::string& body, int& status_code, std::string& resp_body,
                         const Socks5Config* proxy) {
    std::string connect_host = proxy != nullptr && proxy->enabled() ? proxy->host : host;
    int connect_port = proxy != nullptr && proxy->enabled() ? proxy->port : 443;

    int fd = TcpConnect(connect_host, connect_port, 10000);
    if (fd < 0) {
        return false;
    }
    bool ok = true;
    if (proxy != nullptr && proxy->enabled()) {
        if (proxy->type == "http") {
            ok = HttpProxyHandshake(fd, *proxy, host, 443);
        } else {
            ok = Socks5Handshake(fd, *proxy, host, 443);
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

// 明文 HTTP GET（局域网本机统计服务，不走代理不做 TLS）：Connection: close，一次一连接，
// 读到对端关闭为止；解析逻辑与 HttpsRequest 一致（状态行/chunked）
bool PlainHttpGet(const std::string& host, int port, const std::string& path,
                         int& status_code, std::string& resp_body) {
    int fd = TcpConnect(host, port, 5000);
    if (fd < 0) {
        return false;
    }
    std::string req = "GET " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + "\r\nConnection: close\r\nAccept: application/json\r\n\r\n";
    bool ok = true;
    size_t sent = 0;
    while (ok && sent < req.size()) {
        int n = send(fd, req.data() + sent, req.size() - sent, 0);
        if (n <= 0) {
            ok = false;
        } else {
            sent += n;
        }
    }
    std::string raw;
    char buf[1024];
    // 本机统计含 14 天 buckets 与全量模型分类，上限比 HttpsRequest 宽
    while (ok && raw.size() < 48 * 1024) {
        int n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        raw.append(buf, n);
    }
    close(fd);
    if (!ok || raw.empty() || raw.compare(0, 5, "HTTP/") != 0) {
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

bool Base64UrlDecode(const std::string& in, std::string& out) {
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
cJSON* DecodeJwtPayload(const std::string& jwt) {
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

std::string JwtString(cJSON* payload, const char* key) {
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
int DirectFetchUsage(const std::string& access_token, const std::string& account_id,
                            AccountDetail& acc, const Socks5Config* proxy) {
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Authorization", "Bearer " + access_token},
        {"Content-Type", "application/json"},
        {"User-Agent", "codex_cli_rs/0.76.0 (Debian 13.0.0; x86_64) WindowsTerminal"},
        {"Chatgpt-Account-Id", account_id},
    };
    int status = 0;
    std::string body;
    if (!HttpsRequest("chatgpt.com", "/backend-api/wham/usage", "GET", headers, "", status, body,
                      proxy)) {
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
void FetchOfficialExtras(const std::string& access_token, const std::string& account_id,
                                AccountDetail& acc, const Socks5Config* proxy) {
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Authorization", "Bearer " + access_token},
        {"Content-Type", "application/json"},
        {"User-Agent", "codex_cli_rs/0.76.0 (Debian 13.0.0; x86_64) WindowsTerminal"},
        {"Chatgpt-Account-Id", account_id},
    };

    int status = 0;
    std::string body;
    if (HttpsRequest("chatgpt.com", "/backend-api/wham/profiles/me", "GET", headers, "", status, body,
                     proxy) &&
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
                     status, body, proxy) &&
        status == 200) {
        cJSON* root = cJSON_Parse(body.c_str());
        cJSON* count = root != nullptr ? cJSON_GetObjectItem(root, "available_count") : nullptr;
        if (cJSON_IsNumber(count)) {
            acc.reset_credits = count->valueint;
        }
        cJSON_Delete(root);
    }
}

// 把智谱 limits 条目写入对应窗口槽位（unit：3=5小时窗 6=周窗，percentage 为已用百分比）
void ApplyZhipuLimit(cJSON* limit_item, int unit_value, AccountDetail& acc) {
    cJSON* pct = cJSON_GetObjectItem(limit_item, "percentage");
    if (!cJSON_IsNumber(pct)) {
        return;
    }
    int remaining = 100 - (int)(pct->valuedouble + 0.5);
    if (remaining < 0) {
        remaining = 0;
    }
    if (remaining > 100) {
        remaining = 100;
    }
    int reset = -1;
    cJSON* next_reset = cJSON_GetObjectItem(limit_item, "nextResetTime");
    if (cJSON_IsNumber(next_reset) && next_reset->valuedouble > 0) {
        reset = (int)(next_reset->valuedouble / 1000.0) - (int)time(nullptr);
        if (reset <= 0) {
            reset = -1;
        }
    }
    if (unit_value == 3 && acc.remaining_5h < 0) {
        acc.remaining_5h = remaining;
        acc.reset_5h = reset;
    } else if (unit_value == 6 && acc.remaining_weekly < 0) {
        acc.remaining_weekly = remaining;
        acc.reset_weekly = reset;
    }
}

// 直连智谱官方接口 GET open.bigmodel.cn/api/monitor/usage/quota/limit。
// 请求与响应形态照搬 cc-switch 的实测：Authorization 头直接放 API Key 不加 Bearer；
// data.level 为套餐等级；data.limits[] 的 type 取 TOKENS_LIMIT/CREDIT_LIMIT，
// percentage 为已用百分比、nextResetTime 为毫秒时间戳、unit 标识窗口（3=5小时 6=周）。
// 返回 200 成功；HTTP 状态码原样返回；-1 网络失败；-2 响应异常（业务错误/解析失败）
int FetchZhipuUsage(const std::string& api_key, AccountDetail& acc, const Socks5Config* proxy) {
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Authorization", api_key},
        {"Content-Type", "application/json"},
    };
    int status = 0;
    std::string body;
    if (!HttpsRequest("open.bigmodel.cn", "/api/monitor/usage/quota/limit", "GET", headers, "", status, body,
                      proxy)) {
        ESP_LOGE(TAG, "Zhipu usage: request failed (%s)", acc.name.c_str());
        return -1;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "Zhipu usage: HTTP %d (%s)", status, acc.name.c_str());
        return status;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Zhipu usage: bad json (%s)", acc.name.c_str());
        return -2;
    }
    cJSON* success = cJSON_GetObjectItem(root, "success");
    if (cJSON_IsFalse(success)) {
        cJSON* msg = cJSON_GetObjectItem(root, "msg");
        ESP_LOGE(TAG, "Zhipu usage: API error: %s",
                 cJSON_IsString(msg) ? msg->valuestring : "unknown");
        cJSON_Delete(root);
        return -2;
    }
    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (data == nullptr) {
        cJSON_Delete(root);
        return -2;
    }
    cJSON* level = cJSON_GetObjectItem(data, "level");
    if (cJSON_IsString(level) && level->valuestring[0] != '\0') {
        acc.plan = level->valuestring;
    }

    // 窗口分类：unit 显式标识优先；缺失时兜底——无重置时间的条目优先归
    // 5 小时窗，其余按重置时间升序填入空槽（智谱最多两条，与 cc-switch 一致）
    cJSON* fallback[2] = {nullptr, nullptr};
    int fallback_count = 0;
    cJSON* limits = cJSON_GetObjectItem(data, "limits");
    if (cJSON_IsArray(limits)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, limits) {
            cJSON* type = cJSON_GetObjectItem(item, "type");
            if (!cJSON_IsString(type)) {
                continue;
            }
            if (strcasecmp(type->valuestring, "TOKENS_LIMIT") != 0 &&
                strcasecmp(type->valuestring, "CREDIT_LIMIT") != 0) {
                continue;
            }
            cJSON* unit = cJSON_GetObjectItem(item, "unit");
            int unit_value = cJSON_IsNumber(unit) ? unit->valueint : -1;
            if (unit_value == 3 || unit_value == 6) {
                ApplyZhipuLimit(item, unit_value, acc);
            } else if (fallback_count < 2) {
                fallback[fallback_count++] = item;
            }
        }
    }
    if (fallback_count == 2) {
        auto reset_ms = [](cJSON* item) -> long long {
            cJSON* next_reset = cJSON_GetObjectItem(item, "nextResetTime");
            return cJSON_IsNumber(next_reset) ? (long long)next_reset->valuedouble : -1;
        };
        if (reset_ms(fallback[1]) < reset_ms(fallback[0])) {
            std::swap(fallback[0], fallback[1]);
        }
    }
    // 兜底条目按序填入仍空缺的槽位（5h 优先，其次周窗）：
    // 显式 unit 已占住 5h 时，剩余条目应落周窗而非被丢弃
    for (int i = 0; i < fallback_count; i++) {
        ApplyZhipuLimit(fallback[i], acc.remaining_5h < 0 ? 3 : 6, acc);
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Zhipu usage: %s 5h=%d%% weekly=%d%%",
             acc.name.c_str(), acc.remaining_5h, acc.remaining_weekly);
    return 200;
}

// 本机统计服务（PC 端 box2-usage-server，明文 HTTP）GET /usage。
// 服务端聚合 Claude Code / ZCode / Codex CLI 的本地会话记录：当天/本周 token 与费用、
// 近 14 天逐日、累计/峰值/连续天数、按模型分类。
    // 成功时 locals 追加三张维度卡片（当天/本周/累计），各对应一个详情页；
// 失败时追加一个 unavailable 条目。返回 200 成功；HTTP 状态码；-1 网络；-2 解析失败
int FetchLocalUsage(const std::string& host, int port, const std::string& key,
                           const std::string& name_suffix, std::vector<AccountDetail>& locals) {
    std::string path = "/usage";
    if (!key.empty()) {
        path += "?key=" + key;
    }
    int status = 0;
    std::string body;
    if (!PlainHttpGet(host, port, path, status, body)) {
        ESP_LOGE(TAG, "Local usage: request failed (%s:%d)", host.c_str(), port);
        return -1;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "Local usage: HTTP %d (%s:%d)", status, host.c_str(), port);
        return status;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Local usage: bad json");
        return -2;
    }
    auto number_value = [](cJSON* parent, const char* key) -> long long {
        cJSON* item = parent != nullptr ? cJSON_GetObjectItem(parent, key) : nullptr;
        return cJSON_IsNumber(item) ? (long long)item->valuedouble : -1;
    };
    auto cost_value = [](cJSON* parent) -> double {
        cJSON* item = parent != nullptr ? cJSON_GetObjectItem(parent, "cost") : nullptr;
        return cJSON_IsNumber(item) && item->valuedouble >= 0 ? item->valuedouble : -1;
    };

    cJSON* today = cJSON_GetObjectItem(root, "today");
    cJSON* week = cJSON_GetObjectItem(root, "week");
    long long today_tokens = number_value(today, "tokens");
    long long week_tokens = number_value(week, "tokens");
    double today_cost = cost_value(today);
    double week_cost = cost_value(week);
    int today_requests = (int)number_value(today, "requests");
    int week_requests = (int)number_value(week, "requests");
    int reset_day = (int)number_value(root, "reset_after_seconds");
    int reset_week = (int)number_value(root, "weekly_reset_after_seconds");
    if (reset_day <= 0) {
        reset_day = -1;
    }
    if (reset_week <= 0) {
        reset_week = -1;
    }
    long long lifetime_tokens = number_value(root, "lifetime_tokens");
    long long peak_daily_tokens = number_value(root, "peak_daily_tokens");
    long streak_days = (long)number_value(root, "streak_days");
    double lifetime_cost = -1;
    cJSON* cost = cJSON_GetObjectItem(root, "lifetime_cost");
    if (cJSON_IsNumber(cost) && cost->valuedouble >= 0) {
        lifetime_cost = cost->valuedouble;
    }

    // 三张维度卡片：当天/本周/累计，共用同一来源标识
    std::vector<AccountDetail::ModelStat> model_stats;
    cJSON* model_item = nullptr;
    cJSON_ArrayForEach(model_item, cJSON_GetObjectItem(root, "models")) {
        cJSON* name_item = cJSON_GetObjectItem(model_item, "model");
        cJSON* tokens_item = cJSON_GetObjectItem(model_item, "tokens");
        if (!cJSON_IsString(name_item) || !cJSON_IsNumber(tokens_item)) {
            continue;
        }
        AccountDetail::ModelStat stat;
        stat.name = name_item->valuestring;
        stat.tokens = (long long)tokens_item->valuedouble;
        stat.cost = cost_value(model_item);
        cJSON* priced_item = cJSON_GetObjectItem(model_item, "priced");
        stat.priced = !cJSON_IsFalse(priced_item);
        model_stats.push_back(std::move(stat));
    }
    std::vector<std::pair<std::string, long long>> buckets;
    cJSON* bucket = nullptr;
    cJSON_ArrayForEach(bucket, cJSON_GetObjectItem(root, "daily_buckets")) {
        cJSON* date_item = cJSON_GetObjectItem(bucket, "date");
        cJSON* tokens_item = cJSON_GetObjectItem(bucket, "tokens");
        if (cJSON_IsString(date_item) && cJSON_IsNumber(tokens_item)) {
            buckets.emplace_back(date_item->valuestring, (long long)tokens_item->valuedouble);
        }
    }
    cJSON_Delete(root);

    // 每台 PC 一个条目携带全量数据（Claude Code/ZCode/Codex 多 CLI 聚合）：
    // 列表页一张卡浓缩 当天/本周/累计 三行，详情页一页展示该台全部信息
    AccountDetail acc;
    acc.name = name_suffix.empty() ? "本地" : "本地@" + name_suffix;
    acc.email = "AI CLI @ " + host;
    acc.plan = "本地";
    acc.local_stats = true;
    acc.today_tokens = today_tokens;
    acc.today_cost = today_cost;
    acc.requests = today_requests;
    acc.week_tokens = week_tokens;
    acc.week_cost = week_cost;
    acc.week_requests = week_requests;
    acc.reset_5h = reset_day;      // 列表卡片倒计时：到当日午夜
    acc.lifetime_tokens = lifetime_tokens;
    acc.lifetime_cost = lifetime_cost;
    acc.peak_daily_tokens = peak_daily_tokens;
    acc.current_streak_days = streak_days;
    acc.local_models = std::move(model_stats);
    acc.daily_buckets = std::move(buckets);

    locals.push_back(std::move(acc));
    ESP_LOGI(TAG, "Local usage: today=%lld week=%lld lifetime=%lld cost=%.2f",
             today_tokens, week_tokens, lifetime_tokens, lifetime_cost);
    return 200;
}

// OAuth 刷新（codex 官方端点）：在 auth_json 基础上轮换令牌与时间戳，
// 返回 {新 access_token, 更新并序列化后的 auth_json}；失败返回空串。NVS 回写由调用方完成
std::pair<std::string, std::string> RefreshCodexToken(const std::string& auth_json,
                                                      const Socks5Config* proxy) {
    cJSON* root = cJSON_Parse(auth_json.c_str());
    cJSON* refresh_item = root != nullptr ? cJSON_GetObjectItem(root, "refresh_token") : nullptr;
    if (!cJSON_IsString(refresh_item) || refresh_item->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return {};
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
    if (!HttpsRequest("auth.openai.com", "/oauth/token", "POST", headers, body, status, resp,
                      proxy)) {
        ESP_LOGE(TAG, "Token refresh: request failed");
        cJSON_Delete(root);
        return {};
    }
    if (status != 200) {
        ESP_LOGE(TAG, "Token refresh: HTTP %d", status);
        cJSON_Delete(root);
        return {};
    }

    cJSON* token = cJSON_Parse(resp.c_str());
    if (token == nullptr) {
        cJSON_Delete(root);
        return {};
    }
    cJSON* access_item = cJSON_GetObjectItem(token, "access_token");
    if (!cJSON_IsString(access_item)) {
        cJSON_Delete(token);
        cJSON_Delete(root);
        return {};
    }

    // 轮换：access/refresh/id token 都可能更新，必须全部写回
    auto replace_string = [root](const char* key, cJSON* value_root) {
        cJSON* value = value_root ? cJSON_GetObjectItem(value_root, key) : nullptr;
        if (cJSON_IsString(value)) {
            cJSON_ReplaceItemInObject(root, key, cJSON_CreateString(value->valuestring));
        }
    };
    replace_string("access_token", token);
    replace_string("refresh_token", token);
    replace_string("id_token", token);

    time_t now = time(nullptr);
    char time_str[24];
    strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    cJSON_ReplaceItemInObject(root, "last_refresh", cJSON_CreateString(time_str));
    cJSON* expires_item = cJSON_GetObjectItem(token, "expires_in");
    if (cJSON_IsNumber(expires_item)) {
        time_t expired_at = now + expires_item->valueint;
        strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", gmtime(&expired_at));
        cJSON_ReplaceItemInObject(root, "expired", cJSON_CreateString(time_str));
    }
    cJSON_Delete(token);

    std::string access_token = access_item->valuestring;
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed == nullptr) {
        return {};
    }
    std::string updated(printed);
    cJSON_free(printed);
    ESP_LOGI(TAG, "Token refreshed");
    return {access_token, updated};
}
// 从已存令牌 JSON 提取 account_id（去重用）
std::string ExtractAccountId(const std::string& json) {
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
std::string FormatTokens(long long value) {
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

// 费用后缀，如 " ($0.57)"；未知返回空（"费"字不在补字字体，用 $ 表达）
std::string CostSuffix(double cost) {
    if (cost < 0) {
        return "";
    }
    char label[20];
    snprintf(label, sizeof(label), " ($%.2f)", cost);
    return label;
}

// 模型行右侧费用列：未定价/未知显示 "-"
std::string CostText(double cost, bool priced) {
    if (!priced || cost < 0) {
        return "-";
    }
    char label[20];
    snprintf(label, sizeof(label), "$%.2f", cost);
    return label;
}
}  // namespace usage
