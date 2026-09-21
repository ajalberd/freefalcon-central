"""The game's own campaign-map icons, read straight out of the UI art.

The campaign map does not draw generic markers: every unit and objective type
carries an `IconIndex` into the UI image manager, and the art has a full set of
NATO-style symbols -- infantry, armour, mech, artillery, air defence, SAM,
towns, bridges, ports, airbases, ship silhouettes -- in eight team colours.
Reading them means following four files:

1. `art/main/imageids.id`     plain text, `NAME <tab> numeric id`. This is what
                              `IconIndex` is: `ICON_INFANTRY` is 10008.
2. `art/resource/imagerc.irc` `[LOADPRIVATERES] RED_TEAM_ICONS "art\\resource\\reddark"`
                              -- which file holds a given colour's set.
3. `<base>.idx`               int32 size, int32 version, then 60-byte
                              ImageHeader records (`src/ui95/imagersc.h`):
                              type, char ID[32], flags, centre x/y, w, h,
                              image offset, palette size, palette offset.
4. `<base>.rsc`               int32 size, int32 version, then the pixel data
                              the offsets point into.

The offsets are relative to the *data*, which starts after the .rsc's own
8-byte header (`C_Resmgr::LoadData` reads size and version before the buffer),
and that eight-byte shift is the whole trick -- read from byte zero and every
palette comes back black.

Pixels are 8-bit indices; the palette is 256 little-endian RGB555 words sitting
immediately after them. `0x7C1F` -- magenta -- is the colour key, which is how
the icons get their transparent background.
"""

import os
import re
import struct
import zlib

IMAGE_HEADER = struct.Struct("<i32si4h3i")
assert IMAGE_HEADER.size == 60

DATA_HEADER = 8          # int32 size + int32 version, on both .idx and .rsc
COLOUR_KEY = 0x7C1F      # magenta
RSC_IS_IMAGE = 100

# TeamColorIconIDs in src/ui/src/common/teamdata.cpp, in colour-index order.
# A team's colour index is the `team_colour` byte in the campaign header.
TEAM_COLOURS = ["white", "green", "blue", "brown",
                "orange", "yellow", "red", "grey"]

# imagerc.irc resource names for each of those, dark set then light set.
COLOUR_RESOURCES = {
    "white": ("WHITE_TEAM_ICONS", "WHITE_TEAM_ICONS_W"),
    "green": ("GREEN_TEAM_ICONS", "GREEN_TEAM_ICONS_W"),
    "blue": ("BLUE_TEAM_ICONS", "BLUE_TEAM_ICONS_W"),
    "brown": ("BROWN_TEAM_ICONS", "BROWN_TEAM_ICONS_W"),
    "orange": ("ORANGE_TEAM_ICONS", "ORANGE_TEAM_ICONS_W"),
    "yellow": ("YELLOW_TEAM_ICONS", "YELLOW_TEAM_ICONS_W"),
    "red": ("RED_TEAM_ICONS", "RED_TEAM_ICONS_W"),
    "grey": ("GREY_TEAM_ICONS", "GREY_TEAM_ICONS_W"),
}


def load_image_ids(gamedir):
    """`art/main/imageids.id` -> (name->id, id->name)."""
    path = os.path.join(gamedir, "art", "main", "imageids.id")
    by_name, by_id = {}, {}
    try:
        with open(path, "r", encoding="latin-1") as fp:
            for line in fp:
                parts = line.split()
                if len(parts) < 2:
                    continue
                try:
                    value = int(parts[-1])
                except ValueError:
                    continue
                name = parts[0]
                by_name[name] = value
                by_id.setdefault(value, name)
    except OSError:
        pass
    return by_name, by_id


_RES_LINE = re.compile(r'\[LOADP?R?I?V?A?T?E?RES\]\s+(\S+)\s+"([^"]+)"',
                       re.IGNORECASE)


def load_resource_map(gamedir):
    """`art/resource/imagerc.irc` -> {resource name: file base path}."""
    path = os.path.join(gamedir, "art", "resource", "imagerc.irc")
    out = {}
    try:
        with open(path, "r", encoding="latin-1") as fp:
            for line in fp:
                m = _RES_LINE.match(line.strip())
                if m:
                    out[m.group(1)] = m.group(2).replace("\\", "/")
    except OSError:
        pass
    return out


class IconSet:
    """One .idx/.rsc pair: the icons of a single team colour."""

    def __init__(self, base_path):
        self.base = base_path
        self.icons = {}
        self._data = b""
        self._load()

    def _load(self):
        idx_path, rsc_path = self.base + ".idx", self.base + ".rsc"
        if not (os.path.isfile(idx_path) and os.path.isfile(rsc_path)):
            raise FileNotFoundError(self.base)
        with open(idx_path, "rb") as fp:
            idx = fp.read()
        with open(rsc_path, "rb") as fp:
            rsc = fp.read()

        size = struct.unpack_from("<i", idx, 0)[0]
        self._data = rsc[DATA_HEADER:]
        count = size // IMAGE_HEADER.size
        for i in range(count):
            rec = IMAGE_HEADER.unpack_from(idx, DATA_HEADER + i * IMAGE_HEADER.size)
            kind, raw_id, flags, cx, cy, w, h, img, pal_n, pal = rec
            if kind != RSC_IS_IMAGE:
                continue
            name = raw_id.split(b"\0")[0].decode("latin-1")
            self.icons[name] = {
                "w": w, "h": h, "cx": cx, "cy": cy,
                "img": img, "pal": pal, "palN": pal_n, "flags": flags,
            }

    def names(self):
        return sorted(self.icons)

    def rgba(self, name):
        """Decode one icon to (width, height, RGBA bytes)."""
        e = self.icons.get(name)
        if not e:
            return None
        w, h = e["w"], e["h"]
        if w <= 0 or h <= 0:
            return None
        pix = self._data[e["img"]:e["img"] + w * h]
        if len(pix) < w * h:
            return None
        n = min(256, max(0, e["palN"]))
        pal = struct.unpack_from("<%dH" % n, self._data, e["pal"])

        lut = []
        for c in pal:
            if c == COLOUR_KEY:
                lut.append(b"\0\0\0\0")
            else:
                r = ((c >> 10) & 31) * 255 // 31
                g = ((c >> 5) & 31) * 255 // 31
                b = (c & 31) * 255 // 31
                lut.append(bytes((r, g, b, 255)))
        blank = b"\0\0\0\0"
        out = bytearray()
        for v in pix:
            out += lut[v] if v < len(lut) else blank
        return w, h, bytes(out)


def write_png(width, height, rgba):
    """Minimal RGBA PNG. Avoids a Pillow dependency for a handful of sprites."""
    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)                       # filter: none
        raw += rgba[y * stride:(y + 1) * stride]

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR",
                    struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))


class IconAtlas:
    """Every icon of one colour packed into a single PNG.

    One image and one manifest beats 75 sprite requests, and the canvas can
    then blit straight out of it.
    """

    def __init__(self, icon_set, pad=1):
        self.pad = pad
        self.frames = {}
        decoded = []
        for name in icon_set.names():
            got = icon_set.rgba(name)
            if got:
                decoded.append((name, got))
        if not decoded:
            self.width = self.height = 1
            self.png = write_png(1, 1, b"\0\0\0\0")
            return

        # Shelf packing, tallest first. The icons are tiny and few, so this is
        # within a few percent of optimal and costs nothing.
        decoded.sort(key=lambda kv: -kv[1][1])
        max_w = 512
        x = y = row_h = 0
        placed = []
        for name, (w, h, rgba) in decoded:
            if x + w + pad > max_w:
                x = 0
                y += row_h + pad
                row_h = 0
            placed.append((name, x, y, w, h, rgba))
            x += w + pad
            row_h = max(row_h, h)
        height = y + row_h

        buf = bytearray(max_w * height * 4)
        for name, px, py, w, h, rgba in placed:
            for row in range(h):
                dst = ((py + row) * max_w + px) * 4
                src = row * w * 4
                buf[dst:dst + w * 4] = rgba[src:src + w * 4]
            e = icon_set.icons[name]
            self.frames[name] = {"x": px, "y": py, "w": w, "h": h,
                                 "cx": e["cx"], "cy": e["cy"]}

        self.width, self.height = max_w, height
        self.png = write_png(max_w, height, bytes(buf))


class UiArt:
    """Everything the map needs to draw the game's own icons."""

    def __init__(self, gamedir, art_dirs=()):
        self.gamedir = gamedir
        # A theater can override art with its own directory; look there first.
        self.art_dirs = list(art_dirs) + [gamedir]
        self.name_to_id, self.id_to_name = load_image_ids(gamedir)
        self.resources = load_resource_map(gamedir)
        self._sets = {}
        self._atlases = {}

    def _resolve(self, rel):
        for root in self.art_dirs:
            path = os.path.join(root, *rel.split("/"))
            if os.path.isfile(path + ".idx"):
                return path
        return None

    def icon_set(self, colour, light=False):
        key = (colour, bool(light))
        if key in self._sets:
            return self._sets[key]
        names = COLOUR_RESOURCES.get(colour)
        if not names:
            return None
        rel = self.resources.get(names[1 if light else 0])
        if not rel:
            return None
        base = self._resolve(rel)
        if not base:
            return None
        try:
            self._sets[key] = IconSet(base)
        except (OSError, struct.error):
            self._sets[key] = None
        return self._sets[key]

    def atlas(self, colour, light=False):
        key = (colour, bool(light))
        if key in self._atlases:
            return self._atlases[key]
        st = self.icon_set(colour, light)
        self._atlases[key] = IconAtlas(st) if st else None
        return self._atlases[key]

    def icon_name(self, icon_index):
        """The `IconIndex` from a unit or objective row -> an icon name."""
        return self.id_to_name.get(int(icon_index or 0), "")

    def available(self):
        return bool(self.name_to_id) and bool(self.resources)
