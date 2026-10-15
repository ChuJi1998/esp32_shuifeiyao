# 水肥药一体机 · PC 侧服务

把"常开小服务器"当成整个系统的大脑：ESP32 只负责采集和上报，页面、存储、规则、
AI 全部跑在这台 PC 上。**纯 Python 标准库实现，不需要 pip 安装任何依赖**
（Windows / Linux / macOS / 树莓派都行，Python ≥ 3.8）。

```
ESP32(哑设备) ──HTTP POST /api/ingest──▶  PC 服务 ──▶ SQLite
      ▲                                     │
      └──────── 响应里带回命令/策略 ─────────┘
                                  浏览器/手机 ──▶ 网页仪表盘
```

## 1. 快速开始

```powershell
cd server
python app.py                  # 默认 0.0.0.0:8080
python app.py --port 9000 --verbose
```

浏览器打开 `http://localhost:8080/`（同一局域网内其它设备用 `http://<PC的IP>:8080/`）。
首次运行会自动创建 `server/data/agri.db`。

**没有硬件也能先体验**——另开一个窗口跑模拟设备：

```powershell
python sim_device.py --interval 2 \
       --scenario normal        # normal / dry / disease / frost / sensor_fault
       --linkdrop 120           # 每 120 秒模拟一次 8 秒断链
```

## 2. 完全离线（推荐）

页面默认引用本地 `static/vendor/` 里的 Tailwind / Font Awesome / Chart.js：

```powershell
python get_vendor.py            # 下载一次即可, 之后不再需要外网
```

`static/vendor/` 已加入 `.gitignore`（体积约 1.5 MB，随用随下）。

## 3. 命令行参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--host` | `0.0.0.0` | 监听地址；只给本机用可设 `127.0.0.1` |
| `--port` | `8080` | 端口 |
| `--db` | `server/data/agri.db` | SQLite 路径 |
| `--keep-days` | `90` | 读数保留天数，启动时及之后每小时自动清理；`0` 表示永久保留 |
| `--verbose` | 关 | 打印每个请求的访问日志 |

环境变量：

| 变量 | 说明 |
|---|---|
| `DEEPSEEK_API_KEY` | 配置后 `/api/ai` 会真正调用 DeepSeek；不配则返回"将要发送的 prompt"（预览模式） |
| `DEEPSEEK_MODEL` | 默认 `deepseek-chat` |
| `AGRI_RETENTION_DAYS` | 页面上显示的"理论容量"天数，默认 `30`（仅影响显示） |

## 4. 设备上报协议

设备每 `sampleMs` 毫秒发一次请求，服务端把待执行命令塞在响应里回带，**设备不需要轮询**：

```http
POST /api/ingest
{
  "device": "esp32-a1b2c3",
  "fw": "0.5.0",
  "up": 12345,                       // 设备开机秒数
  "sampleMs": 2000,                  // 当前采样周期
  "samples": [                       // 断链恢复后这里会有多条(旧的在前)
    { "up": 12345, "ts": 1792051002, "v": [2.5,210,24,62,12000,45,21,450,620,20.9] }
  ],
  "relays": [0,0,1,0,0,0,0,0,0,0,0,0],   // 12 路, 0=停 1=开 2=闪烁
  "ack": [17, 18]                    // 已执行的命令 id, 确认后不再重复下发
}
```

`ts`（绝对 Unix 秒）可选：设备从断链缓冲补传时会带上，服务端优先采用；
没有 `ts` 时按 `接收时刻 − (up_newest − up)` 换算。同一 `(device, ts)` 只保留一条，
所以补传重发、设备重启导致的重复上报都不会产生重复数据点。

`v` 数组顺序（缺失项用 `null`，不要用 `-1` 混淆真实值）：

| # | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|---|
| 通道 | ws | wd | at | ah | lux | sm | st | sec | co2 | o2 |
| 含义 | 风速 m/s | 风向 ° | 气温 °C | 湿度 % | 光照 lux | 土湿 % | 土温 °C | 土壤EC μS/cm | CO₂ ppm | 氧气 % |

响应：

```json
{
  "ok": true,
  "epoch": 1792051002,
  "commands": [ { "id": 17, "relay": 3, "action": "on" } ],
  "policy": { "maxOn": 600, "onLinkLoss": "keep", "linkTimeout": 30, "sampleMs": 2000 }
}
```

时间戳由 PC 端打，设备不需要 RTC 或对时（这是哑设备方案的关键好处之一）。

### 断链保护策略

| 字段 | 默认 | 含义 |
|---|---|---|
| `maxOn` | `0` | 单路连续通电上限（秒），`0` = 不限制（纯透传） |
| `onLinkLoss` | `keep` | 断链时继电器保持 `keep` 或全部 `off` |
| `linkTimeout` | `0` | 多少秒没收到命令就算断链，`0` = 关闭保护 |
| `sampleMs` | `2000` | 采样/上报周期 |

默认值是"什么都不管"的纯透传模式，需要无人值守安全时再通过页面或
`POST /api/policy` 打开。

## 5. HTTP 接口

设备/脚本用：

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/ingest` | 设备上报（见上） |
| GET | `/health` | 服务与数据概况 |

页面用（形状与固件版 API 一致，所以两套前端可以共用一套 `app.js`）：

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/sensors` | 最新读数，含 `rly[12]`、`age`、`online`、`wdn` 风向名 |
| GET | `/api/status` | 设备在线状态与继电器状态 |
| GET | `/api/on` `\|` `/api/off` `\|` `/api/blink` | `?id=1..12` 下发命令（入队，设备下次上报时取走） |
| GET | `/api/history` | `?src=ram\|flash&res=raw\|min&span=秒&points=N&ch=通道` |
| GET | `/api/history/meta` | 缓冲/库容量与采样间隔 |
| GET | `/api/history.csv` | CSV 导出（含 `datetime` 列） |
| GET | `/api/history/clear`、`/api/history/log/clear` | 清空读数 |
| GET | `/api/history/config?interval=秒` | 设置采样间隔（下发给设备） |
| GET | `/api/history/log/config?interval=秒` | 同上（页面持久化面板用） |
| GET | `/api/rules` | 规则引擎诊断结果 |
| GET | `/api/devices` | 设备列表（`online` / `age` / `fw`） |
| GET | `/api/commands?limit=N` | 最近命令及投递/确认状态 |
| GET/POST | `/api/policy` | 读取/设置断链保护策略 |
| GET | `/api/time` | 返回 PC 的 epoch（设备对时可选用） |
| GET | `/api/ai/config` | AI 是否已配置、当前模型 |
| POST | `/api/ai` | `{"question":"..."}`；上下文由服务端自行组装 |

## 6. 页面

四个页签：**实时数据 / 历史趋势 / 水肥控制 / AI农艺助手**。

- 历史趋势：数据源（实时采样 / 历史库）、时间范围、原始/分钟聚合、多通道归一化对比、
  单通道网格、统计表、CSV 导出、平滑与派生指标（露点 / VPD）、历史库状态卡。
- AI 农艺助手：顶部服务状态卡（设备 / 规则 / 模型 / 历史库），规则引擎诊断结果
  （按严重程度分级、关键指标徽章、VPD 与露点），以及对话区（快捷问题、Enter 发送、
  对话本地保存）。规则引擎是确定性的本地计算，**没有网络也能用**；对话才会调用云端模型。

## 7. 农艺规则引擎

`rules.py` 是设备侧 `agri_rules.cpp` 的 Python 版，输出结构完全一致，共 20 条规则：
土壤缺水/过湿/水分快速下降、EC 偏高/严重偏高/偏低、高湿病害风险、空气干燥、
低温冻害/偏冷/高温胁迫、土温偏低、VPD 过高/过低、风速过大、适宜喷药窗口、
CO₂ 偏高、氧气偏低、传感器数据缺失率、设备掉线。

每条规则输出 `{id, level, title, detail, action}`，`level` 为 `0`(正常) `1`(提示)
`2`(需要关注) `3`(警报)；整体结论取最高级别。

## 8. 数据与运维

- **备份**：直接拷贝 `server/data/agri.db`（SQLite 单文件；服务运行时拷贝也安全）。
- **容量**：2 秒采样约每天 4.3 万条、每年 10 MB 量级，普通小主机放几年没问题。
- **开机自启（Windows）**：任务计划程序 → 新建任务 → 触发器"登录时" →
  操作 `python`，参数 `app.py --port 8080`，起始于 `...\esp32-s3-shui_fei_yao\server`。
- **防火墙**：首次运行放行 8080 端口的"专用网络"即可，设备才能上报。
- **安全**：服务本身**没有鉴权**，只应运行在可信内网，不要映射到公网；
  `DEEPSEEK_API_KEY` 只存在于这台机器的环境变量里，不下发到设备，也不会出现在网页里。

## 9. 文件说明

| 文件 | 作用 |
|---|---|
| `app.py` | HTTP 服务：上报接收、命令回带、历史/规则/AI 接口、静态托管 |
| `store.py` | SQLite 存储层：读数、设备、继电器状态、命令队列、策略 |
| `rules.py` | 农艺规则引擎（与设备侧同构） |
| `sim_device.py` | 模拟 ESP32，无硬件联调用 |
| `get_vendor.py` | 下载前端依赖到 `static/vendor/` 实现离线 |
| `static/` | 前端页面（从固件 `data/` 迁移而来） |

## 10. 配套的设备侧固件

固件已经按哑设备改造完成（`lib/Haredware/forwarder.*` + `sampler.*`），设备只做三件事：

1. 每个采样周期 `POST /api/ingest`（明文 HTTP，不用 TLS，省 ~35KB 堆）；
2. 解析响应里的 `commands`，驱动 12 路继电器，并在下次上报的 `ack` 里确认；
3. 缓存并执行策略 `maxOn` / `onLinkLoss` / `linkTimeout` / `sampleMs` 做本地兜底。

断链时样本进 LittleFS 补传缓冲（前 10 分钟逐条，之后每 15 条留 1 条），
恢复后每次上报顺带补传最多 40 条，带绝对时间戳，PC 端按 `(device, ts)` 去重。

设备地址在串口里配置（掉电保存在 NVS）：

```
host 192.168.1.100 8080     # 指向本服务
show                        # 查看状态
```

| 项目 | 数值 |
| --- | --- |
| 静态 RAM | 46,404 B（14.2%）—— 改造前 132,200 B（40.3%） |
| Flash | 857,169 B（25.6%） |
| 补传缓冲容量 | 47,104 条（≈26 小时逐条；稀疏模式下可覆盖约 16 天） |
| 单个请求体积 | ≈300 B（正常）/ ≈3.7 KB（含 40 条补传） |
