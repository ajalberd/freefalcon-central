"""Watch a running game's Auto Save.cam and log campaign progress to a CSV.

    python watch_save.py [--every 5] [--save "C:/FreeFalcon6/campaign/SAVE/Auto Save.cam"] [--csv live_log.csv]

Every `--every` seconds it checks the file; when it changed it copies it (never holds a lock on
the game's file), decodes it and appends one row. The game only rewrites the autosave every so
often, so a short poll interval just means a change is noticed within seconds.

A mid-campaign save has no objective list of its own, only deltas (.obd) against the scenario it
started from, so the base objectives are read from <SAVE dir>/<Scenario>.cam and the deltas laid
over them.  Blue = teams 1,2,3, Red = teams 5,6 (PRC is allied with DPRK).
"""
import argparse
import collections
import csv
import os
import shutil
import struct
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import saves                                    # noqa: E402
from ffcamp import campfile, entities, lzss, objectives  # noqa: E402

BLUE, RED = (1, 2, 3), (5, 6)
SEOUL, PUSAN = 228, 417   # the "bad guys win" script watches these (save0.tri, event 14)
COLUMNS = ["wall_time", "file_time", "day", "hour", "elapsed_h", "blue_objs", "red_objs",
           "blue_bn", "red_bn", "blue_gnd", "red_gnd", "blue_ad", "red_ad", "blue_air",
           "red_air", "seoul_owner", "pusan_owner", "blue_max_y", "red_min_y", "obj_changes"]

_cache = {}


def class_tables():
    if "rows" not in _cache:
        _cache["rows"], _cache["by"] = saves.load_tables(saves.SOURCES["ff6"])
    return _cache["rows"], _cache["by"]


def base_objectives(save_dir, scenario):
    """{vu id: owner}, {campId: vu id} from the scenario the campaign started from."""
    if scenario not in _cache:
        rows, _ = class_tables()
        path = os.path.join(save_dir, scenario + ".cam")
        cam = campfile.CampaignFile.load(path)
        objs, _ = objectives.decode_objectives(cam.member("obj"), cam.version, rows)
        _cache[scenario] = ({o["id"][0]: o["owner"] for o in objs},
                            {o["campId"]: o["id"][0] for o in objs})
    return _cache[scenario]


def apply_deltas(member, owners):
    """Lay an .obd delta stream over {vu id: owner}; returns the number of owner changes."""
    off = 4
    count = struct.unpack_from("<h", member, off)[0]
    size = struct.unpack_from("<i", member, off + 2)[0]
    raw = lzss.expand(member[off + 6:], size)
    pos = changes = 0
    for _ in range(count):
        num = struct.unpack_from("<I", raw, pos)[0]
        pos += 12                                   # VU_ID (8) + last_repair (4)
        owner, _s, _f, _l, ln = struct.unpack_from("<5B", raw, pos)
        pos += 5 + ln
        if num in owners:
            changes += owners[num] != owner
            owners[num] = owner
    return changes


def snapshot(path, save_dir):
    rows, by = class_tables()
    cam = campfile.CampaignFile.load(path)
    h = cam.header.fields
    scenario = (h.get("Scenario") or "").strip()
    base, camp_to_vu = base_objectives(save_dir, scenario)
    owners = dict(base)
    changes = 0
    if cam.member("obj"):
        objs, _ = objectives.decode_objectives(cam.member("obj"), cam.version, rows)
        owners = {o["id"][0]: o["owner"] for o in objs}
    elif cam.member("obd"):
        changes = apply_deltas(cam.member("obd"), owners)
    own = collections.Counter(owners.values())

    units, _ = entities.decode_units(cam.member("uni"), cam.version, rows)
    side = lambda t: "b" if t in BLUE else "r" if t in RED else None
    bn, gnd, ad, air = (collections.Counter() for _ in range(4))
    ys = {"b": [], "r": []}
    for u in units:
        s = side(u["owner"])
        if not s:
            continue
        veh = sum((u["roster"] >> (2 * i)) & 3 for i in range(16))
        row = by.get(u["classIndex"])
        if u["kind"] == "battalion":
            bn[s] += 1
            if row and int(row.get("Role", 0)) == 5:
                ad[s] += veh
            else:
                gnd[s] += veh
            ys[s].append(u["y"])
        elif u["kind"] == "squadron":
            air[s] += veh
    t = h["CurrentTime"]
    return {
        "day": t // 86400000, "hour": round((t % 86400000) / 3600000, 2),
        "elapsed_h": round((t - h["TE_StartTime"]) / 3600000, 2) if h.get("TE_StartTime") else "",
        "blue_objs": sum(own[k] for k in BLUE), "red_objs": sum(own[k] for k in RED),
        "blue_bn": bn["b"], "red_bn": bn["r"], "blue_gnd": gnd["b"], "red_gnd": gnd["r"],
        "blue_ad": ad["b"], "red_ad": ad["r"], "blue_air": air["b"], "red_air": air["r"],
        "seoul_owner": owners.get(camp_to_vu.get(SEOUL), ""),
        "pusan_owner": owners.get(camp_to_vu.get(PUSAN), ""),
        "blue_max_y": max(ys["b"]) if ys["b"] else "", "red_min_y": min(ys["r"]) if ys["r"] else "",
        "obj_changes": changes,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--every", type=float, default=5.0, help="poll interval, seconds")
    ap.add_argument("--save", default="C:/FreeFalcon6/campaign/SAVE/Auto Save.cam")
    ap.add_argument("--csv", default=os.path.join(HERE, "live_log.csv"))
    ap.add_argument("--once", action="store_true", help="log the current file once and exit")
    a = ap.parse_args()
    save_dir = os.path.dirname(os.path.abspath(a.save))
    new = not os.path.exists(a.csv) or os.path.getsize(a.csv) == 0
    out = open(a.csv, "a", newline="")
    w = csv.DictWriter(out, fieldnames=COLUMNS)
    if new:
        w.writeheader()
    last = None
    print("watching %s every %.0fs -> %s  (Ctrl+C to stop)" % (a.save, a.every, a.csv))
    tmp = os.path.join(tempfile.gettempdir(), "campsim_watch.cam")
    while True:
        try:
            st = os.stat(a.save)
            key = (st.st_mtime_ns, st.st_size)
            if key != last:
                shutil.copyfile(a.save, tmp)          # the game may be writing it: retry next poll
                row = snapshot(tmp, save_dir)
                row["wall_time"] = time.strftime("%H:%M:%S")
                row["file_time"] = time.strftime("%H:%M:%S", time.localtime(st.st_mtime))
                w.writerow(row)
                out.flush()
                last = key
                print("%s  day %s %5.2fh | objs Blue %d Red %d | gnd Blue %d Red %d | air Blue %d Red %d | "
                      "Seoul %s Pusan %s" % (row["wall_time"], row["day"], row["hour"], row["blue_objs"],
                                             row["red_objs"], row["blue_gnd"], row["red_gnd"],
                                             row["blue_air"], row["red_air"], row["seoul_owner"],
                                             row["pusan_owner"]))
            if a.once:
                return
        except (OSError, ValueError, EOFError, struct.error, IndexError) as e:
            print("%s  (skipped: %s)" % (time.strftime("%H:%M:%S"), e))
            if a.once:
                return
        time.sleep(a.every)


if __name__ == "__main__":
    main()
