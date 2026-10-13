"""Run campsim over several seeds in parallel and summarise who wins.

    python run_batch.py --days 10 --seeds 1 2 3 4 [--jobs 4] [--save save0]
    python run_batch.py --theater israel_classic --save save1 --days 19 --set usecfg=1

Each seed writes runs/<save>[-<theater>][-<tag>]-s<seed>.jsonl (played back by viewer.html) and
the matching .log.  Every run uses its own working copy of the exe folder because the engine
writes FFDebug.log beside the exe.  --theater picks another theater from the game's theater.lst
(see README "Other theaters").
"""
import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
EXE_DIR = os.path.normpath(os.environ.get("CAMPSIM_EXE_DIR") or os.path.join(
    HERE, "..", "..", "src", "Falcon4___x64_Release", "Tools", "campsim"))  # CAMPSIM_EXE_DIR: a frozen copy
# gamework/ when it exists (as the README says): it holds the variant saves and .tri scripts (both,
# bothwr, ...) and fixed player options, so results do not shift when the install's pilot settings
# change. Running against the install silently read no variant script -- no trigger fired at all.
GAME = os.path.join(HERE, "gamework") if os.path.isdir(os.path.join(HERE, "gamework")) else r"C:\FreeFalcon6"
BLUE = (1, 2, 3)
RED = (5, 6)   # PRC is folded into DPRK by the engine within the first half hour


def theaters(game):
    """[(name, tdf path relative to the game dir)] from the game's theater.lst."""
    out = []
    lst = os.path.join(game, "theater.lst")
    for ln in open(lst, errors="replace") if os.path.exists(lst) else []:
        ln = ln.strip()
        if not ln or ln[0] in "#;":
            continue
        tdf = ln.replace("\\", "/")
        name = os.path.splitext(os.path.basename(tdf))[0]
        p = os.path.join(game, tdf)
        if os.path.exists(p):
            m = re.search(r"^name\s+(.+)$", open(p, errors="replace").read(), re.M)
            name = m.group(1).strip() if m else name
        out.append((name, tdf))
    return out


_LABELS = {}


def campaign_labels(game):
    """{theater stem ('' = plain Korea): (theater name, [save0, save1, save2 title])} as the game's campaign screen shows them.

    The titles are TXT_SCENARIO_1..3 from the theater's art/main/lcktxtrc.irc (button 1 loads save0 ...); the theater
    name is the .tdf's `name`. Plain Korea (no theater=) is terrdata/theaterdefinition/korea.tdf and the base art dir.
    """
    if game in _LABELS:
        return _LABELS[game]
    out = {}
    entries = [("", "terrdata/theaterdefinition/korea.tdf")] + [
        (os.path.splitext(os.path.basename(t))[0], t) for _, t in theaters(game)]
    for stem, tdf in entries:
        p = os.path.join(game, tdf)
        if not os.path.exists(p):
            continue
        txt = open(p, errors="replace").read()
        nm = re.search(r"^name\s+(.+)$", txt, re.M)
        art = re.search(r"^artdir\s+(\S+)", txt, re.M)
        ad = os.path.join(game, (art.group(1) if art else "art").replace("\\", "/"))
        irc = next((q for q in (os.path.join(ad, "main", "lcktxtrc.irc"), os.path.join(ad, "art", "main", "lcktxtrc.irc"))
                    if os.path.exists(q)), "")     # Korea's artdir is the art folder itself, the ITO theaters' its parent
        scen = ["", "", ""]
        if irc:
            for n, s in re.findall(r'TXT_SCENARIO_([123])\s+"([^"]*)"', open(irc, errors="replace").read()):
                scen[int(n) - 1] = re.sub(r"\s+by FF5\.5$", "", s.strip())
        out[stem or "korea"] = ((nm.group(1).strip() if nm else stem), scen)
        folder = os.path.basename(os.path.dirname(tdf))     # runs name the theater by its folder too (Israel, not Israeli)
        if stem and folder.lower() not in ("theaterdefinition", stem.lower()):
            out[folder] = out[stem]
    _LABELS[game] = out
    return out


def run_parts(game, name):
    """(save, theater stem or '', tag) of a run named <save>[-<theater>][-<tag>]-s<seed>; None for other names."""
    m = re.match(r"^(save[0-2])(?:-(.+?))?-s(\d+)$", name)
    if not m:
        return None
    lab = campaign_labels(game)
    rest = m.group(2) or ""
    for k in sorted(lab, key=len, reverse=True):          # longest stem first: Israel_Classic before Israel
        if k != "korea" and (rest == k or rest.startswith(k + "-")):
            return m.group(1), k, rest[len(k) + 1:]
    return m.group(1), "", rest


_END = {}


def end_labels(game, name):
    """{END_GAME code: what ends the campaign} for a run, from the comments of the .tri the engine reads.

    The engine reads <Scenario>.tri where Scenario is the name stored inside the save's .cam (Israel Classic's save0.cam
    names save2), so the header is consulted. Each #END_GAME is labelled by the // comment lines in the paragraph above
    it; without any, by its #IF conditions. Several blocks with one code are joined with ' / '.
    """
    parts = run_parts(game, name)
    if not parts:
        return {}
    save, stem, _tag = parts
    campdir = os.path.join(game, "campaign", "SAVE")
    if stem and stem != "korea":
        for _, t in theaters(game):
            if os.path.splitext(os.path.basename(t))[0] == stem or os.path.basename(os.path.dirname(t)) == stem:
                m = re.search(r"^campaigndir\s+(\S+)", open(os.path.join(game, t), errors="replace").read(), re.M)
                if m:
                    campdir = os.path.join(game, m.group(1).replace("\\", "/"))
                break
    scen = save
    try:
        sys.path.insert(0, os.path.join(HERE, "..", "campaign-editor"))
        from ffcamp import campfile
        scen = (campfile.CampaignFile.load(os.path.join(campdir, save + ".cam")).header.fields.get("Scenario") or save).strip() or save
    except Exception:
        pass
    tri = os.path.join(campdir, scen + ".tri")
    if not os.path.exists(tri):
        return {}
    key = (tri, os.path.getmtime(tri))
    if key in _END:
        return _END[key]
    lines = [ln.rstrip() for ln in open(tri, errors="replace").read().splitlines()]
    out = {}
    for i, ln in enumerate(lines):
        m = re.match(r"\s*#END_GAME\s+(\d+)", ln)
        if not m:
            continue
        j = i
        while j > 0 and lines[j - 1].strip() and not re.match(r"\s*#ENDIF", lines[j - 1]):
            j -= 1
        para = lines[j:i]
        text = [c.lstrip("/ ").strip() for c in para if c.strip().startswith("//") and not c.strip().startswith("//#")]
        if not text:
            for c in para:
                d = re.match(r"\s*#IF_CAMPAIGN_DAY\s+G\s+(\d+)", c)
                if d:
                    text.append("day %s time limit" % d.group(1))
        lab = " ".join(t for t in text if t)
        if lab and lab not in out.setdefault(int(m.group(1)), []):
            out[int(m.group(1))].append(lab)
    res = {c: " / ".join(v) for c, v in out.items()}
    _END[key] = res
    return res


def run_label(game, name):
    """'Korea - Tiger Spirit' (or 'Israel 2012 - 2007 Fictional War') for a run named <save>[-<theater>][-<tag>]-s<seed>; '' if unknown."""
    parts = run_parts(game, name)
    if not parts:
        return ""
    save, stem, tag = parts
    th, scen = campaign_labels(game).get(stem or "korea", ("", ["", "", ""]))
    title = scen[int(save[-1])]
    return " - ".join(x for x in (th, title) if x) + (" [%s]" % tag if tag else "") + " s" + re.search(r"s(\d+)$", name).group(1)


def theater_args(game, want):
    """Knobs for --theater WANT: a theater.lst entry (its file stem or display name, any case) or a .tdf path.

    Returns (knobs, short name). A theater whose .tdf names its own zipsdir (ITO's ZipsITO) keeps its sim data
    (radar, aircraft) only in zips that campsim's file lookups do not see: they are unpacked once into
    runs/_simroot/<zipsdir> and passed as simroot=.
    """
    tdf = None
    for name, t in theaters(game):
        stem = os.path.splitext(os.path.basename(t))[0]
        if want.lower() in (stem.lower(), name.lower(), t.lower()):
            tdf = t
    if tdf is None and os.path.exists(os.path.join(game, want)):
        tdf = want.replace("\\", "/")
    if tdf is None:
        raise SystemExit("unknown theater %r; theater.lst has: %s" % (
            want, ", ".join("%s (%s)" % (os.path.splitext(os.path.basename(t))[0], n) for n, t in theaters(game))))
    knobs = ["theater=" + tdf]
    m = re.search(r"^zipsdir\s+(\S+)", open(os.path.join(game, tdf), errors="replace").read(), re.M)
    zdir = m.group(1).replace("\\", "/") if m else ""
    if zdir and zdir.lower() != "zips":
        root = os.path.join(HERE, "runs", "_simroot", os.path.basename(zdir))
        if not os.path.isdir(os.path.join(root, "sim")):
            lst = open(os.path.join(game, "ziplist.lst")).read().split()
            for z in lst[1:1 + int(lst[0])]:
                print("unpacking %s/%s -> %s" % (zdir, z, root))
                zipfile.ZipFile(os.path.join(game, zdir, z)).extractall(root)
        knobs.append("simroot=" + root.replace("\\", "/"))
    return knobs, os.path.splitext(os.path.basename(tdf))[0]


def run_one(a, seed):
    runs = os.path.join(HERE, "runs")
    os.makedirs(runs, exist_ok=True)
    th = getattr(a, "thname", "")  # the Campaign Lab (serve.py) passes no theater
    name = "%s%s%s-s%d" % (a.save, ("-" + th) if th else "", ("-" + a.tag) if a.tag else "", seed)
    work = os.path.join(runs, "_work_" + name)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    for f in ["campsim.exe"] + [os.path.basename(p) for p in glob.glob(os.path.join(EXE_DIR, "*.dll"))]:
        shutil.copy(os.path.join(EXE_DIR, f), work)
    tl = os.path.join(runs, name + ".jsonl")
    for ext in (".log", ".jsonl"):   # a rerun is a new run: drop the old files so the Lab's "started" time is this launch
        if os.path.exists(os.path.join(runs, name + ext)):
            os.remove(os.path.join(runs, name + ext))
    with open(os.path.join(runs, name + ".log"), "wb") as log:
        t0 = time.time()
        rc = subprocess.run([os.path.join(work, "campsim.exe"), a.game, a.save,
                        str(a.days), str(seed), tl, str(a.every)] + list(a.set),
                       stdout=log, stderr=subprocess.STDOUT, cwd=work)
    # the engine's own log (rail lines, crash notes) lands beside the exe: keep it with the run
    dbg = os.path.join(work, "FFDebug.log")
    if os.path.exists(dbg):
        shutil.copy(dbg, os.path.join(runs, name + ".ffdebug.log"))
    shutil.rmtree(work, ignore_errors=True)
    if rc.returncode:
        print("seed %d: exe exited 0x%X" % (seed, rc.returncode & 0xFFFFFFFF))
    return seed, summarise(tl), time.time() - t0


def summarise(path):
    frames, fin, meta = [], None, {}
    if not os.path.exists(path):
        return None
    for ln in open(path):
        o = json.loads(ln)
        if o.get("final"):
            fin = o
        elif o.get("meta"):
            meta = o
        else:
            frames.append(o)
    if not frames:
        return None
    s = lambda f, teams, k: sum(f["s"][t][k] for t in teams)
    a, b = frames[0], frames[-1]
    flips = sum(len(f["o"]) for f in frames[1:])
    # objectives whose owner ended up on the other side of the line, vs the first frame
    # per team, for theaters whose sides are not Korea's (team name, ground, air, objectives: first -> last frame)
    teams = [(n, a["s"][t][0], b["s"][t][0], a["s"][t][2], b["s"][t][2], a["s"][t][4], b["s"][t][4])
             for t, n in enumerate(meta.get("teams", [])) if n and any(a["s"][t][k] for k in (0, 2, 4))]
    return dict(flips=flips, teams=teams,
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
    p.add_argument("--theater", default="", help="theater.lst entry (e.g. israel_classic, israel2007, korea_2012) "
                                                 "or a .tdf path relative to the game dir; 'list' prints them")
    a = p.parse_args()
    if a.theater == "list":
        for n, t in theaters(a.game):
            print("  %-16s %-28s %s" % (os.path.splitext(os.path.basename(t))[0], n, t))
        return
    a.thname = ""
    if a.theater:
        knobs, a.thname = theater_args(a.game, a.theater)
        a.set = knobs + list(a.set)
    with ThreadPoolExecutor(a.jobs) as ex:
        for seed, r, wall in ex.map(lambda s: run_one(a, s), a.seeds):
            if not r:
                print("seed %d: no output" % seed)
                continue
            if a.theater:
                print("%s" % run_label(a.game, "%s-%s-s%d" % (a.save, a.thname, seed)))
                print("seed %d  %.1f d  %s  wall %.0fs" % (
                    seed, r["days"], "endgame=%s" % r["endgame"] if r["endgame"] else "no end", wall))
                for n, g0, g1, a0, a1, o0, o1 in r["teams"]:
                    print("   %-14s ground %5d->%-5d air %4d->%-4d objectives %4d->%d" % (n, g0, g1, a0, a1, o0, o1))
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
