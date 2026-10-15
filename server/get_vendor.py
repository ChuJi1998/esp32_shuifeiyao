#!/usr/bin/env python3
"""
把前端依赖的 3 个 CDN 库下载到 static/vendor/, 实现完全离线运行。

用法:
  python get_vendor.py            # 缺什么补什么
  python get_vendor.py --force    # 全部重新下载

下载后 index.html 会引用本地文件, 温室里没有外网也能打开页面。
"""

import argparse
import os
import re
import ssl
import sys
import urllib.error
import urllib.request

BASE = os.path.dirname(os.path.abspath(__file__))
STATIC = os.path.join(BASE, "static")
VENDOR = os.path.join(STATIC, "vendor")

FA_VER = "6.4.0"
FA_BASE = "https://cdnjs.cloudflare.com/ajax/libs/font-awesome/%s/" % FA_VER

FILES = [
    ("https://cdn.tailwindcss.com/3.4.16",
     "tailwind.js"),
    (FA_BASE + "css/all.min.css",
     "fontawesome/css/all.min.css"),
    ("https://cdn.jsdelivr.net/npm/chart.js@4.4.1/dist/chart.umd.js",
     "chart.umd.js"),
]


def fetch(url, timeout=60):
    req = urllib.request.Request(url, headers={
        "User-Agent": "Mozilla/5.0 (vendor-fetch)",
    })
    ctx = ssl.create_default_context()
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as r:
            return r.read()
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        print("  下载失败 %s: %s" % (url, exc))
        return None


def save(rel, data):
    path = os.path.join(VENDOR, rel.replace("/", os.sep))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    print("  %-42s %7.1f KB" % (rel, len(data) / 1024.0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    print("下载前端依赖到 static/vendor/ ...")
    missing = 0
    for url, rel in FILES:
        path = os.path.join(VENDOR, rel.replace("/", os.sep))
        if os.path.exists(path) and not args.force:
            print("  %-42s 已存在 (%6.1f KB)"
                  % (rel, os.path.getsize(path) / 1024.0))
        else:
            data = fetch(url)
            if data:
                save(rel, data)
            else:
                missing += 1

    # Font Awesome 的字体文件按 CSS 里的 url() 引用逐个抓取
    fa_dir = "fontawesome"
    css_rel = fa_dir + "/css/all.min.css"
    css_path = os.path.join(VENDOR, css_rel.replace("/", os.sep))
    if os.path.exists(css_path):
        with open(css_path, "r", encoding="utf-8", errors="replace") as f:
            css = f.read()
        urls = sorted(set(re.findall(r"url\([\"']?([^\"')]+)[\"']?\)", css)))
        for u in urls:
            if u.startswith("data:"):
                continue
            # 相对于 css/ 目录解析, 得到 vendor 下的相对路径
            rel_font = os.path.normpath(
                os.path.join(fa_dir, "css", u.replace("/", os.sep)))
            url_rel = rel_font.replace(os.sep, "/")[len(fa_dir) + 1:]
            dst = os.path.join(VENDOR, rel_font)
            if os.path.exists(dst) and not args.force:
                continue
            data2 = fetch(FA_BASE + url_rel)
            if data2:
                save(rel_font.replace(os.sep, "/"), data2)
            else:
                missing += 1
                print("    跳过 %s" % rel_font)

    print("完成。" if not missing else "完成, 但有 %d 项失败。" % missing)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
