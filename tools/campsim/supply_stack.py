"""The supply stack on Korea Rolling Fire over 10 days: the three fixes on (ReplSplitFix, AirliftNeed, AirliftRunway), battalion supply use x1 / x3 / x4,
with and without the factories destroyed.  Reads the runs made by

    run_batch.py --save save0 --days 10 --seeds 1 2 --tag fx-<tag> --set usecfg=1 noend=1 replfix=1 airliftneed=1 airliftrunway=1 usemult=N [blow=factory:red[:hold]]

with tags m1 m3 m4 (no blow), m3f m4f (factories held down), m4r (factories destroyed at the start, repairable).
Prints DPRK / ROK supply over time and, from the PRODMIX lines, what the DPRK economy is made of and where its production went.
    python supply_stack.py [mix]        (mix: also print the PRODMIX table for every scenario, not just the baseline)"""
import json, os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__))
R = os.path.join(HERE, "runs", "save0-fx-%s-s%d.%s")
SC = [("m1", "use x1, factories intact"), ("m3", "use x3, factories intact"), ("m4", "use x4, factories intact"),
      ("m3f", "use x3, factories held down"), ("m4f", "use x4, factories held down"), ("m4r", "use x4, factories destroyed once")]
HRS = [1, 24, 48, 96, 144, 192, 239]
def load(tag, seed):
    f = open(R % (tag, seed, "jsonl")); json.loads(next(f)); fr = []
    for l in f:
        o = json.loads(l)
        if "t" in o: fr.append(o)
    return fr
def at(fr, h): return min(fr, key=lambda x: abs(x["t"] / 60 - h))
def mean(v): return sum(v) / len(v)
data = {}
for tag, _ in SC:
    try: data[tag] = [load(tag, s) for s in (1, 2)]
    except Exception: pass
M = [("DPRK mean battalion supply %", 6, 30), ("DPRK front-line supply %", 6, 32), ("DPRK battalions under 25%", 6, 33),
     ("DPRK supply pool", 6, 0), ("DPRK replacements pool", 6, 2), ("DPRK production capacity /day", 6, 21),
     ("ROK mean battalion supply %", 2, 30), ("ROK battalions under 25%", 2, 33)]
for name, team, idx in M:
    print("\n== %s (mean of 2 seeds) at hour:" % name, "  ".join("%5d" % h for h in HRS))
    for tag, label in SC:
        if tag in data:
            print("  %-34s" % label, "  ".join("%5.0f" % mean([at(fr, h)["sp"][team][idx] for fr in data[tag]]) for h in HRS))
print("\n== DPRK ground vehicles / objectives held at hour:", "  ".join("%5d" % h for h in HRS))
for tag, label in SC:
    if tag in data:
        print("  %-34s" % label, "  ".join("%4.0f/%4.0f" % (mean([at(fr, h)["s"][6][0] for fr in data[tag]]), mean([at(fr, h)["s"][6][4] for fr in data[tag]])) for h in HRS))
print("\n== ground replacements and airlift: DPRK repl given to battalions / aircraft, no-source resupplies, at the last frame")
for tag, label in SC:
    if tag in data:
        print("  %-34s" % label, "  ".join("%s %.0f" % (n, mean([fr[-1]["sp"][6][i] for fr in data[tag]])) for n, i in (("repl-ground", 18), ("repl-air", 19), ("resupplies", 15), ("no-source", 17))))

LINE = re.compile(r"PRODMIX h(\d+) team (\d+) (\w+)\s+n0 (\d+) own (\d+) captured (\d+) damaged (\d+) dead (\d+) \| full (\d+) now (\d+) \| lost: captured (\d+) damage (\d+) power (\d+)")
def mix(tag):
    rows = {}
    for seed in (1, 2):
        try: txt = open(R % (tag, seed, "log"), errors="replace").read()
        except Exception: return None
        for m in LINE.finditer(txt):
            h, team, typ = int(m[1]), int(m[2]), m[3]
            rows.setdefault((team, typ, h), []).append([int(m[i]) for i in range(4, 14)])
    return {k: [sum(c) / len(c) for c in zip(*v)] for k, v in rows.items()}
tags = [t for t, _ in SC if t in data] if "mix" in sys.argv else ["m1"]
for tag in tags:
    mx = mix(tag)
    if not mx: continue
    print("\n== DPRK economy by type, scenario %s (mean of seeds): n0 own captured damaged dead | full now | lost: captured damage power" % tag)
    for h in (6, 24, 72, 120, 234):
        tot = [0.0] * 10
        print("  hour %d" % h)
        for typ in ("factory", "armybase", "depot", "port", "refinery"):
            r = mx.get((6, typ, h))
            if not r: continue
            tot = [a + b for a, b in zip(tot, r)]
            print("    %-9s %3.0f %3.0f %3.0f %3.0f %3.0f | %6.0f %6.0f | %6.0f %6.0f %6.0f" % ((typ,) + tuple(r)))
        print("    %-9s %3.0f %3.0f %3.0f %3.0f %3.0f | %6.0f %6.0f | %6.0f %6.0f %6.0f" % (("all",) + tuple(tot)))
