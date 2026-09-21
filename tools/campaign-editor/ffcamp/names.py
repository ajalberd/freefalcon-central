"""The campaign's place-name table.

`LoadNames` / `ReadNameString` in `src/campaign/camplib/name.cpp` read a pair
of files: `<stem>.idx` is a short count followed by that many short offsets,
and `<stem>.wch` is one long char stream that the offsets slice up. Objectives
carry a `nameId` into it, which is how "Kuum-ni Airbase" and "Bupyong" get onto
the map instead of the class-table's "32_14 Airbase".

The offsets are signed shorts in the header but are used as file positions, so
a stream longer than 32767 bytes wraps negative. Reading them unsigned is what
the engine effectively does; Strings.wch in the shipped campaigns is already
past that point.
"""

import os
import struct


class NameTable:
    def __init__(self, offsets=(), stream=b""):
        self.offsets = offsets
        self.stream = stream

    def __len__(self):
        return max(0, len(self.offsets) - 1)

    def get(self, sid, default=""):
        if not (0 <= sid < len(self.offsets) - 1):
            return default
        a, b = self.offsets[sid], self.offsets[sid + 1]
        if b <= a or b > len(self.stream):
            return default
        text = self.stream[a:b].split(b"\0")[0]
        return text.decode("latin-1", "replace") or default


def _find(dirpath, filename):
    want = filename.lower()
    try:
        for name in os.listdir(dirpath):
            if name.lower() == want:
                return os.path.join(dirpath, name)
    except OSError:
        pass
    return None


def load(campaign_dir, stem="DEFAULT"):
    """Load <stem>.idx / <stem>.wch from a campaign directory."""
    idx_path = _find(campaign_dir, stem + ".idx")
    wch_path = _find(campaign_dir, stem + ".wch")
    if not idx_path or not wch_path:
        return NameTable()
    try:
        with open(idx_path, "rb") as fp:
            idx = fp.read()
        with open(wch_path, "rb") as fp:
            wch = fp.read()
    except OSError:
        return NameTable()
    if len(idx) < 2:
        return NameTable()

    count = struct.unpack_from("<h", idx, 0)[0]
    if count <= 0 or 2 + count * 2 > len(idx):
        return NameTable()
    offsets = [v & 0xFFFF
               for v in struct.unpack_from("<%dh" % count, idx, 2)]
    return NameTable(offsets, wch)
