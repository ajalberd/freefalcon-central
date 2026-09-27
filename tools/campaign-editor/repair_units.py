"""Repair a campaign save whose unit list holds the same unit many times.

Until September 2026 the game could insert a unit into its active lists a
second time when it was deactivated and reactivated before the campaign's
next list scan -- which the campaign UI does to airmobile infantry every time
it replays a helicopter's past pickup and drop (UnitClass::SetInactive,
campaign/camplib/unit.cpp). Each save then wrote the unit once per copy. One
save held three infantry battalions ~43,000 times each; the unit count is 16
bits, so it wrapped (131,595 units read as 523) and neither the game nor the
editor could load it.

The copies are the same object written again, byte for byte, so the repair
keeps the first record of each VU_ID and drops the rest. The original is kept
beside it as <name>.broken.

    python repair_units.py "C:\\FreeFalcon6\\campaign\\save\\Save-Day 1 10 44 28.cam"
"""

import os
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ffcamp import campdb, campfile, entities, lzss, theater  # noqa: E402


def class_rows_for(path):
    """The class table of the theater whose campaign folder holds this file."""
    folder = os.path.normcase(os.path.abspath(os.path.dirname(path)))
    gamedir = folder
    while gamedir and not os.path.isdir(os.path.join(gamedir, "terrdata")):
        parent = os.path.dirname(gamedir)
        if parent == gamedir:
            raise SystemExit("cannot find the game folder above %s" % path)
        gamedir = parent
    for t in theater.load_theaters(gamedir):
        if os.path.normcase(os.path.abspath(t.campaign_dir(gamedir))) == folder:
            return campdb.CampaignDB(theater.db_dir(gamedir, t)).class_rows()
    raise SystemExit("no theater uses %s as its campaign folder" % folder)


def repair(path):
    cam = campfile.CampaignFile.load(path)
    name = cam._member_name("uni")
    section = cam.members[name]
    size = struct.unpack_from("<i", section, 6)[0]
    raw = lzss.expand(section[10:], size)
    units = entities.walk_units(raw, cam.version, class_rows_for(path))

    seen, keep = set(), []
    for u in units:
        key = tuple(u["id"])
        if key in seen:
            continue
        seen.add(key)
        keep.append(raw[u["_span"][0]:u["_span"][1]])

    dropped = len(units) - len(keep)
    if not dropped:
        print("%s: %d units, no duplicates -- left alone"
              % (os.path.basename(path), len(units)))
        return
    if len(keep) > 32767:
        raise SystemExit("%s: still %d distinct units, more than the game's "
                         "16-bit count holds" % (path, len(keep)))

    fixed = b"".join(keep)
    cam.members[name] = entities.encode_units(fixed, len(keep))
    backup = path + ".broken"
    if not os.path.exists(backup):
        shutil.copy2(path, backup)
    cam.save(path)
    print("%s: %d records -> %d units (%d duplicates dropped); original kept "
          "as %s" % (os.path.basename(path), len(units), len(keep), dropped,
                     os.path.basename(backup)))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    for p in sys.argv[1:]:
        repair(p)
