"""Supply chain over time from a campsim timeline's "sp" rows (see SupplyFrame in campsim main.cpp).

    python supply_report.py runs/save0-x-s1.jsonl [--team 6] [--every 6]

Per row: the team pools, the supply % the China/Russia triggers read, what production added and what
distribution delivered in that interval (with road losses), producer capacity, power, and battalion
supply at the front.

"sent" can be negative: a unit above full supply has a negative need, and the engine draws the
excess back into the pool (stock SupplyUnits behaviour, not a logger artefact).
"""
import argparse
import json

# "sp" indices
POOL_S, POOL_F, POOL_R, LVL_S, LVL_F, RAT_S, RAT_F, RAT_R = range(8)
D = 8  # gSupplyDiag starts here, SUPDIAG_* order
PROD_S, PROD_F, PROD_R, SENT_S, GOT_S, SENT_F, GOT_F, RESUP, LOST, NOSRC, REPL_G, REPL_A = range(D, D + 12)
SITES, CAP_S, REFS, CAP_F, CAPTURED, UNPOWERED, DAMAGED, PWR_N, PWR_ST = range(20, 29)
BN, BN_SUP, FRONT_BN, FRONT_SUP, LOW_BN = range(29, 34)

TEAMS = {2: "ROK", 6: "DPRK"}


def rows(path):
    with open(path) as f:
        for line in f:
            try:
                d = json.loads(line)
            except ValueError:
                continue
            if "sp" in d:
                yield d


def pct(a, b):
    return f"{100 * a // b:3d}%" if b else "  - "


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("timeline")
    ap.add_argument("--team", type=int, default=6)
    ap.add_argument("--every", type=int, default=6, help="hours per row")
    a = ap.parse_args()

    frames = list(rows(a.timeline))
    if not frames:
        print("no 'sp' rows (timeline from a campsim without the supply logger)")
        return
    t = a.team
    print(f"{a.timeline}  team {t} {TEAMS.get(t, '')}")
    print(" hour | pool sup fuel  repl | lvl sup fuel | +prod sup fuel repl/int | sent->got sup   fuel  "
          "| fail | repl g/a | sites cap/day  capt unpw dmg | refin fuelcap | power n st "
          "| bn sup | front bn sup | <25%")
    prev = None
    for d in frames:
        h = d["t"] // 60
        if prev is not None and (d["t"] == prev["t"] or (h % a.every and d is not frames[-1])):
            continue
        s = d["sp"][t]
        p = prev["sp"][t] if prev else [0] * len(s)
        dl = lambda k: s[k] - p[k]
        print(f" {h:4d} | {s[POOL_S]:8d} {s[POOL_F]:4d} {s[POOL_R]:5d} | {s[LVL_S]:7d} {s[LVL_F]:4d} "
              f"| {dl(PROD_S):9d} {dl(PROD_F):4d} {dl(PROD_R):4d}     "
              f"| {dl(SENT_S):5d}->{dl(GOT_S):<5d} {dl(SENT_F):5d}->{dl(GOT_F):<5d}"
              f"| {dl(LOST) + dl(NOSRC):4d} | {dl(REPL_G):4d}/{dl(REPL_A):<3d} "
              f"| {s[SITES]:5d} {s[CAP_S]:7d} {s[CAPTURED]:5d} {s[UNPOWERED]:4d} {s[DAMAGED]:3d} "
              f"| {s[REFS]:5d} {s[CAP_F]:7d} | {s[PWR_N]:7d} {s[PWR_ST]:3d} "
              f"| {s[BN]:3d} {s[BN_SUP]:3d} | {s[FRONT_BN]:8d} {s[FRONT_SUP]:3d} | {s[LOW_BN]:4d}")
        prev = d

    first, last = frames[0]["sp"][t], frames[-1]["sp"][t]
    print()
    print(f"totals: produced supply {last[PROD_S]}, fuel {last[PROD_F]}, replacements {last[PROD_R]}")
    print(f"        delivered supply {last[GOT_S]}/{last[SENT_S]} ({pct(last[GOT_S], last[SENT_S])} after road losses), "
          f"fuel {last[GOT_F]}/{last[SENT_F]} ({pct(last[GOT_F], last[SENT_F])})")
    print(f"        resupplies {last[RESUP]}: nothing arrived {last[LOST]}, no source {last[NOSRC]}; "
          f"replacements to battalions {last[REPL_G]}, squadrons {last[REPL_A]}")
    print(f"        supply capacity/day {first[CAP_S]} -> {last[CAP_S]} ({pct(last[CAP_S], first[CAP_S])}), "
          f"fuel {first[CAP_F]} -> {last[CAP_F]}")


if __name__ == "__main__":
    main()
