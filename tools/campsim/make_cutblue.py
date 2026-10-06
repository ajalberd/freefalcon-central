"""Korea Escalation: Blue combat air cut to Falcon 4.0 (vanilla) amounts.

cut_blue() is called by make_escalation_mod.build(). Run alone, it applies the cut to the installed mod save0 (the
mod must be enabled and not yet cut) and writes it to the mod folder, the live install and campsim's escal.cam. Whole squadrons are deleted, because
supply refills a trimmed squadron to full strength (supply.cpp). Support, transport and airlift squadrons stay.

Vanilla save0 at start: US jets 144 active (F-16 96, F-15C 24, A-10 24), ROK jets 144 (F-16 24, F-4E 48, F-5E 72),
attack helicopters 24 (AH-64; ROK's 96 MD-500s are scouts), US jet reinforcements ~204, ROK none.
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "campaign-editor"))
from ffcamp import entities, workspace  # noqa: E402
import make_ally_mod  # noqa: E402

GAME = r"C:\FreeFalcon6"
GW = os.path.join(HERE, "gamework", "campaign", "SAVE")

# (owner, squadron name, aircraft, active?) -- every match is deleted
CUT = [
    # US active jets: vanilla has F-14, F/A-18 and bombers only as reinforcements; A-10 64 -> 32
    (1, "123rd Fighter Squadron", "F-14D", True), (1, "124th Fighter Squadron", "F-14D", True),
    (1, "310th Strike Fighter Squadron", "F/A-18E", True), (1, "563rd Strike Fighter Squadron", "F/A-18F", True),
    (1, "1st Bomb Squadron", "B-1B", True), (1, "56th Attack Squadron", "A-10", True),
    # US reinforcements: F-117 32 -> 16 (vanilla 12), no F-22 / B-2 (vanilla none), F-14 32 -> 16 (vanilla 24)
    (1, "9th Fighter Squadron", "F-117A", False), (1, "525th Fighter Squadron", "F-22A", False),
    (1, "13th Bomb Squadron", "B-2A", False), (1, "352nd Fighter Squadron", "F-14D", False),
    # attack helicopters 224 -> 32 (one AH-64D squadron)
    (1, "4th Attack Helo Squadron", "AH-64D", True), (2, "412nd Attack Helo Squadron", "AH-1", True),
    (2, "413rd Attack Helo Squadron", "AH-1", True), (2, "320th Attack Helo Squadron", "BO-105", True),
    (2, "454th Attack Squadron", "MD-500 TOW", True), (2, "455th Attack Squadron", "MD-500 TOW", True),
    # ROK active jets 336 -> 144: KF-16C 32, F-4E 48, F-5E 64
    (2, "357th Fighter Squadron", "F-15K", True), (2, "153rd Fighter Squadron", "F-4ESK", True),
    (2, "110th Fighter Squadron", "F-4DSK", True), (2, "151st Fighter Squadron", "F-4DSK", True),
    (2, "161st Fighter Squadron", "KF-16D", True), (2, "561st Fighter Squadron", "KF-16C", True),
    (2, "121st Fighter Squadron", "KF-16C", True), (2, "112nd Fighter Squadron", "F-5E", True),
    # ROK jet reinforcements: vanilla has none
    (2, "555th Fighter Squadron", "F-15K", False), (2, "201st Fighter Squadron", "F-5A", False),
    (2, "206th Fighter Squadron", "F-5E", False), (2, "111st Fighter Squadron", "F-5E", False),
    (2, "159th Fighter Squadron", "KF-16C", False), (2, "123rd Fighter Squadron", "KF-16D", False),
    (2, "169th Fighter Squadron", "F-4ESK", False), (2, "81st Fighter Squadron", "KF-16C", False),
]


def cut_blue(cam, ws):
    """Delete the CUT squadrons from a decoded save (units and the header's squadron list); returns (n, aircraft)."""
    import server  # noqa: E402 -- composed_name lives in the editor server
    names = ws.db.name_index()
    hits = []
    for u in cam.units:
        if u["kind"] != "squadron":
            continue
        _t, d = ws.db.data_row(u["classIndex"])
        ac = names.get(d["VehicleType"][0], "").strip()
        title = server.composed_name(names, u["classIndex"], u.get("nameId"))
        active = not (u["unitFlags"] & 0x20000)
        for c in CUT:
            if (u["owner"], title, ac, active) == c:
                hits.append((u, c))
    found = {c for _u, c in hits}
    missing = [c for c in CUT if c not in found]
    if missing:
        raise SystemExit("not found (already cut?): %s" % missing)
    raw = cam.units_raw
    for u, c in sorted(hits, key=lambda h: -h[0]["_span"][0]):  # back to front, spans stay valid
        raw = entities.delete_unit(raw, u)
    units = entities.walk_units(raw, cam.version, ws.db.class_rows())
    assert len(units) == len(cam.units) - len(hits)
    cam.units_raw, cam.units = raw, units
    cam.members[cam._member_name("uni")] = entities.encode_units(raw, len(units))
    # the new-campaign screen lists squadrons from the header before the units are loaded
    gone = {u["id"][0] & 0xFFFF for u, _c in hits}
    cam.header.squadrons = [s for s in cam.header.squadrons if (s["id"][0] & 0xFFFF) not in gone]
    cam.header_dirty = True
    return len(hits), sum(sum((u["roster"] >> (2 * g)) & 3 for g in range(16)) for u, _c in hits)


def main():
    S = workspace.Session(GAME)
    ws = S.workspace(make_ally_mod.TDF)
    cam = ws.units("save0.cam")
    if sum(1 for u in cam.units if u["kind"] == "battalion" and u["owner"] == 5) < 60:
        raise SystemExit("the install's save0.cam is not the Korea Escalation save (enable the mod)")
    n, planes = cut_blue(cam, ws)
    outs = [os.path.join(GAME, "MODS", "Korea Escalation", "campaign", "SAVE", "save0.cam"),
            os.path.join(GAME, "campaign", "SAVE", "save0.cam"),  # the mod is enabled: JSGME's copy
            os.path.join(GW, "escal.cam")]
    for p in outs:
        cam.save(p)
    print("deleted %d squadrons (%d aircraft) -> %s" % (n, planes, ", ".join(outs)))


if __name__ == "__main__":
    main()
