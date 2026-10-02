"""Air summary per run: python air_report.py save0-fail-s11 ... (jsonl names without extension)"""
import json, sys
def load(n):
    return [json.loads(l) for l in open(n + ".jsonl")]
for n in sys.argv[1:]:
    fr = [f for f in load(n) if "u" in f]
    out = {}
    for t, nm in ((2, "ROK"), (6, "DPRK")):
        by = {}
        for f in fr:
            d = f["t"] // 1440
            c = sum(1 for u in f["u"] if u[1] == 4 and u[2] == t)
            by.setdefault(d, []).append(c)
        out[nm] = [round(sum(v) / len(v), 1) for d, v in sorted(by.items())]
    last = fr[-1]
    print(n, "avg airborne flights/day", out, "obj", last["s"][2][4], last["s"][6][4], "gnd", last["s"][2][0], last["s"][6][0])
