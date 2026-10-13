"""Supply what-ifs on Korea Rolling Fire: how far does the DPRK economy dive when producers or power plants are destroyed,
and does the front notice?  Reads the runs made by

    for each scenario:  python run_batch.py --save save0 --days 5 --seeds 1 2 --tag sup-<tag> --set usecfg=1 noend=1 blow=<type>:red[:hold]

with tags base, fact1, facth, refh, prodh, pow1, powh, allh (blow=factory:red, factory:red:hold, refinery:red:hold,
producers:red:hold, power:red, power:red:hold, all:red:hold).  Prints pools, production, battalion supply, ground vehicles
and objectives per scenario over time (mean of the seeds), then the war result.  `python supply_whatif.py`
"""
import json, os
HERE = os.path.dirname(os.path.abspath(__file__))
R = os.path.join(HERE, "runs", "save0-sup-%s-s%d.jsonl")
SC = [("base","baseline"),("fact1","factories blown at start (repairable)"),("facth","factories held down"),("refh","refineries held down"),
      ("prodh","all producers held down"),("pow1","power plants blown at start (repairable)"),("powh","power plants held down"),("allh","producers + power held down")]
HRS = [1, 12, 24, 48, 72, 96, 119]
def load(tag, seed):
    f = open(R % (tag, seed)); json.loads(next(f)); fr = []
    for l in f:
        o = json.loads(l)
        if "t" in o: fr.append(o)
    return fr
def at(fr, h):
    return min(fr, key=lambda x: abs(x["t"] / 60 - h))
def mean(v): return sum(v) / len(v)
metrics = {
 "supply pool":   lambda x: x["sp"][6][0], "fuel pool": lambda x: x["sp"][6][1], "repl pool": lambda x: x["sp"][6][2],
 "prod cap/day":  lambda x: x["sp"][6][21], "bn mean supply%": lambda x: x["sp"][6][30], "front supply%": lambda x: x["sp"][6][32],
 "bn <25% supply": lambda x: x["sp"][6][33], "ground veh": lambda x: x["s"][6][0], "objectives": lambda x: x["s"][6][4],
}
data = {tag: [load(tag, s) for s in (1, 2)] for tag, _ in SC}
for mname, fn in metrics.items():
    print("\n== DPRK %s (mean of 2 seeds) at hour:" % mname, "  ".join("%4d" % h for h in HRS))
    for tag, label in SC:
        row = [mean([fn(at(fr, h)) for fr in data[tag]]) for h in HRS]
        print("  %-42s" % label, "  ".join("%4.0f" % v for v in row))
print("\n== war result")
for tag, label in SC:
    out = []
    for fr in data[tag]:
        t17 = next((x["t"] / 60 for x in fr if 17 in (x.get("ev") or [])), None)
        last = fr[-1]
        out.append("Blue wins h%s, ROK obj %d, DPRK obj %d" % ("%.0f" % t17 if t17 else "never", last["s"][2][4], last["s"][6][4]))
    print("  %-42s" % label, " | ".join(out))
