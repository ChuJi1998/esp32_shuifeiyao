#!/usr/bin/env python3
"""
水肥药一体机 —— PC 侧服务 (纯 Python 标准库, 无需 pip 安装)

职责:
  * 接收 ESP32 上报 (/api/ingest), 把命令塞在响应里带回 (无需轮询)
  * SQLite 长期存储 + 历史/统计/CSV 接口
  * 托管仪表盘页面 (static/)
  * 本地农艺规则引擎 (/api/rules)
  * AI 助手代理 (/api/ai) —— API Key 只存在于本机环境变量

用法:
  set DEEPSEEK_API_KEY=sk-xxxx        (可选, 不设则 AI 接口返回待发送的 prompt)
  python app.py --port 8080

设备侧 API 形状与固件保持一致, 因此固件端的仪表盘可无缝迁移。
"""

import argparse
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import rules                      # noqa: E402
from store import Store, CHANNELS  # noqa: E402

BASE = os.path.dirname(os.path.abspath(__file__))
STATIC = os.path.join(BASE, "static")

DEEPSEEK_URL = "https://api.deepseek.com/chat/completions"
DEEPSEEK_MODEL = os.environ.get("DEEPSEEK_MODEL", "deepseek-chat")

# 历史库保留期 (天) —— 只影响展示的理论容量, 实际不自动删除
RETENTION_DAYS = int(os.environ.get("AGRI_RETENTION_DAYS", "30"))

# 16 方位
COMPASS = ["N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
           "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"]

# 读取失败时的兼容兜底值 (与固件一致, 保证前端无需改动)
FALLBACK = {"ws": -1, "wd": -1, "at": -999, "ah": -1, "lux": -1,
            "sm": -1, "st": -999, "sec": -1, "co2": -1, "o2": -1}

MIME = {
    ".html": "text/html; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".png": "image/png", ".jpg": "image/jpeg", ".svg": "image/svg+xml",
    ".ico": "image/x-icon", ".woff2": "font/woff2", ".woff": "font/woff",
    ".csv": "text/csv; charset=utf-8",
}


def compass_name(wd):
    if wd is None:
        return "?"
    try:
        idx = int((float(wd) + 11.25) // 22.5) % 16
        return COMPASS[idx]
    except (TypeError, ValueError):
        return "?"


def pick_device(store, q):
    return q.get("device") or None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "AgriHub/1.0"
    store = None
    verbose = False

    # ── 基础设施 ──
    def log_message(self, fmt, *args):
        if self.verbose:
            sys.stderr.write("[%s] %s\n" % (self.address_string(), fmt % args))

    def _cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")

    def _send_bytes(self, data, ctype, code=200, extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self._cors()
        self.end_headers()
        self.wfile.write(data)

    def _send_json(self, obj, code=200, extra=None):
        body = json.dumps(obj, ensure_ascii=False, default=str).encode("utf-8")
        self._send_bytes(body, "application/json; charset=utf-8", code, extra)

    def _read_json(self):
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            return {}
        if n <= 0:
            return {}
        raw = self.rfile.read(n)
        try:
            return json.loads(raw.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return {}

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Content-Length", "0")
        self._cors()
        self.end_headers()

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        try:
            if u.path.startswith("/api/") or u.path == "/health":
                self._api_get(u.path, q)
            else:
                self._static(u.path)
        except BrokenPipeError:
            pass
        except Exception as exc:                      # noqa: BLE001
            self._send_json({"ok": False, "error": "%s: %s"
                             % (type(exc).__name__, exc)}, 500)

    def do_POST(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        body = self._read_json()
        try:
            if u.path == "/api/ingest":
                self._ingest(body)
            elif u.path == "/api/ai":
                self._ai(body)
            elif u.path == "/api/policy":
                self._set_policy(q, body)
            else:
                self._send_json({"ok": False, "error": "not found"}, 404)
        except BrokenPipeError:
            pass
        except Exception as exc:                      # noqa: BLE001
            self._send_json({"ok": False, "error": "%s: %s"
                             % (type(exc).__name__, exc)}, 500)

    # ════════════════════════════════════════════
    # 设备上报
    # ════════════════════════════════════════════

    def _ingest(self, body):
        store = self.store
        dev = body.get("device") or "esp32-01"
        recv = int(time.time())

        # 兼容两种写法: 批量 samples[] 或单条 {"up":..,"v":[...]}
        samples = body.get("samples")
        if not samples and "v" in body:
            samples = [{"up": body.get("up"), "v": body.get("v")}]
        samples = samples or []

        up_newest = body.get("up")
        if up_newest is None and samples:
            up_newest = samples[-1].get("up") or 0

        store.register_device(dev, body.get("fw", ""))
        if samples or body.get("relays") is not None:
            store.add_samples(dev, up_newest or 0, samples,
                              body.get("relays") or [], recv)

        if body.get("ack"):
            store.ack_commands(dev, body.get("ack"))

        cmds = store.take_commands(dev)
        pol = store.policy(dev)
        self._send_json({
            "ok": True,
            "epoch": recv,
            "commands": cmds,
            "policy": {
                "maxOn": pol["max_on"],
                "onLinkLoss": pol["on_link_loss"],
                "linkTimeout": pol["link_timeout"],
                "sampleMs": pol["sample_ms"],
            },
        })

    # ════════════════════════════════════════════
    # 仪表盘接口 (与固件 API 同形状)
    # ════════════════════════════════════════════

    def _dev_or_default(self, q):
        return pick_device(self.store, q)

    def _api_get(self, path, q):
        store = self.store
        now = int(time.time())
        dev = self._dev_or_default(q)

        if path == "/api/sensors":
            d = store.latest(dev)
            if not d:
                self._send_json({"ip": "-", "device": None, "online": False,
                                 "age": 0, "rly": [0] * 12,
                                 **FALLBACK, "wdn": "?"})
                return
            out = {"ip": d["device"], "device": d["device"], "age": now - d["ts"],
                   "rly": store.relay_states(d["device"])}
            for c in CHANNELS:
                v = d.get(c)
                out[c] = FALLBACK[c] if v is None else v
            out["wdn"] = compass_name(d.get("wd"))
            devs = {x["device"]: x for x in store.device_list()}
            out["online"] = devs.get(d["device"], {}).get("online", False)
            self._send_json(out)
            return

        if path == "/api/status":
            d = store.latest(dev)
            devname = d["device"] if d else None
            self._send_json({
                "ip": devname or "-",
                "device": devname,
                "online": any(x["online"] for x in store.device_list()),
                "states": store.relay_states(devname),
            })
            return

        if path in ("/api/on", "/api/off", "/api/blink"):
            try:
                ch = int(q.get("id", "0"))
            except ValueError:
                ch = 0
            if not 1 <= ch <= 12:
                self._send_json({"ok": False, "error": "id must be 1..12"}, 400)
                return
            action = path.split("/")[-1]
            d = store.latest(dev)
            devname = (q.get("device") or (d["device"] if d else None))
            if not devname:
                self._send_json({"ok": False, "error": "no device"}, 400)
                return
            store.queue_command(devname, ch, action)
            self._send_json({"ok": True, "queued": {"relay": ch, "action": action}})
            return

        if path == "/api/history":
            self._history(q, dev)
            return

        if path == "/api/history/meta":
            self._history_meta(q, dev)
            return

        if path == "/api/history.csv":
            self._history_csv(q, dev)
            return

        if path in ("/api/history/clear", "/api/history/log/clear"):
            n = store.clear_readings()
            self._send_json({"ok": True, "deleted": n})
            return

        if path in ("/api/history/config", "/api/history/log/config"):
            if q.get("interval"):
                store.set_policy(dev, sample_ms=int(float(q["interval"]) * 1000))
            pol = store.policy(dev)
            self._send_json({"ok": True, "interval": pol["sample_ms"] / 1000.0})
            return

        if path == "/api/rules":
            self._send_json(rules.evaluate(store, dev))
            return

        if path == "/api/devices":
            self._send_json({"devices": store.device_list()})
            return

        if path == "/api/commands":
            self._send_json({"commands": store.recent_commands(
                int(q.get("limit", 20)))})
            return

        if path == "/api/policy":
            pol = store.policy(dev or "esp32-01")
            self._send_json({"ok": True, "policy": pol})
            return

        if path == "/api/time":
            self._send_json({"ok": True, "valid": True, "epoch": now, "offset": 0})
            return

        if path == "/api/ai/config":
            self._send_json({"configured": bool(os.environ.get("DEEPSEEK_API_KEY")),
                             "model": DEEPSEEK_MODEL})
            return

        if path == "/health":
            cov = store.coverage(dev)
            self._send_json({"ok": True, "epoch": now, "readings": cov["count"],
                             "devices": len(store.device_list()),
                             "ai": bool(os.environ.get("DEEPSEEK_API_KEY"))})
            return

        self._send_json({"ok": False, "error": "not found"}, 404)

    # ── 历史数据 ──
    @staticmethod
    def _caps(interval):
        """保留期内的理论点数: (原始点数, 分钟聚合点数)"""
        raw = int(RETENTION_DAYS * 86400 / max(interval, 0.001))
        return raw, RETENTION_DAYS * 1440

    def _history(self, q, dev):
        store = self.store
        src = q.get("src", "ram")
        res = q.get("res", "raw")
        span = int(float(q.get("span") or 0))
        points = int(float(q.get("points") or 180))
        now = int(time.time())

        # Flash 源在 PC 上就是"历史库", 长跨度自动降采样
        if src == "flash" and res == "raw" and span > 86400:
            res = "hour"

        s = store.series(dev, span=span, points=points, res=res)
        pol = store.policy(dev or "esp32-01")
        cov = store.coverage(dev)
        interval = (pol["sample_ms"] or 2000) / 1000.0
        if res == "min":
            interval = 60
        elif res == "hour":
            interval = 3600
        elif res == "day":
            interval = 86400

        cap, cap_min = self._caps(interval)              # 保留期内的理论容量
        cov_min = 0
        if cov["count"]:
            cov_min = min(cov["count"], (cov["last"] - cov["first"]) // 60 + 1)
        self._send_json({
            "src": src,
            "res": res,
            "now": now,
            "interval": interval,
            "span": span,
            "from": (now - span) if span else cov["first"],
            "count": len(s["t"]),
            "total": s["total"],
            "cap": cap,
            "points": len(s["t"]),
            "stride": s["stride"],
            "wall": True,
            "ch": CHANNELS,
            "t": s["t"],
            "v": s["v"],
            "buffers": {
                "raw": {"count": cov["count"], "cap": cap, "interval": interval},
                "min": {"count": cov_min, "cap": cap_min, "interval": 60},
            },
            "log": {
                "ready": True, "records": cov["count"], "capacity": cap,
                "segs": 1, "bytes": os.path.getsize(self.server.db_path),
                "interval": interval, "oldest": cov["first"], "newest": cov["last"],
                "span": max(0, cov["last"] - cov["first"]), "wall": True,
            },
            "clock": {"valid": True, "epoch": now},
        })

    def _history_meta(self, q, dev):
        store = self.store
        pol = store.policy(dev or "esp32-01")
        cov = store.coverage(dev)
        now = int(time.time())
        interval = (pol["sample_ms"] or 2000) / 1000.0
        cap, cap_min = self._caps(interval)
        cov_min = 0
        if cov["count"]:
            cov_min = min(cov["count"], (cov["last"] - cov["first"]) // 60 + 1)
        self._send_json({
            "now": now,
            "interval": interval,
            "rawCount": cov["count"], "rawCap": cap,
            "rawSpan": int(cap * interval),
            "minCount": cov_min, "minCap": cap_min,
            "minSpan": int(cap_min * 60),
            "latest": cov["last"],
            "clock": {"valid": True, "epoch": now},
            "log": {
                "ready": True, "records": cov["count"], "capacity": cap,
                "segs": 1, "bytes": os.path.getsize(self.server.db_path),
                "interval": interval, "oldest": cov["first"], "newest": cov["last"],
                "span": max(0, cov["last"] - cov["first"]), "wall": True,
            },
        })

    def _history_csv(self, q, dev):
        store = self.store
        span = int(float(q.get("span") or 0))
        src = q.get("src", "flash")
        res = q.get("res", "raw")
        if src == "flash" and res == "raw" and span > 86400:
            res = "hour"
        s = store.series(dev, span=span, points=None, res=res)

        lines = ["epoch,datetime," + ",".join(CHANNELS)]
        for i, ts in enumerate(s["t"]):
            tmv = time.gmtime(ts)
            stamp = time.strftime("%Y-%m-%d %H:%M:%S", tmv)
            vals = []
            for c in CHANNELS:
                v = s["v"][c][i]
                vals.append("" if v is None else ("%.1f" % v if abs(v) < 10000 else "%.0f" % v))
            lines.append("%d,%s,%s" % (ts, stamp, ",".join(vals)))
        body = ("\r\n".join(lines) + "\r\n").encode("utf-8")

        self._send_bytes(body, "text/csv; charset=utf-8", 200,
                         {"Content-Disposition": "attachment; filename=agri_history.csv"})

    # ── 策略配置 ──
    def _set_policy(self, q, body):
        store = self.store
        dev = (body.get("device") or q.get("device") or "esp32-01")
        store.set_policy(
            dev,
            max_on=body.get("maxOn", q.get("maxOn")),
            on_link_loss=body.get("onLinkLoss", q.get("onLinkLoss")),
            link_timeout=body.get("linkTimeout", q.get("linkTimeout")),
            sample_ms=body.get("sampleMs", q.get("sampleMs")),
        )
        self._send_json({"ok": True, "policy": store.policy(dev)})

    # ════════════════════════════════════════════
    # AI 代理
    # ════════════════════════════════════════════

    def _build_prompt(self, question, context):
        ctx = json.dumps(context, ensure_ascii=False, indent=1) if context else "{}"
        system = (
            "你是「水肥药一体机」的农艺助手, 服务于温室/大田的水肥药管理。\n"
            "严格约束:\n"
            "1. 只能依据下面提供的实测数据作答, 不得臆造未提供的数值或历史;\n"
            "2. 数据缺失或为 null 时, 明确说明该项无数据, 不要猜测;\n"
            "3. 不要给出具体农药/肥料的品牌或精确配比剂量, 只给方向与浓度区间建议, "
            "并提醒遵守当地法规与安全防护;\n"
            "4. 涉及灌溉/施肥/用药的建议要说明触发依据 (引用具体数值);\n"
            "5. 用简体中文, 先给结论, 再用 3~6 条要点展开, 总长不超过 300 字。"
        )
        user = "问题: %s\n\n当前实测数据与规则引擎结论:\n%s" % (question or "请根据当前数据给出农艺建议", ctx)
        return system, user

    def _ai_context(self, dev):
        """把实测数据 + 规则结论整理成给模型的上下文"""
        store = self.store
        rep = rules.evaluate(store, dev)
        latest = store.latest(dev) or {}
        cov = store.coverage(dev)
        return {
            "device": dev or latest.get("device"),
            "readings": cov["count"],
            "first_ts": cov["first"],
            "last_ts": cov["last"],
            "age_sec": (int(time.time()) - cov["last"]) if cov["last"] else None,
            "sensors": {c: latest.get(c) for c in CHANNELS},
            "derived": rep.get("derived"),
            "trend": rep.get("trend"),
            "health": rep.get("health"),
            "rules": [{"level": i["level"], "title": i["title"],
                       "detail": i["detail"], "action": i["action"]}
                      for i in rep.get("items", [])],
        }

    def _ai(self, body):
        question = (body.get("question") or "").strip()
        dev = self._dev_or_default({})
        # 上下文一律由服务端自行组装, 客户端传来的内容不作为依据
        context = self._ai_context(dev)
        system, user = self._build_prompt(question, context)

        key = os.environ.get("DEEPSEEK_API_KEY")
        if not key:
            self._send_json({
                "ok": True, "dry_run": True, "answer": None,
                "hint": "未设置 DEEPSEEK_API_KEY, 以下是将要发送给模型的内容",
                "system": system, "user": user,
            })
            return

        payload = json.dumps({
            "model": DEEPSEEK_MODEL,
            "messages": [{"role": "system", "content": system},
                         {"role": "user", "content": user}],
            "temperature": 0.3,
            "stream": False,
        }).encode("utf-8")

        req = urllib.request.Request(
            DEEPSEEK_URL, data=payload,
            headers={"Content-Type": "application/json",
                     "Authorization": "Bearer " + key})
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                data = json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", "replace")[:400]
            self._send_json({"ok": False, "error": "HTTP %s: %s"
                             % (exc.code, detail)}, 502)
            return
        except Exception as exc:                      # noqa: BLE001
            self._send_json({"ok": False, "error": "调用失败: %s" % exc}, 502)
            return

        try:
            answer = data["choices"][0]["message"]["content"]
        except (KeyError, IndexError):
            self._send_json({"ok": False, "error": "响应格式异常", "raw": data}, 502)
            return

        self._send_json({"ok": True, "answer": answer,
                         "model": data.get("model", DEEPSEEK_MODEL),
                         "usage": data.get("usage")})

    # ════════════════════════════════════════════
    # 静态文件
    # ════════════════════════════════════════════

    def _static(self, path):
        if path in ("/", ""):
            path = "/index.html"
        rel = os.path.normpath(path.lstrip("/")).replace("\\", "/")
        if rel.startswith("..") or os.path.isabs(rel):
            self._send_json({"ok": False, "error": "bad path"}, 400)
            return
        full = os.path.join(STATIC, rel)
        if not os.path.isfile(full):
            self._send_json({"ok": False, "error": "not found: " + rel}, 404)
            return
        with open(full, "rb") as fh:
            data = fh.read()
        ext = os.path.splitext(full)[1].lower()
        self._send_bytes(data, MIME.get(ext, "application/octet-stream"))


def main():
    ap = argparse.ArgumentParser(description="水肥药一体机 PC 侧服务")
    ap.add_argument("--host", default="0.0.0.0", help="监听地址 (默认全网卡)")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--db", default=os.path.join(BASE, "data", "agri.db"))
    ap.add_argument("--keep-days", type=int, default=90,
                    help="读数保留天数 (0 = 永久)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    store = Store(args.db)
    if args.keep_days > 0:
        removed = store.prune(args.keep_days)
        if removed:
            print("[store] 清理过期读数 %d 条 (保留 %d 天)" % (removed, args.keep_days))

        def janitor():
            """每小时清理一次过期读数, 避免数据库无限膨胀"""
            while True:
                time.sleep(3600)
                try:
                    n = store.prune(args.keep_days)
                    if n:
                        print("[store] 定时清理过期读数 %d 条" % n)
                except Exception as exc:              # noqa: BLE001
                    print("[store] 清理失败: %s" % exc)

        threading.Thread(target=janitor, daemon=True).start()

    Handler.store = store
    Handler.verbose = args.verbose

    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    srv.db_path = args.db

    print("=" * 56)
    print(" 水肥药一体机 PC 服务已启动")
    print(" 仪表盘   : http://%s:%d/" % (args.host if args.host != "0.0.0.0" else "localhost",
                                          args.port))
    print(" 设备上报 : POST http://<本机IP>:%d/api/ingest" % args.port)
    print(" 数据库   : %s" % args.db)
    print(" AI 助手  : %s" % ("已配置 DEEPSEEK_API_KEY"
                              if os.environ.get("DEEPSEEK_API_KEY")
                              else "未配置 (接口会返回待发送的 prompt)"))
    print(" 按 Ctrl+C 停止")
    print("=" * 56)

    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n[server] 已停止")
        srv.server_close()


if __name__ == "__main__":
    main()
