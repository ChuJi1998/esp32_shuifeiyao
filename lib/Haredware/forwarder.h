#ifndef FORWARDER_H
#define FORWARDER_H

#include <Arduino.h>
#include "sampler.h"

/**
 * @file forwarder.h
 * @brief 上报与命令执行 (哑设备核心)
 *
 *  设备只做三件事:
 *    1. 每个采样周期把最新快照 POST 到 PC 的 /api/ingest;
 *    2. 从响应里取出 commands 驱动 12 路继电器, 并在下次上报时 ack;
 *    3. 从响应里取出 policy (采样周期 / 最长通电 / 断链保护) 本地兜底执行。
 *
 *  断链时样本进 LittleFS 补传缓冲 (前 10 分钟逐条保留, 之后每 15 条留 1 条),
 *  恢复后按批补传, 补完即清空。
 *  PC 地址 / 端口保存在 NVS, 可在串口里用 host 命令修改。
 */

/* 默认上报地址 (首次烧录后可用串口 host 命令改成你的 PC 地址) */
#define FWD_DEFAULT_HOST "192.168.1.100"
#define FWD_DEFAULT_PORT 8080

#define FWD_HTTP_TIMEOUT_MS 4000   /* 单次上报超时(ms), 超过即认为断链 */
#define FWD_DRAIN_MAX       40     /* 单次上报最多补传的旧样本条数 */
#define FWD_ACK_MAX         12     /* 单次最多回执的命令数 */

/* ── 运行状态 (供 OLED 与串口显示) ── */
struct ForwarderStatus {
  bool     wifi;          /* WiFi 已连接 */
  bool     pcOk;          /* 最近一次上报是否成功 */
  bool     everOk;        /* 是否成功上报过 (用于判断是否真的断链) */
  uint32_t okCount;       /* 累计成功次数 */
  uint32_t failCount;     /* 累计失败次数 */
  uint32_t lastOkAgo;     /* 距上次成功的秒数, 0xFFFFFFFF = 从未成功 */
  uint32_t latencyMs;     /* 最近一次往返耗时 */
  uint32_t backlog;       /* 待补传条数 */
  uint32_t drainTotal;    /* 累计补传条数 */
  int      lastCh;        /* 最近执行的命令通道, 0 = 无 */
  char     lastAction[8]; /* 最近执行的命令动作 */
  int      rssi;          /* WiFi 信号强度 dBm */
  char     url[48];       /* 上报地址 host:port */
};

/* ════════════════════════════════════════════
   生命周期
   ════════════════════════════════════════════ */

/* 初始化: 读 NVS 配置 (PC 地址 / 端口), 生成设备 ID */
void setupForwarder(void);

/* 主循环调用: 有新样本则上报, 并执行本地策略兜底 */
void forwarderTick(void);

/* 主循环调用: 处理串口命令 (host/show/on/off/blink/clear/interval) */
void forwarderSerialTask(void);

/* ════════════════════════════════════════════
   状态与控制
   ════════════════════════════════════════════ */

void forwarderStatus(ForwarderStatus *out);

/* 12 路继电器状态: 0=停 1=开 2=频闪 */
const uint8_t *forwarderRelays(void);

/* 修改上报地址 (掉电保存) */
void forwarderSetHost(const char *host, uint16_t port);

/* 设备 ID (由 MAC 生成, 稳定且唯一) */
const char *forwarderDeviceId(void);

#endif /* FORWARDER_H */
