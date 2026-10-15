#ifndef WIFI_HANDLER_H
#define WIFI_HANDLER_H

#include <Arduino.h>
#include <WiFi.h>

/**
 * @file wifi_handler.h
 * @brief WiFi 连接管理模块
 */

/* ── WiFi 配置 ── */
#define WIFI_SSID "H410"
#define WIFI_PASSWORD "H410410@@"

/**
 * @brief 连接 WiFi 并显示信息到 OLED
 * @return true=连接成功, false=连接失败
 */
bool setupWiFi(void);

/**
 * @brief 非阻塞检查 WiFi, 掉线时自动重连 (主循环里调用)
 * @return true=当前已连接
 */
bool wifiEnsureConnected(void);

#endif /* WIFI_HANDLER_H */
