# ATK-DNESP32S3-BOX2 Wi-Fi 板附加功能说明

本板在原厂基础上增加了返回键（XIO_KEY_Q）与 ChatGPT 用量查询功能。设备直连 OpenAI 官方接口（`chatgpt.com/backend-api/wham/*`），不依赖任何中转服务；支持 SOCKS5 / HTTP 代理与 OAuth 令牌自动刷新。

## 按键

| 键 | 短按 | 长按（≥0.8s） |
|---|---|---|
| Q（返回键，XL9555 扩展器） | 待机：查询用量 / 面板已显示：关闭面板；对话中：打断返回待机 | 进入 / 退出配置模式 |
| M | 面板显示时：进详情页 / 详情页返回列表；否则：开始/停止对话 | 电池供电时关机；开机起始状态进配网 |
| L / R（音量键） | 详情页内：切换账号（循环）；否则音量 ± | 静音 / 最大音量 |

Q 键有效电平无原理图依据（L 键低有效、M 键高有效），上电时按空闲电平自动探测；若开机瞬间按住 Q 键会导致极性学反，重启即恢复。

## 界面

- **列表页**：标题行右侧显示数据更新时间；每账号一张卡片（名字 + 各自 5h 重置倒计时 + 套餐徽章（Pro 紫/Team 黄/Free 灰/Plus 蓝）+ 5h/周剩余进度条）。进度条颜色：绿 ≥50%、黄 25–50%、红 <25%。查询失败账号名字前有红色 `!`，失败窗口行自动隐藏。
- **详情页**（列表页按 M）：邮箱 + 套餐徽章、订阅有效期、限额窗口块（5 小时 / 每周 / 代码审查，无数据的自动隐藏，含重置倒计时）、可用限额重置积分、近 14 天每日 token 柱状图（超峰值 80% 的日子转黄）、连续使用天数与日峰值、累计 token、Token 刷新时间。音量键切换账号，M 键返回。
- 面板显示期间设备进入对话会自动收起；15/30 秒无操作自动关闭（配置了自动刷新则常驻）。

## 配置（长按 Q，浏览器打开屏幕显示的地址）

1. **直连账号**：粘贴 Codex 令牌文件（`~/.codex/auth.json`，或从其他来源导出的同格式 json）导入，需含 `refresh_token`。最多 8 个账号，同 account_id 重复导入覆盖，可单删。
2. **网络代理**：SOCKS5 或 HTTP（Clash/v2ray 混合端口均支持），含账号密码认证；可一键清除。
3. **自动刷新**：间隔分钟，0 = 关闭（面板 15 秒自动关闭），>0 = 面板常驻并按间隔自动重查。

配置页 5 分钟无操作自动关闭。所有配置存 NVS，即改即生效，无需重新编译。

## 数据来源与令牌保活

| 端点（chatgpt.com） | 用途 |
|---|---|
| `GET /backend-api/wham/usage` | 5h/每周/代码审查限额窗口、套餐类型 |
| `GET /backend-api/wham/profiles/me` | 每日 token 用量桶、累计/峰值/连续天数 |
| `GET /backend-api/wham/rate-limit-reset-credits` | 可用限额重置积分 |

令牌保活：access_token 过期或收到 401 时自动走 `auth.openai.com/oauth/token`（codex 官方 client_id）刷新，轮换后的新令牌立即回写 NVS。**注意 OpenAI refresh_token 为轮换制：同一账号不要同时在其他工具（CPA/Codex CLI）上保活，会互相失效；令牌失效时详情页标红提示重新导入。**

TLS 证书走设备证书 bundle 校验，依赖系统时间正确（联网后自动同步）。

## 构建

```
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;build_box2.defaults" \
       -DIDF_TARGET=esp32s3 -B build_box2 build
```

或使用 ESP-IDF 6.0.x 正常流程选择 `CONFIG_BOARD_TYPE_ATK_DNESP32S3_BOX2_WIFI`。
