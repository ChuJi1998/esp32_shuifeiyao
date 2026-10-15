#ifndef SAMPLER_H
#define SAMPLER_H

#include <Arduino.h>

/**
 * @file sampler.h
 * @brief 传感器采样器 (哑设备版)
 *
 *  职责只有一个: 按固定节奏读一遍 7 路 Modbus 传感器, 保存"最近一次快照"。
 *  不做历史缓冲、不做聚合、不对外提供 HTTP —— 历史与页面全部交给 PC 端。
 *
 *  采样值无效时以 NAN 记录, 上报时序列化为 null, 让 PC 端知道"这一路没数据",
 *  而不是把 -1/-999 这类哨兵值当真实值存进数据库。
 */

/* ════════════════════════════════════════════
   通道定义 (顺序 = 上报数组 v[] 的顺序)
   ════════════════════════════════════════════ */
enum SensorChannel {
  CH_WS = 0,   /* 风速    m/s    */
  CH_WD,       /* 风向    °      */
  CH_AT,       /* 空气温度 °C     */
  CH_AH,       /* 空气湿度 %RH    */
  CH_LUX,      /* 光照    lux    */
  CH_SM,       /* 土壤湿度 %      */
  CH_ST,       /* 土壤温度 °C     */
  CH_SEC,      /* 土壤EC  μS/cm  */
  CH_CO2,      /* CO2     ppm    */
  CH_O2,       /* 氧气    %       */
  CH_COUNT
};

/* 通道标识符 (与 PC 端字段名一致, 便于日志对照) */
extern const char *const SAMPLER_CH_ID[CH_COUNT];

/* 一次完整采集的快照 */
struct SensorSnapshot {
  uint32_t t;                /* 采集时刻 (开机秒) */
  float    v[CH_COUNT];      /* 各通道数值, 无效为 NAN */
  int16_t  wdCompass;        /* 风向 16 方位索引, 无效为 -1 */
  uint16_t count;            /* 保留字段, 恒为 1 */
};

/* 采样间隔(秒): 可由 PC 端下发策略修改, 掉电保存 */
#define SAMPLE_INTERVAL_MIN 1
#define SAMPLE_INTERVAL_MAX 3600
#define SAMPLE_INTERVAL_DEF 2

/* ════════════════════════════════════════════
   接口
   ════════════════════════════════════════════ */

/* 初始化: 从 NVS 恢复采样间隔, 并立即采集一次 */
void setupSampler(void);

/* 主循环调用: 到点则采集一次, 返回 true 表示产生了新样本 */
bool samplerTick(void);

/* 最近一次快照 (从未采样返回 NULL) */
const SensorSnapshot *samplerLatest(void);

uint32_t samplerIntervalSec(void);
void samplerSetIntervalSec(uint32_t seconds);

uint32_t samplerUptimeSec(void);

/* 采样统计 (成功次数 / 有效通道占比), 供串口与 OLED 显示 */
void samplerStats(uint32_t *samples, uint8_t *chValid);

#endif /* SAMPLER_H */
