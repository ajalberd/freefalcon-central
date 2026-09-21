"""The .cam / .tac container and the campaign header inside it.

A .cam is what the engine calls a "campressed" file (StartReadCampFile in
src/campaign/campupd/campaign.cpp):

    int32  offset of the directory
    ...    member payloads, back to back
    at offset:
    int32  member count
    then per member: uint8 name length, name, int32 offset, int32 size

Members are named "<campaign>.<ext>". The ones that matter here:

    .ver   campaign data version, as ASCII digits ("73")
    .cmp   the campaign header -- everything on the Campaign tab
    .obj   objective list (aggregated, LZSS'd, VU entity stream)
    .uni   unit list
    .tea   per-team data
    .plt   pilot records, .wth weather, .evt events, .pst / .obd deltas

.cmp itself is  int32 outer_size, int32 raw_size, LZSS(raw).  The decoded
stream is a flat sequence of fields whose shape depends on the version number
in .ver -- CampaignClass::Decode is the reference, and the version gates below
mirror it.  Only versions >= 52 are written back; that covers every shipped
campaign (all are v73) and avoids guessing at layouts we cannot test against.
"""

import struct

from . import lzss

TEAMS = 8
CAMP_NAME_SIZE = 40
SQUAD_UI_SIZE = 68        # SquadUIInfoClass, natural alignment
DISK_EVENT_SIZE = 20      # DiskUIEventNode, see the x64 note in cmpclass.cpp


# --- container ---------------------------------------------------------------

def read_container(buf):
    """Return {member name: bytes} preserving on-disk order."""
    out = {}
    if len(buf) < 8:
        return out
    dir_off = struct.unpack_from("<i", buf, 0)[0]
    count = struct.unpack_from("<i", buf, dir_off)[0]
    p = dir_off + 4
    for _ in range(count):
        ln = buf[p]
        p += 1
        name = buf[p:p + ln].decode("latin-1")
        p += ln
        off, size = struct.unpack_from("<ii", buf, p)
        p += 8
        out[name] = buf[off:off + size]
    return out


def build_container(members, order=None):
    """Inverse of read_container. `order` keeps the original member order."""
    names = list(order or members.keys())
    for n in members:
        if n not in names:
            names.append(n)

    body = bytearray(struct.pack("<i", 0))
    entries = []
    for name in names:
        data = members[name]
        entries.append((name, len(body), len(data)))
        body.extend(data)

    dir_off = len(body)
    body.extend(struct.pack("<i", len(entries)))
    for name, off, size in entries:
        raw = name.encode("latin-1")
        body.append(len(raw))
        body.extend(raw)
        body.extend(struct.pack("<ii", off, size))

    struct.pack_into("<i", body, 0, dir_off)
    return bytes(body)


# --- the .cmp header ---------------------------------------------------------

class _Cursor:
    def __init__(self, buf):
        self.buf = buf
        self.p = 0

    def take(self, n):
        chunk = self.buf[self.p:self.p + n]
        if len(chunk) != n:
            raise ValueError("campaign header truncated at byte %d" % self.p)
        self.p += n
        return chunk

    def u8(self):
        return self.take(1)[0]

    def i8(self):
        return struct.unpack("<b", self.take(1))[0]

    def i16(self):
        return struct.unpack("<h", self.take(2))[0]

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def i32(self):
        return struct.unpack("<i", self.take(4))[0]

    def i32s(self, n):
        return list(struct.unpack("<%di" % n, self.take(4 * n)))

    def text(self, n):
        return self.take(n).split(b"\0")[0].decode("latin-1")

    def textr(self, n):
        raw = self.take(n)
        return raw.split(b"\0")[0].decode("latin-1"), raw


def _pad_text(s, n, original=None):
    """Fixed-size char array. Keeps `original` when the text is unchanged.

    These are memcpy'd whole by the engine, so whatever followed the NUL in
    the original array is still on disk. Rewriting it with zeroes would change
    the file for no reason, and makes a no-op save impossible to verify.
    """
    raw = str(s).encode("latin-1", "replace")[:n]
    if original is not None and original.split(b"\0")[0] == raw.split(b"\0")[0]:
        return original
    return raw + b"\0" * (n - len(raw))


# Scalar fields decoded in order, after the TE block and the team block.
# (name, reader-method, writer-format)
_TAIL_SCALARS = [
    ("lastMajorEvent", "u32", "<I"),
    ("lastResupply", "u32", "<I"),
    ("lastRepair", "u32", "<I"),
    ("lastReinforcement", "u32", "<I"),
    ("TimeStamp", "i16", "<h"),
    ("Group", "i16", "<h"),
    ("GroundRatio", "i16", "<h"),
    ("AirRatio", "i16", "<h"),
    ("AirDefenseRatio", "i16", "<h"),
    ("NavalRatio", "i16", "<h"),
    ("Brief", "i16", "<h"),
    ("TheaterSizeX", "i16", "<h"),
    ("TheaterSizeY", "i16", "<h"),
    ("CurrentDay", "u8", "<B"),
    ("ActiveTeams", "u8", "<B"),
    ("DayZero", "u8", "<B"),
    ("EndgameResult", "u8", "<B"),
    ("Situation", "u8", "<B"),
    ("EnemyAirExp", "u8", "<B"),
    ("EnemyADExp", "u8", "<B"),
    ("BullseyeName", "u8", "<B"),
    ("BullseyeX", "i16", "<h"),
    ("BullseyeY", "i16", "<h"),
]

# Fields the UI is allowed to change. Everything else round-trips untouched:
# the engine rebuilds tasking times and map data anyway, and rewriting them
# from a GUI would only invite a desync.
EDITABLE = {
    "CurrentTime", "TE_StartTime", "TE_TimeLimit", "TE_VictoryPoints",
    "TE_type", "TE_number_teams", "TE_team", "TE_flags",
    "GroundRatio", "AirRatio", "AirDefenseRatio", "NavalRatio", "Brief",
    "CurrentDay", "ActiveTeams", "DayZero", "EndgameResult", "Situation",
    "EnemyAirExp", "EnemyADExp", "BullseyeName", "BullseyeX", "BullseyeY",
    "TheaterName", "Scenario", "UIName", "Tempo",
}

FIELD_DOC = {
    "CurrentTime": "Campaign clock, in milliseconds since day zero",
    "TE_StartTime": "Tactical engagement start time (ms)",
    "TE_TimeLimit": "Tactical engagement time limit (ms)",
    "TE_VictoryPoints": "Points needed to win a tactical engagement",
    "TE_flags": "Tactical engagement option bits",
    "GroundRatio": "Friendly vs enemy ground strength at start (%)",
    "AirRatio": "Friendly vs enemy air strength at start (%)",
    "AirDefenseRatio": "Friendly vs enemy air-defence strength at start (%)",
    "NavalRatio": "Friendly vs enemy naval strength at start (%)",
    "Brief": "Index of the theater briefing text",
    "CurrentDay": "Day of the campaign",
    "ActiveTeams": "How many teams take part",
    "DayZero": "Day the war starts",
    "EndgameResult": "0 = running; otherwise who won",
    "Situation": "How the war is going for the player's team",
    "BullseyeName": "Bullseye name index",
    "BullseyeX": "Bullseye grid X",
    "BullseyeY": "Bullseye grid Y",
    "TheaterName": "Theater this campaign belongs to",
    "Scenario": "Scenario name shown in the UI",
    "UIName": "Campaign title shown in the UI",
    "Tempo": "Campaign pace (affects resupply and reinforcement rates)",
}

TEAM_FIELD_DOC = {
    "flag": "Team flag / nationality index",
    "colour": "Map colour index",
    "name": "Team name",
    "motto": "Team motto shown on the briefing",
}


class CampaignHeader:
    """A decoded .cmp payload that can be edited and re-encoded."""

    def __init__(self, version, raw):
        self.version = version
        self.raw = raw
        self.fields = {}
        self.teams = []
        self.squadrons = []
        self.events = ([], [])
        self.map_data = b""
        self.tail = b""        # everything after the last field we model
        self.trailing = 0
        self._text_raw = {}    # fixed char arrays, exactly as they were read
        self._decode()

    # -- decode ---------------------------------------------------------------

    def _decode(self):
        v = self.version
        if v < 52:
            raise ValueError(
                "campaign data version %d is older than this editor supports "
                "(needs 52+; every shipped campaign is 73)" % v)

        c = _Cursor(self.raw)
        f = self.fields
        f["CurrentTime"] = c.u32()
        f["TE_StartTime"] = c.u32()
        f["TE_TimeLimit"] = c.u32()
        f["TE_VictoryPoints"] = c.i32()
        f["TE_type"] = c.i32()
        f["TE_number_teams"] = c.i32()
        f["TE_number_aircraft"] = c.i32s(TEAMS)
        f["TE_number_f16s"] = c.i32s(TEAMS)
        f["TE_team"] = c.i32()
        f["TE_team_pts"] = c.i32s(TEAMS)
        f["TE_flags"] = c.i32()

        for i in range(TEAMS):
            flag, colour = c.u8(), c.u8()
            name, name_raw = c.textr(20)
            motto, motto_raw = c.textr(200)
            self._text_raw["team%d.name" % i] = name_raw
            self._text_raw["team%d.motto" % i] = motto_raw
            self.teams.append({"flag": flag, "colour": colour,
                               "name": name, "motto": motto})

        for name, kind, _fmt in _TAIL_SCALARS:
            f[name] = getattr(c, kind)()

        for name in ("TheaterName", "Scenario", "SaveFile", "UIName"):
            f[name], self._text_raw[name] = c.textr(CAMP_NAME_SIZE)
        f["PlayerSquadronID"] = list(struct.unpack("<II", c.take(8)))

        self.events = (self._read_events(c), self._read_events(c))

        map_size = c.i16()
        self.map_data = c.take(map_size) if map_size > 0 else b""
        f["_CampMapSize"] = map_size

        f["LastIndexNum"] = c.i16()
        n_squad = c.i16()
        for _ in range(max(0, n_squad)):
            self.squadrons.append(self._read_squadron(c))

        f["Tempo"] = c.u8()
        if v >= 43:
            f["CreatorIP"] = c.i32()
            f["CreationTime"] = c.i32()
            f["CreationRand"] = c.i32()

        self.tail = self.raw[c.p:]
        self.trailing = len(self.tail)

    @staticmethod
    def _read_events(c):
        out = []
        for _ in range(max(0, c.i16())):
            # DiskUIEventNode is pack(4): short x, short y, u32 time, u8
            # flags, u8 team, 2 bytes of padding, then the two dead x86
            # pointer slots. 20 bytes, and the padding is real bytes on disk.
            node = c.take(DISK_EVENT_SIZE)
            x, y, when, flags, team = struct.unpack_from("<hhIBB", node, 0)
            size = c.i16()
            text = c.take(max(0, size)).decode("latin-1")
            out.append({"x": x, "y": y, "time": when, "flags": flags,
                        "team": team, "text": text, "_raw": node})
        return out

    @staticmethod
    def _read_squadron(c):
        raw = c.take(SQUAD_UI_SIZE)
        x, y, id_num, id_creator = struct.unpack_from("<ffII", raw, 0)
        d_index, name_id, airbase_icon, patch = struct.unpack_from("<4h", raw, 16)
        specialty, strength, country = struct.unpack_from("<3B", raw, 24)
        airbase = raw[27:67].split(b"\0")[0].decode("latin-1")
        return {"x": x, "y": y, "id": [id_num, id_creator],
                "dIndex": d_index, "nameId": name_id,
                "airbaseIcon": airbase_icon, "squadronPatch": patch,
                "specialty": specialty, "currentStrength": strength,
                "country": country, "airbaseName": airbase,
                "_raw": raw}

    SQUAD_LAYOUT = [
        ("x", "<f", 0), ("y", "<f", 4),
        ("dIndex", "<h", 16), ("nameId", "<h", 18),
        ("airbaseIcon", "<h", 20), ("squadronPatch", "<h", 22),
        ("specialty", "<B", 24), ("currentStrength", "<B", 25),
        ("country", "<B", 26),
    ]

    @classmethod
    def _write_squadron(cls, s):
        out = bytearray(s.get("_raw") or bytes(SQUAD_UI_SIZE))
        for key, fmt, off in cls.SQUAD_LAYOUT:
            struct.pack_into(fmt, out, off, s[key])
        struct.pack_into("<II", out, 8, *s["id"])
        out[27:67] = _pad_text(s["airbaseName"], 40,
                               bytes(out[27:67]) if s.get("_raw") else None)
        return bytes(out)

    # -- encode ---------------------------------------------------------------

    def encode(self):
        f = self.fields
        out = bytearray()
        out += struct.pack("<III i", f["CurrentTime"], f["TE_StartTime"],
                           f["TE_TimeLimit"], f["TE_VictoryPoints"])
        out += struct.pack("<ii", f["TE_type"], f["TE_number_teams"])
        out += struct.pack("<%di" % TEAMS, *f["TE_number_aircraft"])
        out += struct.pack("<%di" % TEAMS, *f["TE_number_f16s"])
        out += struct.pack("<i", f["TE_team"])
        out += struct.pack("<%di" % TEAMS, *f["TE_team_pts"])
        out += struct.pack("<i", f["TE_flags"])

        for i, t in enumerate(self.teams):
            out += struct.pack("<BB", t["flag"] & 0xFF, t["colour"] & 0xFF)
            out += _pad_text(t["name"], 20,
                             self._text_raw.get("team%d.name" % i))
            out += _pad_text(t["motto"], 200,
                             self._text_raw.get("team%d.motto" % i))

        for name, _kind, fmt in _TAIL_SCALARS:
            out += struct.pack(fmt, f[name])

        for name in ("TheaterName", "Scenario", "SaveFile", "UIName"):
            out += _pad_text(f[name], CAMP_NAME_SIZE, self._text_raw.get(name))
        out += struct.pack("<II", *f["PlayerSquadronID"])

        for queue in self.events:
            out += struct.pack("<h", len(queue))
            for e in queue:
                node = bytearray(e.get("_raw") or bytes(DISK_EVENT_SIZE))
                struct.pack_into("<hhIBB", node, 0, e["x"], e["y"],
                                 e["time"], e["flags"], e["team"])
                out += bytes(node)
                raw = e["text"].encode("latin-1", "replace")
                out += struct.pack("<h", len(raw)) + raw

        out += struct.pack("<h", f["_CampMapSize"])
        out += self.map_data

        out += struct.pack("<hh", f["LastIndexNum"], len(self.squadrons))
        for s in self.squadrons:
            out += self._write_squadron(s)

        out += struct.pack("<B", f["Tempo"] & 0xFF)
        if self.version >= 43:
            out += struct.pack("<iii", f["CreatorIP"], f["CreationTime"],
                               f["CreationRand"])

        out += self.tail
        return bytes(out)

    def to_section(self):
        """Re-wrap as the bytes of a .cmp container member."""
        raw = self.encode()
        packed = lzss.compress(raw)
        return struct.pack("<ii", len(packed) + 4, len(raw)) + packed


def decode_header(section, version):
    outer, raw_size = struct.unpack_from("<ii", section, 0)
    raw = lzss.expand(section[8:], raw_size)
    return CampaignHeader(version, raw)


class CampaignFile:
    """A .cam or .tac on disk."""

    def __init__(self, path, members, order, stem):
        self.path = path
        self.members = members
        self.order = order
        self.stem = stem
        self.version = 0
        self.header = None
        # Only sections that were actually edited get re-encoded. Everything
        # else is written back as the exact bytes it was read as, so a save
        # that touched one unit does not disturb the objective list, the
        # weather, or the pilot roster.
        self.header_dirty = False

    @classmethod
    def load(cls, path):
        with open(path, "rb") as fp:
            members = read_container(fp.read())
        order = list(members.keys())
        stem = order[0].rsplit(".", 1)[0] if order else ""
        cam = cls(path, members, order, stem)

        ver = cam.member("ver")
        if ver:
            try:
                cam.version = int(ver.decode("latin-1").strip("\0 \r\n") or 0)
            except ValueError:
                cam.version = 0
        cmp_ = cam.member("cmp")
        if cmp_:
            cam.header = decode_header(cmp_, cam.version)
        return cam

    def member(self, ext):
        for name, data in self.members.items():
            if name.rsplit(".", 1)[-1].lower() == ext.lower():
                return data
        return None

    def _member_name(self, ext):
        for name in self.members:
            if name.rsplit(".", 1)[-1].lower() == ext.lower():
                return name
        return None

    def save(self, path=None):
        path = path or self.path
        if self.header is not None and self.header_dirty:
            name = self._member_name("cmp")
            if name:
                self.members[name] = self.header.to_section()
                self.header_dirty = False
        blob = build_container(self.members, self.order)
        tmp = path + ".tmp"
        with open(tmp, "wb") as fp:
            fp.write(blob)
        import os
        os.replace(tmp, path)
        self.path = path

    def summary(self):
        return [{"name": n, "bytes": len(self.members[n])} for n in self.order]
