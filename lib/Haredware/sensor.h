#ifndef SENSOR_H
#define SENSOR_H

#include <Arduino.h>
#include <HardwareSerial.h>

/**
 * @file sensor.h
 * @brief Modbus RTU 多传感器采集模块 (7路传感器, RS485总线)
 *
 *  设备清单 (共用 UART2: GPIO4/TX, GPIO5/RX, 9600bps):
 *  ┌────────┬──────────┬────────────────┬──────────┐
 *  │ 地址   │ 传感器   │ 寄存器         │ 输出     │
 *  ├────────┼──────────┼────────────────┼──────────┤
 *  │ 0x01   │ 风速     │ 0x0000         │ m/s      │
 *  │ 0x02   │ 风向     │ 0x0000/0x0001  │ 角度/方位 │
 *  │ 0x03   │ 温湿度   │ 0x0000/0x0001  │ °C / %RH │
 *  │ 0x04   │ 光照     │ 0x0000+0x0001  │ lux(32b) │
 *  │ 0x05   │ 土壤     │ 0x0000~0x0002  │ 湿度/温度/EC │
 *  │ 0x06   │ CO2      │ 0x0010         │ ppm      │
 *  │ 0x07   │ 氧气     │ 0x000A         │ % O₂     │
 *  └────────┴──────────┴────────────────┴──────────┘
 */

/* ── 传感器 Modbus 地址 ── */
#define ADDR_WIND_SPEED     0x01    /* 风速传感器 */
#define ADDR_WIND_DIR       0x02    /* 风向传感器 */
#define ADDR_AIR_TH         0x03    /* 空气温湿度传感器 */
#define ADDR_LIGHT          0x04    /* 光照传感器 */
#define ADDR_SOIL           0x05    /* 土壤传感器 */
#define ADDR_CO2            0x06    /* CO2传感器 */
#define ADDR_O2             0x07    /* 氧气传感器 */

/* ── 寄存器地址 ── */
#define REG_WIND_SPEED      0x0000  /* 风速值 (地址0x01) */
#define REG_WIND_DIR        0x0000  /* 风向角度 0-360° (地址0x02) */
#define REG_WIND_COMPASS    0x0001  /* 16方位 0-15 (地址0x02) */
#define REG_AIR_TEMP        0x0000  /* 空气温度 */
#define REG_AIR_HUMI        0x0001  /* 空气湿度 */
#define REG_LIGHT_HI        0x0000  /* 光照高16位 */
#define REG_LIGHT_LO        0x0001  /* 光照低16位 */
#define REG_SOIL_MOIST      0x0000  /* 土壤湿度 */
#define REG_SOIL_TEMP       0x0001  /* 土壤温度 */
#define REG_SOIL_EC         0x0002  /* 土壤电导率 */
#define REG_CO2             0x0010  /* CO2浓度 */
#define REG_O2              0x000A  /* 氧气浓度 */

/* ── RS485 方向控制引脚 ── */
#define RS485_DE_RE_PIN     -1

/* ── Modbus 读取超时(ms) ── */
#define MODBUS_TIMEOUT_MS   200

/* ── 风向传感器数据结构 ── */
struct WindDirData {
  float angle;      /* 风向角度 0-360° */
  int   compass;    /* 16方位索引 0-15 */
  const char *name; /* 方位名称 (N/NNE/.../NNW) */
};

/* ── 土壤传感器数据结构 ── */
struct SoilData {
  float moisture;   /* 土壤湿度 (%) */
  float temp;       /* 土壤温度 (°C) */
  float ec;         /* 电导率 (μS/cm) */
};

/* ── 外部全局传感器RS485串口对象 ── */
extern HardwareSerial SENSOR_SER;

/* 风向 16 方位名称, idx 越界返回 "???" */
const char *windDirName(int idx);

/* ════════════════════════════════════════════
   初始化 & 扫描
   ════════════════════════════════════════════ */
void setup_SensorRS485(void);
void scanModbusDevices(void);

/* ════════════════════════════════════════════
   通用 Modbus RTU 读取
   ════════════════════════════════════════════ */
bool modbusReadHoldingRegister(uint8_t slaveAddr, uint16_t regAddr, uint16_t *value);
bool modbusReadMultipleRegisters(uint8_t slaveAddr, uint16_t startAddr, uint8_t count, uint16_t *values);

/* ════════════════════════════════════════════
   各传感器读取函数
   ════════════════════════════════════════════ */
float readWindSpeed(void);           /* 风速 (m/s), 失败返回 -1 */
bool  readWindDirData(WindDirData *wd); /* 风向(角度+16方位), 失败返回 false */
float readAirTemp(void);             /* 空气温度 (°C), 失败返回 -999 */
float readAirHumidity(void);         /* 空气湿度 (%RH), 失败返回 -1 */
float readLight(void);               /* 光照度 (lux), 失败返回 -1 */
bool  readSoilData(SoilData *data);  /* 土壤三合一, 失败返回 false */
int   readCO2(void);                 /* CO2 (ppm), 失败返回 -1 */
float readOxygen(void);              /* 氧气 (%), 失败返回 -1 */

#endif /* SENSOR_H */
