"""Take the rail files that were installed in place out of the install, so the Korea Rail War JSGME mod
can switch them on and off.

    python clean_install.py            # list what would change
    python clean_install.py --write    # back everything up, then clean

Before the mod existed, rail was installed straight into C:\\FreeFalcon6: rail.txt/rail.json in
terrdata\\korea, the Train class in the class tables (FALCON4.ct/.UCD, four copies) and teunits.lst
(three), and FFViper-rail.exe. The mod ships all of these now. This puts the stock versions back (from
the *.bak-pre-train copies install_train_class.py made) and moves the rail files away.

Every file it changes or removes -- and the backups it consumes -- goes to
_rail-clean-backup\\<stamp>\\ with a restore.bat that puts the install back exactly as it was.
"""

import argparse
import datetime
import json
import os
import shutil

DB_DIRS = ["terrdata\\objects", "campaign\\SAVE\\campaigndb", "campaign\\korea2012\\campaigndb",
           "campaign\\korea1980s\\campaigndb"]
CAMPAIGN_DIRS = ["campaign\\SAVE", "campaign\\korea1980s", "campaign\\korea2012"]


def plan(game):
    """[(action, rel, rel_backup_source)]: 'restore' copies rel.bak-pre-train over rel, 'remove' moves rel away."""
    out = []
    kdir = os.path.join(game, "terrdata", "korea")
    for n in sorted(os.listdir(kdir)):
        if n.lower().startswith("rail.") and (n.lower().endswith((".txt", ".json")) or ".bak" in n.lower()):
            out.append(("remove", os.path.join("terrdata", "korea", n)))
    for rel in DB_DIRS:
        for f in ("FALCON4.ct", "FALCON4.UCD"):
            r = os.path.join(rel, f)
            if os.path.exists(os.path.join(game, r + ".bak-pre-train")):
                out.append(("restore", r))
    for rel in CAMPAIGN_DIRS:
        r = os.path.join(rel, "teunits.lst")
        if os.path.exists(os.path.join(game, r + ".bak-pre-train")):
            out.append(("restore", r))
    if os.path.exists(os.path.join(game, "FFViper-rail.exe")):
        out.append(("remove", "FFViper-rail.exe"))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--gamedir", default=r"C:\FreeFalcon6")
    ap.add_argument("--write", action="store_true")
    a = ap.parse_args()
    g = a.gamedir
    steps = plan(g)
    if not steps:
        print("nothing to clean: the install has no rail files in place")
        return
    for act, rel in steps:
        print("  %-8s %s%s" % (act, rel, "  (stock from .bak-pre-train)" if act == "restore" else ""))
    if not a.write:
        print("dry run. Add --write to back up and clean.")
        return

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    bdir = os.path.join(g, "_rail-clean-backup", stamp)
    os.makedirs(bdir)
    saved = []  # (backup name, rel) -- restore.bat copies each back

    def keep(rel):
        name = "%03d_%s" % (len(saved), os.path.basename(rel))
        shutil.copy2(os.path.join(g, rel), os.path.join(bdir, name))
        saved.append((name, rel))

    for act, rel in steps:
        p = os.path.join(g, rel)
        keep(rel)
        if act == "restore":
            keep(rel + ".bak-pre-train")
            shutil.copyfile(p + ".bak-pre-train", p)  # in place: keeps the file's identity (hard links)
            os.remove(p + ".bak-pre-train")
        else:
            os.remove(p)

    with open(os.path.join(bdir, "restore.bat"), "w", newline="\r\n") as f:
        f.write("@echo off\nrem Puts the rail files back in place exactly as before clean_install.py.\n")
        f.write("rem Disable the Korea Rail War mod in JSGME first.\ncd /d \"%~dp0\"\n")
        for name, rel in saved:
            f.write("copy /y \"%s\" \"..\\..\\%s\" >nul || goto fail\n" % (name, rel))
        f.write("echo Restored.\npause\nexit /b 0\n:fail\necho Restore FAILED.\npause\nexit /b 1\n")
    json.dump({"when": stamp, "files": saved}, open(os.path.join(bdir, "manifest.json"), "w"), indent=1)

    left = plan(g)
    print("cleaned; backup and restore.bat in %s" % bdir)
    print("still in place: %s" % (left or "nothing"))


if __name__ == "__main__":
    main()
