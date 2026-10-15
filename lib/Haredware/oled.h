#ifndef OLED_H
#define OLED_H


#include <Arduino.h>
#include <Wire.h>
#include "relay.h"
#include "Adafruit_SSD1306.h"
#include "HardwareSerial.h"

/* OLED显示屏对象（128x64, I2C） */
extern Adafruit_SSD1306 display;

void setup_OLED(void);
void oled_showText(const char *text);

/* 多行状态显示 (1 倍字体, 每行最多 21 字符, 最多 8 行) */
void oled_showLines(const char *const *lines, uint8_t count);



#endif /* OLED_H */
