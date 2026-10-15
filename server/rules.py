"""
农艺规则引擎 (PC 侧)

从 SQLite 读取最新读数、近期趋势与数据有效率, 生成与设备侧
/api/rules 完全相同结构的建议列表。规则完全离线, 毫秒级返回。

建议条目结构:
    {"id": str, "level": 0..3, "title": str, "detail": str, "action": str}
    level: 0=OK 1=INFO 2=WARN 3=CRIT
"""

import time

from store import CHANNELS

TREND_WINDOW = 1800      # 趋势窗口 (秒)
HEALTH_POINTS = 60       # 有效率统计点数
MAX_ITEMS = 10

CH_NAMES = {
    "ws": "风速", "wd": "风向", "at": "气温", "ah": "湿度", "lux": "光照",
    "sm": "土壤湿度", "st": "土壤温度", "sec": "土壤EC", "co2": "CO2", "o2": "氧气",
}


def _num(x):
    return None if x is None else float(x)


def _dew(at, ah):
    import math
    g = math.log(ah / 100.0) + (17.27 * at) / (237.7 + at)
    return (237.7 * g) / (17.27 - g)


def _vpd(at, ah):
    import math
    return 0.6108 * math.exp((17.27 * at) / (at + 237.3)) * (1 - ah / 100.0)


def evaluate(store, device=None):
    now = int(time.time())

    rep = {
        "src": "rules",
        "now": now,
        "clock": {"valid": True, "epoch": now},
        "hasData": False,
        "metrics": {c: None for c in CHANNELS},
        "derived": {"vpd": None, "dew": None},
        "trend": {"valid": False, "span": 0},
        "health": {c: 0 for c in CHANNELS},
        "worst": 0,
        "levelCount": [0, 0, 0, 0],
        "items": [],
    }

    latest = store.latest(device)
    if not latest:
        return rep

    m = {c: _num(latest.get(c)) for c in CHANNELS}
    rep["hasData"] = True
    rep["metrics"] = m

    at, ah = m["at"], m["ah"]
    if at is not None and ah is not None and 0.5 < ah <= 100:
        rep["derived"]["vpd"] = round(_vpd(at, ah), 3)
        rep["derived"]["dew"] = round(_dew(at, ah), 2)

    # ── 趋势: 窗口内首尾差 (跳过缺失值) ──
    series = store.series(device, span=TREND_WINDOW, points=600, res="raw")
    ts = series.get("t") or []
    v = series.get("v") or {}
    trend = {"valid": False, "span": 0}
    if len(ts) >= 4:
        trend["span"] = ts[-1] - ts[0]
        for c in CHANNELS:
            arr = [x for x in (v.get(c) or [])]
            idx = [i for i, x in enumerate(arr) if x is not None]
            if len(idx) >= 2:
                trend[c] = round(arr[idx[-1]] - arr[idx[0]], 3)
                trend["valid"] = True
    rep["trend"] = trend

    # ── 数据有效率 ──
    rows = store.all_rows(device, span=TREND_WINDOW)
    tail = rows[-HEALTH_POINTS:] if rows else []
    if tail:
        for c in CHANNELS:
            ok = sum(1 for r in tail if r[c] is not None)
            rep["health"][c] = int(ok * 100 / len(tail))
    worst_ch = min(CHANNELS, key=lambda c: rep["health"][c]) if tail else None

    # ════════════════════════════════════════════
    # 规则
    # ════════════════════════════════════════════
    items = []

    def add(rid, level, title, action, detail):
        rep["levelCount"][level] += 1
        rep["worst"] = max(rep["worst"], level)
        if len(items) < MAX_ITEMS:
            items.append({"id": rid, "level": level, "title": title,
                          "detail": detail, "action": action})

    ws, lux, sm, st, sec, co2, o2 = (m["ws"], m["lux"], m["sm"],
                                     m["st"], m["sec"], m["co2"], m["o2"])
    vpd, dew = rep["derived"]["vpd"], rep["derived"]["dew"]

    # 1) 土壤水分 → 灌溉
    if sm is not None:
        if sm < 25:
            add("soil_very_dry", 3, "土壤严重缺水", "立即灌溉",
                "土壤湿度 %.1f%% (阈值 25%%)" % sm)
        elif sm < 35:
            add("soil_dry", 2, "土壤偏干", "建议尽快灌溉",
                "土壤湿度 %.1f%% (适宜 35~75%%)" % sm)
        elif sm > 85:
            add("soil_wet", 2, "土壤过湿", "暂停灌溉, 检查排水",
                "土壤湿度 %.1f%% (上限 85%%)" % sm)

        dsm = trend.get("sm")
        if dsm is not None and dsm < -8:
            add("soil_drop", 2, "土壤水分快速下降",
                "缩短灌溉间隔或延长单次时长",
                "近 %d 分钟下降 %.1f 个百分点"
                % (trend["span"] // 60, -dsm))

    # 2) EC / 施肥
    if sec is not None:
        if sec > 2500:
            add("ec_crit", 3, "电导率严重偏高", "立即淋洗(灌溉冲洗)",
                "EC %.0f μS/cm (上限 2500)" % sec)
        elif sec > 1500:
            add("ec_high", 2, "盐分偏高", "建议淋洗降低 EC",
                "EC %.0f μS/cm (适宜 200~1500)" % sec)
        elif sec < 200:
            add("ec_low", 1, "养分浓度偏低", "可追施水溶肥",
                "EC %.0f μS/cm (适宜 200~1500)" % sec)

    # 3) 高湿病害风险
    if ah is not None and at is not None:
        if ah > 85 and 15 <= at <= 28:
            add("disease", 2, "高湿病害风险", "通风降湿, 必要时预防性用药",
                "湿度 %.1f%% + %.1f°C (病害高发区间)" % (ah, at))
        elif ah < 30:
            add("air_dry", 1, "空气干燥", "注意加湿或遮阳", "空气湿度 %.1f%%" % ah)

    # 4) 温度胁迫
    if at is not None:
        if at < 3:
            add("frost", 3, "低温冻害风险", "加温或覆盖保温",
                "气温 %.1f°C (阈值 3°C)" % at)
        elif at < 8:
            add("cold", 2, "气温偏低", "注意保温", "气温 %.1f°C" % at)
        elif at > 35:
            add("heat", 2, "高温胁迫", "加强通风遮阳",
                "气温 %.1f°C (上限 35°C)" % at)
    if st is not None and st < 12:
        add("soil_cold", 1, "土温偏低, 根系活性弱", "减少单次灌溉量",
            "土温 %.1f°C (适宜 12~28°C)" % st)

    # 5) 蒸发需求 VPD
    if vpd is not None:
        if vpd > 2.0:
            add("vpd_high", 2, "蒸发需求过高", "通风或加湿, 增加灌溉频次",
                "VPD %.2f kPa (上限 2.0)" % vpd)
        elif vpd < 0.2:
            add("vpd_low", 1, "蒸发过弱", "加强通风",
                "VPD %.2f kPa (下限 0.2)" % vpd)

    # 6) 风 / 喷药窗口
    if ws is not None:
        if ws > 8:
            add("wind_strong", 2, "风速过大", "不宜喷药/喷灌 (飘移风险)",
                "风速 %.1f m/s (上限 8)" % ws)
        elif 1 <= ws <= 5 and ah is not None and ah < 85 and at is not None \
                and 10 <= at <= 30 and (lux is None or lux < 30000):
            add("spray_window", 0, "适宜喷药窗口", "可安排在此时段作业",
                "风速 %.1f m/s, %.1f°C, 湿度 %.1f%%" % (ws, at, ah))

    # 7) CO2 / 氧气
    if co2 is not None and co2 > 1500:
        add("co2_high", 1, "CO2 浓度偏高", "适当通风",
            "CO2 %.0f ppm (上限 1500)" % co2)
    if o2 is not None and o2 < 19.5:
        add("o2_low", 2, "氧气浓度偏低", "加强通风",
            "氧气 %.1f%% (正常 20.9%%)" % o2)

    # 8) 传感器数据健康
    if worst_ch and rep["health"][worst_ch] < 50:
        add("sensor_health", 2, "传感器数据缺失", "检查该路传感器接线与供电",
            "%s 有效率仅 %d%% (最近 %d 点)"
            % (CH_NAMES.get(worst_ch, worst_ch), rep["health"][worst_ch], len(tail)))

    # 9) 断链告警 (PC 侧独有)
    devs = [d for d in store.device_list() if d.get("age", 0) >= 0]
    offline = [d for d in devs if not d.get("online")]
    if offline:
        add("device_offline", 3, "采集设备掉线",
            "检查设备供电与 WiFi, 断链期间的自动化已暂停",
            "%s 已失联 %d 秒" % (offline[0]["device"], offline[0]["age"]))

    if not items:
        add("all_ok", 0, "各项指标正常", "保持当前管理方式",
            "已检查 %d 项指标" % len(CHANNELS))

    items.sort(key=lambda x: -x["level"])
    rep["items"] = items
    return rep
