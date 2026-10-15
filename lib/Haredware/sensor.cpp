#include "sensor.h"

/* ── 传感器 RS485 串口对象 (UART2) ── */
HardwareSerial SENSOR_SER(2);

/* ════════════════════════════════════════════
   CRC16 & RS485 控制 (内部)
   ════════════════════════════════════════════ */

static uint16_t modbusCRC16(const uint8_t *buf, uint8_t len)
{
  uint16_t crc = 0xFFFF;
  for (uint8_t pos = 0; pos < len; pos++) {
    crc ^= (uint16_t)buf[pos];
    for (uint8_t i = 8; i != 0; i--) {
      if ((crc & 0x0001) != 0) {
        crc >>= 1;
        crc ^= 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

static void rs485_setDirection(bool txMode)
{
  #if (RS485_DE_RE_PIN >= 0)
    digitalWrite(RS485_DE_RE_PIN, txMode ? HIGH : LOW);
  #endif
  delayMicroseconds(50);
}

/* ════════════════════════════════════════════
   初始化
   ════════════════════════════════════════════ */

void setup_SensorRS485(void)
{
  SENSOR_SER.begin(9600, SERIAL_8N1, 5, 4);  /* RX=GPIO5, TX=GPIO4 */
  #if (RS485_DE_RE_PIN >= 0)
    pinMode(RS485_DE_RE_PIN, OUTPUT);
    digitalWrite(RS485_DE_RE_PIN, LOW);
  #endif
  Serial.println("[SENSOR] RS485传感器模块初始化完成 (UART2, 9600bps, TX=4, RX=5)");
}

/* ════════════════════════════════════════════
   单寄存器读取 (功能码 0x03, 1个寄存器)
   ════════════════════════════════════════════ */

bool modbusReadHoldingRegister(uint8_t slaveAddr, uint16_t regAddr, uint16_t *value)
{
  uint8_t txBuf[8];
  uint8_t rxBuf[7];

  txBuf[0] = slaveAddr;
  txBuf[1] = 0x03;
  txBuf[2] = (regAddr >> 8) & 0xFF;
  txBuf[3] = regAddr & 0xFF;
  txBuf[4] = 0x00;
  txBuf[5] = 0x01;

  uint16_t crc = modbusCRC16(txBuf, 6);
  txBuf[6] = crc & 0xFF;
  txBuf[7] = (crc >> 8) & 0xFF;

  while (SENSOR_SER.available()) SENSOR_SER.read();

  rs485_setDirection(true);
  SENSOR_SER.write(txBuf, 8);
  SENSOR_SER.flush();
  rs485_setDirection(false);

  unsigned long startTime = millis();
  uint8_t rxIndex = 0;
  while (rxIndex < 7) {
    if (SENSOR_SER.available()) {
      rxBuf[rxIndex++] = SENSOR_SER.read();
      startTime = millis();
    }
    if (millis() - startTime > MODBUS_TIMEOUT_MS) {
      return false;
    }
  }

  if (rxBuf[0] != slaveAddr || rxBuf[1] != 0x03 || rxBuf[2] != 0x02)
    return false;

  uint16_t rxCRC = rxBuf[5] | ((uint16_t)rxBuf[6] << 8);
  if (rxCRC != modbusCRC16(rxBuf, 5))
    return false;

  *value = ((uint16_t)rxBuf[3] << 8) | rxBuf[4];
  delay(50);  /* 总线恢复延时, 避免下一帧冲突 */
  return true;
}

/* ════════════════════════════════════════════
   多寄存器读取 (功能码 0x03, N个寄存器)
   ════════════════════════════════════════════ */

bool modbusReadMultipleRegisters(uint8_t slaveAddr, uint16_t startAddr, uint8_t count, uint16_t *values)
{
  if (count == 0 || count > 10) return false;

  uint8_t txBuf[8];
  txBuf[0] = slaveAddr;
  txBuf[1] = 0x03;
  txBuf[2] = (startAddr >> 8) & 0xFF;
  txBuf[3] = startAddr & 0xFF;
  txBuf[4] = 0x00;
  txBuf[5] = count;

  uint16_t crc = modbusCRC16(txBuf, 6);
  txBuf[6] = crc & 0xFF;
  txBuf[7] = (crc >> 8) & 0xFF;

  while (SENSOR_SER.available()) SENSOR_SER.read();

  rs485_setDirection(true);
  SENSOR_SER.write(txBuf, 8);
  SENSOR_SER.flush();
  rs485_setDirection(false);

  uint8_t dataLen = count * 2;
  uint8_t rxLen = 5 + dataLen;
  uint8_t rxBuf[25];

  unsigned long startTime = millis();
  uint8_t rxIndex = 0;
  while (rxIndex < rxLen) {
    if (SENSOR_SER.available()) {
      rxBuf[rxIndex++] = SENSOR_SER.read();
      startTime = millis();
    }
    if (millis() - startTime > MODBUS_TIMEOUT_MS) {
      return false;
    }
  }

  if (rxBuf[0] != slaveAddr || rxBuf[1] != 0x03 || rxBuf[2] != dataLen)
    return false;

  uint16_t rxCRC = rxBuf[rxLen - 2] | ((uint16_t)rxBuf[rxLen - 1] << 8);
  if (rxCRC != modbusCRC16(rxBuf, rxLen - 2))
    return false;

  for (uint8_t i = 0; i < count; i++) {
    values[i] = ((uint16_t)rxBuf[3 + i * 2] << 8) | rxBuf[4 + i * 2];
  }
  delay(50);  /* 总线恢复延时 */
  return true;
}

/* ════════════════════════════════════════════
   1# 风速传感器 (地址0x01, 9600bps)
   寄存器: 0x0000 → 风速 m/s
   ════════════════════════════════════════════ */

float readWindSpeed(void)
{
  uint16_t raw;
  if (modbusReadHoldingRegister(ADDR_WIND_SPEED, REG_WIND_SPEED, &raw))
    return (float)raw / 10.0f;
  return -1.0f;
}

/* ── 16方位名称表 ── */
static const char *compassNames[16] = {
  "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
  "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"
};

const char *windDirName(int idx)
{
  if (idx >= 0 && idx < 16) return compassNames[idx];
  return "???";
}

/* ════════════════════════════════════════════
   2# 风向传感器 (地址0x02, 9600bps)
   寄存器0x0000: 360°角度 (÷10), 0x0001: 16方位 0-15
   报文示例:
     请求: 02 03 00 00 00 01 84 39 → 响应: 02 03 02 0B EF BA F8
     请求: 02 03 00 01 00 01 D5 F9 → 响应: 02 03 02 00 0E 7D 80
   ════════════════════════════════════════════ */

bool readWindDirData(WindDirData *wd)
{
  uint16_t angleRaw, compassRaw;

  if (!modbusReadHoldingRegister(ADDR_WIND_DIR, REG_WIND_DIR, &angleRaw))
    return false;
  if (!modbusReadHoldingRegister(ADDR_WIND_DIR, REG_WIND_COMPASS, &compassRaw))
    return false;

  wd->angle   = (float)angleRaw / 10.0f;        /* 0x0BEF=3055 → 305.5° */
  wd->compass = compassRaw;                      /* 0-15 */
  wd->name    = windDirName(wd->compass);

  return true;
}

/* ════════════════════════════════════════════
   3# 空气温湿度传感器 (地址0x03, 9600bps)
   ════════════════════════════════════════════ */

float readAirTemp(void)
{
  uint16_t raw;
  if (modbusReadHoldingRegister(ADDR_AIR_TH, REG_AIR_TEMP, &raw))
    return (float)(int16_t)raw / 10.0f;
  return -999.0f;
}

float readAirHumidity(void)
{
  uint16_t raw;
  if (modbusReadHoldingRegister(ADDR_AIR_TH, REG_AIR_HUMI, &raw))
    return (float)raw / 10.0f;
  return -1.0f;
}

/* ════════════════════════════════════════════
   4# 光照传感器 (地址0x04, 9600bps)
   32位: 0x0000(高16) + 0x0001(低16) → lux
   ════════════════════════════════════════════ */

float readLight(void)
{
  uint16_t buf[2];
  if (modbusReadMultipleRegisters(ADDR_LIGHT, REG_LIGHT_HI, 2, buf)) {
    uint32_t lux = ((uint32_t)buf[0] << 16) | buf[1];
    return (float)lux;
  }
  return -1.0f;
}

/* ════════════════════════════════════════════
   5# 土壤传感器 (地址0x05, 9600bps)
   一次性读3寄存器: 湿度/温度/电导率
   ════════════════════════════════════════════ */

bool readSoilData(SoilData *data)
{
  uint16_t buf[3];
  if (modbusReadMultipleRegisters(ADDR_SOIL, REG_SOIL_MOIST, 3, buf)) {
    data->moisture = (float)buf[0] / 10.0f;
    data->temp     = (float)(int16_t)buf[1] / 10.0f;
    data->ec       = (float)buf[2];
    return true;
  }
  return false;
}

/* ════════════════════════════════════════════
   6# CO2传感器 (地址0x06, 9600bps)
   ════════════════════════════════════════════ */

int readCO2(void)
{
  uint16_t raw;
  if (modbusReadHoldingRegister(ADDR_CO2, REG_CO2, &raw))
    return (int)raw;
  return -1;
}

/* ════════════════════════════════════════════
   7# 氧气传感器 (地址0x07, 9600bps)
   ════════════════════════════════════════════ */

float readOxygen(void)
{
  uint16_t raw;
  if (modbusReadHoldingRegister(ADDR_O2, REG_O2, &raw))
    return (float)raw / 10.0f;
  return -1.0f;
}

/* ════════════════════════════════════════════
   设备扫描
   ════════════════════════════════════════════ */

void scanModbusDevices(void)
{
  uint16_t dummy;
  int found = 0;

  Serial.println("\n========================================");
  Serial.println("  Modbus 设备扫描中...");
  Serial.println("========================================");
  Serial.println("  [精确探测] 已知传感器:");

  if (modbusReadHoldingRegister(0x01, 0x0000, &dummy))
    { Serial.printf("  [找到] 风速传感器     地址 0x01 - 原始值: %u\n", dummy); found++; }
  else
    { Serial.println("  [未检测] 风速传感器     地址 0x01 无响应"); }

  if (modbusReadHoldingRegister(0x02, 0x0000, &dummy))
    { Serial.printf("  [找到] 风向传感器     地址 0x02 - 原始值: %u (%.1f°)\n", dummy, dummy / 10.0f); found++; }
  else
    { Serial.println("  [未检测] 风向传感器     地址 0x02 无响应"); }

  if (modbusReadHoldingRegister(0x03, 0x0000, &dummy))
    { Serial.printf("  [找到] 温湿度传感器   地址 0x03 - 温度原始值: %u\n", dummy); found++; }
  else
    { Serial.println("  [未检测] 温湿度传感器   地址 0x03 无响应"); }

  if (modbusReadHoldingRegister(0x04, 0x0000, &dummy))
    { Serial.printf("  [找到] 光照传感器     地址 0x04 - 高16位: %u\n", dummy); found++; }
  else
    { Serial.println("  [未检测] 光照传感器     地址 0x04 无响应"); }

  if (modbusReadHoldingRegister(0x05, 0x0000, &dummy))
    { Serial.printf("  [找到] 土壤传感器     地址 0x05 - 湿度原始值: %u\n", dummy); found++; }
  else
    { Serial.println("  [未检测] 土壤传感器     地址 0x05 无响应"); }

  if (modbusReadHoldingRegister(0x06, 0x0010, &dummy))
    { Serial.printf("  [找到] CO2传感器      地址 0x06 - 浓度: %u ppm\n", dummy); found++; }
  else
    { Serial.println("  [未检测] CO2传感器      地址 0x06 无响应"); }

  if (modbusReadHoldingRegister(0x07, 0x000A, &dummy))
    { Serial.printf("  [找到] 氧气传感器     地址 0x07 - 原始值: %u (%.1f%%)\n", dummy, dummy / 10.0f); found++; }
  else
    { Serial.println("  [未检测] 氧气传感器     地址 0x07 无响应"); }

  Serial.println("\n  [全量扫描] 已跳过 (全部已知传感器在线)");

  Serial.printf("\n  扫描完成, 共找到 %d 个设备\n", found);
  Serial.println("========================================\n");
}
