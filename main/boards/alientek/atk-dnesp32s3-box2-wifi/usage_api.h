// Box2 AI 用量查询：数据结构 + 网络传输层 + 数据源适配层。
// 从板实例解耦：代理配置、NVS 回写等实例依赖一律经参数传入，
// 面板渲染（LVGL）与配置页留在板文件。
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace usage {

// 直连模式可选代理（socks5 或 http CONNECT，均支持账号密码认证）
struct Socks5Config {
    std::string type;  // "socks5" | "http"
    std::string host;
    int port = 0;
    std::string user;
    std::string pass;
    bool enabled() const { return !host.empty() && port > 0; }
};

// 本机统计服务（PC 端 box2-usage-server）地址
struct LocalUsageConfig {
    std::string host;
    int port = 3939;
    std::string key;  // 可空
};

// 单账号/单维度用量数据：remaining/reset 为 -1 表示查询失败。
// 字段按数据源分三组：公共展示 / 官方 API 账号 / 本机统计维度卡片
struct AccountDetail {
    // ---- 公共展示字段 ----
    std::string email;
    std::string plan;
    std::string name;                // 邮箱前缀，无邮箱时为 账号N/维度名
    std::string subscription_until;  // 订阅有效期（显示子串）
    std::string last_refresh;        // Token 最近刷新（显示子串）
    bool unavailable = false;  // 401/403 等令牌异常/服务不可达标记
    std::vector<std::pair<std::string, long long>> daily_buckets;  // 每日 token 用量
    // ---- 官方 API 账号（wham/usage、profiles/me、智谱）----
    long long lifetime_tokens = -1;
    long long peak_daily_tokens = -1;
    long current_streak_days = -1;
    int reset_credits = -1;  // 可用限额重置积分
    // 窗口用量统一为"已用百分比"（与 cc-switch 的 utilization 一致）。
    // 窗口身份按 limit_window_seconds 识别，不按 primary/secondary 位置：
    // 免费与 prolite 套餐的次窗口是 30 天而非每周
    int used_5h = -1;
    int used_weekly = -1;
    int used_monthly = -1;  // 30 天窗口（免费/prolite 次窗口）
    int used_cr = -1;       // 代码审查窗口
    int reset_5h = -1;
    int reset_weekly = -1;
    int reset_monthly = -1;
    int reset_cr = -1;
    std::string account_id;  // 查询时需要
    // ---- 本机统计（PC 端 box2-usage-server），无官方配额概念 ----
    // 每台 PC 一个条目携带全量数据：列表一页两台（卡片三行），详情一页一台
    bool local_stats = false;      // 本机条目与官方账号的语义分派
    long long today_tokens = -1;
    long long week_tokens = -1;
    double today_cost = -1;
    double week_cost = -1;
    double lifetime_cost = -1;
    int requests = -1;          // 当天请求数
    int week_requests = -1;     // 本周请求数
    struct ModelStat {
        std::string name;
        long long tokens;
        double cost;
        bool priced;
    };
    std::vector<ModelStat> local_models;
};

// ---------- 展示格式化（渲染层与数据源共用） ----------
std::string FormatTokens(long long value);
std::string CostSuffix(double cost);          // " ($0.57)"，未知返回空
std::string CostText(double cost, bool priced);  // 模型行费用列，未定价 "-"

// ---------- 传输层（可选代理 + TLS；代理参数为空指针即直连） ----------
int TcpConnect(const std::string& host, int port, int timeout_ms);
bool HttpsRequest(const std::string& host, const std::string& path, const char* method,
                  const std::vector<std::pair<std::string, std::string>>& headers,
                  const std::string& body, int& status_code, std::string& resp_body,
                  const Socks5Config* proxy);
// 明文 HTTP GET（局域网本机统计服务）：Connection: close，一次一连接
bool PlainHttpGet(const std::string& host, int port, const std::string& path,
                  int& status_code, std::string& resp_body);

// ---------- JWT / 账号工具 ----------
std::string ExtractAccountId(const std::string& auth_json);
void FillDetailFromIdToken(AccountDetail& acc, const std::string& id_token);
// 只保留查询、刷新与展示所需字段，并把 id_token 中的展示信息提取到顶层，
// 避免两个长 JWT 同时写入 NVS 单值而超过约 4KB 的限制。
std::string CompactCodexAuthJson(const std::string& auth_json);

// ---------- 数据源适配（返回 200 成功；HTTP 状态码原样；-1 网络；-2 解析/业务失败） ----------
int DirectFetchUsage(const std::string& access_token, const std::string& account_id,
                     AccountDetail& acc, const Socks5Config* proxy);
void FetchOfficialExtras(const std::string& access_token, const std::string& account_id,
                         AccountDetail& acc, const Socks5Config* proxy);
int FetchZhipuUsage(const std::string& api_key, AccountDetail& acc, const Socks5Config* proxy);
// 本机统计：成功时 locals 每台 PC 追加一个携带全量数据的条目；name_suffix 用于多 PC 区分
int FetchLocalUsage(const std::string& host, int port, const std::string& key,
                    const std::string& name_suffix, std::vector<AccountDetail>& locals);

// OAuth 刷新（codex 官方端点）：轮换令牌并返回紧凑 auth_json，
// 返回 {新 access_token, 更新并序列化后的 auth_json}，NVS 回写由调用方完成
std::pair<std::string, std::string> RefreshCodexToken(const std::string& auth_json,
                                                      const Socks5Config* proxy);

}  // namespace usage
