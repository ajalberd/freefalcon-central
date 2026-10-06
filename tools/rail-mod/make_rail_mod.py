"""Build the "Korea Rail War" JSGME mod: the rail build of the game, the rail network, the Train class,
rail-bridge objectives and the Korea Escalation campaign, in one folder JSGME can switch on and off.

    python make_rail_mod.py                  # build MODS\\Korea Rail War from a clean install
    python make_rail_mod.py --check          # only report what the install looks like

It never writes to the install itself: everything is assembled in a staging copy (--stage) and the
changed files are copied into the mod folder. The install must be clean for the files the mod ships
(no rail.txt/rail.json, stock class tables): see --check, and RAIL-MOD.md for the clean-up.

Sources (each rebuild picks up the latest):
  * the game:        <worktree>\\Falcon4___x64_Release\\FFViper.exe, shipped as FFViper-rail.exe
                     (the rail-tracks build; main stays FFViper.exe)
  * the network:     tools\\rail-mod\\korea\\rail.txt + rail.json (osm_rail.py + add_rail_lines.py)
  * the campaign:    MODS\\Korea Escalation\\campaign\\SAVE\\{save0.cam, save0.tri, Falcon4.AII}
                     (tools\\campsim\\make_escalation_mod.py); the other campaign files from the install
  * class tables:    the install's stock tables (or their *.bak-pre-train copies, if a Train class was
                     installed in place), plus install_train_class.py and add_rail_bridges.py run on the
                     staging copy
  * settings:        config\\mods\\Korea Rail War.cfg, written here (read after FFViper.cfg by the rail
                     build; see ReadFalcon4Config)
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
EDITOR = os.path.join(REPO, "tools", "campaign-editor")
MOD_NAME = "Korea Rail War"

CAMPAIGN_DIRS = ["campaign\\SAVE", "campaign\\korea1980s", "campaign\\korea2012"]
DB_DIRS = ["terrdata\\objects", "campaign\\SAVE\\campaigndb", "campaign\\korea2012\\campaigndb",
           "campaign\\korea1980s\\campaigndb"]
ESCALATION = ["save0.cam", "save0.tri", "Falcon4.AII"]

CFG = """// Korea Rail War (JSGME mod): settings for FFViper-rail.exe. Read after FFViper.cfg, so these win.
// Disable the mod in JSGME to remove this file. See "Korea Rail War README.txt".

// Rail: the network in terrdata\\korea\\rail.txt; supply trains on the Pyongbu Line
set g_bRailTrains 1
set g_sRailTrainLines "Pyongbu"
set g_bRailTrack 1
set g_bRailMapAllTrains 0

// Troop trains: battalions ride when it beats the road march (RAIL.md "Troop trains")
set g_bRailTroops 1
set g_nRailTroopTrains 6

// Rail mobilisation: China and Russia come in by train once they join
set g_bRailWave 1
set g_sRailWaveCountries "5,4"
set g_nRailWaveTrains 12

// Korea Escalation's recommended settings (tools\\campsim\\KOREA-ESCALATION.md)
set g_bAlertScramble 1
set g_bInitTrueLosses 1
set g_nCounterAttackInitiative 15
set g_nCaptureInitiative 2
"""

README = """Korea Rail War
==============

Railways in the Korea campaign, on top of the Korea Escalation campaign.

ENABLE
  1. In JSGME, DISABLE "Korea Escalation" first (both mods change campaign\\SAVE\\save0.cam).
  2. Enable "Korea Rail War".
  3. Start FFViper-rail.exe (your FFViper.exe is not touched) and begin a NEW Korea campaign.

WHAT IS IN IT
  * FFViper-rail.exe: the game built from the rail-tracks branch (main + rail).
  * The railway network (terrdata\\korea\\rail.txt): 21 routes from OpenStreetMap -- the North and
    South Korean main lines, China (Shenyang-Dandong into Sinuiju, Meihekou-Ji'an into Manpo),
    Russia (Khasan into Tumangang). Drawn on the campaign map (right-click > Rail lines) and in 3D.
  * Supply trains on the Pyongbu Line: real units that shuttle supply to the front; spot and
    bomb them like any column.
  * Troop trains: a battalion with a long march takes the train when that is faster (it must be
    near a line at both ends, on track its side holds). It stays a normal unit on the map --
    it can be spotted and struck; an enemy ground unit within 5 km makes it get off.
  * Rail mobilisation: when China (and Russia) join, their armies far from the front are sent by
    train to railheads 20-60 km behind it, 12 trains at a time.
  * Rail-bridge objectives on the long rail river crossings, border bridges included: drop one
    and the line is cut -- trains stop short, troop trains unload and march.
  * Korea Escalation's campaign (save0.cam, save0.tri, Falcon4.AII): China and Russia staged,
    Blue air cut to Falcon 4.0 amounts, war ends only with Pyongyang and Wonsan.
  * config\\mods\\Korea Rail War.cfg: the settings above, plus Escalation's recommended ones.

SAVES
  A campaign started with this mod needs it: its trains are units of a class (Train) that only
  the mod's class tables have. Do not load such a save with the mod disabled.

BUILT
  {when} by tools\\rail-mod\\make_rail_mod.py (rail-tracks {commit}). Rail data (c) OpenStreetMap
  contributors, ODbL.
"""


def sha(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def baseline(path):
    """The stock version of an install file: its *.bak-pre-train copy if the Train class was installed
    in place, else the file itself."""
    bak = path + ".bak-pre-train"
    return bak if os.path.exists(bak) else path


def made_by_us(out):
    """The mod folder's README says this script wrote it. (JSGME copies every file in a mod into the
    game folder, so there is no separate marker file.)"""
    try:
        with open(os.path.join(out, MOD_NAME + " README.txt"), encoding="utf-8", errors="replace") as f:
            return "make_rail_mod.py" in f.read()
    except OSError:
        return False


def is_reparse(p):
    """A junction or symlink: removed itself, never walked into."""
    try:
        return bool(os.lstat(p).st_file_attributes & 0x400)  # FILE_ATTRIBUTE_REPARSE_POINT
    except OSError:
        return False


def remove_stage(stage):
    for root, dirs, _files in os.walk(stage, topdown=True):
        for d in list(dirs):
            p = os.path.join(root, d)
            if is_reparse(p):
                os.rmdir(p)  # the junction only; the install behind it is untouched
                dirs.remove(d)
    shutil.rmtree(stage)


def junction(link, target):
    subprocess.run(["cmd", "/c", "mklink", "/J", link, target], check=True, stdout=subprocess.DEVNULL)


def stage_install(game, stage, escalation):
    """A staging game folder: real copies of what the tools write, junctions to the rest."""
    if os.path.exists(stage):
        remove_stage(stage)
    os.makedirs(stage)
    for name in os.listdir(game):
        src = os.path.join(game, name)
        if os.path.isdir(src) and name.lower() not in ("terrdata", "campaign", "mods"):
            junction(os.path.join(stage, name), src)
        elif os.path.isfile(src) and os.path.splitext(name)[1].lower() in (".lst", ".cfg", ".ini", ".txt"):
            shutil.copy2(src, os.path.join(stage, name))  # theater.lst: how the tools find the theaters
    tdir = os.path.join(stage, "terrdata")
    os.makedirs(tdir)
    for name in os.listdir(os.path.join(game, "terrdata")):
        src = os.path.join(game, "terrdata", name)
        if os.path.isdir(src) and name.lower() != "objects":
            junction(os.path.join(tdir, name), src)
    odir = os.path.join(tdir, "objects")
    os.makedirs(odir)
    for name in os.listdir(os.path.join(game, "terrdata", "objects")):
        if name.upper().startswith("FALCON4.") and ".bak" not in name.lower():
            shutil.copy2(baseline(os.path.join(game, "terrdata", "objects", name)), os.path.join(odir, name))
    for rel in CAMPAIGN_DIRS:
        src, dst = os.path.join(game, rel), os.path.join(stage, rel)
        shutil.copytree(src, dst, ignore=shutil.ignore_patterns("*.bak*", "_*backup*"))
        for root, _dirs, files in os.walk(dst):
            for f in files:
                orig = os.path.join(src, os.path.relpath(os.path.join(root, f), dst))
                if os.path.exists(orig + ".bak-pre-train"):
                    shutil.copy2(orig + ".bak-pre-train", os.path.join(root, f))
    for f in ESCALATION:
        shutil.copy2(os.path.join(escalation, "campaign", "SAVE", f), os.path.join(stage, "campaign", "SAVE", f))


def run(args, log):
    log("  $ " + " ".join(os.path.basename(a) if i == 1 else a for i, a in enumerate(args)))
    r = subprocess.run(args, capture_output=True, text=True, encoding="utf-8", errors="replace")
    for line in (r.stdout + r.stderr).splitlines():
        log("    " + line)
    if r.returncode:
        raise SystemExit("failed: %s" % " ".join(args))


def check_install(game):
    """What would make the mod collide with the install (files it ships that are not stock there)."""
    issues = []
    for f in ("rail.txt", "rail.json"):
        if os.path.exists(os.path.join(game, "terrdata", "korea", f)):
            issues.append("terrdata\\korea\\%s is in the install (the mod ships it)" % f)
    for rel in DB_DIRS:
        for f in ("FALCON4.ct", "FALCON4.UCD"):
            if os.path.exists(os.path.join(game, rel, f + ".bak-pre-train")):
                issues.append("%s\\%s has the Train class installed in place" % (rel, f))
    for rel in CAMPAIGN_DIRS:
        if os.path.exists(os.path.join(game, rel, "teunits.lst.bak-pre-train")):
            issues.append("%s\\teunits.lst has Train installed in place" % rel)
    if os.path.exists(os.path.join(game, "FFViper-rail.exe")):
        issues.append("FFViper-rail.exe is in the install (the mod ships it)")
    return issues


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--gamedir", default=r"C:\FreeFalcon6")
    ap.add_argument("--exe", default=os.path.join(REPO, "Falcon4___x64_Release", "FFViper.exe"))
    ap.add_argument("--stage", default=os.path.join(tempfile.gettempdir(), "korea-rail-war-stage"))
    ap.add_argument("--check", action="store_true", help="report the install's state and stop")
    a = ap.parse_args()
    game = a.gamedir
    mods = os.path.join(game, "MODS")
    escalation = os.path.join(mods, "Korea Escalation")
    out = os.path.join(mods, MOD_NAME)

    issues = check_install(game)
    print("install: %s" % ("clean for the mod's files" if not issues else "NOT clean:"))
    for i in issues:
        print("  - " + i)
    if a.check:
        return
    for need in (a.exe, os.path.join(escalation, "campaign", "SAVE", "save0.cam"),
                 os.path.join(HERE, "korea", "rail.json")):
        if not os.path.exists(need):
            raise SystemExit("missing: %s" % need)
    if os.path.exists(out) and not made_by_us(out):
        raise SystemExit("%s exists and was not made by this script -- not touching it" % out)

    log = print
    log("staging in %s" % a.stage)
    stage_install(game, a.stage, escalation)
    rail_json = os.path.join(HERE, "korea", "rail.json")
    py = sys.executable
    run([py, os.path.join(EDITOR, "install_train_class.py"), "--gamedir", a.stage], log)
    run([py, os.path.join(EDITOR, "add_rail_bridges.py"), "--gamedir", a.stage, "--rail", rail_json, "--write"], log)

    # The mod: every staged file that differs from what the install (or Escalation) has, plus ours.
    files = {}
    for rel in DB_DIRS + CAMPAIGN_DIRS:
        sdir = os.path.join(a.stage, rel)
        for root, dirs, names in os.walk(sdir):
            dirs[:] = [d for d in dirs if not d.startswith("_")]  # _railbridge-backup and the like
            for n in names:
                if ".bak" in n.lower():
                    continue
                p = os.path.join(root, n)
                r = os.path.relpath(p, a.stage)
                orig = baseline(os.path.join(game, r))  # stock, even if the install has rail in place
                is_esc = r.lower() in {("campaign\\save\\" + e).lower() for e in ESCALATION}
                if is_esc or not os.path.exists(orig) or sha(p) != sha(orig):
                    files[r] = p
    files["terrdata\\korea\\rail.txt"] = os.path.join(HERE, "korea", "rail.txt")
    files["terrdata\\korea\\rail.json"] = rail_json
    files["FFViper-rail.exe"] = a.exe

    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    for r, src in sorted(files.items()):
        dst = os.path.join(out, r)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
    os.makedirs(os.path.join(out, "config", "mods"), exist_ok=True)
    with open(os.path.join(out, "config", "mods", MOD_NAME + ".cfg"), "w", newline="\r\n") as f:
        f.write(CFG)
    commit = subprocess.run(["git", "-C", REPO, "rev-parse", "--short", "HEAD"], capture_output=True,
                            text=True).stdout.strip()
    with open(os.path.join(out, MOD_NAME + " README.txt"), "w", newline="\r\n") as f:
        f.write(README.format(when=time.strftime("%Y-%m-%d %H:%M"), commit=commit))

    log("\n%s:" % out)
    total = 0
    for root, _d, names in os.walk(out):
        for n in sorted(names):
            p = os.path.join(root, n)
            total += os.path.getsize(p)
            log("  %-60s %9d" % (os.path.relpath(p, out), os.path.getsize(p)))
    log("%d files, %.1f MB" % (sum(len(n) for _r, _d, n in os.walk(out)), total / 1e6))
    if issues:
        log("\nNOTE: the install is not clean for the mod's files (above); clean it before enabling.")


if __name__ == "__main__":
    main()
