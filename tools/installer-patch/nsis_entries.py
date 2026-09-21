r"""Read and edit an NSIS header's instruction block.

`patch_installer.py` rewrites strings. This reads the code around them, which is
what it takes to answer "why did that check fail" and to switch a check off.

An NSIS 2 header opens with a flags dword and eight (offset, count) block
descriptors: PAGES, SECTIONS, ENTRIES, STRINGS, LANGTABLES, CTLCOLORS, BGFONT,
DATA. Entries are 28 bytes -- an opcode and six parameters. A parameter is an
offset into the string block, a literal, or a **branch target stored as
index + 1**, so zero means "no jump, fall through to the next instruction".

Strings carry variable references inline: byte 0xFD, then the variable index in
two 7-bit bytes with the high bit set (0xFC skip, 0xFE shell folder, 0xFD
variable, 0xFF language string). That is how `$INSTDIR` survives into a message.

Nothing here writes a file; `Installer.build()` in patch_installer does that,
and everything below edits the header in place first.
"""

import struct

# The opcodes this tool needs to recognise by name. The rest print as opN.
OPS = {
    1: "Return", 2: "Jump", 3: "Abort", 4: "Quit", 5: "Call",
    6: "UpdateText", 7: "Sleep", 10: "SetFileAttributes", 11: "CreateDir",
    12: "IfFileExists", 13: "SetFlag", 14: "IfFlag", 15: "GetFlag",
    16: "Rename", 17: "GetFullPathName", 18: "SearchPath",
    19: "GetTempFileName", 20: "ExtractFile", 21: "DeleteFile",
    22: "MessageBox", 23: "RMDir", 24: "StrLen", 25: "StrCpy", 26: "StrCmp",
    27: "ReadEnvStr", 28: "IntCmp", 29: "IntOp", 30: "IntFmt", 31: "PushPop",
    32: "FindWindow", 33: "SendMessage", 34: "IsWindow", 35: "GetDlgItem",
    39: "ShowWindow", 40: "ShellExec", 41: "Execute", 42: "GetFileTime",
    43: "GetDLLVersion", 44: "RegisterDLL", 45: "CreateShortcut",
    46: "CopyFiles", 48: "WriteINIStr", 49: "ReadINIStr", 50: "DeleteReg",
    51: "WriteReg", 52: "ReadRegStr", 53: "RegEnum", 62: "WriteUninstaller",
}

ENTRY_SIZE = 28
BLOCK_NAMES = ["PAGES", "SECTIONS", "ENTRIES", "STRINGS", "LANGTABLES",
               "CTLCOLORS", "BGFONT", "DATA"]

# Which parameter of each conditional is "the test failed" and which is "passed".
# (index of the branch taken when the condition HOLDS, when it does NOT)
CONDITIONALS = {
    12: (1, 2),   # IfFileExists  file, if_exists, if_not
    26: (2, 3),   # StrCmp        a, b, if_equal, if_not_equal
    14: (1, 2),   # IfFlag
}

VARS = (["$%d" % i for i in range(10)] + ["$R%d" % i for i in range(10)]
        + ["$CMDLINE", "$INSTDIR", "$OUTDIR", "$EXEDIR", "$LANGUAGE"])


class Entries:
    """The ENTRIES block of one installer, decoded and editable."""

    def __init__(self, installer):
        self.inst = installer
        h = installer.header
        self.blocks = [struct.unpack_from("<II", h, 4 + i * 8)
                       for i in range(8)]
        self.entries_at, self.count = self.blocks[2]
        self.strings_at = self.blocks[3][0]
        self.strings_end = self.blocks[4][0]

    # -- strings --------------------------------------------------------------

    @property
    def strings(self):
        return self.inst.header[self.strings_at:self.strings_end]

    def text(self, offset):
        """One string-table entry, with variable references spelled out."""
        s = self.strings
        if not (0 <= offset < len(s)):
            return None
        end = s.find(b"\0", offset)
        if end < 0:
            return None
        raw, out, i = s[offset:end], [], 0
        while i < len(raw):
            c = raw[i]
            if c >= 0xFC and i + 2 < len(raw):
                n = (raw[i + 1] & 0x7F) | ((raw[i + 2] & 0x7F) << 7)
                if c == 0xFD:
                    out.append(VARS[n] if n < len(VARS) else "$var%d" % n)
                elif c == 0xFE:
                    out.append("$SHELL%d" % n)
                elif c == 0xFF:
                    out.append("$LANG%d" % n)
                else:
                    out.append(chr(raw[i + 1]))
                i += 3
            else:
                out.append(chr(c))
                i += 1
        return "".join(out)

    def find_text(self, needle):
        """Offsets of string-table entries containing `needle` (case-folded)."""
        want = needle.lower()
        out, at = [], 0
        s = self.strings
        while at < len(s):
            end = s.find(b"\0", at)
            if end < 0:
                break
            if want in s[at:end].decode("latin-1").lower():
                out.append(at)
            at = end + 1
        return out

    # -- instructions ---------------------------------------------------------

    def get(self, i):
        o = self.entries_at + i * ENTRY_SIZE
        which = struct.unpack_from("<I", self.inst.header, o)[0]
        params = list(struct.unpack_from("<6I", self.inst.header, o + 4))
        return which, params

    def op(self, i):
        which, _ = self.get(i)
        return OPS.get(which, "op%d" % which)

    def set_param(self, i, n, value):
        """Rewrite one parameter of one instruction, in place."""
        o = self.entries_at + i * ENTRY_SIZE + 4 + n * 4
        header = bytearray(self.inst.header)
        struct.pack_into("<I", header, o, value & 0xFFFFFFFF)
        self.inst.header = bytes(header)

    def describe(self, i):
        which, params = self.get(i)
        bits = []
        for p in params:
            t = self.text(p)
            bits.append("%r" % t if t and t.strip() else str(p))
        return "%4d  %-16s %s" % (i, self.op(i), "  ".join(bits))

    def dump(self, lo, hi):
        return [self.describe(i) for i in range(max(0, lo),
                                                min(self.count, hi))]
