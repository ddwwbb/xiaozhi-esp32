# ATK-DNESP32S3-BOX2 Wi-Fi 板使用说明

本板在原厂基础上增加了返回键（XIO_KEY_Q）与 AI 用量查询功能：设备直连官方接口查询 ChatGPT（`chatgpt.com/backend-api/wham/*`）与智谱 GLM 套餐（`open.bigmodel.cn/api/monitor/usage/quota/limit`）的 token 用量，并可直连局域网 PC 上的 `box2-usage-server` 查看 Claude Code 的用量与费用（按模型分类），不依赖任何中转服务，支持 SOCKS5 / HTTP 代理与 OAuth 令牌自动刷新。

## 一、快速开始

### 1. 获取固件并烧录

从仓库 [Releases](https://github.com/ddwwbb/xiaozhi-esp32/releases/tag/box2-latest) 下载 `xiaozhi-box2-wifi-*.bin`（全合并镜像，含引导/分区表/主程序/资源），一条命令刷机：

```bash
esptool --chip esp32s3 -b 460800 write_flash 0x0 xiaozhi-box2-wifi-*.bin
```

本地编译：`idf.py -DIDF_TARGET=esp32s3 -B build set-target esp32s3 menuconfig build`，板型选 `Alientek ATK-DNESP32S3-BOX2 Wi-Fi`。每次推送到 main / feature 分支，GitHub Actions 会自动编译并更新 Release。

### 2. 首次开机配网

首次开机设备进入待激活状态，**长按 M 键**进入 Wi-Fi 配网模式，连接设备热点（或按提示操作），选择家里 2.4G Wi-Fi 完成配置。激活与服务器地址由小智服务端下发。

## 二、AI 语音助手使用

| 操作 | 方式 |
|---|---|
| 唤醒 | 语音唤醒词"小智小智"，或待机时**短按 M 键**开始对话 |
| 结束/打断 | 对话中再按 M 或按 Q 键打断；说话中松开即送出（单击说话模式） |
| 音量 | L/R 音量键短按 ±，长按静音/最大音量 |
| 关机 | 电池供电时长按 M 键；Type-C 供电时保持常亮 |

对话界面为表情 + 字幕（默认主题），语音识别、大模型回答、TTS 播报均由服务端配置决定。

## 三、AI 用量查询（ChatGPT + 智谱 + 本机统计）

### 按键

| 键 | 短按 | 长按（≥0.8s） |
|---|---|---|
| Q（返回键） | 待机：查询用量 / 面板已显示：关闭面板；对话中：打断返回待机 | 进入 / 退出配置模式 |
| M | 面板显示时：进详情页 / 详情页返回列表；否则：开始/停止对话 | 关机 / 配网（起始状态） |
| L / R | 详情页内：切换账号（循环）；列表页：翻页（每页 3 个账号卡片）；否则音量 ± | 静音 / 最大音量 |

Q 键有效电平无原理图依据（L 键低有效、M 键高有效），上电按空闲电平自动探测；开机瞬间按住 Q 会导致极性学反，重启即恢复。

### 首次配置（长按 Q，浏览器打开屏幕显示的地址）

1. **导入账号令牌**：把 Codex 令牌文件内容（`~/.codex/auth.json`，或同格式导出 json）粘贴导入，需含 `refresh_token`。最多 8 个账号，同账号重复导入覆盖，可单删。
2. **导入智谱令牌**：粘贴智谱开放平台（open.bigmodel.cn）的 API Key（形如 `xxxxxxxxxx.xxxxxxxx`），查询 GLM 套餐（如 GLM Coding Plan）的 token 用量。最多 4 个，同 Key 重复导入覆盖，可单删。
3. **网络代理**（可选）：SOCKS5 或 HTTP（Clash/v2ray 混合端口均可），支持账号密码认证，可一键清除。**代理范围**按目标分别指定：ChatGPT 默认走代理（国内访问官方接口必需），智谱默认直连（国内站，走国外出口的代理反而可能不通）；两者均可改为直连/走代理。本机统计始终明文直连，不经代理。
4. **本机统计**（可选）：PC 上运行 `box2-usage-server` 时填其局域网地址与端口（默认 3939），面板末尾追加"本地CC"卡片，展示 Claude Code 的当日/本周 token 与费用、近 14 天趋势、按模型分类用量与累计统计。地址留空保存即关闭；密钥为服务端 `config.json` 配置 `key` 时填写（仅 ASCII 可见字符，不能含 `& # % " ' < >` 空格）。
5. **自动刷新**：间隔分钟，0 = 关闭（面板 15 秒自动关闭），>0 = 面板常驻并按间隔自动重查。

配置存 NVS 即改即生效，无需重新编译。配置页 5 分钟无操作自动关闭。

### 界面

- **列表页**：**分页显示，每页完整展示 3 张账号卡片**（240x320 屏），导航行显示页码（如 `< 1/2 >`）与按键提示，L/R 翻页（循环）；标题行右侧显示数据更新时间；每账号一张卡片（名字 + 各自 5h 重置倒计时 + 套餐徽章（Pro 紫/Team 黄/Free 灰/Plus 蓝）+ 5h/周剩余进度条）。智谱账号名字显示为 `智谱*xxxxxx`（Key 后 6 位），套餐徽章为官方返回的套餐等级。本机统计卡片名字为"本地CC"（徽章"本地"），数值行显示当日/本周 token 与费用（如 `日 1.2万($0.03) 周 1242万($4.66)`），进度条仅在服务端配置了上限（quota）时出现（tag 为 日/周），右侧倒计时为到当日午夜。进度条颜色：绿 ≥50%、黄 25–50%、红 <25%。查询失败账号名字前有红色 `!`，无数据的窗口行自动隐藏；本机统计服务不可达只影响本地卡片，其余账号正常。
- **详情页**（列表页按 M）：与列表卡片统一样式，逐账号展示。ChatGPT 账号：邮箱 + 套餐徽章、订阅有效期、限额窗口块（5 小时 / 每周 / 代码审查，含重置倒计时）、可用限额重置积分、近 14 天每日 token 柱状图（超峰值 80% 的日子转黄）、连续使用天数与日峰值、累计 token、Token 刷新时间。智谱账号显示与列表相同的内容（名称 + 套餐等级徽章、5h/周窗口块含重置倒计时），无数据的块自动隐藏。本机统计卡片：`Claude Code @ <PC地址>` + "本地"徽章、当天/本周用量块（右侧 token 与费用，配 quota 才有进度条）、近 14 天柱状图、**模型分类**（token 降序 Top4 + 其余汇总，ccswitch 各供应商用量一目了然，未定价模型费用显示 `-`）、连续天数与日峰值、累计 token（含费用）；服务不可达时状态行提示"服务不可达，检查PC端服务"。音量键切换账号，M 键返回。
- 面板显示期间进入对话会自动收起；15/30 秒无操作自动关闭（配置了自动刷新则常驻）。

## 四、数据来源与令牌保活

| 端点 | 用途 |
|---|---|
| `GET chatgpt.com/backend-api/wham/usage` | 5h/每周/代码审查限额窗口、套餐类型 |
| `GET chatgpt.com/backend-api/wham/profiles/me` | 每日 token 用量桶、累计/峰值/连续天数 |
| `GET chatgpt.com/backend-api/wham/rate-limit-reset-credits` | 可用限额重置积分 |
| `GET open.bigmodel.cn/api/monitor/usage/quota/limit` | 智谱 GLM 套餐 5h/每周窗口已用百分比、重置时间、套餐等级 |
| `GET http://<PC局域网IP>:3939/usage` | 本机 Claude Code 用量（明文 HTTP，不经代理） |

令牌保活：access_token 过期或收到 401 时自动走 `auth.openai.com/oauth/token`（codex 官方 client_id）刷新，轮换后的新令牌立即回写 NVS。**OpenAI refresh_token 为轮换制：同一账号不要同时在其他工具（CPA/Codex CLI）上保活，会互相失效**；令牌失效时详情页标红提示重新导入。

智谱查询：API Key 以 `Authorization` 头直连（**不加 Bearer 前缀**），请求与响应形态参照 [cc-switch](https://github.com/farion1231/cc-switch) 的实测；`limits[]` 按 `unit` 区分窗口（3=5 小时，6=每周），`percentage` 为已用百分比。该接口只提供窗口百分比与重置时间，无每日明细与累计 token。

本机统计：PC 端运行 `box2-usage-server`（Node ≥18 零依赖，Windows/macOS 均可），增量解析 `~/.claude/projects/**/*.jsonl`——ccswitch 切换各供应商产生的用量都在这里，按行内 `model` 字段区分模型并按官方单价换算费用。服务端部署与开机自启见其 README。设备端地址只存 PC 的内网 IP 与端口，PC 的 DHCP IP 变化后改配置页即可，无需重刷固件。

TLS 证书走设备证书 bundle 校验，依赖系统时间正确（联网后自动同步）。

## 五、固件更新

- 重新刷机：下载新 Release 固件按快速开始刷写
- 在线 OTA：由小智服务端下发（版本号见固件启动日志）
- 出现异常时：Type-C 供电下重启设备即可恢复，配置（令牌/代理/刷新间隔）保存在设备存储中不会丢失
