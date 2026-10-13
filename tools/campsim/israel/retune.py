"""Retune the seven older Israel campaigns toward a 5-10 day result with a fair split.

python retune.py   applies every edit below to the install; always starts from <file>.bak-pre-retune (made once)
Edits: exact-text replacements in a .tri, a minimum campaign day before the endings can fire (GATE), extra end blocks (ADD),
and ground-vehicle cuts on a .cam (roster bits, like build_modern.py).
"""
import os, shutil, struct, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "campaign-editor"))
from ffcamp import entities, workspace  # noqa: E402

TH = r"C:\FreeFalcon6\Theaters"
TDF = {"Israel": "Theaters/Israel/Israeli.tdf", "Israel2007": "Theaters/Israel2007/Israel2007.tdf",
       "Israel_Classic": "Theaters/Israel_Classic/Israel_Classic.tdf"}

TRI = {
    ("Israel", "save0.tri"): [   # 1967: the West Bank alone fell in hours; add Sinai to the canal and the Hermon
        ("#IF_CONTROLLED 2 A 69 71 74 79 111 171 246 259 733 2112 3045", "#IF_CONTROLLED 2 A 79 111 259 246 223 2796 1908 1008"),
        ("#IF_CAMPAIGN_DAY G 7", "#IF_CAMPAIGN_DAY G 9"),
    ],
    ("Israel_Classic", "save0.tri"): [   # runs as the Classic "1967" button: Israel already holds the old city here, so win = the canal
        ("#IF_CONTROLLED 2 A 1908 79 70 109 262 683 1980", "#IF_CONTROLLED 2 A 504 503 3182 3183 3204 3223 3226"),
        ("#IF_CAMPAIGN_DAY G 6", "#IF_CAMPAIGN_DAY G 8"),
    ],
    ("Israel", "save1.tri"): [   # 1973
        ("#IF_CAMPAIGN_DAY G 18", "#IF_CAMPAIGN_DAY G 10"),
        ("#IF_CONTROLLED 2 A 371 330 1008 1137 1204 1908", "#IF_CONTROLLED 2 A 1008 1137"),
        ("#IF_CONTROLLED 6 A 1908 529 507", "#IF_CONTROLLED 6 A 1908 529 371"),
    ],
    ("Israel_Classic", "save1.tri"): [
        ("#IF_CAMPAIGN_DAY G 18", "#IF_CAMPAIGN_DAY G 10"),
        ("#IF_CONTROLLED 2 A 670 690 1008 1137 1204 1908", "#IF_CONTROLLED 2 A 1008 1137"),
    ],
    ("Israel", "save2.tri"): [   # 1982
        ("#IF_CAMPAIGN_DAY G 30", "#IF_CAMPAIGN_DAY G 9"),
        ("#IF_CONTROLLED 2 A 24 568 523 501 509 515 1856 1846", "#IF_CONTROLLED 2 A 501 503"),
    ],
    ("Israel_Classic", "save2.tri"): [
        ("#IF_CAMPAIGN_DAY G 30", "#IF_CAMPAIGN_DAY G 9"),
        ("#IF_CONTROLLED 2 A 617 523 501 509 515 543 545 550", "#IF_CONTROLLED 2 A 617 523 501 509"),
    ],
    ("Israel2007", "save2.tri"): [   # 2007: nobody won by day 18-25; holding out ten days is an Israeli victory, as in 2004/2012
        ("#IF_CAMPAIGN_DAY G 30\n#DO_EVENT 5\n#END_GAME 5", "#IF_CAMPAIGN_DAY G 10\n#DO_EVENT 5\n#END_GAME 6"),
    ],
}
# no ending may fire before campaign day N (day N = N-1 days elapsed): (theater, file) -> (N, exact #IF_CONTROLLED lines of the end blocks)
GATE = {
    ("Israel", "save0.tri"): (7, ["#IF_CONTROLLED 2 A 79 111 259 246 223 2796 1908 1008"]),
    ("Israel_Classic", "save0.tri"): (5, ["#IF_CONTROLLED 2 A 504 503 3182 3183 3204 3223 3226", "#IF_CONTROLLED 6 A 69 71 74"]),
    ("Israel", "save1.tri"): (5, ["#IF_CONTROLLED 2 A 1008 1137", "#IF_CONTROLLED 6 A 1908 529 371"]),
    ("Israel_Classic", "save1.tri"): (5, ["#IF_CONTROLLED 2 A 1008 1137", "#IF_CONTROLLED 6 A 1908 667 687"]),
    ("Israel", "save2.tri"): (4, ["#IF_CONTROLLED 2 A 501 503"]),
    ("Israel_Classic", "save2.tri"): (5, ["#IF_CONTROLLED 2 A 617 523 501 509"]),
}
# text inserted before the first #ENDSCRIPT
ADD = {
    ("Israel2007", "save2.tri"): "// The Arab alliance also wins by taking Tel Aviv: Tel Aviv, Kiryat Ono, Ben Gurion Airbase\n"
                                 "#IF_CONTROLLED 6 A 183 208 27\n#DO_EVENT 7\n#END_GAME 7\n#ENDIF\n\n",
}
# (theater, save) -> {team: fraction of ground vehicles removed from every battalion}
CUT = {("Israel2007", "save2"): {5: 0.30, 6: 0.28},
       ("Israel", "save0"): {2: 0.30},                 # 1967: Egypt collapsed 5280 -> 900 in under 5 days
       ("Israel", "save2"): {5: 0.40, 6: 0.40},        # 1982: nobody reached Beirut by day 9
       ("Israel_Classic", "save2"): {5: 0.20, 6: 0.20}}   # the Classic "1967": Israel never won


def gate(t, spec):
    """Wrap each listed #IF_CONTROLLED block in #IF_CAMPAIGN_DAY G N ... #ENDIF."""
    if not spec:
        return t
    n, lines = spec
    L = t.split("\n")
    for want in lines:
        k = next(i for i, x in enumerate(L) if x.strip() == want)
        d, e = 0, k
        while e < len(L):
            s_ = L[e].strip()
            if s_.startswith("#IF"):
                d += 1
            elif s_.startswith("#ENDIF"):
                d -= 1
                if d == 0:
                    break
            e += 1
        L.insert(e + 1, "#ENDIF")
        L.insert(k, "#IF_CAMPAIGN_DAY G %d" % n)
    return "\n".join(L)


def edit_tri(th, name, edits):
    p = os.path.join(TH, th, "campaign", name)
    if not os.path.exists(p + ".bak-pre-retune"):
        shutil.copy2(p, p + ".bak-pre-retune")
    src = open(p + ".bak-pre-retune", "rb").read().decode("latin-1")     # always start from the pre-retune file
    crlf = "\r\n" in src
    t = src.replace("\r\n", "\n")
    for old, new in edits:
        assert t.count(old) >= 1, (th, name, old)
        t = t.replace(old, new)
    t = gate(t, GATE.get((th, name)))
    if (th, name) in ADD:
        k = t.index("#ENDSCRIPT")
        t = t[:k] + ADD[(th, name)] + t[k:]
    open(p, "wb").write((t.replace("\n", "\r\n") if crlf else t).encode("latin-1"))
    print("edited", th, name, len(edits))


def cut_cam(th, save, cuts):
    p = os.path.join(TH, th, "campaign", save + ".cam")
    bak = p + ".bak-pre-retune"
    if not os.path.exists(bak):
        shutil.copy2(p, bak)
    shutil.copy2(bak, p)
    ws = workspace.TheaterWorkspace(r"C:\FreeFalcon6", TDF[th])
    cam = ws.units(save + ".cam")
    roff = entities.base_offsets(cam.version)["campId"] + 2 + 4
    raw = bytearray(cam.units_raw)
    gone = 0
    for u in cam.units:
        f = cuts.get(u["owner"])
        if u["kind"] != "battalion" or not f:
            continue
        at = u["_span"][0] + roff
        r = struct.unpack_from("<I", raw, at)[0]
        cnt = [(r >> (2 * g)) & 3 for g in range(16)]
        tot = sum(cnt)
        want = max(1, tot - int(round(tot * f)))
        g = u["id"][0] % 16
        while sum(cnt) > want:
            if cnt[g] > 0 and sum(cnt) > 1:
                cnt[g] -= 1
            g = (g + 1) % 16
        struct.pack_into("<I", raw, at, sum(c << (2 * i) for i, c in enumerate(cnt)))
        gone += tot - sum(cnt)
    cam.members[cam._member_name("uni")] = entities.encode_units(bytes(raw), len(cam.units))
    cam.save(p)
    print("cut", th, save, cuts, gone, "vehicles")


def main():
    for (th, name), edits in TRI.items():
        edit_tri(th, name, edits)
    for (th, save), cuts in CUT.items():
        cut_cam(th, save, cuts)


if __name__ == "__main__":
    main()
