#ifndef SYSCLOCK_H
#define SYSCLOCK_H

#include <Arduino.h>

/**
 * @file sysclock.h
 * @brief 系统绝对时间 (由 PC 校准)
 *
 *  ESP32 本身没有 RTC, 因此时间基准由 PC 提供: 每次上报后从响应里的 epoch
 *  换算出「开机秒 → Unix 秒」的偏移量:
 *
 *      epoch = uptime + offset
 *
 *  偏移量只存在 RAM 中 —— 这个关系式只在同一次开机内成立, 重启后累计开机秒
 *  归零, 沿用旧偏移会得到错误时间。未校准前样本不带 ts, PC 端用 up 换算,
 *  结果同样精确。
 */

/* 初始化 (开机时调用): 清空时间基准, 等待 PC 校准 */
void setupSysClock(void);

/* 用 PC 返回的 Unix 时间校准 (由 forwarder 在收到上报响应时调用) */
void clockSyncFromBrowser(uint32_t t0);

/* 本次开机是否已校准 */
bool clockIsValid(void);

/* 当前 Unix 时间(秒), 未校准返回 0 */
uint32_t clockNow(void);

/* uptime → epoch 的偏移 (秒); 未校准为 0 */
uint32_t clockOffset(void);

#endif /* SYSCLOCK_H */
