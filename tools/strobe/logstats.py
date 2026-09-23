#!/usr/bin/env python3
"""logstats.py -- turn one launch's player_log.txt into the numbers the test
table records: sync, frame, vsync, gpu and the jellywave build/upload lines.

    logstats.py player_log.txt [--json out.json] [--home-seconds 25]

player_log.txt is truncated at every launch (plog.cpp opens it "w"), so one
pull after one protocol run is exactly one launch.

The xmb: line is emitted every 60 frames.  Windows reported:
  home    the first --home-seconds of xmb lines after the first two (warm-up:
          thumbnails still streaming in), which the protocol spends on Home
  all     every xmb line
  tail    the last 10 s -- the protocol ends back on Home
  per-profile, when the build carries ui_strobe_test's `strobe=N:name` field

SYNC is the number that separates the controls: it is rsxSync() after the
wave + card submission, i.e. how long the RSX took to finish that work.
"""

import argparse
import json
import re
import signal
import statistics
import sys
from datetime import datetime

RX_TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\]\s*(.*)$")
RX_KV = re.compile(r"(\w+)=([-\d.]+)")


def parse(path):
    xmb, jw, other = [], [], []
    for line in open(path, "r", errors="replace"):
        m = RX_TS.match(line.rstrip("\n"))
        if not m:
            continue
        ts = datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S")
        body = m.group(2)
        if body.startswith("xmb: frame="):
            kv = {k: float(v) for k, v in RX_KV.findall(body)}
            sm = re.search(r"strobe=(\d+):(\S+)", body)
            kv["profile"] = "%s:%s" % (sm.group(1), sm.group(2)) if sm else None
            kv["ts"] = ts
            xmb.append(kv)
        elif body.startswith("jellywave:"):
            kv = {k: float(v) for k, v in RX_KV.findall(body)}
            kv["ts"] = ts
            jw.append(kv)
        elif body.startswith(("wave:", "strobe_test:", "theme:", "card_gpu:", "text_gpu:")):
            other.append((ts, body))
    return xmb, jw, other


def summ(rows, key):
    v = [r[key] for r in rows if key in r]
    if not v:
        return None
    v.sort()
    def pct(p):
        return v[min(len(v) - 1, int(round(p * (len(v) - 1))))]
    return dict(n=len(v), med=statistics.median(v), min=v[0], p10=pct(0.10),
                p90=pct(0.90), max=v[-1])


def window(xmb, name, rows):
    out = {"window": name, "lines": len(rows)}
    if rows:
        out["from"] = rows[0]["ts"].strftime("%H:%M:%S")
        out["to"] = rows[-1]["ts"].strftime("%H:%M:%S")
    for k in ("frame", "sync", "vsync", "gpu", "text", "chrome", "cards", "tgpu", "other"):
        s = summ(rows, k)
        if s:
            out[k] = s
    return out


def fmt(w):
    def g(k, scale=1.0, unit="us"):
        s = w.get(k)
        if not s:
            return "%s=n/a" % k
        return "%s=%.0f%s[%.0f..%.0f]" % (k, s["med"] * scale, unit, s["p10"] * scale, s["p90"] * scale)
    fr = w.get("frame")
    frs = "frame=%.2fms[%.2f..%.2f]" % (fr["med"], fr["p10"], fr["p90"]) if fr else "frame=n/a"
    return "%-10s n=%-3d %s %s %s %s" % (w["window"], w["lines"], frs, g("sync"), g("vsync"), g("gpu"))


def main():
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--json")
    ap.add_argument("--home-seconds", type=float, default=25.0)
    a = ap.parse_args()

    xmb, jw, other = parse(a.log)
    res = {"log": a.log, "xmb_lines": len(xmb), "jellywave_lines": len(jw), "windows": []}
    if not xmb:
        print("no xmb: frame= lines -- was plog enabled (jellyfin_settings.txt) and did the XMB run?")
        if a.json:
            json.dump(res, open(a.json, "w"), indent=1, default=str)
        sys.exit(1)

    steady = xmb[2:] if len(xmb) > 4 else xmb
    t0 = steady[0]["ts"]
    home = [r for r in steady if (r["ts"] - t0).total_seconds() <= a.home_seconds]
    tend = xmb[-1]["ts"]
    tail = [r for r in xmb if (tend - r["ts"]).total_seconds() <= 10.0]
    wins = [window(xmb, "home", home), window(xmb, "all", xmb), window(xmb, "tail", tail)]
    profs = []
    for r in xmb:
        if r["profile"] and r["profile"] not in profs:
            profs.append(r["profile"])
    for p in profs:
        rows = [r for r in xmb if r["profile"] == p]
        wins.append(window(xmb, p, rows[1:] if len(rows) > 2 else rows))
    res["windows"] = wins

    if jw:
        res["jellywave"] = {k: summ(jw, k) for k in ("gen", "amort", "up", "verts", "draws",
                                                     "repaired", "dropped") if summ(jw, k)}
    res["config_lines"] = [b for _, b in other if b.startswith(("wave:", "theme:"))][:8]

    print("log      %s" % a.log)
    for b in res["config_lines"]:
        print("  %s" % b)
    for w in wins:
        print("  " + fmt(w))
    if jw:
        j = res["jellywave"]
        def m(k):
            return ("%.0f" % j[k]["med"]) if k in j else "n/a"
        rep = j.get("repaired", {}).get("max", 0) if "repaired" in j else "n/a"
        drp = j.get("dropped", {}).get("max", 0) if "dropped" in j else "n/a"
        print("  jellywave gen=%sus up=%sus verts=%s draws=%s repaired(max)=%s dropped(max)=%s"
              % (m("gen"), m("up"), m("verts"), m("draws"), rep, drp))
    h = wins[0]
    if "sync" in h and "frame" in h:
        print("TABLE    sync=%.1fms frame=%.2fms vsync=%.1fms"
              % (h["sync"]["med"] / 1000.0, h["frame"]["med"], h["vsync"]["med"] / 1000.0))
    if a.json:
        json.dump(res, open(a.json, "w"), indent=1, default=str)


if __name__ == "__main__":
    main()
