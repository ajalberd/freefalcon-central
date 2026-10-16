"""What does each supply lever do on average?  Least-squares over every run in spread/results.jsonl.

    python spread_levers.py [b|f]        (default: both stagings, with a China-at-the-border term)

Each run's knob string is turned into numbers (usemult, noprod, prodsup, prodrepl, ...), and a few outcomes are regressed on them.  The
coefficient is "the change in the outcome per step of the lever, the others held", with a rough standard error, so a lever whose
effect is inside the noise shows up as such.  Outcomes: the North's front-line battalion supply low point (how much supply is felt),
Blue's share of the objectives at h240, Blue's vehicles over the North's at h240, whether Blue won before h192, and Chinese battalions left.
"""
import json, os, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import supply_spread as S

FEATS = [("usemult", 1.0, "use x (per +1)"), ("noprod", 0.0, "depots/army bases do not produce"),
         ("prodsup", 1.5, "production scale (per -0.5)"), ("prodrepl", 1.0, "replacement scale (per -0.5)"),
         ("noairlift", 0.0, "no airlift credit"), ("startsup", 100.0, "start supply (per -25 points)"),
         ("maxratio", 0.5, "max share of need per resupply (per -0.1)"), ("gndsupdef", 180.0, "ground resupply interval, other orders (per +60 min)"),
         ("sliders", 0.0, "economic sliders up, bridges bombed")]


def features(cfg):
    k = {n: d for n, d, _ in FEATS}
    for tok in S.CONFIGS[cfg].split():
        if tok.startswith("objpri="):
            k["sliders"] = 1.0
            continue
        name, _, val = tok.partition("=")
        if name == "noprod":
            k["noprod"] = 1.0 if val == "1" else 0.0
        elif name == "noairlift":
            k["noairlift"] = 1.0
        elif name in k:
            k[name] = float(val)
    return [k["usemult"] - 1.0, k["noprod"], (1.5 - k["prodsup"]) / 0.5, (1.0 - k["prodrepl"]) / 0.5, k["noairlift"],
            (100.0 - k["startsup"]) / 25.0, (0.5 - k["maxratio"]) / 0.1, (k["gndsupdef"] - 180.0) / 60.0, k["sliders"]]


OUT = [("front_min", "North front supply low point (%)", lambda r: r["front_min"]),
       ("share", "Blue share of objectives at h240 (%)", lambda r: 100.0 * r["h240"]["obj_b"] / max(1, r["h240"]["obj_b"] + r["h240"]["obj_d"])),
       ("vr", "Blue / North vehicles at h240", lambda r: r["h240"]["veh_b"] / max(1.0, r["h240"]["veh_d"])),
       ("win", "Blue won before h192 (0/1)", lambda r: 1.0 if (r["blue_win"] is not None and r["blue_win"] < 192) else 0.0),
       ("china", "Chinese battalions left", lambda r: r["china_alive"]),
       ("low", "North battalions under 25% at h240", lambda r: r["h240"]["low_d"])]


def main():
    sel = sys.argv[1] if len(sys.argv) > 1 else None
    rows = [r for r in S.load() if r["cfg"] in S.CONFIGS and (sel is None or r["stage"] == sel)]
    X = np.array([[1.0] + features(r["cfg"]) + ([1.0 if r["stage"] == "b" else 0.0] if sel is None else []) for r in rows])
    names = ["(constant)"] + [d for _, _, d in FEATS] + (["China at the border"] if sel is None else [])
    print("%d runs, %d configs\n" % (len(rows), len({r["cfg"] for r in rows})))
    for key, label, fn in OUT:
        y = np.array([fn(r) for r in rows], dtype=float)
        beta, res, rank, _ = np.linalg.lstsq(X, y, rcond=None)
        dof = max(1, len(y) - X.shape[1])
        resid = y - X @ beta
        s2 = float(resid @ resid) / dof
        cov = s2 * np.linalg.pinv(X.T @ X)
        se = np.sqrt(np.clip(np.diag(cov), 0, None))
        print("== %s (mean %.2f, residual sd %.2f)" % (label, y.mean(), s2 ** 0.5))
        for n, b, e in zip(names, beta, se):
            if n == "(constant)":
                continue
            flag = "  <-- clear" if e > 0 and abs(b) > 2 * e else ""
            print("   %-52s %+8.2f  (+-%.2f)%s" % (n, b, e, flag))
        print()


if __name__ == "__main__":
    main()
