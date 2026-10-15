
#include <Arduino.h>
#include <Wire.h>
#include <LittleFS.h>
#include "Adafruit_SSD1306.h"
#include "HardwareSerial.h"

#include "relay.h"
#include "oled.h"
#include "wifi_handler.h"
#include "sensor.h"
#include "sampler.h"
#include "flash_log.h"
#include "sysclock.h"
#include "forwarder.h"

/**
 * @file main.cpp
 * @brief ESP32-S3 水肥药一体机 —— 采集上报端 (哑设备)
 *
 *  设备只负责:
 *    1. 按周期读 7 路 RS485 传感器;
 *    2. 把快照 POST 给常开 PC 上的 server/app.py;
 *    3. 执行 PC 回传的继电器命令与策略, 断链时先本地缓冲再补传。
 *
 *  页面、历史存储、规则引擎、AI 助手全部跑在 PC 上, 详见 server/README.md。
 */

/**
 * @brief 打印一次采样结果到串口
 * @param s 采样快照 (无效通道以 NAN 标记)
 */
static void printSnapshot(const SensorSnapshot *s)
{
  if (!s) return;

  Serial.println("======== 采样结果 ========");

  /* 1# 风速 */
  if (!isnan(s->v[CH_WS]))
    Serial.printf("  [风速]   %.1f m/s\n", s->v[CH_WS]);
  else
    Serial.println("  [风速]   读取失败");

  /* 2# 风向 (角度 + 16方位) */
  if (s->wdCompass >= 0)
    Serial.printf("  [风向]   %.1f° (%s)\n", s->v[CH_WD], windDirName(s->wdCompass));
  else
    Serial.println("  [风向]   读取失败");

  /* 3# 空气温湿度 */
  if (!isnan(s->v[CH_AT]) && !isnan(s->v[CH_AH]))
    Serial.printf("  [空气]   温度: %.1f°C  湿度: %.1f%%RH\n", s->v[CH_AT], s->v[CH_AH]);
  else
    Serial.println("  [空气]   读取失败");

  /* 4# 光照 */
  if (!isnan(s->v[CH_LUX]))
    Serial.printf("  [光照]   %.0f lux\n", s->v[CH_LUX]);
  else
    Serial.println("  [光照]   读取失败");

  /* 5# 土壤 */
  if (!isnan(s->v[CH_SM]) && !isnan(s->v[CH_ST]) && !isnan(s->v[CH_SEC]))
    Serial.printf("  [土壤]   湿度: %.1f%%  温度: %.1f°C  EC: %.0f μS/cm\n",
                  s->v[CH_SM], s->v[CH_ST], s->v[CH_SEC]);
  else
    Serial.println("  [土壤]   读取失败");

  /* 6# CO2 */
  if (!isnan(s->v[CH_CO2]))
    Serial.printf("  [CO2]    %d ppm\n", (int)s->v[CH_CO2]);
  else
    Serial.println("  [CO2]    读取失败");

  /* 7# 氧气 */
  if (!isnan(s->v[CH_O2]))
    Serial.printf("  [O2]     %.1f %%\n", s->v[CH_O2]);
  else
    Serial.println("  [O2]     读取失败");

  Serial.println("============================\n");
}

void setup(void)
{
  Serial.begin(115200);
  delay(500);  /* 等待串口稳定 */

  Serial.println("\n========================================");
  Serial.println("  ESP32-S3 采集上报端启动中...");
  Serial.println("  模式: 哑设备 (数据与逻辑都在 PC 端)");
  Serial.println("========================================\n");

  setup_SensorRS485();  /* 传感器RS485: UART2, GPIO4(TX)/GPIO5(RX) */

  /* 启动时扫描一次RS485总线, 确认所有传感器地址 */
  scanModbusDevices();

  setup_RS485();        /* 继电器RS485: UART1, GPIO16(TX)/GPIO17(RX) */
  setup_OLED();
  setupSysClock();      /* 时间基准: 首次上报后由 PC 校准 (存 NVS) */
  setupSampler();       /* 采样器: 完成首次采样 */

  /* 文件系统: 断链补传缓冲区使用 */
  if (!LittleFS.begin(true))
    Serial.println("[MAIN] LittleFS 挂载失败 (断链补传将不可用)");

  setupFlashLog();      /* 补传缓冲: 扫描分段、建立索引 */
  setupWiFi();
  setupForwarder();     /* 上报模块: 读 PC 地址, 生成设备 ID */

  Serial.println("\n[MAIN] 初始化完成, 开始上报");
  Serial.println("========================================\n");
}

void loop(void)
{
  wifiEnsureConnected();   /* 掉线自动重连 (非阻塞) */
  forwarderSerialTask();   /* 串口命令: host / interval / on / off / show */

  /* ── 到达采样时刻: 采集全部传感器 → 打印到串口 ── */
  if (samplerTick()) {
    printSnapshot(samplerLatest());
  }

  /* ── 有新样本则上报 PC, 并执行本地策略兜底 ── */
  forwarderTick();
}
