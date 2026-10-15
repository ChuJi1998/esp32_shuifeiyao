#ifndef FLASH_LOG_H
#define FLASH_LOG_H

#include <Arduino.h>
#include "sampler.h"      /* SensorSnapshot / CH_COUNT */

/**
 * @file flash_log.h
 * @brief 断链补传缓冲 (LittleFS 分段环形日志)
 *
 *  哑设备不存"历史", 只存"没送出去的样本"：
 *    - 每次上报失败 (PC 关机 / 网络不通) 就把这一条样本追加进缓冲;
 *    - 连上以后每次上报顺带补传一批 (从最旧的开始), 补完即清空。
 *
 *  存储结构:
 *    /hl/<seq>.bin = 24 字节段头 + 最多 FLOG_SEG_RECS 条定长记录
 *    记录 24 字节 = 时间戳(uint32) + 10 通道(int16, 见 CH_SCALE 缩放)
 *
 *  环形策略:
 *    - 段写满后新建下一段 (段号单调递增)
 *    - 段数超过 FLOG_MAX_SEGS 或剩余空间不足时, 删除最旧的段
 *    - 段条数以文件长度推算, 因此追加时无需回写头部 (掉电安全)
 *
 *  时间戳:
 *    - 已从 PC 拿到过 epoch 时为 Unix 秒 (段头 flags bit0 = 1)
 *    - 否则为开机秒; 时间基准变化时自动换段, 保证同段内基准一致
 */

#define FLOG_HDR_SIZE     24
#define FLOG_REC_SIZE     24
#define FLOG_SEG_RECS     1024
#define FLOG_MAX_SEGS     46      /* 容量 = 46 × 1024 ≈ 4.7 万条 ≈ 1.08 MB */
#define FLOG_MIN_FREE     40960   /* 剩余空间下限(字节), 低于则删最旧段 (约 10 个 4KB 块) */

#define FLOG_MAX_POINTS   300     /* 单次读取最多返回的点数 */

/* int16 无效值标记 */
#define FLOG_INVALID      (-32768)

/* ── 缓冲状态 ── */
struct FlogInfo {
  bool     ready;        /* 缓冲可用 */
  uint16_t segs;         /* 当前段数 */
  uint32_t records;      /* 已存(待补传)记录数 */
  uint32_t capacity;     /* 最大记录数 */
  uint32_t bytes;        /* 已占用字节 */
  uint32_t oldestTs;     /* 最旧记录时间戳 */
  uint32_t newestTs;     /* 最新记录时间戳 */
  bool     wallClock;    /* 时间戳是否为绝对时间 */
};

/* ════════════════════════════════════════════
   生命周期
   ════════════════════════════════════════════ */

/* 初始化 (需先挂载 LittleFS): 扫描段文件、恢复索引 */
void setupFlashLog(void);

/* 追加一条待补传记录 (上报失败时调用), 返回是否写入成功 */
bool flashLogAppend(const SensorSnapshot *s);

bool flashLogReady(void);
void flashLogInfo(FlogInfo *out);

/* 清空缓冲 (补传完成或串口 clear 命令) */
void flashLogClear(void);

/* ════════════════════════════════════════════
   查询 (补传时从最旧的开始读)
   ════════════════════════════════════════════ */

uint32_t flashLogTotal(void);

/* 返回第一条 ts >= cutoff 的全局下标 (无则返回总数) */
uint32_t flashLogFindFirst(uint32_t cutoffTs);

/* 读取全局下标 idx (0 = 最旧) 的记录 */
bool flashLogRead(uint32_t idx, uint32_t *ts, int16_t *vals);

/* 结束批量读取, 关闭缓存的文件句柄 */
void flashLogCloseRead(void);

/* 解码单个通道值 (无效返回 NAN) */
float flashLogDecode(int ch, int16_t raw);

#endif /* FLASH_LOG_H */
