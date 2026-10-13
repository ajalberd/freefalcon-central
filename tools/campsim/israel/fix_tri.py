"""Patch the Israel theater campaign scripts (.tri) -- structure bugs, stale objective IDs, end conditions.

python fix_tri.py [--write]     (without --write: writes *.fixed beside nothing, prints the diffs only)

Originals are kept once as <file>.bak-orig. Structural fixes (all scripts):
  T1 offensive/defensive toggle: "#IF_EVENT_PLAYED X / #IF_ON_OFFENSIVE t / ... #DO_EVENT X / #RESET_EVENT Y" can never
     fire (it needs itself); Korea's stock pattern tests the OTHER event: -> "#IF_EVENT_PLAYED Y"
  T2 one-shot blocks "#IF_EVENT_PLAYED N / <cond> / ... #DO_EVENT N" without #ELSE run only after N already fired:
     insert #ELSE, and close the outer #IF_EVENT_PLAYED if its #ENDIF is missing
  T3 falls/regained "#IF_EVENT_PLAYED N / <lost> ... #ENDIF / #ELSE / <taken> ... #DO_EVENT N / #ENDIF": add the
     missing outer #ENDIF, and RESET_EVENT N in the lost branch (else its SHIFT_INITIATIVE repeats every check)
"""
import difflib, os, re, shutil, sys

TH = r"C:\FreeFalcon6\Theaters"


def is_cmd(ln, tok=None):
    s = ln.strip()
    return s.startswith("#") and (tok is None or s.split()[0] == tok)


def nxt(L, i):
    """Index of the next command line after i."""
    j = i + 1
    while j < len(L) and not is_cmd(L[j]):
        j += 1
    return j


def block_end(L, i):
    """i is an #IF line: index of its matching #ENDIF (an #ELSE at depth 0 is noted, not a terminator)."""
    d, j = 0, i
    while j < len(L):
        s = L[j].strip()
        if s.startswith("#ENDSCRIPT"):
            return None
        if s.startswith("#IF"):
            d += 1
        elif s.startswith("#ENDIF"):
            d -= 1
            if d == 0:
                return j
        j += 1
    return None


def structural(L, nl):
    out = list(L)
    i = 0
    while i < len(out):
        m = re.match(r"\s*#IF_EVENT_PLAYED\s+(\d+)\s*$", out[i])
        if not m:
            i += 1
            continue
        n = m.group(1)
        j = nxt(out, i)
        if j >= len(out):
            break
        first = out[j].strip()
        if first.startswith("#ELSE"):
            i = j
            continue
        if first.startswith("#IF_ON_OFFENSIVE") or first.startswith("#IF_ON_DEFENSIVE"):   # T1
            e = block_end(out, j)
            body = [x.strip() for x in out[j:e]]
            r = [b.split()[1] for b in body if b.startswith("#RESET_EVENT")]
            if "#DO_EVENT " + n in body and r and r[0] != n:
                out[i] = out[i].replace(n, r[0], 1)
            i = j
            continue
        if not first.startswith("#IF"):
            i = j
            continue
        e = block_end(out, j)           # the inner condition block
        if e is None:
            i = j
            continue
        inner = [x.strip() for x in out[j:e]]
        after = nxt(out, e)
        aft = out[after].strip() if after < len(out) else ""
        if ("#DO_EVENT " + n) in inner and not aft.startswith("#ELSE"):            # T2
            out.insert(i + 1, "#ELSE")
            e += 1
            after = nxt(out, e)
            if not out[after].strip().startswith("#ENDIF"):
                out.insert(e + 1, "#ENDIF")
            i = e
            continue
        if aft.startswith("#ELSE"):                                                # T3
            k = nxt(out, after)
            if out[k].strip().startswith("#IF"):
                e2 = block_end(out, k)
                if e2 is not None and ("#DO_EVENT " + n) in [x.strip() for x in out[k:e2]]:
                    if not any(x == "#RESET_EVENT " + n for x in inner):
                        out.insert(e, "#RESET_EVENT " + n)
                        e2 += 1
                    if not out[nxt(out, e2)].strip().startswith("#ENDIF"):
                        out.insert(e2 + 1, "#ENDIF")
        i = j
    return out


def sub_ids(L, table):
    """table: {(cmd prefix regex, old id list): new id list} applied to whole-line matches."""
    out = []
    for ln in L:
        s = ln.strip()
        for (pre, old), new in table.items():
            m = re.match(r"(#%s)\s+(.*?)\s*$" % pre, s)
            if m and tuple(int(x) for x in m.group(2).split()) == tuple(old):
                ln = "%s %s" % (m.group(1), " ".join(str(x) for x in new))
                break
        out.append(ln)
    return out


def replace_line(L, old, new, count=1):
    out, done = [], 0
    for ln in L:
        if done < count and ln.strip() == old:
            out.append(new)
            done += 1
        else:
            out.append(ln)
    assert done == count, (old, done)
    return out


def seed(L, ev):
    """Make sure '#SET_EVENT ev' is in the init section (fixes the malformed 'SET_EVENT n#')."""
    out = [("#SET_EVENT %s" % ev) if re.match(r"\s*SET_EVENT\s+\d+#", ln) else ln for ln in L]
    if not any(ln.strip() == "#SET_EVENT %s" % ev for ln in out):
        k = next(i for i, ln in enumerate(out) if ln.strip().startswith("#ENDINIT"))
        out.insert(k, "#SET_EVENT %s" % ev)
    return out


# 1973 objective IDs: Israel_Classic numbering -> the Israel / Israel2007 theaters' numbering
R73 = {("IF_CONTROLLED 2 O", (669, 663, 664,)): [559, 663, 664],
       ("IF_CONTROLLED 6 A", (669, 663, 664,)): [559, 663, 664],
       ("IF_CONTROLLED 2 A", (669, 663, 664,)): [559, 663, 664],
       ("SET_PAK_PRIORITY 2", (669, 100,)): [559, 100], ("SET_PAK_PRIORITY 6", (669, 100,)): [559, 100],
       ("SET_PAK_PRIORITY 2", (1205, 100,)): [1204, 100], ("SET_PAK_PRIORITY 6", (1205, 100,)): [1204, 100],
       ("SET_PAK_PRIORITY 2", (1138, 100,)): [1137, 100], ("SET_PAK_PRIORITY 6", (1138, 100,)): [1137, 100],
       ("IF_CONTROLLED 2 A", (670, 690, 1008, 1137, 1204, 1908,)): [371, 330, 1008, 1137, 1204, 1908]}
# 1982: Classic -> Israel theater (the Lebanese towns kept their numbers; airbases, Chouf villages did not)
R82 = {("IF_CONTROLLED 2 O", (502, 503, 504, 523, 524, 525, 526, 532, 533, 535, 536, 617,)):
           [502, 503, 504, 523, 524, 525, 526, 532, 533, 535, 536, 24],
       ("IF_CONTROLLED 6 A", (502, 520, 521, 522, 527, 532, 533, 535, 536, 553,)): [502, 520, 521, 522, 532, 533, 535, 536],
       ("IF_CONTROLLED 2 A", (502, 520, 521, 522, 527, 532, 533, 535, 536, 553,)): [502, 520, 521, 522, 532, 533, 535, 536],
       ("IF_CONTROLLED 2 O", (545, 549, 550,)): [1856, 624, 1846],
       ("IF_CONTROLLED 6 A", (545, 549, 550,)): [1856, 624, 1846],
       ("IF_CONTROLLED 2 A", (545, 549, 550,)): [1856, 624, 1846],
       ("IF_CONTROLLED 2 O", (617, 618, 509, 559, 640,)): [24, 370, 509, 16],
       ("IF_CONTROLLED 6 A", (617, 618, 640, 527, 637,)): [24, 370, 16, 637],
       ("IF_CONTROLLED 2 A", (617, 618, 640, 527, 637,)): [24, 370, 16, 637],
       ("IF_CONTROLLED 2 A", (617, 523, 501, 509, 515, 543, 545, 550,)): [24, 568, 523, 501, 509, 515, 1856, 1846]}


def fix(theater, save, L):
    nl = "\n"
    key = (theater, save)
    if key in (("Israel", "save0"), ("Israel2007", "save0")):          # 1967, RAM22 layout
        if key == ("Israel", "save0"):
            L = replace_line(L, "//#END_GAME 10", "#END_GAME 10")            # Israel's conquest win, as documented
        L = structural(L, nl)
    elif save == "save1":                                                     # 1973 (same script in all three)
        L = seed(L, 3)
        L = structural(L, nl)
        if theater != "Israel_Classic":
            L = sub_ids(L, R73)
            arab = [1908, 529, 507] if theater == "Israel" else [1908, 561, 225]
            L = sub_ids(L, {("IF_CONTROLLED 6 A", (1908, 667, 687,)): arab})
            if theater == "Israel2007":
                L = replace_line(L, "//  Arabs own Hermon ,Bir Jif Jaffar & Baluza",
                                 "//  Arabs own Hermon, Ramon AB & Ovda AB (Egypt holds all of Sinai in this save)")
    elif key in (("Israel", "save2"), ("Israel_Classic", "save0")):          # 1982
        L = seed(L, 2)                                                        # the author's (malformed) seed
        L = structural(L, nl)
        if theater == "Israel":
            L = sub_ids(L, R82)
    elif key == ("Israel_Classic", "save2"):                                  # 1967, Peled's layout
        L = seed(L, 4)
        L = structural(L, nl)
    elif key == ("Israel2007", "save2"):                                      # 2007
        L = seed(L, 3)
        L = structural(L, nl)
        # the end checks sat inside "US not yet joined & Israel on offensive": close that block first
        k = next(i for i, ln in enumerate(L) if ln.strip() == "#CHANGE_RELATIONS 1 2 1")
        e = next(i for i in range(k, len(L)) if L[i].strip().startswith("#ENDIF"))
        e2 = nxt(L, e)
        assert L[e2].strip().startswith("#ENDIF")
        L[e2 + 1:e2 + 1] = ["#ENDIF", "#ENDIF"]
        # the Syria win was below #ENDSCRIPT (never read), and Syria already holds Hermon: make it Haifa
        s = next(i for i, ln in enumerate(L) if ln.strip().startswith("#IF_CONTROLLED 5 A 1908 391 2145"))
        tail_end = next(i for i in range(s, len(L)) if L[i].strip().startswith("#ENDIF"))
        del L[s:tail_end + 1]
        es = next(i for i, ln in enumerate(L) if ln.strip().startswith("#ENDSCRIPT"))
        L[es:es] = ["", "// Syria (merged into team 6 at the start) wins if it takes Haifa: Bat Galim, Carmel, Hadar,",
                    "// Haifa Naval Base",
                    "#IF_CONTROLLED 6 A 350 284 288 25", "#DO_EVENT 7", "#END_GAME 7", "#ENDIF", ""]
    return L


def main():
    write = "--write" in sys.argv
    out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else None
    for th in ("Israel", "Israel2007", "Israel_Classic"):
        for s in ("save0", "save1", "save2"):
            p = os.path.join(TH, th, "campaign", s + ".tri")
            bak = p + ".bak-orig"
            src = bak if os.path.exists(bak) else p
            raw = open(src, "rb").read().decode("latin-1")
            L = raw.replace("\r\n", "\n").split("\n")
            new = fix(th, s, list(L))
            new = [x.rstrip() for x in new]   # the engine strips only CR/LF: "#ENDIF " is not an ENDIF
            dest = {"save0": "save2", "save2": "save0"}.get(s, s) if th == "Israel_Classic" else s   # Classic .cam scenario names are swapped
            print("=" * 20, th, s)
            for d in difflib.unified_diff(L, new, lineterm="", n=1):
                print(d)
            if out:
                d = os.path.join(out, th); os.makedirs(d, exist_ok=True)
                open(os.path.join(d, dest + ".tri"), "wb").write("\r\n".join(new).encode("latin-1"))
            p = os.path.join(TH, th, "campaign", dest + ".tri")
            bak = p + ".bak-orig"
            if write:
                if not os.path.exists(bak):
                    shutil.copy2(p, bak)
                open(p, "wb").write("\r\n".join(new).encode("latin-1"))


if __name__ == "__main__":
    main()
