"""Build a JSGME mod that adds two stand-in weapons to the Korea databases:

  GBU-39 SDB     a clone of GBU-38/GP JDAM  (same model, same SMS behaviour, lighter and smaller)
  AGM-158 JASSM  a clone of Storm Shadow    (same model and missile data, JASSM range and weight)

    python add_weapons.py [--gamedir C:\\FreeFalcon6] [--out <mod folder>]

CBU-103 / CBU-105 need nothing: the databases already carry them as "CBU-87/103 CEM" and "CBU-97/105 SFW".

What it does, per database folder (terrdata\\objects and each campaign\\*\\CampaignDB):
  * FALCON4.SWD   a new sim-weapon row (the sim reads SimWeaponDataTable[class.vehicleDataIndex]), cloned from the
                  source weapon's row, with its own mnemonic and weight/area
  * FALCON4.WCD   a new weapon row (Index = its class-table row)
  * FALCON4.ct    a new class-table row cloned from the source, pointing at those two rows
  * FALCON4.WLD   the new weapon id in the F-16 hardpoint lists that already carry its source (SDB beside the
                  GBU-38, JASSM beside the AGM-88), same quantity, first free slot
Everything is appended, so no existing index moves. The output is a mod folder laid out like the game directory;
nothing under the game directory is touched. Enable it in JSGME AFTER "Korea Rail War" (both replace FALCON4.ct;
this one is built from whatever ct is installed, Train class included) and disable it before disabling Rail War.

Limits, so nobody is surprised in the loadout screen: the models are the source weapons' (the SDB looks like a
JDAM, the JASSM like a Storm Shadow), the SDB falls like a JDAM instead of gliding, and the campaign's AI loadouts
do not use either weapon -- only you can load them.
"""

import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "campaign-editor"))

from ffcamp import campdb  # noqa: E402

DB_DIRS = [r"terrdata\objects", r"campaign\korea2012\CampaignDB", r"campaign\korea1980s\CampaignDB",
           r"campaign\SAVE\CampaignDB"]
EXTS = ["ct", "WCD", "SWD", "WLD"]

# (new name, new mnemonic, source weapon name prefix, WCD overrides, SWD overrides, anchor weapon in the lists)
NEW = [
    ("GBU-39 SDB", "GBU39", "GBU-38/GP JDAM", {"Strength": 110, "Weight": 285, "BlastRadius": 150},
     {"weight": 285.0, "area": 2.5}, "GBU-38/GP JDAM"),
    ("AGM-158 JASSM", "JASSM", "Storm Shadow", {"Range": 370, "Weight": 2250},
     {"weight": 2250.0}, "AGM-88"),
]
LIST_PREFIXES = ("F-16", "KF-16")


def text(v):
    v = v.decode("latin1") if isinstance(v, bytes) else v
    return v.split("\x00")[0]


def find_file(d, ext):
    want = "falcon4." + ext.lower()
    for n in os.listdir(d):
        if n.lower() == want:
            return n
    return None


def build(src, dst):
    os.makedirs(dst, exist_ok=True)
    for ext in EXTS:
        name = find_file(src, ext)
        if not name:
            return "missing FALCON4.%s -- skipped" % ext
        shutil.copyfile(os.path.join(src, name), os.path.join(dst, name))

    db = campdb.CampaignDB(dst)
    ct, wcd, swd, wld = (db.table(n) for n in ("class", "weapon", "simweapon", "weaponlist"))
    names = [text(r["Name"]) for r in wcd.rows]
    report = []

    for new_name, mnemonic, src_name, wcd_over, swd_over, anchor in NEW:
        if new_name in names:
            report.append("%s already present" % new_name)
            continue

        s = next((i for i, n in enumerate(names) if n.startswith(src_name)), None)
        a = next((i for i, n in enumerate(names) if n.startswith(anchor)), None)
        if s is None or a is None:
            report.append("%s: source %r or anchor %r not found -- skipped" % (new_name, src_name, anchor))
            continue

        src_wcd = wcd.rows[s]
        src_ct_index = src_wcd["Index"]
        src_ct = ct.rows[src_ct_index]
        if src_ct["dataPtr"] != s:
            report.append("%s: class %d does not point back at weapon %d -- skipped" % (new_name, src_ct_index, s))
            continue

        new_swd, new_wcd, new_ct = len(swd.rows), len(wcd.rows), len(ct.rows)

        srow = dict(swd.rows[src_ct["vehicleDataIndex"]])
        srow["mnemonic"] = mnemonic
        srow.update(swd_over)
        swd.rows.append(srow)
        swd.raw.append(None)
        swd.dirty = True

        wrow = dict(src_wcd)
        wrow["Name"] = new_name
        wrow["Index"] = new_ct
        wrow.update(wcd_over)
        wcd.rows.append(wrow)
        wcd.raw.append(None)
        wcd.dirty = True
        names.append(new_name)

        crow = dict(src_ct)
        crow["classInfo_"] = list(src_ct["classInfo_"])
        crow["visType"] = list(src_ct["visType"])
        crow["vehicleDataIndex"] = new_swd
        crow["dataPtr"] = new_wcd
        ct.rows.append(crow)
        ct.raw.append(None)
        ct.dirty = True

        added = 0
        for li, l in enumerate(wld.rows):
            if not text(l["Name"]).startswith(LIST_PREFIXES) or a not in l["WeaponID"]:
                continue
            ids = list(l["WeaponID"])
            qty = list(l["Quantity"])
            if new_wcd in ids or 0 not in ids:
                continue
            slot = ids.index(0)
            ids[slot] = new_wcd
            qty[slot] = qty[ids.index(a)] or 1
            l["WeaponID"], l["Quantity"] = ids, qty
            added += 1
        wld.dirty = True
        report.append("%s: class %d, weapon %d, sim data %d, in %d list(s)" % (new_name, new_ct, new_wcd, new_swd, added))

    db.save_dirty()

    # read it back
    back = campdb.CampaignDB(dst)
    bct, bwcd, bswd = back.table("class"), back.table("weapon"), back.table("simweapon")
    for new_name, mnemonic, *_ in NEW:
        i = next((k for k, r in enumerate(bwcd.rows) if text(r["Name"]) == new_name), None)
        if i is None:
            continue
        c = bct.rows[bwcd.rows[i]["Index"]]
        ok = c["dataPtr"] == i and text(bswd.rows[c["vehicleDataIndex"]]["mnemonic"]) == mnemonic
        report.append("  verify %s: %s" % (new_name, "ok" if ok else "FAILED"))
    return "; ".join(report)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--gamedir", default=r"C:\FreeFalcon6")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    out = a.out or os.path.join(a.gamedir, "MODS", "Avionics New Weapons")
    for rel in DB_DIRS:
        src = os.path.join(a.gamedir, rel)
        dst = os.path.join(out, rel)
        print("%-34s %s" % (rel, build(src, dst) if os.path.isdir(src) else "missing"))
    print("mod folder:", out)


if __name__ == "__main__":
    main()
