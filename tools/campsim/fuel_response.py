"""How fast does the aircraft fleet feel a refinery strike?

    python fuel_response.py CONTROL TAG:SIDE:HOUR [TAG:SIDE:HOUR ...]

CONTROL is the run tag with the same stock and no strike; each TAG is a strike run (SIDE = red, blue or both,
HOUR = when the strike started). Reads the AIRFUEL lines of runs/save0-TAG-s*.log (6 h counters: flights, stock,
fuel pool) and prints, for every struck side, the flights per 6 h against the control, mean over seeds:
the hours after the strike until flights are 20 % down (12 h smoothing), the change 24 / 72 / 120 h after, and the
mean stock in four-ship sorties at those points.
"""
import glob
import os
import re
import statistics as st
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PAT = re.compile(r"AIRFUEL h=(\d+) team (\d): .*?(\d+) squadrons, (\d+) aircraft, mean stock (\d+) = (\d+) four-ship sorties each, "
                 r"(\d+) with under 4 sorties; flights (\d+), refused for fuel (\d+)(?:, fuel pool (-?\d+))?")


def load(tag):
    """{team: {hour: [flights, sorties, pool] averaged over seeds}} -- team 2 = Blue, 6 = the North."""
    acc = {}
    for f in glob.glob(os.path.join(HERE, "runs", "save0-%s-s*.log" % tag)):
        for line in open(f, errors="ignore"):
            m = PAT.match(line)
            if m:
                h, t = int(m.group(1)), int(m.group(2))
                acc.setdefault(t, {}).setdefault(h, []).append((int(m.group(8)), int(m.group(6)), int(m.group(10) or 0)))
    return {t: {h: [st.mean(x[i] for x in v) for i in range(3)] for h, v in hs.items()} for t, hs in acc.items()}


def smooth(series, h):
    """Flights over the 12 h ending at hour h (this reading and the one before)."""
    a, b = series.get(h), series.get(h - 6)
    return (a[0] + b[0]) / 2 if a and b else None


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return

    ctl = load(sys.argv[1])
    print("control %s; flights per 6 h against it, struck side only (T20 = hours after the strike to reach -20 %%)" % sys.argv[1])
    print("%-26s %-5s %5s %4s  %6s %6s %6s  %s" % ("run", "side", "hour", "T20", "+24h", "+72h", "+120h", "sorties of stock at +0/+24/+72/+120"))

    for spec in sys.argv[2:]:
        tag, side, hour = spec.split(":")
        hour = int(hour)
        run = load(tag)

        for team in ((2,) if side == "blue" else (6,) if side == "red" else (2, 6)):
            r, c = run.get(team, {}), ctl.get(team, {})
            t20 = None

            for h in range(hour + 12, 241, 6):
                a, b = smooth(r, h), smooth(c, h)

                if a is not None and b and a / b <= 0.8:
                    t20 = h - hour
                    break

            def at(off):
                h = hour + off
                a, b = smooth(r, h), smooth(c, h)
                return "%+5.0f%%" % (100.0 * (a / b - 1)) if a is not None and b else "     -"

            stock = "/".join("%.0f" % r[h][1] if h in r else "-" for h in (hour, hour + 24, hour + 72, hour + 120))
            print("%-26s %-5s %5d %4s  %s %s %s  %s" % (tag, "Blue" if team == 2 else "Red", hour,
                                                         "-" if t20 is None else t20, at(24), at(72), at(120), stock))


main()
