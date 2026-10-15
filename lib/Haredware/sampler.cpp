#include "sampler.h"
#include "sensor.h"

#include <Preferences.h>
#include <math.h>

/**
 * @file sampler.cpp
 * @brief 传感器采样器实现 (见 sampler.h)
 */

const char *const SAMPLER_CH_ID[CH_COUNT] = {
  "ws", "wd", "at", "ah", "lux", "sm", "st", "sec", "co2", "o2"
};

/* ── 状态 ── */
static SensorSnapshot latest;
static bool     latestValid = false;

static uint32_t intervalSec = SAMPLE_INTERVAL_DEF;
static uint32_t lastSampleMs = 0;
static bool     pendingSample = true;      /* 首次调用立即采样 */

static uint32_t statSamples = 0;
static uint32_t statChOk = 0;
static uint32_t statChAll = 0;

static Preferences prefs;

uint32_t samplerUptimeSec(void) { return millis() / 1000UL; }

/* ════════════════════════════════════════════
   采集
   ════════════════════════════════════════════ */

static void snapshotRead(SensorSnapshot *s)
{
  s->t = samplerUptimeSec();
  s->count = 1;
  s->wdCompass = -1;

  /* 1# 风速 */
  float ws = readWindSpeed();
  s->v[CH_WS] = (ws >= 0) ? ws : NAN;

  /* 2# 风向 */
  WindDirData wd;
  if (readWindDirData(&wd)) {
    s->v[CH_WD] = wd.angle;
    s->wdCompass = (int16_t)wd.compass;
  } else {
    s->v[CH_WD] = NAN;
  }

  /* 3# 空气温湿度 */
  float at = readAirTemp();
  float ah = readAirHumidity();
  s->v[CH_AT] = (at > -900) ? at : NAN;
  s->v[CH_AH] = (ah >= 0) ? ah : NAN;

  /* 4# 光照 */
  float lux = readLight();
  s->v[CH_LUX] = (lux >= 0) ? lux : NAN;

  /* 5# 土壤三合一 */
  SoilData soil;
  if (readSoilData(&soil)) {
    s->v[CH_SM]  = soil.moisture;
    s->v[CH_ST]  = soil.temp;
    s->v[CH_SEC] = soil.ec;
  } else {
    s->v[CH_SM] = s->v[CH_ST] = s->v[CH_SEC] = NAN;
  }

  /* 6# CO2 */
  int co2 = readCO2();
  s->v[CH_CO2] = (co2 >= 0) ? (float)co2 : NAN;

  /* 7# 氧气 */
  float o2 = readOxygen();
  s->v[CH_O2] = (o2 >= 0) ? o2 : NAN;
}

/* ════════════════════════════════════════════
   初始化 / 周期任务
   ════════════════════════════════════════════ */

void setupSampler(void)
{
  prefs.begin("smp", false);
  uint32_t sec = prefs.getUInt("itv", SAMPLE_INTERVAL_DEF);
  prefs.end();

  if (sec < SAMPLE_INTERVAL_MIN || sec > SAMPLE_INTERVAL_MAX) sec = SAMPLE_INTERVAL_DEF;
  intervalSec = sec;

  Serial.printf("[SMPL] 采样器就绪: 间隔 %lus (%u 通道)\n",
                (unsigned long)intervalSec, (unsigned)CH_COUNT);

  samplerTick();      /* 立即采一次, 让首次上报就有数据 */
}

bool samplerTick(void)
{
  uint32_t nowMs = millis();

  if (!pendingSample && (nowMs - lastSampleMs) < intervalSec * 1000UL) return false;

  pendingSample = false;
  lastSampleMs = nowMs;

  SensorSnapshot s;
  snapshotRead(&s);

  latest = s;
  latestValid = true;

  uint8_t ok = 0;
  for (int c = 0; c < CH_COUNT; c++) {
    if (!isnan(s.v[c])) ok++;
  }
  statSamples++;
  statChOk += ok;
  statChAll += CH_COUNT;

  return true;
}

/* ════════════════════════════════════════════
   访问接口
   ════════════════════════════════════════════ */

const SensorSnapshot *samplerLatest(void) { return latestValid ? &latest : NULL; }

uint32_t samplerIntervalSec(void) { return intervalSec; }

void samplerSetIntervalSec(uint32_t seconds)
{
  if (seconds < SAMPLE_INTERVAL_MIN) seconds = SAMPLE_INTERVAL_MIN;
  if (seconds > SAMPLE_INTERVAL_MAX) seconds = SAMPLE_INTERVAL_MAX;
  if (seconds == intervalSec) return;

  intervalSec = seconds;
  pendingSample = true;      /* 新间隔立即生效 */

  prefs.begin("smp", false);
  prefs.putUInt("itv", seconds);
  prefs.end();

  Serial.printf("[SMPL] 采样间隔改为 %lu 秒 (PC 下发)\n", (unsigned long)seconds);
}

void samplerStats(uint32_t *samples, uint8_t *chValid)
{
  if (samples) *samples = statSamples;
  if (chValid) *chValid = statChAll ? (uint8_t)(statChOk * 100UL / statChAll) : 0;
}
