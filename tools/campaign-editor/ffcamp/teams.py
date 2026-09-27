"""The .tea member: each team's war statistics.

`SaveTeams` (campaign/camplib/team.cpp) writes a short team count, then per
team a fixed head (`TeamClass::Save(FILE*)`) followed by its air, ground and
naval tasking managers, whose length varies with the mission requests queued
at the time. Only the head is read here, and the next team is found by its
own head rather than by parsing the managers:

    +0   VU_ID            8
    +8   entityType       u16  (the same for every team)
    +10  who, cteam       u8, u8
    +12  flags            i16
    +14  member[8]        u8   (countries on the team)
    +22  stance[8]        i16
    +38  first colonel/commander/wingman, last wingman   4 x i16
    +46  air, air-defence, ground, naval experience      4 x u8
    +50  initiative       i16
    +52  supplyAvail, fuelAvail, replacementsAvail       3 x u16
    +58  playerRating     f32
    +62  lastPlayerMission u32
    +66  currentStats     TeamStatusType, 16 bytes
    +82  startStats       TeamStatusType, 16 bytes
    +98  reinforcement    i16

TeamStatusType (campaign/include/team.h): airDefenseVehs, aircraft,
groundVehs, ships, supply, fuel, airbases (u16 each), supplyLevel,
fuelLevel (u8, percent). Valid for data version > 53 (every shipped file).
"""

import struct

STAT_FIELDS = ("airDefenseVehs", "aircraft", "groundVehs", "ships", "supply",
               "fuel", "airbases", "supplyLevel", "fuelLevel")
_STATS = struct.Struct("<7H2B")
HEAD = 100


def _stats(b, off):
    return dict(zip(STAT_FIELDS, _STATS.unpack_from(b, off)))


def _head(b, off):
    who, cteam = b[off + 10], b[off + 11]
    exp = struct.unpack_from("<4B", b, off + 46)
    initiative, sup, fuel, repl = struct.unpack_from("<h3H", b, off + 50)
    return {
        "who": who, "cteam": cteam,
        "flags": struct.unpack_from("<h", b, off + 12)[0],
        # RelType (campaign/include/cmpglobl.h): 0 none, 1 allied,
        # 2 friendly, 3 neutral, 4 hostile, 5 war.
        "stance": list(struct.unpack_from("<8h", b, off + 22)),
        "experience": {"air": exp[0], "airDefense": exp[1], "ground": exp[2],
                       "naval": exp[3]},
        "initiative": initiative,
        "supplyAvail": sup, "fuelAvail": fuel, "replacementsAvail": repl,
        "current": _stats(b, off + 66),
        "start": _stats(b, off + 82),
        "reinforcement": struct.unpack_from("<h", b, off + 98)[0],
        "_at": off,
    }


def decode(tea):
    """Every team's head, in file order. Empty if the member is not there."""
    if not tea or len(tea) < 2 + HEAD:
        return []
    count = struct.unpack_from("<h", tea, 0)[0]
    first = 2
    etype = struct.unpack_from("<H", tea, first + 8)[0]
    out = [_head(tea, first)]
    pos = first + HEAD
    while len(out) < count:
        # The next head: same entity type, and its `who` one past the last.
        want = struct.pack("<H", etype)
        found = -1
        at = tea.find(want, pos)
        while at != -1:
            start = at - 8
            if start >= pos and start + HEAD <= len(tea) and \
                    tea[at + 2] == out[-1]["who"] + 1 and tea[at + 3] < 8:
                found = start
                break
            at = tea.find(want, at + 1)
        if found < 0:
            break
        out.append(_head(tea, found))
        pos = found + HEAD
    return out
