#include "oled.h"
#include <Arduino.h>
#include <Wire.h>
#include "relay.h"
#include "Adafruit_SSD1306.h"
#include "HardwareSerial.h"

/* OLED显示屏对象（128x64, I2C） */
Adafruit_SSD1306 display(128, 64, &Wire, -1);



void setup_OLED(void)
{
  /* 初始化I2C总线：SDA=GPIO12, SCL=GPIO11 */
  Wire.begin(12, 11);

  /* 初始化SSD1306显示屏，地址0x3C */
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
  {
    Serial.println("[ERROR] SSD1306 OLED初始化失败！");
    while (1)
      ;
  }

  /* 显示启动画面 */
  oled_showText("System Ready");
}

/**
 * @brief OLED显示一行文字（左上角，2倍大小）
 * @param text 要显示的文字字符串
 * @return void
 */
void oled_showText(const char *text)
{
  display.clearDisplay();                 // 清除显示缓冲区
  display.setTextSize(1);                 // 设置文本大小为1倍
  display.setTextColor(SSD1306_WHITE);    // 设置文本颜色为白色
  display.setCursor(0, 0);               // 设置文本起始位置
  display.println(text);                  // 将文本写入显示缓冲区
  display.display();                      // 将缓冲区内容显示到屏幕上
}

/**
 * @brief OLED显示多行文本（1倍字体, 每行 21 字符, 最多 8 行）
 * @param lines 行文本数组
 * @param count 行数
 */
void oled_showLines(const char *const *lines, uint8_t count)
{
  if (!lines || count == 0) return;
  if (count > 8) count = 8;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);

  for (uint8_t i = 0; i < count; i++) {
    display.println(lines[i] ? lines[i] : "");
  }
  display.display();
}
