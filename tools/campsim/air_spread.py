"""How is the aircraft fuel stock spread over the squadrons?

    python air_spread.py TAG [TAG ...]

Reads the AIRSPREAD lines of runs/save0-TAG-s*.log (6 h counters, scratch campsim exe10): sorties of fuel per squadron (min, p10,
median, p90, max), squadrons under 2 sorties and the fuel the squadrons ask the pool for. Prints the mean over the seeds at hours 24, 72, 120, 168, 240.
"""
import glob
import os
import re
import statistics as st
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PAT = re.compile(r"AIRSPREAD h=(\d+) team (\d): sorties per squadron min (\d+) p10 (\d+) median (\d+) p90 (\d+) max (\d+); (\d+) under 2 sorties; "
                 r"fuel wanted by the squadrons (-?\d+) points")
HOURS = (24, 72, 120, 168, 240)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return

    print("%-14s %-5s %5s %5s  %5s %6s %6s %6s %6s  %7s %9s" % ("tag", "side", "hour", "seeds", "min", "p10", "median", "p90", "max", "<2 sort", "wanted pts"))

    for tag in sys.argv[1:]:
        acc = {}

        for f in glob.glob(os.path.join(HERE, "runs", "save0-%s-s*.log" % tag)):
            if f.endswith("ffdebug.log"):
                continue

            for line in open(f, errors="ignore"):
                m = PAT.match(line)
                if m and int(m.group(1)) in HOURS:
                    acc.setdefault((int(m.group(2)), int(m.group(1))), []).append([int(m.group(i)) for i in range(3, 10)])

        for team in (2, 6):
            for h in HOURS:
                v = acc.get((team, h))

                if v:
                    c = [st.mean(x[i] for x in v) for i in range(7)]
                    print("%-14s %-5s %5d %5d  %5.0f %6.0f %6.0f %6.0f %6.0f  %7.1f %9.0f" % (tag, "Blue" if team == 2 else "Red", h, len(v), *c))


main()
