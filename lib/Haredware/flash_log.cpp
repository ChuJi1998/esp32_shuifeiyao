#include "flash_log.h"
#include "sysclock.h"

#include <LittleFS.h>
#include <math.h>
#include <string.h>

/* ════════════════════════════════════════════
   常量
   ════════════════════════════════════════════ */
#define FLOG_DIR        "/hl"
#define FLOG_MAGIC      "HLOG"
#define FLOG_VERSION    1
#define FLOG_HDR_CRC_OFF 8      /* 段头 CRC16 字段偏移 */

/* 各通道 int16 缩放系数 (编码: round(v × scale); 解码: raw ÷ scale) */
static const float CH_SCALE[CH_COUNT] = {
  100.0f,   /* 风速   m/s,  ±327 m/s     */
  10.0f,    /* 风向   °,    0~3276°      */
  100.0f,   /* 气温   °C,   ±327 °C      */
  10.0f,    /* 湿度   %RH,  0~3276 %RH   */
  0.5f,     /* 光照   lux,  0~65534 lux  */
  10.0f,    /* 土湿   %,    0~3276 %     */
  100.0f,   /* 土温   °C,   ±327 °C      */
  1.0f,     /* 土壤EC μS/cm 0~32767      */
  1.0f,     /* CO2    ppm,  0~32767      */
  100.0f    /* 氧气   %,     0~327 %     */
};

/* ── 分段头 (24 字节) ── */
struct FlogHeader {
  char     magic[4];
  uint8_t  version;
  uint8_t  flags;      /* bit0: baseTs 为绝对时间 */
  uint8_t  recSize;
  uint8_t  reserved;
  uint16_t crc;
  uint16_t count;      /* 保留: 实际条数以文件长度为准 */
  uint32_t seq;
  uint32_t baseTs;     /* 首条记录时间戳 */
  uint32_t reserved2;
} __attribute__((packed));

/* ── 内存索引 ── */
struct SegEntry {
  uint32_t seq;
  uint32_t baseTs;
  uint32_t count;
  uint32_t start;      /* 该段首条记录的全局下标 */
  uint8_t  wall;
};

static SegEntry g_idx[FLOG_MAX_SEGS + 4];
static uint16_t g_idxNum = 0;
static uint32_t g_total = 0;

/* ── 当前写入段 ── */
static uint32_t g_seqMax = 0;
static uint32_t g_actSeq = 0;
static uint32_t g_actCount = 0;
static uint8_t  g_actWall = 0;
static uint32_t g_lastTs = 0;
static bool     g_ready = false;

/* ── 读取句柄缓存 ── */
static File     g_curFile;
static uint32_t g_curSeq = 0;
static uint32_t g_curNextIdx = 0xFFFFFFFF;
static bool     g_curOpen = false;

static uint32_t upSec(void) { return millis() / 1000UL; }

/* ════════════════════════════════════════════
   工具
   ════════════════════════════════════════════ */

static uint16_t crc16(const uint8_t *d, size_t n, uint16_t crc)
{
  for (size_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
  }
  return crc;
}

static uint16_t headerCrc(const uint8_t *h)
{
  uint16_t c = crc16(h, FLOG_HDR_CRC_OFF, 0xFFFF);
  return crc16(h + FLOG_HDR_CRC_OFF + 2,
               FLOG_HDR_SIZE - FLOG_HDR_CRC_OFF - 2, c);
}

static void segPath(uint32_t seq, char *buf, size_t n)
{
  snprintf(buf, n, "%s/%010lu.bin", FLOG_DIR, (unsigned long)seq);
}

static int16_t encodeCh(int ch, float v)
{
  if (isnan(v)) return FLOG_INVALID;
  float s = v * CH_SCALE[ch];
  if (s > 32767.0f)  s = 32767.0f;
  if (s < -32767.0f) s = -32767.0f;
  return (int16_t)lroundf(s);
}

float flashLogDecode(int ch, int16_t raw)
{
  if (raw == FLOG_INVALID) return NAN;
  return (float)raw / CH_SCALE[ch];
}

void flashLogCloseRead(void)
{
  if (g_curOpen) {
    g_curFile.close();
    g_curOpen = false;
  }
  g_curSeq = 0;
  g_curNextIdx = 0xFFFFFFFF;
}

/* ════════════════════════════════════════════
   索引维护
   ════════════════════════════════════════════ */

static void removeOldestSegment(void)
{
  if (g_idxNum == 0) return;

  uint32_t seq = g_idx[0].seq;
  char path[24];
  segPath(seq, path, sizeof(path));

  if (g_curOpen && g_curSeq == seq) flashLogCloseRead();

  LittleFS.remove(path);

  uint32_t removed = g_idx[0].count;
  if (g_total >= removed) g_total -= removed;
  else g_total = 0;

  for (uint16_t i = 1; i < g_idxNum; i++) {
    g_idx[i - 1] = g_idx[i];
    g_idx[i - 1].start -= removed;
  }
  g_idxNum--;

  Serial.printf("[FLOG] 空间回收: 删除最旧分段 %s (%lu 条)\n",
                path, (unsigned long)removed);
}

/* 新建分段, baseTs 用即将写入的首条记录时间戳 */
static bool createSegment(uint32_t baseTs)
{
  /* 段数上限 */
  while (g_idxNum >= FLOG_MAX_SEGS) removeOldestSegment();

  uint32_t seq = g_seqMax + 1;
  char path[24];
  segPath(seq, path, sizeof(path));

  File f = LittleFS.open(path, "w");
  if (!f) {
    Serial.printf("[FLOG] 创建分段失败: %s\n", path);
    return false;
  }

  FlogHeader h;
  memset(&h, 0, sizeof(h));
  memcpy(h.magic, FLOG_MAGIC, 4);
  h.version = FLOG_VERSION;
  h.flags   = clockIsValid() ? 1 : 0;
  h.recSize = FLOG_REC_SIZE;
  h.count   = 0;
  h.seq     = seq;
  h.baseTs  = baseTs;
  h.crc     = headerCrc((const uint8_t *)&h);

  size_t wr = f.write((const uint8_t *)&h, FLOG_HDR_SIZE);
  f.close();

  if (wr != FLOG_HDR_SIZE) {
    LittleFS.remove(path);
    Serial.printf("[FLOG] 分段头写入失败: %s\n", path);
    return false;
  }

  /* 剩余空间保护 */
  while (g_idxNum > 0 &&
         (LittleFS.totalBytes() - LittleFS.usedBytes()) < FLOG_MIN_FREE) {
    removeOldestSegment();
  }

  SegEntry *e = &g_idx[g_idxNum++];
  e->seq    = seq;
  e->baseTs = baseTs;
  e->count  = 0;
  e->wall   = h.flags;
  e->start  = g_total;

  g_actSeq   = seq;
  g_actCount = 0;
  g_actWall  = h.flags;
  g_seqMax   = seq;

  Serial.printf("[FLOG] 新建分段 %s (baseTs=%lu, wall=%d)\n",
                path, (unsigned long)baseTs, (int)h.flags);
  return true;
}

/* ════════════════════════════════════════════
   写入
   ════════════════════════════════════════════ */

static bool appendRecord(const SensorSnapshot *s)
{
  uint32_t ts = clockIsValid() ? clockNow() : upSec();
  if (ts <= g_lastTs) ts = g_lastTs + 1;      /* 保持时间戳单调, 便于二分查找 */

  bool needNew = (g_idxNum == 0) ||
                 (g_actCount >= FLOG_SEG_RECS) ||
                 ((g_actWall ? 1 : 0) != (clockIsValid() ? 1 : 0));

  if (needNew && !createSegment(ts)) return false;

  uint8_t rec[FLOG_REC_SIZE];
  memcpy(rec, &ts, 4);
  for (int c = 0; c < CH_COUNT; c++) {
    int16_t e = encodeCh(c, s->v[c]);
    memcpy(rec + 4 + c * 2, &e, 2);
  }

  char path[24];
  segPath(g_actSeq, path, sizeof(path));

  File f = LittleFS.open(path, "a");
  if (!f) return false;
  size_t wr = f.write(rec, FLOG_REC_SIZE);
  f.close();

  if (wr != FLOG_REC_SIZE) return false;

  g_actCount++;
  g_total++;
  g_idx[g_idxNum - 1].count = g_actCount;
  g_lastTs = ts;
  return true;
}

bool flashLogAppend(const SensorSnapshot *s)
{
  if (!g_ready || !s) return false;
  return appendRecord(s);
}

/* ════════════════════════════════════════════
   读取
   ════════════════════════════════════════════ */

static bool ensureReadFile(uint16_t si)
{
  if (g_curOpen && g_curSeq == g_idx[si].seq) return true;

  flashLogCloseRead();

  char path[24];
  segPath(g_idx[si].seq, path, sizeof(path));
  g_curFile = LittleFS.open(path, "r");
  if (!g_curFile) {
    g_curOpen = false;
    return false;
  }
  g_curOpen = true;
  g_curSeq = g_idx[si].seq;
  return true;
}

bool flashLogRead(uint32_t idx, uint32_t *ts, int16_t *vals)
{
  if (idx >= g_total) return false;

  uint16_t si = 0;
  while (si < g_idxNum && idx >= g_idx[si].start + g_idx[si].count) si++;
  if (si >= g_idxNum) return false;

  /* 连续读取时文件指针已在正确位置, 无需再次寻道 */
  bool sequential = g_curOpen && (g_curSeq == g_idx[si].seq) && (g_curNextIdx == idx);
  if (!sequential) {
    if (!ensureReadFile(si)) return false;
    uint32_t off = FLOG_HDR_SIZE + (idx - g_idx[si].start) * FLOG_REC_SIZE;
    if (!g_curFile.seek(off)) return false;
  }

  uint8_t rec[FLOG_REC_SIZE];
  if (g_curFile.read(rec, FLOG_REC_SIZE) != FLOG_REC_SIZE) {
    g_curNextIdx = 0xFFFFFFFF;
    return false;
  }
  g_curNextIdx = idx + 1;

  if (ts)   memcpy(ts, rec, 4);
  if (vals) memcpy(vals, rec + 4, CH_COUNT * 2);
  return true;
}

uint32_t flashLogFindFirst(uint32_t cutoff)
{
  if (g_total == 0 || g_idxNum == 0) return 0;

  /* 段级定位: 最后一个 baseTs <= cutoff 的段 (baseTs 单调递增) */
  uint16_t cand = 0;
  for (uint16_t i = 1; i < g_idxNum; i++) {
    if (g_idx[i].baseTs <= cutoff) cand = i;
    else break;
  }

  SegEntry *e = &g_idx[cand];

  /* 段内二分 */
  uint32_t lo = 0, hi = e->count;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    uint32_t ts = 0;
    if (!flashLogRead(e->start + mid, &ts, NULL)) break;
    if (ts < cutoff) lo = mid + 1;
    else             hi = mid;
  }

  uint32_t res = e->start + lo;
  if (res >= e->start + e->count) {
    res = (cand + 1 < g_idxNum) ? g_idx[cand + 1].start : g_total;
  }
  return res;
}

/* ════════════════════════════════════════════
   初始化
   ════════════════════════════════════════════ */

struct RawSeg {
  uint32_t seq;
  uint32_t size;
};

static uint16_t scanSegFiles(RawSeg *list, uint16_t maxList)
{
  uint16_t n = 0;

  File dir = LittleFS.open(FLOG_DIR);
  if (!dir || !dir.isDirectory()) return 0;

  File f = dir.openNextFile();
  while (f && n < maxList) {
    if (!f.isDirectory()) {
      const char *full = f.name();
      const char *base = strrchr(full, '/');
      base = base ? base + 1 : full;

      unsigned long seq = 0;
      if (sscanf(base, "%lu", &seq) == 1 && seq > 0) {
        list[n].seq  = (uint32_t)seq;
        list[n].size = f.size();
        n++;
      }
    }
    f = dir.openNextFile();
  }
  dir.close();
  return n;
}

static void sortBySeq(RawSeg *list, uint16_t n)
{
  for (uint16_t i = 1; i < n; i++) {
    RawSeg key = list[i];
    int16_t j = (int16_t)i - 1;
    while (j >= 0 && list[j].seq > key.seq) {
      list[j + 1] = list[j];
      j--;
    }
    list[j + 1] = key;
  }
}

static bool readHeader(uint32_t seq, FlogHeader *h)
{
  char path[24];
  segPath(seq, path, sizeof(path));

  File f = LittleFS.open(path, "r");
  if (!f) return false;
  size_t rd = f.read((uint8_t *)h, FLOG_HDR_SIZE);
  f.close();

  if (rd != FLOG_HDR_SIZE) return false;
  if (memcmp(h->magic, FLOG_MAGIC, 4) != 0) return false;
  if (h->version != FLOG_VERSION || h->recSize != FLOG_REC_SIZE) return false;
  return (h->crc == headerCrc((const uint8_t *)h));
}

void setupFlashLog(void)
{
  if (!LittleFS.begin(true)) {
    Serial.println("[FLOG] LittleFS 不可用, 补传缓冲已禁用");
    g_ready = false;
    return;
  }

  if (!LittleFS.exists(FLOG_DIR)) {
    LittleFS.mkdir(FLOG_DIR);
    Serial.printf("[FLOG] 创建日志目录 %s\n", FLOG_DIR);
  }

  /* 扫描已有分段并建立索引 */
  RawSeg list[FLOG_MAX_SEGS + 8];
  uint16_t n = scanSegFiles(list, FLOG_MAX_SEGS + 8);
  sortBySeq(list, n);

  /* 超出段数上限的旧段直接删除 */
  if (n > FLOG_MAX_SEGS) {
    for (uint16_t i = 0; i < n - FLOG_MAX_SEGS; i++) {
      char path[24];
      segPath(list[i].seq, path, sizeof(path));
      LittleFS.remove(path);
      Serial.printf("[FLOG] 启动清理旧分段 %s\n", path);
    }
    memmove(list, list + (n - FLOG_MAX_SEGS),
            sizeof(RawSeg) * FLOG_MAX_SEGS);
    n = FLOG_MAX_SEGS;
  }

  g_idxNum = 0;
  g_total = 0;

  for (uint16_t i = 0; i < n; i++) {
    FlogHeader h;
    if (!readHeader(list[i].seq, &h) || list[i].size < FLOG_HDR_SIZE) {
      char path[24];
      segPath(list[i].seq, path, sizeof(path));
      LittleFS.remove(path);
      Serial.printf("[FLOG] 分段头损坏, 已删除 %s\n", path);
      continue;
    }

    uint32_t cnt = (list[i].size - FLOG_HDR_SIZE) / FLOG_REC_SIZE;
    if (cnt > FLOG_SEG_RECS) cnt = FLOG_SEG_RECS;

    if (cnt == 0 && i + 1 < n) {      /* 中间的空段属于异常残留 */
      char path[24];
      segPath(list[i].seq, path, sizeof(path));
      LittleFS.remove(path);
      continue;
    }

    SegEntry *e = &g_idx[g_idxNum++];
    e->seq    = list[i].seq;
    e->baseTs = h.baseTs;
    e->count  = cnt;
    e->wall   = h.flags;
    e->start  = g_total;
    g_total  += cnt;
  }

  if (g_idxNum > 0) {
    SegEntry *last = &g_idx[g_idxNum - 1];
    g_seqMax   = last->seq;
    g_actSeq   = last->seq;
    g_actCount = last->count;
    g_actWall  = last->wall;
    if (g_actCount > 0)
      flashLogRead(last->start + last->count - 1, &g_lastTs, NULL);
  }

  flashLogCloseRead();
  g_ready = true;

  uint32_t freeKB = (LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024;
  Serial.printf("[FLOG] 补传缓冲就绪: %u 段 / %lu 条 / 剩余 %lu KB\n",
                (unsigned)g_idxNum, (unsigned long)g_total, (unsigned long)freeKB);
}

/* ════════════════════════════════════════════
   配置与维护
   ════════════════════════════════════════════ */

bool flashLogReady(void) { return g_ready; }

uint32_t flashLogTotal(void) { return g_total; }

void flashLogInfo(FlogInfo *out)
{
  memset(out, 0, sizeof(*out));

  out->ready     = g_ready;
  out->segs      = g_idxNum;
  out->records   = g_total;
  out->capacity  = (uint32_t)FLOG_MAX_SEGS * FLOG_SEG_RECS;
  out->wallClock = (g_idxNum > 0) && (g_idx[g_idxNum - 1].wall != 0);

  uint32_t bytes = 0;
  for (uint16_t i = 0; i < g_idxNum; i++)
    bytes += FLOG_HDR_SIZE + g_idx[i].count * FLOG_REC_SIZE;
  out->bytes = bytes;

  if (g_idxNum > 0) {
    out->oldestTs = g_idx[0].baseTs;
    out->newestTs = g_idx[g_idxNum - 1].baseTs + g_idx[g_idxNum - 1].count - 1;
  }
}

void flashLogClear(void)
{
  flashLogCloseRead();

  for (uint16_t i = 0; i < g_idxNum; i++) {
    char path[24];
    segPath(g_idx[i].seq, path, sizeof(path));
    LittleFS.remove(path);
  }

  g_idxNum   = 0;
  g_total    = 0;
  g_seqMax   = 0;
  g_actSeq   = 0;
  g_actCount = 0;
  g_actWall  = 0;
  g_lastTs   = 0;

  Serial.println("[FLOG] 补传缓冲已清空");
}
