"""Supply tuning on Korea Rolling Fire: starting level, consumption multiplier and store size.  Reads runs made as
    run_batch.py --save save0 --days 5 --seeds 1 2 --tag st-<tag> --set usecfg=1 noend=1 [stocks=1 startsup=50 storestart=50 storecap=25 usemult=N ...]
Prints, for DPRK and ROK, mean battalion supply %, the front line's, and the number of battalions under 25%, at a few hours
(mean of the seeds).  `python supply_tune.py`"""
import json, os
HERE = os.path.dirname(os.path.abspath(__file__))
R = os.path.join(HERE, "runs", "save0-st-%s-s%d.jsonl")
SC = [("ctrl","pool, start 100%, use x1 (control)"),("m3","pool, use x3"),("m5","pool, use x5"),("s50m3","pool, start 50%, use x3"),
      ("k1","stores cap25 start50, use x3"),("k2","stores cap25 start50, use x5"),("k2na","  ... no airlift"),("k2pow","  ... power held down"),
      ("k2arm","  ... army bases held down"),("k2dep","  ... depots held down"),("k2all","  ... everything held down")]
HRS = [1, 24, 48, 72, 96, 119]
def load(tag, seed):
    f = open(R % (tag, seed)); json.loads(next(f)); fr = []
    for l in f:
        o = json.loads(l)
        if "t" in o: fr.append(o)
    return fr
def at(fr, h): return min(fr, key=lambda x: abs(x["t"] / 60 - h))
def mean(v): return sum(v) / len(v)
data = {}
for tag, _ in SC:
    try: data[tag] = [load(tag, s) for s in (1, 2)]
    except Exception as e: pass
for team, nm in ((6, "DPRK"), (2, "ROK")):
    for mname, idx in (("mean battalion supply %", 30), ("front-line supply %", 32), ("battalions under 25%", 33)):
        print("\n== %s %s at hour:" % (nm, mname), "  ".join("%4d" % h for h in HRS))
        for tag, label in SC:
            if tag not in data: continue
            print("  %-38s" % label, "  ".join("%4.0f" % mean([at(fr, h)["sp"][team][idx] for fr in data[tag]]) for h in HRS))
