"""Add the "Train" unit class to a Korea install, for the rail trains (railnet.cpp).

    python install_train_class.py [--gamedir C:\\FreeFalcon6]

What it writes, each file backed up once as <name>.bak-pre-train, and safe to
run again (anything already present is left alone):

  * FALCON4.ct / FALCON4.UCD: a class-table row and a unit-table row copied
    from the 16-KrAz Supply battalion (class 77 / unit 201), named "Train",
    with sptype RAIL_TRAIN_SPTYPE (20, railnet.h). Appended at the end, so no
    existing index moves. Written to the engine's copy (the theater's
    `objectdir`, terrdata\\objects for every Korea theater) and to the editor's
    per-campaign copies (`specialdbdir`), which must stay row-for-row equal.
  * teunits.lst in each Korea campaign folder: a "3 13 20" line, so the TE
    editor's Add Battalion offers Train under Equipment "Arty/Rocket" (where
    that file keeps Supply).

The game never reads `specialdbdir` -- OpenCampFile maps .ct and the .*CD
tables onto FalconObjectDataDir -- so the objectdir copy is the one that counts.
"""

import argparse
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ffcamp import campdb  # noqa: E402

SRC_CT, SRC_UCD = 77, 201
SPTYPE = 20                      # RAIL_TRAIN_SPTYPE in src/campaign/include/railnet.h
KEY = [3, 6, 1, 13, SPTYPE]      # land, unit, battalion, STYPE_UNIT_SUPPLY, sptype

DB_DIRS = [r"terrdata\objects",                # what the engine loads
           r"campaign\save\campaigndb",       # the editor's copies
           r"campaign\Korea2012\campaigndb",
           r"campaign\korea1980s\campaigndb"]
CAMPAIGN_DIRS = [r"campaign\save", r"campaign\korea1980s", r"campaign\korea2012"]


def backup(path):
    bak = path + ".bak-pre-train"
    if not os.path.exists(bak):
        shutil.copy2(path, bak)


def add_class(d):
    db = campdb.CampaignDB(d)
    ct, ucd = db.table("class"), db.table("unit")
    if ct is None or ucd is None:
        return "no FALCON4.ct / .UCD here -- skipped"
    if any(r["classInfo_"][:5] == KEY for r in ct.rows):
        return "Train class already present"
    if ct.rows[SRC_CT]["dataPtr"] != SRC_UCD or ucd.rows[SRC_UCD]["Index"] != SRC_CT:
        return "class %d is not the Supply battalion this expects -- skipped" % SRC_CT
    backup(ct.path)
    backup(ucd.path)
    new_ct, new_ucd = len(ct.rows), len(ucd.rows)

    urow = dict(ucd.rows[SRC_UCD])
    urow["Name"] = "Train"
    urow["Index"] = new_ct
    ucd.rows.append(urow)
    ucd.raw.append(None)
    ucd.dirty = True

    crow = dict(ct.rows[SRC_CT])
    crow["classInfo_"] = list(crow["classInfo_"])
    crow["classInfo_"][4] = SPTYPE
    crow["dataPtr"] = new_ucd
    ct.rows.append(crow)
    ct.raw.append(None)
    ct.dirty = True
    db.save_dirty()

    back = campdb.CampaignDB(d)
    c, u = back.table("class").rows[new_ct], back.table("unit").rows[new_ucd]
    ok = c["classInfo_"][:5] == KEY and c["dataPtr"] == new_ucd and u["Index"] == new_ct
    return "added class %d / unit %d (%s)" % (new_ct, new_ucd,
                                              "verified" if ok else "VERIFY FAILED")


def add_teunits(camp):
    names = [f for f in os.listdir(camp) if f.lower() == "teunits.lst"]
    if not names:
        return "no teunits.lst -- skipped"
    path = os.path.join(camp, names[0])
    data = open(path, "rb").read()
    nl = b"\r\n" if b"\r\n" in data else b"\n"
    lines = data.split(nl)
    if any(re.match(rb"^\s*3\s+13\s+%d\s*$" % SPTYPE, ln) for ln in lines):
        return "Train already listed"
    at = [i for i, ln in enumerate(lines) if re.match(rb"^\s*3\s+13\s+\d+\s*$", ln)]
    if not at:
        return "no Supply entries to sit beside -- skipped"
    backup(path)
    lines.insert(at[-1] + 1, b"      3      13       %d" % SPTYPE)
    open(path, "wb").write(nl.join(lines))
    return "listed"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--gamedir", default=r"C:\FreeFalcon6")
    args = ap.parse_args()
    for rel in DB_DIRS:
        d = os.path.join(args.gamedir, rel)
        print("%-34s %s" % (rel, add_class(d) if os.path.isdir(d) else "missing"))
    for rel in CAMPAIGN_DIRS:
        d = os.path.join(args.gamedir, rel)
        print("%-34s %s" % (rel + "\\teunits.lst",
                            add_teunits(d) if os.path.isdir(d) else "missing"))


if __name__ == "__main__":
    main()
