#include "wifi_handler.h"
#include "oled.h"

/**
 * @brief 连接 WiFi（开放网络，无密码）
 * @return true=成功, false=超时失败
 */
bool setupWiFi(void)
{
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          /* 关闭省电, 保证 2 秒周期的上报延迟稳定 */
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi");
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500);
    Serial.print(".");
    retry++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to WiFi");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("\nFailed to connect to WiFi");
    oled_showText("WiFi Failed");
    return false;
  }
}

bool wifiEnsureConnected(void)
{
  if (WiFi.status() == WL_CONNECTED) return true;

  /* 每 15 秒重试一次, 不阻塞采样 */
  static uint32_t lastTryMs = 0;
  uint32_t now = millis();
  if (lastTryMs != 0 && (now - lastTryMs) < 15000UL) return false;
  lastTryMs = now;

  Serial.println("[WIFI] 连接已断开, 重新连接...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  return false;
}
