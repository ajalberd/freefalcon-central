"""Unattended spread of 10-day Korea Rolling Fire runs: what makes supply matter and still leaves a close, Blue-leaning war.

    python supply_spread.py phase1            screening: every config below, seeds 1 2, China at the border
    python supply_spread.py phase2 [N]        the N best phase-1 configs (default 6), seeds 1-6, China at the border AND forward, plus the control
    python supply_spread.py report [phase]    print the ranked table from spread/results.jsonl

Every run: 10 days, Korea Escalation save0 (China forward, `save0`) or the same with China staged on its own side of the Yalu (`chnb`, make_ally_mod.py
prc-border), the China-joins rule chn_appr2 (ROK holds Sariwon / Hwangju / Anbyon / Wonsan), the North's counteroffensive when 8 allied battalions are at the
front (alliedcounter=1:8:80), noend=1, the engine's fixes at their defaults, plus the supply knobs of the config.  One line per finished run goes to
spread/results.jsonl (hourly numbers are kept in runs/<save>-sp-<config>-s<seed>.jsonl, also viewable in viewer.html).

What "fair, dynamic, Blue-sided by day 10" means here (score, lower is better; spread_score below):
  - Blue holds 50-62% of the two sides' objectives at h240 (they start at 46%),
  - Blue's ground vehicles are 1.0-2.0x the North's at h240,
  - the war does not end before h192 (no Blue win), and it is not a rout either way,
  - supply is felt: the North's front-line battalion supply dips under 85% at some hour, and under 50% is a collapse (penalised).
"""
import json, os, re, subprocess, sys, threading, time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "spread")
RES = os.path.join(OUT, "results.jsonl")
EXE = os.environ.get("CAMPSIM_EXE_DIR", "")
BASE = "usecfg=1 noend=1 tri=chn_appr2 alliedcounter=1:8:80"
PAR = int(os.environ.get("SPREAD_JOBS", "11"))
STAGE = {"b": "chnb", "f": "save0"}
lock = threading.Lock()

NP = "noprod=1"
SLIDERS = "objpri=prod:100,pol:100,depot:100,roads:60"
# name -> campsim knobs (on top of BASE)
CONFIGS = {
    "c0": "",
    "u2": "usemult=2", "u3": "usemult=3", "u4": "usemult=4", "u5": "usemult=5",
    "np": NP, "npu2": NP + " usemult=2", "npu3": NP + " usemult=3", "npu4": NP + " usemult=4",
    "p10": "prodsup=1.0", "p10u2": "prodsup=1.0 usemult=2", "p075u2": "prodsup=0.75 usemult=2",
    "p05": "prodsup=0.5", "p05u2": "prodsup=0.5 usemult=2",
    "npp10": NP + " prodsup=1.0", "npp075u2": NP + " prodsup=0.75 usemult=2", "npp05": NP + " prodsup=0.5",
    "npu3r50": NP + " usemult=3 prodrepl=0.5", "npu3r25": NP + " usemult=3 prodrepl=0.25", "npu3r10": NP + " usemult=3 prodrepl=0.1",
    "u2r25": "usemult=2 prodrepl=0.25", "r25": "prodrepl=0.25", "r10": "prodrepl=0.1",
    "u2na": "usemult=2 noairlift=1", "u3na": "usemult=3 noairlift=1", "npu3na": NP + " usemult=3 noairlift=1", "na": "noairlift=1",
    "npu3s70": NP + " usemult=3 startsup=70", "npu3s50": NP + " usemult=3 startsup=50", "u3s50": "usemult=3 startsup=50",
    "u2mr25": "usemult=2 maxratio=0.25", "npu3mr25": NP + " usemult=3 maxratio=0.25", "npu3mr35": NP + " usemult=3 maxratio=0.35",
    "npu3rs240": NP + " usemult=3 resup=240", "npu3rs480": NP + " usemult=3 resup=480", "rs480": "resup=480",
    "sl": SLIDERS, "npu3sl": NP + " usemult=3 " + SLIDERS, "u2sl": "usemult=2 " + SLIDERS,
    "npu3r50mr35": NP + " usemult=3 prodrepl=0.5 maxratio=0.35",
    "npu3r25na": NP + " usemult=3 prodrepl=0.25 noairlift=1",
    "npu2p075r50": NP + " usemult=2 prodsup=0.75 prodrepl=0.5",
    "npu4r50": NP + " usemult=4 prodrepl=0.5",
    "npu3r50s70mr35": NP + " usemult=3 prodrepl=0.5 startsup=70 maxratio=0.35",
    "npu3r50sl": NP + " usemult=3 prodrepl=0.5 " + SLIDERS,
    "npu3r25sl": NP + " usemult=3 prodrepl=0.25 " + SLIDERS,
    "npu2p05r25": NP + " usemult=2 prodsup=0.5 prodrepl=0.25",
    "npu3rs240r50": NP + " usemult=3 resup=240 prodrepl=0.5",
    "npu3p075": NP + " usemult=3 prodsup=0.75",
    "npu3mr25r50": NP + " usemult=3 maxratio=0.25 prodrepl=0.5",
    "npu3na_sl": NP + " usemult=3 noairlift=1 " + SLIDERS,
    # ground resupply cadence (resup= only touches the squadrons' fuel arithmetic, so npu3rs* above are replicates of npu3)
    "gd360": "gndsupdef=360", "gd720": "gndsupdef=720", "npu3gd360": NP + " usemult=3 gndsupdef=360", "npu3gd720": NP + " usemult=3 gndsupdef=720",
    "npu3go60": NP + " usemult=3 gndsupoff=60", "npu2gd360": NP + " usemult=2 gndsupdef=360", "npu3gd360r50": NP + " usemult=3 gndsupdef=360 prodrepl=0.5",
    # phase 3: push supply hard on purpose (does a starved North change who holds what?)
    "h1": NP + " usemult=4 startsup=50 prodsup=0.5 gndsupdef=360 maxratio=0.3",
    "h2": "usemult=4 gndsupdef=360 startsup=60 prodsup=0.75",
    "h3": NP + " usemult=3 gndsupdef=720 prodsup=0.75",
    "h4": "usemult=5 startsup=50 prodsup=0.5 noairlift=1",
    "h5": NP + " usemult=5 startsup=40 prodsup=0.4 gndsupdef=720 maxratio=0.25 noairlift=1 prodrepl=0.25",
    "h6": NP + " usemult=3 startsup=60 prodsup=0.75 gndsupdef=360 objpri=prod:100,pol:100,depot:100,roads:100",
    # phase 5: tilt the war toward Blue without supply: how hard the China counteroffensive comes
    "t16": NP + " prodsup=0.75 usemult=2 alliedcounter=1:16:80",
    "t24": NP + " prodsup=0.75 usemult=2 alliedcounter=1:24:80",
    "tcap": NP + " prodsup=0.75 usemult=2 alliedcounter=0 alliedcapture=1:8:80",
    "toff": NP + " prodsup=0.75 usemult=2 alliedcounter=0",
    "t10": NP + " prodsup=0.75 usemult=2 alliedcounter=1:10:80",
    "t12": NP + " prodsup=0.75 usemult=2 alliedcounter=1:12:80",
    "t12k120": NP + " prodsup=0.75 usemult=2 alliedcounter=1:12:120",
    "t9": NP + " prodsup=0.75 usemult=2 alliedcounter=1:9:80",
    "m8k100": NP + " prodsup=0.75 usemult=2 alliedcounter=1:8:100",
    "m8k120": NP + " prodsup=0.75 usemult=2 alliedcounter=1:8:120",
    "m9k60": NP + " prodsup=0.75 usemult=2 alliedcounter=1:9:60",
    "m9k100": NP + " prodsup=0.75 usemult=2 alliedcounter=1:9:100",
    "m10k60": NP + " prodsup=0.75 usemult=2 alliedcounter=1:10:60",
    "m10k100": NP + " prodsup=0.75 usemult=2 alliedcounter=1:10:100",
    "g9_2p5": NP + " prodsup=0.75 usemult=2 alliedcounter=1:9:80 hcg=2.5",
    "g9_4": NP + " prodsup=0.75 usemult=2 alliedcounter=1:9:80 hcg=4",
    "g10_2p5": NP + " prodsup=0.75 usemult=2 alliedcounter=1:10:80 hcg=2.5",
    "g10_4": NP + " prodsup=0.75 usemult=2 alliedcounter=1:10:80 hcg=4",
    # phase 10: the same Blue-leaning settings, but Blue cannot end the war before campaign day 9 (chn_appr3.tri gates event 17)
    "d9": NP + " prodsup=0.75 usemult=2 alliedcounter=1:9:80 tri=chn_appr3",
    "d10": NP + " prodsup=0.75 usemult=2 alliedcounter=1:10:80 tri=chn_appr3",
    "dg10": NP + " prodsup=0.75 usemult=2 alliedcounter=1:10:80 hcg=2.5 tri=chn_appr3",
    "d10x": NP + " prodsup=0.75 usemult=2 alliedcounter=1:10:80 tri=chn_appr4",   # Blue cannot end the war before campaign day 11: it always runs the ten days
    "npu3gd360go60": NP + " usemult=3 gndsupdef=360 gndsupoff=60", "npu2gd720go120": NP + " usemult=2 gndsupdef=720 gndsupoff=120",
}


def runname(stage, cfg, seed):
    return "%s-sp-%s-s%d" % (STAGE[stage], cfg, seed)


def hour(fr, h):
    return min(fr, key=lambda x: abs(x["t"] / 60 - h))


def analyse(stage, cfg, seed):
    base = os.path.join(HERE, "runs", runname(stage, cfg, seed))
    fr = [json.loads(l) for l in open(base + ".jsonl") if l.startswith('{"t"')]
    log = open(base + ".log", errors="replace").read()
    ev = {int(a): int(c) / 60.0 for a, c in re.findall(r"EVENT (\d+) FIRED at min (\d+)", log)}
    ids5 = {u[0] for u in fr[0]["u"] if u[2] == 5 and u[1] <= 1}
    e = fr[-1]
    m = {"stage": stage, "cfg": cfg, "seed": seed, "hours": round(e["t"] / 60.0), "china_in": ev.get(11), "blue_win": ev.get(17),
         "dprk_off_pct": round(100.0 * sum(1 for x in fr if x["a"][6][0] == 4) / len(fr)),
         "china_alive": sum(1 for u in e["u"] if u[0] in ids5), "china_start": len(ids5)}
    for h in (72, 144, 240):
        f = hour(fr, h)
        m["h%d" % h] = {"obj_d": f["s"][6][4], "obj_b": f["s"][2][4], "veh_d": f["s"][6][0], "veh_b": f["s"][2][0],
                        "sup_d": f["sp"][6][30], "front_d": f["sp"][6][32], "low_d": f["sp"][6][33], "sup_b": f["sp"][2][30],
                        "prod_d": f["sp"][6][21]}
    fronts = [x["sp"][6][32] for x in fr if x["t"] / 60 >= 12 and x["sp"][6][29] > 0]
    m["front_min"] = min(fronts) if fronts else None
    m["front_mean"] = round(sum(fronts) / len(fronts), 1) if fronts else None
    return m


def score(m):
    """Lower is better.  See the module docstring."""
    f = m["h240"]
    share = f["obj_b"] / float(max(1, f["obj_b"] + f["obj_d"]))
    vr = f["veh_b"] / float(max(1, f["veh_d"]))
    s = 0.0
    s += 12.0 * max(0.0, 0.50 - share, share - 0.62)            # outside 50-62%
    s += 0.6 * max(0.0, 1.0 - vr, vr - 2.0)                      # outside 1.0-2.0
    if m["blue_win"] is not None and m["blue_win"] < 192:
        s += 1.0 + (192 - m["blue_win"]) / 96.0
    if m["china_in"] is None:
        s += 0.5
    fm = m["front_min"]
    if fm is not None:
        if fm >= 85:
            s += 0.4 + (fm - 85) / 50.0                          # supply never felt
        elif fm < 50:
            s += (50 - fm) / 40.0                                # collapse
    return round(s, 3)


def run_one(stage, cfg, seed, days=10):
    name = runname(stage, cfg, seed)
    with lock:
        if os.path.exists(RES):
            for l in open(RES):
                o = json.loads(l)
                if o["stage"] == stage and o["cfg"] == cfg and o["seed"] == seed and o["hours"] >= days * 24 - 2:
                    return o
    tag = "sp-" + cfg
    knobs = (BASE + " " + CONFIGS[cfg]).split()
    cmd = [sys.executable, os.path.join(HERE, "run_batch.py"), "--save", STAGE[stage], "--days", str(days), "--seeds", str(seed),
           "--jobs", "1", "--tag", tag, "--set"] + knobs
    env = dict(os.environ)
    if EXE:
        env["CAMPSIM_EXE_DIR"] = EXE
    t0 = time.time()
    r = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=HERE)
    try:
        m = analyse(stage, cfg, seed)
    except Exception as ex:  # a crashed run: record it so the queue moves on
        m = {"stage": stage, "cfg": cfg, "seed": seed, "error": str(ex), "tail": r.stdout[-300:], "hours": 0}
    m["wall_s"] = round(time.time() - t0)
    if "error" not in m:
        m["score"] = score(m)
    with lock:
        os.makedirs(OUT, exist_ok=True)
        open(RES, "a").write(json.dumps(m) + "\n")
    print("done %-28s %s" % (name, ("score %.2f" % m["score"]) if "score" in m else m.get("error")), flush=True)
    return m


def load():
    out = []
    if os.path.exists(RES):
        for l in open(RES):
            try:
                o = json.loads(l)
            except Exception:
                continue
            if "score" in o:
                out.append(o)
    return out


def ranked(rows):
    """Per (stage, config): mean score and the mean numbers, over the seeds run."""
    g = {}
    for o in rows:
        g.setdefault((o["stage"], o["cfg"]), []).append(o)
    out = []
    for (st, cfg), rs in g.items():
        n = len(rs)
        avg = lambda f: sum(f(r) for r in rs) / n
        share = avg(lambda r: r["h240"]["obj_b"] / float(max(1, r["h240"]["obj_b"] + r["h240"]["obj_d"])))
        out.append({"stage": st, "cfg": cfg, "n": n, "score": avg(lambda r: r["score"]), "share": share,
                    "vr": avg(lambda r: r["h240"]["veh_b"] / float(max(1, r["h240"]["veh_d"]))),
                    "front_min": avg(lambda r: r["front_min"] if r["front_min"] is not None else 100),
                    "front240": avg(lambda r: r["h240"]["front_d"]), "low240": avg(lambda r: r["h240"]["low_d"]),
                    "wins": sum(1 for r in rs if r["blue_win"] is not None),
                    "china_in": [round(r["china_in"]) if r["china_in"] is not None else None for r in rs],
                    "china_alive": avg(lambda r: r["china_alive"]), "scores": [r["score"] for r in rs],
                    "knobs": CONFIGS.get(cfg, "")})
    out.sort(key=lambda r: r["score"])
    return out


def report(stage=None, top=0):
    rows = ranked([o for o in load() if stage in (None, o["stage"])])
    print("%-3s %-18s %2s %6s | %6s %5s %6s %6s %5s %4s | %-22s | knobs" % ("stg", "config", "n", "score", "Bshare", "veh B/D", "fr.min", "fr.240", "<25%", "wins", "China in (h)"))
    for r in rows[:top or None]:
        print("%-3s %-18s %2d %6.2f | %5.0f%% %6.2f %6.0f %6.0f %5.0f %4d | %-22s | %s" % (
            r["stage"], r["cfg"], r["n"], r["score"], 100 * r["share"], r["vr"], r["front_min"], r["front240"], r["low240"], r["wins"],
            ",".join(str(x) for x in r["china_in"]), r["knobs"]))
    return rows


def queue(items):
    print("queue: %d runs, %d at a time" % (len(items), PAR), flush=True)
    with ThreadPoolExecutor(max_workers=PAR) as ex:
        list(ex.map(lambda a: run_one(*a), items))


def main():
    ph = sys.argv[1] if len(sys.argv) > 1 else "report"
    os.makedirs(OUT, exist_ok=True)
    if ph == "phase1":
        queue([("b", cfg, s) for s in (1, 2) for cfg in CONFIGS])
    elif ph == "phase2":
        n = int(sys.argv[2]) if len(sys.argv) > 2 else 6
        queue([("b", cfg, s) for s in (1, 2) for cfg in CONFIGS])   # configs added since phase 1 get their screening seeds first
        rows = [r for r in ranked([o for o in load() if o["stage"] == "b"]) if r["cfg"] != "c0"]
        # a config only counts if supply is felt without a collapse
        good = [r for r in rows if r["front_min"] < 85][: n]
        picks = ["c0"] + [r["cfg"] for r in good]
        print("phase 2 picks:", picks, flush=True)
        items = [(st, cfg, s) for s in (1, 2, 3, 4, 5, 6) for cfg in picks for st in ("b", "f")]
        queue(items)
    elif ph == "phase3":
        queue([("b", cfg, s) for s in (1, 2, 3, 4, 5, 6) for cfg in ("h1", "h2", "h3", "h4", "h5", "h6")])
    elif ph == "phase4":   # more seeds for the two best phase-2 configs and the control, China at the border
        queue([("b", cfg, s) for s in (7, 8, 9, 10, 11, 12) for cfg in ("c0", "npp075u2", "npu3r50mr35")])
    elif ph == "phase5":
        queue([("b", cfg, s) for s in (1, 2, 3, 4, 5, 6) for cfg in ("t16", "t24", "tcap", "toff")])
    elif ph == "phase6":
        queue([("b", cfg, s) for s in (1, 2, 3, 4, 5, 6) for cfg in ("t10", "t12", "t12k120")])
    elif ph == "phase7":
        queue([("b", cfg, s) for s in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10) for cfg in ("t9",)] + [("b", "t10", s) for s in (7, 8, 9, 10, 11, 12)])
    elif ph == "phase8":   # finer settings between the two counteroffensive dial positions: even (8) and Blue-ending (9+)
        queue([("b", cfg, s) for s in (1, 2, 3, 4, 5, 6) for cfg in ("m8k100", "m8k120", "m9k60", "m9k100", "m10k60", "m10k100")])
    elif ph == "phase9":   # slow Blue's ground war a little (aircraft hit chance vs ground units) so a Blue-leaning setting does not end on day 3-5
        queue([("b", cfg, s) for s in (1, 2, 3, 4, 5, 6) for cfg in ("g9_2p5", "g9_4", "g10_2p5", "g10_4")])
    elif ph == "phase10":
        queue([("b", cfg, s) for s in (1, 2, 3, 4, 5, 6) for cfg in ("d9", "d10", "dg10")])
    elif ph == "phase11":   # confirm the day-9-gated settings on six more seeds
        queue([("b", cfg, s) for s in (7, 8, 9, 10, 11, 12) for cfg in ("d9", "d10")])
    elif ph == "phase12":   # the chosen setting (d10) with China forward instead of at the border: 12 seeds
        queue([("f", "d10", s) for s in range(1, 13)])
    elif ph == "phase13":
        queue([("b", "d10x", s) for s in range(1, 13)] + [("f", "d10x", s) for s in range(1, 7)])
    elif ph == "report":
        report(sys.argv[2] if len(sys.argv) > 2 else None, 0)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
