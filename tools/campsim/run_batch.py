"""Run campsim over several seeds in parallel and summarise who wins.

    python run_batch.py --days 10 --seeds 1 2 3 4 [--jobs 4] [--save save0]

Each seed writes runs/<save>-s<seed>.jsonl (played back by viewer.html) and
runs/<save>-s<seed>.log.  Every run uses its own working copy of the exe folder
because the engine writes FFDebug.log beside the exe.
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
EXE_DIR = os.path.normpath(os.path.join(
    HERE, "..", "..", "src", "Falcon4___x64_Release", "Tools", "campsim"))
GAME = r"C:\FreeFalcon6"
BLUE = (1, 2, 3)
RED = (5, 6)   # PRC is folded into DPRK by the engine within the first half hour


def run_one(a, seed):
    runs = os.path.join(HERE, "runs")
    os.makedirs(runs, exist_ok=True)
    name = "%s%s-s%d" % (a.save, ("-" + a.tag) if a.tag else "", seed)
    work = os.path.join(runs, "_work_" + name)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    for f in ["campsim.exe"] + [os.path.basename(p) for p in glob.glob(os.path.join(EXE_DIR, "*.dll"))]:
        shutil.copy(os.path.join(EXE_DIR, f), work)
    tl = os.path.join(runs, name + ".jsonl")
    with open(os.path.join(runs, name + ".log"), "wb") as log:
        t0 = time.time()
        rc = subprocess.run([os.path.join(work, "campsim.exe"), a.game, a.save,
                        str(a.days), str(seed), tl, str(a.every)] + list(a.set),
                       stdout=log, stderr=subprocess.STDOUT, cwd=work)
    shutil.rmtree(work, ignore_errors=True)
    if rc.returncode:
        print("seed %d: exe exited 0x%X" % (seed, rc.returncode & 0xFFFFFFFF))
    return seed, summarise(tl), time.time() - t0


def summarise(path):
    frames, fin = [], None
    if not os.path.exists(path):
        return None
    for ln in open(path):
        o = json.loads(ln)
        if o.get("final"):
            fin = o
        elif not o.get("meta"):
            frames.append(o)
    if not frames:
        return None
    s = lambda f, teams, k: sum(f["s"][t][k] for t in teams)
    a, b = frames[0], frames[-1]
    flips = sum(len(f["o"]) for f in frames[1:])
    # objectives whose owner ended up on the other side of the line, vs the first frame
    return dict(flips=flips, 
        days=b["t"] / 1440, done=bool(fin), endgame=fin["endgame"] if fin else None,
        obj=[(s(a, BLUE, 4), s(a, RED, 4)), (s(b, BLUE, 4), s(b, RED, 4))],
        gnd=[(s(a, BLUE, 0), s(a, RED, 0)), (s(b, BLUE, 0), s(b, RED, 0))],
        air=[(s(a, BLUE, 2), s(a, RED, 2)), (s(b, BLUE, 2), s(b, RED, 2))])


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--days", type=int, default=10)
    p.add_argument("--seeds", type=int, nargs="+", default=[1, 2, 3, 4])
    p.add_argument("--jobs", type=int, default=4)
    p.add_argument("--save", default="save0")
    p.add_argument("--game", default=GAME)
    p.add_argument("--tag", default="", help="name suffix for the run files")
    p.add_argument("--set", nargs="*", default=[], help="campsim knobs, e.g. speed=2 cost=0.5 strip=6:20")
    p.add_argument("--every", type=int, default=60, help="frame interval, campaign minutes")
    a = p.parse_args()
    with ThreadPoolExecutor(a.jobs) as ex:
        for seed, r, wall in ex.map(lambda s: run_one(a, s), a.seeds):
            if not r:
                print("seed %d: no output" % seed)
                continue
            (ob0, od0), (ob1, od1) = r["obj"]
            (gb0, gd0), (gb1, gd1) = r["gnd"]
            print("seed %d  %.1f d  %s  wall %.0fs" % (
                seed, r["days"], "endgame=%s" % r["endgame"] if r["done"] else "INCOMPLETE", wall))
            print("   objectives  Blue %d->%d   DPRK %d->%d   (%d ownership changes)" % (ob0, ob1, od0, od1, r["flips"]))
            print("   ground veh  Blue %d->%d (%.0f%%)  DPRK %d->%d (%.0f%%)" % (
                gb0, gb1, 100.0 * gb1 / max(1, gb0), gd0, gd1, 100.0 * gd1 / max(1, gd0)))


if __name__ == "__main__":
    main()
