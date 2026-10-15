#!/usr/bin/env python3
"""
假设备模拟器 —— 在没有 ESP32 的情况下把 PC 服务跑通

它按真实协议的格式周期上报数据, 并处理服务端返回的继电器命令,
还会故意制造缺失值、断链、传感器掉线等场景, 便于验证规则引擎与页面。

用法:
  python sim_device.py --url http://127.0.0.1:8080 --interval 2 --scenario normal
  # 场景: normal | dry | disease | frost | sensor_fault | linkdrop
"""

import argparse
import json
import math
import random
import sys
import time
import urllib.error
import urllib.request

CHANNELS = ["ws", "wd", "at", "ah", "lux", "sm", "st", "sec", "co2", "o2"]

SCENARIOS = {
    #            ws    wd    at    ah    lux    sm    st    sec   co2   o2
    "normal":   (2.5, 210, 24.0, 62.0, 12000, 45.0, 21.0, 450, 620, 20.9),
    "dry":      (3.2, 190, 30.0, 38.0, 45000, 22.0, 27.0,  900, 520, 20.9),
    "disease":  (0.8, 120, 21.0, 92.0,  3000, 70.0, 19.0,  380, 700, 20.9),
    "frost":    (1.5, 350,  1.5, 78.0,   800, 52.0,  8.0,  300, 480, 20.9),
    "sensor_fault": (2.5, 210, 24.0, 62.0, None, 45.0, 21.0, 450, 620, 20.9),
}


def build_sample(scenario, phase, relays):
    base = SCENARIOS.get(scenario, SCENARIOS["normal"])
    v = []
    for i, b in enumerate(base):
        if b is None:
            v.append(None)
            continue
        # 每个通道加不同的周期波动, 让曲线看起来自然
        wave = math.sin(phase + i) * (0.06 * abs(b) if b else 0.05)
        noise = random.uniform(-0.01, 0.01) * (abs(b) if b else 1)
        val = b + wave + noise
        v.append(round(val, 2))
    return v


def post(url, obj, timeout=5):
    data = json.dumps(obj).encode("utf-8")
    req = urllib.request.Request(url, data=data,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:8080")
    ap.add_argument("--device", default="esp32-sim")
    ap.add_argument("--interval", type=float, default=2.0)
    ap.add_argument("--scenario", default="normal", choices=list(SCENARIOS))
    ap.add_argument("--linkdrop", type=float, default=0,
                    help="每隔多少秒模拟一次断链(持续 8 秒)")
    ap.add_argument("--count", type=int, default=0, help="上报次数, 0 = 无限")
    args = ap.parse_args()

    ingest = args.url.rstrip("/") + "/api/ingest"
    relays = [0] * 12
    pending = []                      # 模拟断链期间的补传缓冲 (与固件行为一致)
    t0 = time.time()
    n = 0
    last_drop = 0.0

    print("[sim] 上报目标 %s (设备 %s, 场景 %s)" % (ingest, args.device, args.scenario))

    while True:
        up = int(time.time() - t0)
        phase = up / 40.0
        sample = {"up": up, "v": build_sample(args.scenario, phase, relays)}

        if args.linkdrop and (up - last_drop) >= args.linkdrop:
            last_drop = up
            missed = max(1, int(8 / args.interval))
            print("[sim] —— 模拟断链 8 秒 (%d 个样本进补传缓冲) ——" % missed)
            time.sleep(8)
            up = int(time.time() - t0)          # 恢复后的当前开机秒
            phase = up / 40.0
            sample = {"up": up, "v": build_sample(args.scenario, phase, relays)}
            for k in range(missed):
                pending.append({
                    "up": up - (missed - k),
                    "v": build_sample(args.scenario, phase - (missed - k) * 0.05, relays),
                })

        samples = ([sample] if not pending
                   else pending[-60:] + [sample])
        payload = {
            "device": args.device,
            "fw": "sim-1.0",
            "up": up,
            "sampleMs": int(args.interval * 1000),
            "samples": samples,
            "relays": relays,
        }
        try:
            res = post(ingest, payload)
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            print("[sim] 上报失败: %s (样本进补传缓冲)" % exc)
            pending.append(sample)
            time.sleep(args.interval)
            continue

        if pending:
            print("[sim] 补传 %d 条" % len(pending))
            pending = []

        for cmd in res.get("commands", []):
            ch = int(cmd.get("relay", 0))
            act = cmd.get("action", "on")
            state = {"on": 1, "blink": 2}.get(act, 0)
            if 1 <= ch <= 12:
                relays[ch - 1] = state
                print("[sim] 执行命令 #%s: 通道 %d → %s" % (cmd.get("id"), ch, act))

        n += 1
        if n % 15 == 0 or n <= 2:
            pol = res.get("policy", {})
            print("[sim] 第 %d 次上报, up=%ds, 间隔 %sms, 策略 maxOn=%s"
                  % (n, up, pol.get("sampleMs"), pol.get("maxOn")))

        if args.count and n >= args.count:
            print("[sim] 已上报 %d 次, 退出" % n)
            return

        time.sleep(args.interval)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n[sim] 已停止")
        sys.exit(0)
