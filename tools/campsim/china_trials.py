"""China and initiative trials: three seeds per arm, campaigns end when a win condition is met (no noend).

    python china_trials.py run [round]       queue the arms of a round (default: all) through run_batch.py, 3 seeds each
    python china_trials.py report [round]    the table from china/results.jsonl

Per run: how it ended (event 17 Blue holds Pyongyang and Wonsan, 14 the North takes Seoul or Pusan, 15 stalemate at 240 h) and when, Blue's share of the
objectives at the end, the first minute a Chinese battalion fired on a Blue unit / on a Blue battalion (the PRCFIRST line), Chinese volleys at Blue, Blue's initiative
after hour 100, and how many Chinese battalions are within 80 km of Blue's northern edge at the end.
Set CAMPSIM_EXE_DIR to the LTCG build (tools/campsim/build_ltcg.ps1)."""
import json
import os
import re
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "china")
RES = os.path.join(OUT, "results.jsonl")
PAR = int(os.environ.get("TRIAL_JOBS", "12"))
SEEDS = (1, 2, 3)
BASE = "usecfg=1 tri=chn_appr2 alliedcounter=1:8:80 noprod=1 prodsup=0.75 usemult=2"
lock = threading.Lock()

# arm -> (save, extra knobs, round)
ARMS = {
    "b_base": ("chnb", "", 1),              # China staged inland (the 86-battalion Escalation set, y 680-800)
    "d_base": ("chdd", "", 1),              # ... ground battalions at Dandong, the Pyongbu line's Yalu crossing
    "d_spent": ("chdd", "turnmode=1", 1),   # a spent offensive no longer holds the other side down
    "d_shared": ("chdd", "turnmode=4", 1),  # starting an offensive does not force the enemy defensive
    "d_ready": ("chdd", "turnmode=8", 1),   # the allied strike needs fit units
    "d_spent_ready": ("chdd", "turnmode=9", 1),
    "d_all": ("chdd", "turnmode=13", 1),
    "d_indep": ("chdd", "initindep=1", 1),  # per-team initiative instead of zero-sum
    "d_own": ("chdd", "chinaown=1", 1),     # China a team of its own (stock zero-sum initiative)
    "d_own_indep": ("chdd", "chinaown=1 initindep=1", 1),
    "d_own_indep_shared": ("chdd", "chinaown=1 initindep=1 turnmode=4", 1),
    "d_initw": ("chdd", "initw=8:1:1", 1),  # ground-weighted initiative blend
    # round 2: why the Chinese fire so late (they start at 100% supply and burn it neutral, see neutralnouse) and how soon they could
    "d_nn": ("chdd", "neutralnouse=1", 2),
    "d_nn_shared": ("chdd", "neutralnouse=1 turnmode=4", 2),
    "d_nn_join1": ("chdd", "neutralnouse=1 tri=chn_join1", 2),     # China joins on day 1 instead of when the ROK holds Sariwon / Hwangju / Anbyon / Wonsan
    "d_nn_join0": ("chdd", "neutralnouse=1 tri=chn_join0", 2),     # ... at once
    "d_nn_join1_shared": ("chdd", "neutralnouse=1 tri=chn_join1 turnmode=4", 2),
    "f_base": ("chfr", "", 2),                                       # ground battalions 70-150 km inside the North at the start
    "f_nn": ("chfr", "neutralnouse=1", 2),
    "f_nn_shared": ("chfr", "neutralnouse=1 turnmode=4", 2),
    # round 3: China a team of its own, now with tasking on the North's objectives and the North's supply pool
    "o_own": ("chdd", "chinaown=1 neutralnouse=1", 3),
    "o_own_shared": ("chdd", "chinaown=1 neutralnouse=1 turnmode=4", 3),
    "o_own_is": ("chdd", "chinaown=1 neutralnouse=1 initindep=1 turnmode=4", 3),
    "o_own_is_join1": ("chdd", "chinaown=1 neutralnouse=1 initindep=1 turnmode=4 tri=chn_join1", 3),
    "o_own_is_join0": ("chdd", "chinaown=1 neutralnouse=1 initindep=1 turnmode=4 tri=chn_join0", 3),
    "o_own_is_front": ("chfr", "chinaown=1 neutralnouse=1 initindep=1 turnmode=4", 3),
    # round 4: per-team offensive bars (cfg CounterAttackInitiative 15 lets everyone attack at 15; under shared offensives Blue never stops)
    "o_bar": ("chdd", "chinaown=1 neutralnouse=1 initindep=1 turnmode=4 teambar=1:50,2:50,3:50,5:30,6:15", 4),   # Blue 50, China 30, DPRK 15
    "o_bar50": ("chdd", "chinaown=1 neutralnouse=1 initindep=1 turnmode=4 teambar=1:50,2:50,3:50,5:50,6:50", 4),  # stock bar for all
    # round 5: neutralhold -- neutral China stays on the Yalu until it joins (rounds 1-4: its own tasking walked 79 of 86 battalions ~100 km inland by hour 24)
    "h_is": ("chdd", "chinaown=1 neutralnouse=1 neutralhold=1 initindep=1 turnmode=4", 5),
    "h_bar": ("chdd", "chinaown=1 neutralnouse=1 neutralhold=1 initindep=1 turnmode=4 teambar=1:50,2:50,3:50,5:30,6:15", 5),
    "h_bar50": ("chdd", "chinaown=1 neutralnouse=1 neutralhold=1 initindep=1 turnmode=4 teambar=1:50,2:50,3:50,5:50,6:50", 5),
}


def runname(arm, seed):
    return "%s-ct-%s-s%d" % (ARMS[arm][0], arm, seed)


def analyse(arm, seed):
    base = os.path.join(HERE, "runs", runname(arm, seed))
    fr = [json.loads(l) for l in open(base + ".jsonl") if l.startswith('{"t"')]
    log = open(base + ".log", errors="replace").read()
    ev = {int(a): int(c) / 60.0 for a, c in re.findall(r"EVENT (\d+) FIRED at min (\d+)", log)}
    m = {"arm": arm, "seed": seed, "hours": round(fr[-1]["t"] / 60.0, 1)}
    res = re.findall(r"RESULT seed=\d+ endgame=(\d+) engine=(\d+) campaign_h=([\d.]+)", log)
    m["endgame"] = int(res[-1][0]) if res else None
    m["end_h"] = float(res[-1][2]) if res else m["hours"]
    m["blue_win_h"] = ev.get(17)
    m["north_win_h"] = ev.get(14)
    m["china_in_h"] = ev.get(11)
    e = fr[-1]
    m["share_end"] = round(100.0 * e["s"][2][4] / max(1, e["s"][2][4] + e["s"][6][4]), 1)
    pf = re.findall(r"PRCFIRST h=\d+ first fire on another team min (\d+), on a Blue unit min (\d+), on a Blue battalion min (\d+), volleys at Blue (\d+)", log)
    if pf:
        a, b, c, d = (int(x) for x in pf[-1])
        m["prc_first_fire_h"] = round(a / 60.0, 1) if a else None
        m["prc_first_blue_h"] = round(b / 60.0, 1) if b else None
        m["prc_first_blue_bn_h"] = round(c / 60.0, 1) if c else None
        m["prc_volleys_blue"] = d
    pat = re.compile(r"INIT h=(\d+) team 2 vs \d+: .*initiative (\d+)")
    ini = [int(g[1]) for g in pat.findall(log) if int(g[0]) >= 100]
    m["blue_init_after100"] = round(sum(ini) / len(ini), 1) if ini else None
    ids5 = {u[0] for u in fr[0]["u"] if u[2] == 5 and u[1] <= 1}
    blue = sorted(u[4] for u in e["u"] if u[2] in (1, 2, 3) and u[1] == 0)[-10:]
    fy = sum(blue) / len(blue) if blue else 0
    prc = [u for u in e["u"] if u[0] in ids5]
    m["prc_alive"] = len(prc)
    m["prc_near_front_end"] = sum(1 for u in prc if abs(u[4] - fy) <= 80)
    m["prc_capture_orders_end"] = sum(1 for u in prc if u[9] == 1)
    return m


def run_one(arm, seed):
    with lock:
        if os.path.exists(RES) and any(json.loads(l)["arm"] == arm and json.loads(l)["seed"] == seed for l in open(RES)):
            return
    save, knobs, _ = ARMS[arm]
    cmd = [sys.executable, os.path.join(HERE, "run_batch.py"), "--save", save, "--days", "10", "--seeds", str(seed), "--jobs", "1", "--tag", "ct-" + arm,
           "--set"] + (BASE + " " + knobs).split()
    t0 = time.time()
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=HERE)
    try:
        m = analyse(arm, seed)
    except Exception as ex:
        m = {"arm": arm, "seed": seed, "error": str(ex), "tail": r.stdout[-300:]}
    m["wall_s"] = round(time.time() - t0)
    with lock:
        os.makedirs(OUT, exist_ok=True)
        open(RES, "a").write(json.dumps(m) + "\n")
    print("done %-34s end %s h%s share %s" % (runname(arm, seed), m.get("endgame"), m.get("end_h"), m.get("share_end", m.get("error"))), flush=True)


def load():
    seen = {}
    if os.path.exists(RES):
        for l in open(RES):
            r = json.loads(l)
            if "error" not in r:
                seen[(r["arm"], r["seed"])] = r
    return list(seen.values())


def fmt(v, f="%.0f"):
    return "-" if v is None else f % v


def report(rnd=None):
    rows = load()
    print("%-20s %s" % ("arm", "per seed: end(event@hour) share% | first Chinese fire on a Blue unit / battalion (h), volleys | Chinese near front at end | Blue init after h100"))
    for arm, (save, knobs, r) in ARMS.items():
        if rnd and r != rnd:
            continue
        rs = sorted((x for x in rows if x["arm"] == arm), key=lambda x: x["seed"])
        if not rs:
            continue
        cells = []
        for x in rs:
            e = x.get("endgame")
            end = "%s@%s" % (e, fmt(x.get("end_h")))
            cells.append("%s %s%% | %s/%s %s | %s | %s" % (end, fmt(x.get("share_end")), fmt(x.get("prc_first_blue_h"), "%.0f"), fmt(x.get("prc_first_blue_bn_h"), "%.0f"),
                                                       x.get("prc_volleys_blue"), x.get("prc_near_front_end"), fmt(x.get("blue_init_after100"))))
        print("%-20s %s" % (arm, "   ||   ".join(cells)))


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "report"
    rnd = int(sys.argv[2]) if len(sys.argv) > 2 else None
    os.makedirs(OUT, exist_ok=True)
    if cmd == "run":
        items = [(a, s) for s in SEEDS for a, (_, _, r) in ARMS.items() if not rnd or r == rnd]
        print("queue: %d runs, %d at a time" % (len(items), PAR), flush=True)
        with ThreadPoolExecutor(max_workers=PAR) as ex:
            list(ex.map(lambda a: run_one(*a), items))
    else:
        report(rnd)


if __name__ == "__main__":
    main()
