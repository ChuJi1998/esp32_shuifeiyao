"""
存储层 —— SQLite 读写 (读数 / 命令队列 / 设备状态 / 继电器策略)

只依赖 Python 标准库。
"""

import os
import sqlite3
import threading
import time

# 通道顺序 (与固件上报的紧凑数组一致)
CHANNELS = ["ws", "wd", "at", "ah", "lux", "sm", "st", "sec", "co2", "o2"]

SCHEMA = """
CREATE TABLE IF NOT EXISTS readings (
  ts     INTEGER NOT NULL,
  device TEXT    NOT NULL,
  up     INTEGER,
  ws REAL, wd REAL, at REAL, ah REAL, lux REAL,
  sm REAL, st REAL, sec REAL, co2 REAL, o2 REAL
);
CREATE INDEX IF NOT EXISTS idx_readings_dev_ts ON readings(device, ts);

CREATE TABLE IF NOT EXISTS devices (
  device     TEXT PRIMARY KEY,
  fw         TEXT,
  first_seen INTEGER,
  last_seen  INTEGER,
  last_up    INTEGER
);

CREATE TABLE IF NOT EXISTS relay_state (
  device TEXT,
  ch     INTEGER,
  state  INTEGER,
  ts     INTEGER,
  PRIMARY KEY (device, ch)
);

CREATE TABLE IF NOT EXISTS commands (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  device       TEXT,
  ch           INTEGER,
  action       TEXT,
  created_at   INTEGER,
  delivered_at INTEGER,
  ack_at       INTEGER
);
CREATE INDEX IF NOT EXISTS idx_cmd_pending ON commands(device, delivered_at);

CREATE TABLE IF NOT EXISTS policy (
  device       TEXT PRIMARY KEY,
  max_on       INTEGER DEFAULT 0,       -- 单路最长开启秒数, 0 = 不限制
  on_link_loss TEXT    DEFAULT 'keep',  -- keep | off
  link_timeout INTEGER DEFAULT 0,       -- 断链判定秒数, 0 = 不判定
  sample_ms    INTEGER DEFAULT 2000
);
"""


class Store:
    def __init__(self, path):
        path = os.path.abspath(path)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self._lock = threading.Lock()
        self._db = sqlite3.connect(path, check_same_thread=False)
        self._db.row_factory = sqlite3.Row
        with self._lock:
            self._db.execute("PRAGMA journal_mode=WAL")
            self._db.executescript(SCHEMA)
            self._db.commit()

    # ── 内部 ──
    def _exec(self, sql, args=()):
        with self._lock:
            self._db.execute(sql, args)
            self._db.commit()

    def _query(self, sql, args=()):
        with self._lock:
            return self._db.execute(sql, args).fetchall()

    def _pick_device(self, device):
        if device:
            return device
        rows = self._query("SELECT device FROM devices ORDER BY last_seen DESC LIMIT 1")
        return rows[0]["device"] if rows else None

    # ════════════════════════════════════════════
    # 设备注册与读数
    # ════════════════════════════════════════════

    def register_device(self, device, fw):
        now = int(time.time())
        self._exec(
            """INSERT INTO devices(device, fw, first_seen, last_seen)
               VALUES(?,?,?,?)
               ON CONFLICT(device) DO UPDATE SET fw=excluded.fw""",
            (device, fw, now, now))
        self._exec("INSERT OR IGNORE INTO policy(device) VALUES(?)", (device,))

    def add_samples(self, device, up_newest, samples, relays, recv_ts):
        """samples: [{"up": int, "ts": epoch?, "v": [10 个值或 null]}], 由旧到新

        时间戳优先用样本自带的 ts (设备从断链缓冲补传的样本带绝对时间),
        没有 ts 时按"接收时刻 - 开机秒差"换算。
        同一 (device, ts) 只保留一条: 补传重发或同一秒内的多帧都不会产生重复点。
        """
        rows = []
        for s in samples:
            v = list(s.get("v") or [])
            v = [None] * len(CHANNELS) if not v else v
            v = (v + [None] * len(CHANNELS))[:len(CHANNELS)]

            try:
                ts = int(s.get("ts") or 0)
            except (TypeError, ValueError):
                ts = 0
            if ts <= 0:
                try:
                    gap = max(0, int(up_newest) - int(s.get("up") or up_newest))
                except (TypeError, ValueError):
                    gap = 0
                ts = recv_ts - gap

            rows.append((ts, device, s.get("up")) + tuple(v))

        if rows:
            # 同一批里也要去重 (设备补传时可能多个样本落到同一秒)
            uniq = {}
            for r in rows:
                uniq[r[0]] = r
            rows = list(uniq.values())

            cols = ",".join(CHANNELS)
            marks = ",".join("?" * (3 + len(CHANNELS)))
            with self._lock:
                for r in rows:
                    self._db.execute(
                        "DELETE FROM readings WHERE device=? AND ts=?", (r[1], r[0]))
                self._db.executemany(
                    "INSERT INTO readings(ts, device, up, %s) VALUES (%s)"
                    % (cols, marks), rows)

        with self._lock:
            self._db.execute(
                "UPDATE devices SET last_seen=?, last_up=? WHERE device=?",
                (recv_ts, up_newest, device))
            for ch, state in enumerate(relays or [], start=1):
                self._db.execute(
                    """INSERT INTO relay_state(device, ch, state, ts) VALUES(?,?,?,?)
                       ON CONFLICT(device, ch)
                       DO UPDATE SET state=excluded.state, ts=excluded.ts""",
                    (device, ch, int(state), recv_ts))
            self._db.commit()

    def latest(self, device=None):
        device = self._pick_device(device)
        if not device:
            return None
        rows = self._query(
            "SELECT * FROM readings WHERE device=? ORDER BY ts DESC LIMIT 1", (device,))
        if not rows:
            return None
        d = dict(rows[0])
        d["device"] = device
        return d

    def coverage(self, device=None):
        device = self._pick_device(device)
        if not device:
            return {"count": 0, "first": 0, "last": 0}
        r = self._query(
            "SELECT COUNT(*) AS c, MIN(ts) AS a, MAX(ts) AS b FROM readings WHERE device=?",
            (device,))[0]
        return {"count": r["c"], "first": r["a"] or 0, "last": r["b"] or 0}

    # ════════════════════════════════════════════
    # 历史序列 (SQL 聚合 + 抽稀)
    # ════════════════════════════════════════════

    def series(self, device=None, span=0, points=180, res="raw"):
        """res: raw | min | hour | day; points=None 表示不抽稀"""
        device = self._pick_device(device)
        if not device:
            return {"t": [], "v": {c: [] for c in CHANNELS}, "total": 0,
                    "stride": 1, "bucket": 0}

        start = 0 if not span else int(time.time()) - int(span)

        cnt = self._query(
            "SELECT COUNT(*) AS c FROM readings WHERE device=? AND ts>=?",
            (device, start))[0]["c"]

        # 自动降采样, 保证单次查询返回行数可控
        bucket = {"raw": 0, "min": 60, "hour": 3600, "day": 86400}.get(res, 0)
        MAXROWS = 20000
        if bucket == 0 and cnt > MAXROWS:
            bucket = 60
        if bucket == 60 and cnt // 60 > MAXROWS:
            bucket = 3600
        if bucket == 3600 and cnt // 3600 > MAXROWS:
            bucket = 86400

        if bucket:
            sql = (
                "SELECT (ts/%d)*%d AS bt, %s FROM readings "
                "WHERE device=? AND ts>=? GROUP BY bt ORDER BY bt"
            ) % (bucket, bucket, ", ".join("AVG(%s) AS %s" % (c, c) for c in CHANNELS))
        else:
            sql = ("SELECT ts AS bt, %s FROM readings WHERE device=? AND ts>=? ORDER BY ts"
                   % ", ".join(CHANNELS))

        rows = self._query(sql, (device, start))
        if not rows:
            return {"t": [], "v": {c: [] for c in CHANNELS}, "total": 0,
                    "stride": 1, "bucket": bucket}

        if points is None:
            step = 1
        else:
            points = max(10, min(int(points), 600))
            step = max(1, (len(rows) + points - 1) // points) if len(rows) > points else 1
        picked = rows[::step] if step > 1 else rows

        return {
            "t": [int(r["bt"]) for r in picked],
            "v": {c: [r[c] for r in picked] for c in CHANNELS},
            "total": len(rows),
            "stride": step,
            "bucket": bucket,
        }

    def all_rows(self, device=None, span=0):
        """按时间正序返回原始行 (CSV 导出用)"""
        device = self._pick_device(device)
        if not device:
            return []
        start = 0 if not span else int(time.time()) - int(span)
        return self._query(
            "SELECT ts, %s FROM readings WHERE device=? AND ts>=? ORDER BY ts"
            % ", ".join(CHANNELS), (device, start))

    def prune(self, keep_days):
        """删除超过 keep_days 的读数, 返回删除行数"""
        if not keep_days or keep_days <= 0:
            return 0
        cutoff = int(time.time()) - int(keep_days) * 86400
        with self._lock:
            cur = self._db.execute("DELETE FROM readings WHERE ts < ?", (cutoff,))
            self._db.commit()
            return cur.rowcount

    def clear_readings(self):
        """清空全部读数, 返回删除行数"""
        with self._lock:
            cur = self._db.execute("DELETE FROM readings")
            self._db.commit()
            return cur.rowcount

    # ════════════════════════════════════════════
    # 继电器
    # ════════════════════════════════════════════

    def relay_states(self, device=None):
        device = self._pick_device(device)
        states = [0] * 12
        if not device:
            return states
        for r in self._query(
                "SELECT ch, state FROM relay_state WHERE device=? ORDER BY ch", (device,)):
            ch = int(r["ch"])
            if 1 <= ch <= 12:
                states[ch - 1] = int(r["state"])
        return states

    def queue_command(self, device, ch, action):
        self._exec(
            "INSERT INTO commands(device, ch, action, created_at) VALUES(?,?,?,?)",
            (device, int(ch), action, int(time.time())))

    def take_commands(self, device, limit=16):
        """取出未投递命令, 取出即标记已投递 (不会重复下发)"""
        rows = self._query(
            "SELECT id, ch, action FROM commands "
            "WHERE device=? AND delivered_at IS NULL ORDER BY id LIMIT ?",
            (device, limit))
        if not rows:
            return []
        ids = [r["id"] for r in rows]
        self._exec(
            "UPDATE commands SET delivered_at=? WHERE id IN (%s)"
            % ",".join("?" * len(ids)), (int(time.time()), *ids))
        return [{"id": r["id"], "relay": r["ch"], "action": r["action"]} for r in rows]

    def ack_commands(self, device, ids):
        if not ids:
            return
        ids = [int(i) for i in ids]
        self._exec(
            "UPDATE commands SET ack_at=? WHERE device=? AND id IN (%s)"
            % ",".join("?" * len(ids)), (int(time.time()), device, *ids))

    def recent_commands(self, limit=20):
        return [dict(r) for r in self._query(
            "SELECT * FROM commands ORDER BY id DESC LIMIT ?", (int(limit),))]

    # ════════════════════════════════════════════
    # 策略 / 设备列表
    # ════════════════════════════════════════════

    def policy(self, device):
        rows = self._query("SELECT * FROM policy WHERE device=?", (device,))
        if not rows:
            self._exec("INSERT OR IGNORE INTO policy(device) VALUES(?)", (device,))
            rows = self._query("SELECT * FROM policy WHERE device=?", (device,))
        if rows:
            return dict(rows[0])
        return {"device": device, "max_on": 0, "on_link_loss": "keep",
                "link_timeout": 0, "sample_ms": 2000}

    def set_policy(self, device, max_on=None, on_link_loss=None,
                   link_timeout=None, sample_ms=None):
        cur = self.policy(device)
        self._exec(
            """INSERT INTO policy(device, max_on, on_link_loss, link_timeout, sample_ms)
               VALUES(?,?,?,?,?)
               ON CONFLICT(device) DO UPDATE SET
                 max_on=excluded.max_on,
                 on_link_loss=excluded.on_link_loss,
                 link_timeout=excluded.link_timeout,
                 sample_ms=excluded.sample_ms""",
            (device,
             cur["max_on"] if max_on is None else int(max_on),
             cur["on_link_loss"] if on_link_loss is None else on_link_loss,
             cur["link_timeout"] if link_timeout is None else int(link_timeout),
             cur["sample_ms"] if sample_ms is None else int(sample_ms)))

    def device_list(self, online_window=10):
        now = int(time.time())
        out = []
        for r in self._query("SELECT * FROM devices ORDER BY device"):
            d = dict(r)
            d["online"] = (now - (d["last_seen"] or 0)) < online_window
            d["age"] = now - (d["last_seen"] or 0)
            out.append(d)
        return out
