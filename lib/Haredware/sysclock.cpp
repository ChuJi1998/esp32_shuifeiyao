#include "sysclock.h"

/**
 * @file sysclock.cpp
 * @brief 时间基准 (见 sysclock.h)
 *
 *  偏移量只保存在 RAM 里:  epoch = uptime + clockOff
 *  每次收到 PC 的 /api/ingest 响应都会重新校准, 因此不需要 NVS,
 *  也不会出现"重启后沿用旧偏移得到错误时间"的问题。
 */

/* 合理性下限: 2020-01-01 */
#define CLOCK_MIN_EPOCH 1577836800UL
/* 合理性上限: 2100-01-01 */
#define CLOCK_MAX_EPOCH 4102444800UL

static uint32_t clockOff = 0;      /* epoch = uptime + clockOff */
static bool     clockOk = false;

static uint32_t uptimeSec(void) { return millis() / 1000UL; }

void setupSysClock(void)
{
  clockOff = 0;
  clockOk = false;
  Serial.println("[CLOCK] 时间基准待定, 首次成功上报后由 PC 校准");
  Serial.println("[CLOCK] 未校准前按开机秒记录, PC 端会用 up 换算成绝对时间");
}

void clockSyncFromBrowser(uint32_t t0)
{
  if (t0 < CLOCK_MIN_EPOCH || t0 > CLOCK_MAX_EPOCH) return;

  uint32_t off = t0 - uptimeSec();
  uint32_t diff = (clockOff > off) ? (clockOff - off) : (off - clockOff);

  clockOff = off;
  if (!clockOk || diff >= 3) {
    clockOk = true;
    Serial.printf("[CLOCK] 时间已校准: UTC %lu (偏移 %lu 秒)\n",
                  (unsigned long)t0, (unsigned long)off);
  }
}

bool clockIsValid(void) { return clockOk; }

uint32_t clockOffset(void) { return clockOk ? clockOff : 0; }

uint32_t clockNow(void)
{
  if (!clockOk) return 0;
  return uptimeSec() + clockOff;
}
