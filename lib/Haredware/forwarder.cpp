#include "forwarder.h"
#include "relay.h"
#include "oled.h"
#include "flash_log.h"
#include "sysclock.h"
#include "wifi_handler.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>

/**
 * @file forwarder.cpp
 * @brief 上报 + 命令执行 + 断链补传 (见 forwarder.h)
 *
 *  与 PC 端约定的报文格式 (由 server/app.py 的 /api/ingest 定义):
 *
 *    上报: {"device","fw","up","sampleMs",
 *           "samples":[{"up":n[,"ts":epoch],"v":[10 个值或 null]}],
 *           "relays":[12],"ack":[id]}
 *    响应: {"ok","epoch","commands":[{"id","relay","action"}],
 *           "policy":{"maxOn","onLinkLoss","linkTimeout","sampleMs"}}
 *
 *  这里用手写的小解析器读取响应 —— 报文是我们自己的服务端生成的,
 *  结构固定; 省掉一个 JSON 库可以少占约 20KB Flash。
 */

#define FWD_FW_VERSION "0.5.0"

/* ════════════════════════════════════════════
   状态
   ════════════════════════════════════════════ */

static Preferences prefs;
static char     g_host[40];
static uint16_t g_port = FWD_DEFAULT_PORT;
static char     g_dev[24];

/* 上报统计 */
static uint32_t g_lastSentT = 0xFFFFFFFF;    /* 上次上报的样本时刻(开机秒) */
static bool     g_pcOk = false;
static bool     g_everOk = false;
static uint32_t g_okCount = 0;
static uint32_t g_failCount = 0;
static uint32_t g_lastOkMs = 0;
static uint32_t g_latencyMs = 0;
static uint32_t g_drainTotal = 0;
static uint32_t g_drainCursor = 0;
static bool     g_draining = false;
static uint32_t g_sentThisPost = 0;

/* 命令回执 */
static uint16_t g_ack[FWD_ACK_MAX];
static uint8_t  g_ackN = 0;
static int      g_lastCh = 0;
static char     g_lastAction[8] = "-";

/* 继电器本地状态 */
static uint8_t  g_rly[12];
static uint32_t g_onSince[12];

/* 策略 (PC 下发, 本地兜底) */
static uint32_t g_maxOnSec = 0;
static uint32_t g_linkTimeout = 0;
static bool     g_onLinkLossOff = false;
static bool     g_linkCutDone = false;

static uint32_t g_lastOledMs = 0;

/* ════════════════════════════════════════════
   继电器分发
   ════════════════════════════════════════════ */

/* 动作索引: 0=off 1=on 2=blink */
static void (*const RL_ACT[12][3])(void) = {
  { Relay1_off,  Relay1_on,  Relay1_blink  },
  { Relay2_off,  Relay2_on,  Relay2_blink  },
  { Relay3_off,  Relay3_on,  Relay3_blink  },
  { Relay4_off,  Relay4_on,  Relay4_blink  },
  { Relay5_off,  Relay5_on,  Relay5_blink  },
  { Relay6_off,  Relay6_on,  Relay6_blink  },
  { Relay7_off,  Relay7_on,  Relay7_blink  },
  { Relay8_off,  Relay8_on,  Relay8_blink  },
  { Relay9_off,  Relay9_on,  Relay9_blink  },
  { Relay10_off, Relay10_on, Relay10_blink },
  { Relay11_off, Relay11_on, Relay11_blink },
  { Relay12_off, Relay12_on, Relay12_blink },
};

static int actionIndex(const char *action)
{
  if (!action) return -1;
  if (strcmp(action, "off") == 0)   return 0;
  if (strcmp(action, "on") == 0)    return 1;
  if (strcmp(action, "blink") == 0) return 2;
  return -1;
}

static void relayExec(uint8_t ch, const char *action, uint16_t id)
{
  if (ch < 1 || ch > 12) return;
  int ai = actionIndex(action);
  if (ai < 0) return;

  RL_ACT[ch - 1][ai]();

  g_rly[ch - 1] = (uint8_t)ai;
  g_onSince[ch - 1] = (ai == 1) ? millis() : 0;
  g_lastCh = ch;
  strncpy(g_lastAction, action, sizeof(g_lastAction) - 1);
  g_lastAction[sizeof(g_lastAction) - 1] = 0;

  Serial.printf("[FWD] 执行命令 #%u: 通道 %u → %s\n", (unsigned)id, (unsigned)ch, action);
}

static void ackPush(uint16_t id)
{
  if (g_ackN >= FWD_ACK_MAX) return;
  g_ack[g_ackN++] = id;
}

static void allRelaysOff(const char *reason)
{
  Serial.printf("[FWD] %s: 全部通道关闭\n", reason);
  for (uint8_t i = 0; i < 12; i++) {
    if (g_rly[i] != 0) {
      RL_ACT[i][0]();
      g_rly[i] = 0;
      g_onSince[i] = 0;
    }
  }
}

/* ════════════════════════════════════════════
   极简 JSON 解析 (只认服务端生成的固定格式)
   ════════════════════════════════════════════ */

static bool jsonLong(const String &s, const char *key, long *out)
{
  char pat[20];
  snprintf(pat, sizeof(pat), "\"%s\":", key);
  int p = s.indexOf(pat);
  if (p < 0) return false;
  p += (int)strlen(pat);
  while (p < (int)s.length() && s[p] == ' ') p++;
  if (p >= (int)s.length()) return false;
  char *endp = NULL;
  long v = strtol(s.c_str() + p, &endp, 10);
  if (endp == s.c_str() + p) return false;    /* 不是数字 (例如 null) */
  *out = v;
  return true;
}

static bool jsonStr(const String &s, const char *key, char *out, size_t n)
{
  char pat[20];
  snprintf(pat, sizeof(pat), "\"%s\":", key);
  int p = s.indexOf(pat);
  if (p < 0) return false;
  p += (int)strlen(pat);
  while (p < (int)s.length() && s[p] == ' ') p++;
  if (p >= (int)s.length() || s[p] != '"') return false;

  p++;
  int e = s.indexOf('"', p);
  if (e < 0) return false;
  size_t len = (size_t)(e - p);
  if (len >= n) len = n - 1;
  memcpy(out, s.c_str() + p, len);
  out[len] = 0;
  return true;
}

/* ════════════════════════════════════════════
   JSON 组装
   ════════════════════════════════════════════ */

static void appendValue(String &j, float v, uint8_t ch)
{
  if (isnan(v)) { j += "null"; return; }
  switch (ch) {
    case CH_SEC: j += String((long)lroundf(v)); break;      /* μS/cm 整数 */
    case CH_CO2: j += String((long)lroundf(v)); break;      /* ppm 整数 */
    case CH_LUX: j += String((long)lroundf(v)); break;      /* lux 整数 */
    default:     j += String(v, 1);            break;       /* 一位小数 */
  }
}

static void appendSample(String &j, const SensorSnapshot *s, bool withTs, uint32_t ts)
{
  j += "{\"up\":";
  j += String(s->t);
  if (withTs) {
    j += ",\"ts\":";
    j += String(ts);
  }
  j += ",\"v\":[";
  for (int c = 0; c < CH_COUNT; c++) {
    if (c) j += ",";
    appendValue(j, s->v[c], (uint8_t)c);
  }
  j += "]}";
}

/* 从补传缓冲取最多 count 条旧样本追加到 samples 数组, 返回实际条数 */
static uint32_t appendBacklog(String &j, uint32_t count, bool *first)
{
  if (!flashLogReady()) return 0;

  uint32_t total = flashLogTotal();
  if (total == 0) {
    g_draining = false;
    g_drainCursor = 0;
    return 0;
  }

  if (!g_draining) {
    g_draining = true;
    g_drainCursor = 0;
  }

  FlogInfo info;
  flashLogInfo(&info);
  bool wall = info.wallClock;      /* 缓冲里存的是绝对时间还是开机秒 */

  int16_t vals[CH_COUNT];
  uint32_t ts = 0;
  uint32_t sent = 0;
  uint32_t skipped = 0;

  while (sent < count && g_drainCursor < total) {
    if (!flashLogRead(g_drainCursor, &ts, vals)) break;

    /* 未校准时记录的是开机秒; 若大于当前开机秒, 说明这条记录来自上一次开机,
       时间基准已经失效, 跳过而不是伪造时间戳 */
    if (!wall && ts > samplerUptimeSec()) {
      g_drainCursor++;
      skipped++;
      continue;
    }

    SensorSnapshot s;
    memset(&s, 0, sizeof(s));
    s.count = 1;
    s.wdCompass = -1;
    s.t = wall ? 0 : ts;
    for (int c = 0; c < CH_COUNT; c++) s.v[c] = flashLogDecode(c, vals[c]);

    if (!*first) j += ",";
    *first = false;
    appendSample(j, &s, wall, ts);

    g_drainCursor++;
    sent++;
  }

  if (skipped) {
    Serial.printf("[FWD] 跳过 %lu 条上次开机的补传记录 (时间基准已失效)\n",
                  (unsigned long)skipped);
  }

  flashLogCloseRead();
  return sent;
}

static void buildBody(String &j, const SensorSnapshot *s)
{
  j.reserve(4096);

  j = "{\"device\":\"";
  j += g_dev;
  j += "\",\"fw\":\"" FWD_FW_VERSION "\",\"up\":";
  j += String(s->t);
  j += ",\"sampleMs\":";
  j += String(samplerIntervalSec() * 1000UL);

  j += ",\"samples\":[";
  bool first = true;
  g_sentThisPost = appendBacklog(j, FWD_DRAIN_MAX, &first);
  if (!first) j += ",";
  appendSample(j, s, clockIsValid(), clockNow());
  j += "]";

  j += ",\"relays\":[";
  for (uint8_t i = 0; i < 12; i++) {
    if (i) j += ",";
    j += String((int)g_rly[i]);
  }
  j += "]";

  if (g_ackN) {
    j += ",\"ack\":[";
    for (uint8_t i = 0; i < g_ackN; i++) {
      if (i) j += ",";
      j += String((unsigned)g_ack[i]);
    }
    j += "]";
  }

  j += "}";
}

/* ════════════════════════════════════════════
   断链缓冲策略
   ════════════════════════════════════════════ */

/**
 * 断链初期 (前 10 分钟) 每个样本都留;
 * 长时间断链后改为每 FWD_BUF_THIN 个样本只留 1 个,
 * 这样 1.08MB 的缓冲足够覆盖十几天的稀疏记录, 也大幅降低 Flash 写入量。
 */
#define FWD_BUF_FULL_SEC  600
#define FWD_BUF_THIN      15

static uint32_t g_offlineSinceMs = 0;
static uint32_t g_offlineCount = 0;

static bool backlogAppend(const SensorSnapshot *s)
{
  if (!flashLogReady()) return false;

  uint32_t nowMs = millis();
  if (g_offlineSinceMs == 0) {
    g_offlineSinceMs = nowMs;
    g_offlineCount = 0;
  }

  bool fullRate = (nowMs - g_offlineSinceMs) < FWD_BUF_FULL_SEC * 1000UL;
  g_offlineCount++;

  if (!fullRate && (g_offlineCount % FWD_BUF_THIN) != 0) return false;

  return flashLogAppend(s);
}

static void backlogRecover(void)
{
  g_offlineSinceMs = 0;
  g_offlineCount = 0;
}

/* ════════════════════════════════════════════
   HTTP
   ════════════════════════════════════════════ */

static int httpPost(const String &body, String &resp)
{
  char url[64];
  snprintf(url, sizeof(url), "http://%s:%u/api/ingest", g_host, (unsigned)g_port);

  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(FWD_HTTP_TIMEOUT_MS);
  http.setReuse(false);
  if (!http.begin(client, url)) return -1;
  http.addHeader("Content-Type", "application/json");

  int code = http.POST((uint8_t *)body.c_str(), body.length());
  if (code == 200) resp = http.getString();
  http.end();
  return code;
}

static void handleResponse(const String &resp)
{
  long v = 0;

  /* 上报成功一次就把 PC 的时间当作基准, 断链期间也能记下绝对时间 */
  if (jsonLong(resp, "epoch", &v) && v > 1600000000L) {
    clockSyncFromBrowser((uint32_t)v);
  }

  /* ── 命令 ── */
  int cp = resp.indexOf("\"commands\":");
  if (cp >= 0) {
    int a = resp.indexOf('[', cp);
    int z = (a < 0) ? -1 : resp.indexOf(']', a);
    int p = (a < 0) ? -1 : a + 1;
    while (p > 0 && z > p) {
      int ob = resp.indexOf('{', p);
      if (ob < 0 || ob > z) break;
      int cb = resp.indexOf('}', ob);
      if (cb < 0) break;

      String obj = resp.substring(ob, cb + 1);
      long ch = 0, id = 0;
      char act[10] = {0};
      if (jsonLong(obj, "relay", &ch) && jsonStr(obj, "action", act, sizeof(act))) {
        jsonLong(obj, "id", &id);
        relayExec((uint8_t)ch, act, (uint16_t)id);
        ackPush((uint16_t)id);
      }
      p = cb + 1;
    }
  }

  /* ── 策略 ── */
  if (jsonLong(resp, "maxOn", &v))       g_maxOnSec = (v > 0) ? (uint32_t)v : 0;
  if (jsonLong(resp, "linkTimeout", &v)) g_linkTimeout = (v > 0) ? (uint32_t)v : 0;

  char loss[12] = {0};
  if (jsonStr(resp, "onLinkLoss", loss, sizeof(loss))) {
    g_onLinkLossOff = (strcmp(loss, "off") == 0);
  }
  if (jsonLong(resp, "sampleMs", &v) && v >= SAMPLE_INTERVAL_MIN * 1000L) {
    samplerSetIntervalSec((uint32_t)(v / 1000L));
  }
}

/* 上报成功后推进补传进度, 全部补完则清空缓冲 */
static void drainCommit()
{
  if (!g_draining) return;

  g_drainTotal += g_sentThisPost;

  if (g_drainCursor >= flashLogTotal()) {
    flashLogClear();
    g_draining = false;
    g_drainCursor = 0;
    Serial.printf("[FWD] 补传完成, 累计补传 %lu 条\n", (unsigned long)g_drainTotal);
  }
}

/* ════════════════════════════════════════════
   本地策略兜底
   ════════════════════════════════════════════ */

static void policyTick(void)
{
  uint32_t nowMs = millis();

  /* 1. 单路最长连续通电 —— 防止页面/网络异常时一直开着水泵 */
  if (g_maxOnSec) {
    for (uint8_t i = 0; i < 12; i++) {
      if (g_rly[i] != 1 || g_onSince[i] == 0) continue;
      if ((nowMs - g_onSince[i]) > g_maxOnSec * 1000UL) {
        Serial.printf("[FWD] 通道 %u 超过最长通电 %lus, 自动关闭\n",
                      (unsigned)(i + 1), (unsigned long)g_maxOnSec);
        relayExec((uint8_t)(i + 1), "off", 0);
      }
    }
  }

  /* 2. 断链保护 —— 与 PC 失联超过阈值时按策略处理 */
  if (g_linkTimeout && g_everOk && !g_linkCutDone &&
      (nowMs - g_lastOkMs) > g_linkTimeout * 1000UL) {
    g_linkCutDone = true;
    if (g_onLinkLossOff) allRelaysOff("断链保护");
    else Serial.println("[FWD] 断链超时, 策略为保持现状");
  }
}

/* ════════════════════════════════════════════
   OLED 状态页
   ════════════════════════════════════════════ */

static const char *devTail(void)
{
  size_t n = strlen(g_dev);
  return (n > 6) ? g_dev + n - 6 : g_dev;
}

static void displayTick(void)
{
  uint32_t nowMs = millis();
  if (g_lastOledMs != 0 && (nowMs - g_lastOledMs) < 1000UL) return;
  g_lastOledMs = nowMs;

  char l0[24], l1[24], l2[24], l3[24], l4[24], l5[24];
  const SensorSnapshot *s = samplerLatest();
  FlogInfo fi;
  memset(&fi, 0, sizeof(fi));
  flashLogInfo(&fi);

  bool wifi = (WiFi.status() == WL_CONNECTED);
  bool online = wifi && g_pcOk;

  snprintf(l0, sizeof(l0), "%s %s", devTail(), online ? "ONLINE" : (wifi ? "SEND.." : "WIFI--"));

  if (wifi) snprintf(l1, sizeof(l1), "%s:%u", g_host, (unsigned)g_port);
  else      snprintf(l1, sizeof(l1), "PC %s:%u", g_host, (unsigned)g_port);

  snprintf(l2, sizeof(l2), "TX %lu  ERR %lu", (unsigned long)g_okCount,
           (unsigned long)g_failCount);
  snprintf(l3, sizeof(l3), "BUF %lu  %lus", (unsigned long)fi.records,
           (unsigned long)samplerIntervalSec());

  if (s && !isnan(s->v[CH_AT]) && !isnan(s->v[CH_SM])) {
    snprintf(l4, sizeof(l4), "T%.1f H%.0f S%.0f%%", s->v[CH_AT],
             isnan(s->v[CH_AH]) ? 0.0f : s->v[CH_AH], s->v[CH_SM]);
  } else {
    snprintf(l4, sizeof(l4), "sensor --");
  }

  if (g_lastCh) snprintf(l5, sizeof(l5), "#%d %s %ddBm", g_lastCh, g_lastAction, WiFi.RSSI());
  else          snprintf(l5, sizeof(l5), "no cmd  %ddBm", WiFi.RSSI());

  const char *lines[6] = { l0, l1, l2, l3, l4, l5 };
  oled_showLines(lines, 6);
}

/* ════════════════════════════════════════════
   初始化 / 主循环
   ════════════════════════════════════════════ */

static void buildDeviceId(void)
{
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  snprintf(g_dev, sizeof(g_dev), "esp32-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

void setupForwarder(void)
{
  prefs.begin("fwd", false);
  String host = prefs.getString("host", FWD_DEFAULT_HOST);
  g_port = (uint16_t)prefs.getUShort("port", FWD_DEFAULT_PORT);
  prefs.end();

  if (host.length() == 0 || host.length() >= sizeof(g_host)) host = FWD_DEFAULT_HOST;
  strncpy(g_host, host.c_str(), sizeof(g_host) - 1);
  g_host[sizeof(g_host) - 1] = 0;
  if (g_port == 0) g_port = FWD_DEFAULT_PORT;

  buildDeviceId();

  Serial.printf("[FWD] 上报目标 http://%s:%u/api/ingest (设备 %s, 固件 %s)\n",
                g_host, (unsigned)g_port, g_dev, FWD_FW_VERSION);
  Serial.printf("[FWD] 断链缓冲 %lu 条可用, 单次最多补传 %d 条\n",
                (unsigned long)(FLOG_MAX_SEGS * FLOG_SEG_RECS), FWD_DRAIN_MAX);
  Serial.println("[FWD] 串口命令: host <IP> [端口] | interval <秒> | on/off/blink <1-12> | show | clear");
}

void forwarderTick(void)
{
  const SensorSnapshot *s = samplerLatest();
  if (s && s->t != g_lastSentT) {
    if (WiFi.status() != WL_CONNECTED) {
      /* 无网络: 直接进补传缓冲, 不再空等 */
      g_failCount++;
      g_pcOk = false;
      g_lastSentT = s->t;
      if (backlogAppend(s)) {
        FlogInfo fi;
        flashLogInfo(&fi);
        Serial.printf("[FWD] WiFi 断开, 样本转入补传缓冲 (%lu 条)\n", (unsigned long)fi.records);
      }
    } else {
      g_lastSentT = s->t;

      String body, resp;
      buildBody(body, s);

      uint32_t t0 = millis();
      int code = httpPost(body, resp);
      g_latencyMs = millis() - t0;

      if (code == 200) {
        g_okCount++;
        g_pcOk = true;
        g_everOk = true;
        g_lastOkMs = millis();
        g_linkCutDone = false;
        g_ackN = 0;
        backlogRecover();
        handleResponse(resp);
        drainCommit();
      } else {
        g_failCount++;
        g_pcOk = false;
        Serial.printf("[FWD] 上报失败 (code %d, %lums), 样本转入补传缓冲\n",
                      code, (unsigned long)g_latencyMs);
        if (backlogAppend(s)) {
          FlogInfo fi;
          flashLogInfo(&fi);
          Serial.printf("[FWD] 缓冲 %lu 条\n", (unsigned long)fi.records);
        }
      }
    }
  }

  policyTick();
  displayTick();
}

/* ════════════════════════════════════════════
   串口命令
   ════════════════════════════════════════════ */

void forwarderSetHost(const char *host, uint16_t port)
{
  if (!host || !*host) return;
  strncpy(g_host, host, sizeof(g_host) - 1);
  g_host[sizeof(g_host) - 1] = 0;
  if (port) g_port = port;

  prefs.begin("fwd", false);
  prefs.putString("host", g_host);
  prefs.putUShort("port", g_port);
  prefs.end();

  Serial.printf("[FWD] 上报地址已保存: %s:%u\n", g_host, (unsigned)g_port);
}

static void printStatus(void)
{
  FlogInfo fi;
  memset(&fi, 0, sizeof(fi));
  flashLogInfo(&fi);

  Serial.println("──────── 设备状态 ────────");
  Serial.printf("  设备 ID   : %s  (固件 %s)\n", g_dev, FWD_FW_VERSION);
  Serial.printf("  上报地址  : http://%s:%u/api/ingest\n", g_host, (unsigned)g_port);
  Serial.printf("  采样间隔  : %lu 秒\n", (unsigned long)samplerIntervalSec());
  Serial.printf("  WiFi      : %s  %d dBm  %s\n",
                (WiFi.status() == WL_CONNECTED) ? "已连接" : "未连接",
                WiFi.RSSI(), WiFi.localIP().toString().c_str());
  Serial.printf("  上报统计  : 成功 %lu / 失败 %lu, 最近耗时 %lums\n",
                (unsigned long)g_okCount, (unsigned long)g_failCount,
                (unsigned long)g_latencyMs);
  Serial.printf("  补传缓冲  : %lu / %u 条 (%lu 字节), 累计补传 %lu 条\n",
                (unsigned long)fi.records, (unsigned)fi.capacity,
                (unsigned long)fi.bytes, (unsigned long)g_drainTotal);
  Serial.printf("  时间基准  : %s (epoch %lu)\n",
                clockIsValid() ? "绝对时间" : "开机秒", (unsigned long)clockNow());
  Serial.printf("  策略      : maxOn %lus, 断链 %lus, 断链动作 %s\n",
                (unsigned long)g_maxOnSec, (unsigned long)g_linkTimeout,
                g_onLinkLossOff ? "off" : "keep");
  Serial.print("  继电器    :");
  for (uint8_t i = 0; i < 12; i++) Serial.printf(" %u", g_rly[i]);
  Serial.println();
  Serial.println("──────────────────────────");
}

void forwarderSerialTask(void)
{
  if (!Serial.available()) return;

  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  int sp = line.indexOf(' ');
  String cmd = (sp < 0) ? line : line.substring(0, sp);
  String arg = (sp < 0) ? String() : line.substring(sp + 1);
  arg.trim();
  cmd.toLowerCase();

  if (cmd == "show" || cmd == "status") {
    printStatus();
  } else if (cmd == "host") {
    if (arg.length() == 0) {
      Serial.println("[FWD] 用法: host <IP> [端口]");
      return;
    }
    int sp2 = arg.indexOf(' ');
    String ip = (sp2 < 0) ? arg : arg.substring(0, sp2);
    uint16_t port = g_port;
    if (sp2 > 0) {
      int p = arg.substring(sp2 + 1).toInt();
      if (p > 0 && p < 65536) port = (uint16_t)p;
    }
    forwarderSetHost(ip.c_str(), port);
  } else if (cmd == "interval") {
    long sec = arg.toInt();
    if (sec <= 0) {
      Serial.println("[FWD] 用法: interval <秒> (1~3600)");
      return;
    }
    samplerSetIntervalSec((uint32_t)sec);
  } else if (cmd == "on" || cmd == "off" || cmd == "blink") {
    long ch = arg.toInt();
    if (ch < 1 || ch > 12) {
      Serial.println("[FWD] 用法: on|off|blink <1-12>");
      return;
    }
    relayExec((uint8_t)ch, cmd.c_str(), 0);
  } else if (cmd == "clear") {
    flashLogClear();
  } else if (cmd == "help") {
    Serial.println("[FWD] host <IP> [端口] | interval <秒> | on|off|blink <1-12> | show | clear");
  } else {
    Serial.printf("[FWD] 未知命令: %s (输入 help 查看)\n", cmd.c_str());
  }
}

/* ════════════════════════════════════════════
   状态导出
   ════════════════════════════════════════════ */

void forwarderStatus(ForwarderStatus *out)
{
  if (!out) return;
  memset(out, 0, sizeof(*out));

  FlogInfo fi;
  memset(&fi, 0, sizeof(fi));
  flashLogInfo(&fi);

  out->wifi        = (WiFi.status() == WL_CONNECTED);
  out->pcOk        = g_pcOk;
  out->everOk      = g_everOk;
  out->okCount     = g_okCount;
  out->failCount   = g_failCount;
  out->lastOkAgo   = g_everOk ? (millis() - g_lastOkMs) / 1000UL : 0xFFFFFFFF;
  out->latencyMs   = g_latencyMs;
  out->backlog     = fi.records;
  out->drainTotal  = g_drainTotal;
  out->lastCh      = g_lastCh;
  out->rssi        = out->wifi ? WiFi.RSSI() : 0;
  snprintf(out->url, sizeof(out->url), "%s:%u", g_host, (unsigned)g_port);
  strncpy(out->lastAction, g_lastAction, sizeof(out->lastAction) - 1);
}

const uint8_t *forwarderRelays(void) { return g_rly; }

const char *forwarderDeviceId(void) { return g_dev; }
