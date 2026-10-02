"""Compare the start-of-war order of battle of the Korea 'Red Herring' save0 in
FreeFalcon6, vanilla Falcon 4.0 and BMS 4.37.

    python compare.py            # markdown to stdout
"""
import collections
import saves

SIDES = [("Blue (US+ROK+JP)", {1, 2, 3}), ("DPRK", {6}), ("PRC", {5}), ("CIS", {4})]
TEAM = {1: "US", 2: "ROK", 3: "Japan", 4: "CIS", 5: "PRC", 6: "DPRK"}
GRO_AIRDEFENSE = 5


def side_of(t):
    for n, s in SIDES:
        if t in s:
            return n
    return None


def strength(units):
    out = collections.defaultdict(collections.Counter)
    for u in units:
        n = side_of(u["owner"])
        if n is None:
            continue
        k, c = u["kind"], out[n]
        c["units"] += 1
        c["u_" + k] += 1
        if k == "battalion":
            key = "ad" if u["role"] == GRO_AIRDEFENSE else "gnd"
            c[key] += u["veh"]
            c[key + "_full"] += u["full"]
        elif k == "squadron":
            c["air"] += u["veh"]
            c["air_full"] += u["full"]
        elif k == "taskforce":
            c["sea"] += u["veh"]
    return out


def table(headers, rows):
    o = ["| " + " | ".join(headers) + " |", "|" + "---|" * len(headers)]
    o += ["| " + " | ".join(str(c) for c in r) + " |" for r in rows]
    return "\n".join(o)


def ratio(a, b):
    return "%.2fx" % (a / b) if b else "inf"


def main():
    S = {k: saves.load(k) for k in ("vanilla", "bms", "ff6")}
    ST = {k: strength(v["units"]) for k, v in S.items()}
    print("# Korea 'Red Herring' save0 - order of battle\n")
    hdr = ["save", "cam ver", "start clock", "units", "objectives"]
    print(table(hdr, [[v["label"], v["cam"].version,
                       "%02d:00" % (v["cam"].header.fields["CurrentTime"] // 3600000 % 24),
                       len(v["units"]), len(v["objs"]) or "n/a"] for v in S.values()]))
    print("\n## Per side (vehicles = engine roster count, aircraft counted per airframe)\n")
    rows = []
    for k, v in S.items():
        for n, _ in SIDES:
            c = ST[k][n]
            rows.append([v["label"], n, c["units"], c["u_battalion"], c["u_brigade"],
                         c["u_squadron"], c["u_taskforce"], c["gnd"], c["ad"],
                         c["air"], c["sea"]])
    print(table(["save", "side", "units", "bns", "bdes", "sqns", "TFs",
                 "ground veh", "AD veh", "aircraft", "ships"], rows))
    print("\n## Balance DPRK : Blue\n")
    rows = []
    for m, key in (("units", "units"), ("battalions", "u_battalion"),
                   ("ground vehicles", "gnd"), ("air-defense vehicles", "ad"),
                   ("aircraft", "air"), ("ships", "sea")):
        r = [m]
        for k in S:
            d, b = ST[k]["DPRK"][key], ST[k]["Blue (US+ROK+JP)"][key]
            r.append("%d : %d (%s)" % (d, b, ratio(d, b)))
        rows.append(r)
    print(table(["metric"] + [v["label"] for v in S.values()], rows))
    print("\n## Ground composition by unit type (vehicles)\n")
    for k, v in S.items():
        print("**%s**\n" % v["label"])
        for n in ("Blue (US+ROK+JP)", "DPRK"):
            c = collections.Counter()
            for u in v["units"]:
                if side_of(u["owner"]) == n and u["kind"] == "battalion":
                    c[u["name"].strip() or "?"] += u["veh"]
            print("- %s: %s" % (n, ", ".join("%s %d" % kv for kv in c.most_common(9))))
        print()
    print("## Aircraft per owner and squadron type\n")
    for k, v in S.items():
        print("**%s**\n" % v["label"])
        for t in (1, 2, 3, 5, 6):
            c = collections.Counter()
            for u in v["units"]:
                if u["owner"] == t and u["kind"] == "squadron":
                    c[u["name"].strip() or "?"] += u["veh"]
            if c:
                print("- %s: %s" % (TEAM[t], ", ".join("%s %d" % kv for kv in c.most_common(8))))
        print()


main()
