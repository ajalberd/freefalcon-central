"""Move one objective by whole grid cells, in every file that places it.

An objective's features (runway pieces, taxiways, buildings) and its runway
points (PtData: takeoff, landing, parking) are stored as offsets from the
objective, and TACAN stations are keyed by its campaign id, so they all move
with it. What does not move by itself is every copy of the objective: the
campaign's own save0.cam, TE files carrying an objective list, and saves made
from them. This finds each file whose objective with the given id has the given
name and old position, and moves it there. Files where that id is a different
objective (another scenario's numbering) are left alone.

Links to neighbouring objectives are kept; their costs were set for the old
spot, which for a move of a cell or two changes nothing that matters.

    python move_objective.py --name "Changyon Highway Strip" --east 1          # report only
    python move_objective.py --name "Changyon Highway Strip" --east 1 --write  # back up, then move
    python move_objective.py --rollback                                         # undo the last move

Backups go to <campaign dir>/_move-backup/<stamp>/ with a manifest and a
restore .bat (no Python needed).
"""

import argparse
import datetime
import glob
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ffcamp import objectives  # noqa: E402
from ffcamp.workspace import TheaterWorkspace  # noqa: E402

BACKUP = "_move-backup"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--gamedir", default=r"C:\FreeFalcon6")
    ap.add_argument("--theater", default=r"terrdata\theaterdefinition\korea.tdf")
    ap.add_argument("--name", help="objective name, exactly as the campaign shows it")
    ap.add_argument("--east", type=int, default=0, help="cells (km) east; negative = west")
    ap.add_argument("--north", type=int, default=0, help="cells (km) north; negative = south")
    ap.add_argument("--source", default="save0.cam", help="file that defines where it is now")
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--rollback", action="store_true")
    args = ap.parse_args()

    ws = TheaterWorkspace(args.gamedir, args.theater)
    bak_root = os.path.join(ws.campaign_dir, BACKUP)

    if args.rollback:
        runs = sorted(glob.glob(os.path.join(bak_root, "*", "manifest.json")))
        if not runs:
            raise SystemExit("no move to roll back in %s" % bak_root)
        man = json.load(open(runs[-1]))
        for name in man["files"]:
            shutil.copy2(os.path.join(os.path.dirname(runs[-1]), name),
                         os.path.join(ws.campaign_dir, name))
            print("  restored %s" % name)
        os.rename(runs[-1], runs[-1] + ".rolled-back")
        print("rolled back: %s is at (%d, %d) again" % (man["name"], man["from"][0], man["from"][1]))
        return

    if not args.name or not (args.east or args.north):
        raise SystemExit("give --name and a move (--east / --north)")

    names = ws.name_table()
    src = [o for o in ws.objectives(args.source).objectives
           if names.get(o["nameId"], "").strip() == args.name]
    if len(src) != 1:
        raise SystemExit("%d objectives named %r in %s" % (len(src), args.name, args.source))
    ref = src[0]
    old = (ref["x"], ref["y"])
    new = (old[0] + args.east, old[1] + args.north)
    print("%s (id %s, campaign id %d): (%d, %d) -> (%d, %d)"
          % (args.name, ref["id"], ref["campId"], old[0], old[1], new[0], new[1]))

    # Anything else already standing in the destination cell?
    for o in ws.objectives(args.source).objectives:
        if o is not ref and (o["x"], o["y"]) == new:
            print("  note: %s (%s) is already in cell (%d, %d)"
                  % (names.get(o["nameId"], "?"), o["typeName"], new[0], new[1]))

    tacan = ws.tacan().get(ref["campId"])
    print("  TACAN: %s" % ("channel %s -- keyed by campaign id, moves with it" % tacan["label"]
                           if tacan else "none"))

    todo = []
    for f in ws.campaign_files():
        cam = ws.objectives(f["file"])
        if cam.member("obj") is None:
            continue          # borrows another file's list
        for n, o in enumerate(cam.objectives):
            if o["id"] == ref["id"]:
                same = names.get(o["nameId"], "").strip() == args.name and (o["x"], o["y"]) == old
                print("  %-30s %s" % (f["file"], "moves" if same else
                      "skipped (id %s here is %s at (%d, %d))"
                      % (o["id"], names.get(o["nameId"], "?"), o["x"], o["y"])))
                if same:
                    todo.append((f["file"], cam, n, o))
    if not args.write:
        print("dry run: nothing written. Add --write to back up and move.")
        return

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    bdir = os.path.join(bak_root, stamp)
    os.makedirs(bdir)
    for fname, cam, n, o in todo:
        shutil.copy2(cam.path, os.path.join(bdir, fname))
    with open(os.path.join(bdir, "restore.bat"), "w", newline="\r\n") as f:
        f.write("@echo off\nrem Undo the move of %s. Close the game and the campaign editor first.\n"
                % args.name)
        f.write("cd /d \"%~dp0\"\n")
        for fname, _c, _n, _o in todo:
            f.write("copy /y \"%s\" \"..\\..\\%s\" >nul || goto fail\n" % (fname, fname))
        f.write("echo Restored.\npause\nexit /b 0\n:fail\necho Restore FAILED.\npause\nexit /b 1\n")
    json.dump({"name": args.name, "id": ref["id"], "from": old, "to": new,
               "files": [t[0] for t in todo], "when": stamp},
              open(os.path.join(bdir, "manifest.json"), "w"), indent=1)

    for fname, cam, n, o in todo:
        raw = cam.objectives_raw
        raw = objectives.patch_objective(raw, o, cam.version, "x", new[0])
        raw = objectives.patch_objective(raw, o, cam.version, "y", new[1])
        o["x"], o["y"] = new
        cam.objectives_raw = raw
        cam.objectives_dirty = True
        cam.dirty = True

    class NoBackup:          # ours is already made, with a restore script
        def keep(self, path):
            return None

    written = ws.save(NoBackup())
    for w in written:
        print("  wrote %s" % w)

    # Read back from disk.
    ws2 = TheaterWorkspace(args.gamedir, args.theater)
    for fname, _c, _n, _o in todo:
        o = [o for o in ws2.objectives(fname).objectives if o["id"] == ref["id"]][0]
        print("  check %-30s now at (%d, %d)" % (fname, o["x"], o["y"]))
        if (o["x"], o["y"]) != new:
            raise SystemExit("read-back mismatch in %s -- restore with %s" % (fname, bdir))
    print("moved. Undo: python %s --rollback   (or run %s\\restore.bat)"
          % (os.path.basename(__file__), bdir))


if __name__ == "__main__":
    main()
