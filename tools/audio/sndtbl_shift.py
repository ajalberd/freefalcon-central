#!/usr/bin/env python3
"""Shift the table volume of the BMS-imported cockpit sounds against FF's own mix.

Why: FF6's in-cockpit F-16 engine sums to about -27 dB (Int.wav is -46 dB mean, EngRumbleInt carries it),
while BMS 4.38's sums to about -6 dB -- BMS's whole mix runs ~21 dB hotter (the afterburner layer ~14 dB).
Sounds imported at BMS's own table volumes (airflow, AoA, gear wind, buffet, wheel brakes, breathing,
ECS, seat motor, canopy latch) therefore played 14-21 dB too loud against the engine. Clicks, guns,
touchdown, tail scrape and AbInt were already levelled to FF's files and are NOT in this list.

The BMS baseline of each row is recorded in <table>.bms-baseline.json the first time, so re-running with a
different --offset always starts from BMS's value instead of stacking shifts.

Usage:
    python sndtbl_shift.py --offset -1800            (hundredths of a dB, applied to every listed id)
    python sndtbl_shift.py --offset -1800 --show     (print, change nothing)
"""

import argparse
import json
import os
import shutil

TABLES = [r"C:\FreeFalcon6\sounds\f4sndtbl.txt", r"C:\FreeFalcon6\Theaters\Israel\sounds\f4sndtbl.txt"]

# id -> name, for the BMS-level environmental/body sounds (see soundfx.h)
IDS = {
    300: "ecsstart", 301: "ecsloop", 302: "ecsend",
    307: "breathcalm", 308: "breathfast", 309: "breathshort",
    310: "buffeting", 311: "wheelbrake",
    317: "canopylock", 318: "canopyunlock",
    319: "seatup", 320: "seatdown",
    321: "airflowcanopy", 322: "aoalowspeed", 323: "aoahighspeed", 324: "gearwind", 325: "windinckpt",
}
# BMS lists gearwind at +500, which DirectSound cannot play (0 is the ceiling); keep that intent in the baseline
BMS_OVERRIDE = {324: 500}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--offset", type=int, required=True, help="hundredths of a dB (e.g. -1800 = -18 dB)")
    ap.add_argument("--show", action="store_true")
    args = ap.parse_args()

    for path in TABLES:
        if not os.path.exists(path):
            continue
        raw = open(path, "rb").read().decode("latin-1")
        lines = raw.split("\r\n")
        base_path = path + ".bms-baseline.json"
        baseline = json.load(open(base_path)) if os.path.exists(base_path) else {}
        n = -1
        changed = []
        for i, line in enumerate(lines):
            if line.startswith("#") or not line.strip():
                continue
            n += 1
            if n not in IDS:
                continue
            cols = line.split("\t")
            key = str(n)
            if key not in baseline:
                baseline[key] = BMS_OVERRIDE.get(n, int(float(cols[5])))
            new = max(-10000, min(0, baseline[key] + args.offset))
            changed.append("%3d %-14s BMS %+6d -> %+6d" % (n, IDS[n], baseline[key], new))
            cols[5] = str(new)
            lines[i] = "\t".join(cols)
        print(path)
        for c in changed:
            print("   " + c)
        if args.show:
            continue
        if not os.path.exists(path + ".bak-mixshift"):
            shutil.copy2(path, path + ".bak-mixshift")
        open(path, "wb").write("\r\n".join(lines).encode("latin-1"))
        json.dump(baseline, open(base_path, "w"), indent=1)


if __name__ == "__main__":
    main()
