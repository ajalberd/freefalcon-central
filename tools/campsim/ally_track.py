"""Follow China's (team 5) and Russia's (team 4) starting battalions through a run.

    python ally_track.py <run-name> [<run-name> ...]

When China/Russia join, their units are handed to DPRK (team 6), so they are tracked by unit
id from the first frame. Prints, at a few times: battalions alive, vehicles, mean grid y
(north is larger; the front starts near y 470-500), how many are moving, how many have CAPTURE
orders, and how many came within 60 km of the starting front.
"""
import json
import os
import re
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FRONT_Y = 490


def track(name):
    base = os.path.join(HERE, "runs", name)
    log = open(base + ".log", errors="replace").read()
    ev = {int(a): int(b) for a, b in re.findall(r"EVENT (\d+) FIRED at min (\d+)", log)}
    frames = [json.loads(l) for l in open(base + ".jsonl") if l.startswith('{"t"')]
    print("%s: China h%s, Russia h%s, Blue wins %s" % (
        name, ev.get(11, 0) // 60, ev.get(12, 0) // 60,
        "h%d" % (ev[17] // 60) if 17 in ev else "no"))
    for team, label in ((5, "PRC"), (4, "CIS")):
        ids = {u[0] for u in frames[0]["u"] if u[2] == team and u[1] == 0}
        if not ids:
            continue
        hours = sorted({0, 24, 48, int(frames[-1]["t"] / 60)})
        for h in hours:
            f = min(frames, key=lambda f: abs(f["t"] - h * 60))
            b = [u for u in f["u"] if u[0] in ids]
            if not b:
                continue
            print("   %s h%-3d bns %2d  veh %4d  mean y %4.0f  moving %2d  CAPTURE %2d  within 60 km of front %2d" % (
                label, h, len(b), sum(u[5] for u in b), statistics.mean(u[4] for u in b),
                sum(1 for u in b if u[7]), sum(1 for u in b if u[9] == 1),
                sum(1 for u in b if u[4] < FRONT_Y + 60)))


if __name__ == "__main__":
    for n in sys.argv[1:]:
        track(n)
