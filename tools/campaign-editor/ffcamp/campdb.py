"""Read and write a theater's CampaignDB directory.

A CampaignDB is ~20 flat tables plus the class table that ties them together.
The class table (FALCON4.ct) is the index: each of its entries carries an
8-byte class key, the visual model ids, a dataType saying which per-type table
describes it, and a dataPtr that on disk is the row number in that table.

Loading is lazy per table and writing only touches tables that changed, so an
edit to one weapon rewrites FALCON4.WCD and nothing else.
"""

import os
import struct

from . import records


class Table:
    """One CampaignDB file: a framing, a spec, and the rows."""

    def __init__(self, spec, rows, ffdbc, path, raw=None):
        self.spec = spec
        self.rows = rows
        self.ffdbc = ffdbc
        self.path = path
        # Bytes each row was read from, or None for rows added here. Used to
        # keep padding and post-NUL junk intact on rows nobody edited.
        self.raw = raw if raw is not None else [None] * len(rows)
        self.dirty = False

    @classmethod
    def load(cls, spec, path):
        with open(path, "rb") as fp:
            buf = fp.read()
        if len(buf) < 4:
            return cls(spec, [], True, path)

        head = struct.unpack_from("<h", buf, 0)[0]
        if head == 0:
            # FFDBC framing: real count lives in the last two bytes.
            count = struct.unpack_from("<h", buf, len(buf) - 2)[0]
            ffdbc = True
            body_end = len(buf) - 2
        else:
            count = head
            ffdbc = False
            body_end = len(buf)

        if count < 0:
            count += 1 << 16
        avail = (body_end - 2) // spec.size
        if count > avail:
            raise ValueError(
                "%s: header claims %d rows but only %d fit in %d bytes"
                % (os.path.basename(path), count, avail, len(buf)))

        raw = [bytes(buf[2 + i * spec.size:2 + (i + 1) * spec.size])
               for i in range(count)]
        rows = [spec.unpack(r) for r in raw]
        return cls(spec, rows, ffdbc, path, raw)

    def to_bytes(self):
        while len(self.raw) < len(self.rows):
            self.raw.append(None)
        body = b"".join(self.spec.pack(r, self.raw[i])
                        for i, r in enumerate(self.rows))
        n = len(self.rows)
        if self.ffdbc:
            return struct.pack("<h", 0) + body + struct.pack("<H", n & 0xFFFF)
        return struct.pack("<H", n & 0xFFFF) + body

    def save(self, path=None):
        path = path or self.path
        tmp = path + ".tmp"
        with open(tmp, "wb") as fp:
            fp.write(self.to_bytes())
        os.replace(tmp, path)
        self.path = path
        self.dirty = False

    def blank_row(self):
        row = {}
        for f in self.spec.fields:
            if f.is_pad:
                continue
            if f.kind == "str":
                row[f.name] = ""
            elif f.count > 1:
                row[f.name] = [0] * f.count
            elif f.kind == "f32":
                row[f.name] = 0.0
            else:
                row[f.name] = 0
        return row


def _find(dirpath, ext):
    """Locate FALCON4.<ext> case-insensitively; the shipped casing varies."""
    want = ("falcon4." + ext).lower()
    try:
        entries = os.listdir(dirpath)
    except OSError:
        return None
    for name in entries:
        if name.lower() == want:
            return os.path.join(dirpath, name)
    return None


class CampaignDB:
    def __init__(self, dirpath):
        self.dir = dirpath
        self._tables = {}
        self._missing = set()

    def path_for(self, spec):
        return _find(self.dir, spec.ext) or os.path.join(
            self.dir, "FALCON4." + spec.ext)

    def table(self, name):
        """Load a table on first use. Returns None if the file is absent."""
        if name in self._tables:
            return self._tables[name]
        if name in self._missing:
            return None
        spec = records.BY_NAME[name]
        path = _find(self.dir, spec.ext)
        if path is None:
            self._missing.add(name)
            return None
        tbl = Table.load(spec, path)
        self._tables[name] = tbl
        return tbl

    def present(self):
        """Table names whose file exists on disk, in UI order."""
        out = []
        for spec in records.ALL_SPECS:
            if _find(self.dir, spec.ext) is not None:
                out.append(spec.name)
        return out

    def save_dirty(self):
        saved = []
        for name, tbl in self._tables.items():
            if tbl.dirty:
                tbl.save()
                saved.append(os.path.basename(tbl.path))
        return saved

    # --- cross-table helpers -------------------------------------------------

    def class_rows(self):
        tbl = self.table("class")
        return tbl.rows if tbl else []

    def data_row(self, class_index):
        """The per-type row a class-table entry points at, or None.

        class_index is 0-based into FALCON4.ct. In the engine an entity's
        entityType_ is this index plus VU_LAST_ENTITY_TYPE (100).
        """
        rows = self.class_rows()
        if not (0 <= class_index < len(rows)):
            return None, None
        ent = rows[class_index]
        tname = records.DTYPE_TABLE.get(ent["dataType"])
        if tname is None:
            return None, None
        tbl = self.table(tname)
        if tbl is None:
            return None, None
        idx = ent["dataPtr"]
        if not (0 <= idx < len(tbl.rows)):
            return tname, None
        return tname, tbl.rows[idx]

    def class_name(self, class_index):
        """What GetClassName() would return for this class-table index."""
        tname, row = self.data_row(class_index)
        if row is None:
            return ""
        return row.get("Name", row.get("mnemonic", ""))

    def name_index(self):
        """class-table index -> display name, for every entry that has one."""
        out = {}
        for i in range(len(self.class_rows())):
            nm = self.class_name(i)
            if nm:
                out[i] = nm
        return out
