#!/usr/bin/env python3
r"""Retarget an old Falcon 4 theater installer at FreeFalcon's registry key.

Falcon 4.0 recorded its install path in

    HKLM\Software\MicroProse\Falcon\4.0    baseDir = D:\GOG Games\Falcon 4.0

and FreeFalcon, being a separate install, uses `...\Falcon\4.1` instead:

    HKLM\Software\MicroProse\Falcon\4.1    baseDir = C:\FreeFalcon6
                                           FFver   = 6.0

Theater installers written for plain Falcon 4 read the `4.0` key, so on a
machine that has both they unpack into the wrong game -- and one that then
checks `FFver` (which only exists under `4.1`) aborts with "Unable to read
FreeFalcon version" no matter what is installed.

The fix is one byte. The key lives as a literal string in the installer's
NSIS header, `4.0` and `4.1` are the same length, and the registry is
case-insensitive, so the string can be rewritten where it sits without
disturbing NSIS's string table. What this script has to do is rebuild the
container around it: decompress the header block, patch, recompress, fix the
three sizes NSIS records and the CRC32 it checks at startup.

The original file is never modified; the patched installer is written beside
it under a new name.

    python patch_installer.py "ITO2 V4c.exe"              # patch it
    python patch_installer.py "ITO2 V4c.exe" --list       # just look
    python patch_installer.py *.exe --to 4.1              # a whole folder
    python patch_installer.py --emit-shim falcon41.reg    # the other way out

Tested against NSIS 2.x installers with LZMA, bzip2, zlib and stored headers.
For an installer this cannot read (Inno Setup, InstallShield, a self-extractor
it does not recognise), use --emit-shim: mirroring 4.1's values into a 4.0 key
makes every such installer find the right directory, at the cost of pointing
anything that really wants Falcon 4.0 at FreeFalcon until you remove it.
"""

import argparse
import bz2
import glob
import lzma
import os
import struct
import sys
import zlib

# NSIS marks the first header with this, right after a 4-byte flags field.
SIGNATURE = b"\xef\xbe\xad\xde" + b"NullsoftInst"

# The stub CRCs the file from 512 bytes in, leaving the DOS stub out of it.
CRC_SKIP = 512

FH_FLAGS_NO_CRC = 4
FH_FLAGS_FORCE_CRC = 8

COMPRESSED = 0x80000000


class Unsupported(Exception):
    """The file is not something this script knows how to rewrite."""


# --- the compressed block ----------------------------------------------------

def _lzma_filters(props):
    """NSIS writes a bare LZMA1 stream: 5 bytes of properties, then data."""
    byte, dict_size = props[0], struct.unpack_from("<I", props, 1)[0]
    if byte >= 9 * 5 * 5:
        raise Unsupported("LZMA properties byte %d is out of range" % byte)
    lc = byte % 9
    rest = byte // 9
    return [{"id": lzma.FILTER_LZMA1, "lc": lc, "lp": rest % 5,
             "pb": rest // 5, "dict_size": dict_size}]


def _looks_like_lzma(blob):
    """A bare LZMA1 stream: a properties byte, then a sane dictionary size.

    NSIS always builds with lc=3 lp=0 pb=2 (0x5D) and a power-of-two window,
    so this is a tighter test than it looks.
    """
    if len(blob) < 6 or blob[0] >= 9 * 5 * 5:
        return False
    dict_size = struct.unpack_from("<I", blob, 1)[0]
    return (1 << 12) <= dict_size <= (1 << 30) and not (dict_size & (dict_size - 1))


def decompress(blob, expected):
    """Inflate one NSIS block, returning (bytes, codec, codec-state)."""
    if _looks_like_lzma(blob):
        filters = _lzma_filters(blob[:5])
        dec = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=filters)
        # `expected` of -1 means "as much as there is": a solid archive's
        # stream is the header and every packed file at once, and only the
        # stream itself knows how long that is.
        out = (dec.decompress(blob[5:]) if expected < 0
               else dec.decompress(blob[5:], expected))
        return out, "lzma", (blob[:5], filters)
    if blob[:3] == b"BZh":
        return bz2.decompress(blob), "bzip2", None
    try:
        return zlib.decompress(blob, -zlib.MAX_WBITS), "deflate", None
    except zlib.error:
        raise Unsupported("the header block uses a compressor I don't know "
                          "(first bytes %s)" % blob[:6].hex())


def compress(data, codec, state):
    """Deflate a block the way the stub that reads it expects."""
    if codec == "lzma":
        props, filters = state
        # LZMA1 in raw mode takes no end marker, which is what NSIS wants: it
        # decodes exactly as many bytes as the header says and stops.
        c = lzma.LZMACompressor(format=lzma.FORMAT_RAW, filters=filters)
        return props + c.compress(data) + c.flush()
    if codec == "bzip2":
        return bz2.compress(data, 9)
    if codec == "deflate":
        c = zlib.compressobj(9, zlib.DEFLATED, -zlib.MAX_WBITS)
        return c.compress(data) + c.flush()
    raise Unsupported("cannot recompress %s" % codec)


# --- the container -----------------------------------------------------------

class Installer:
    """An NSIS installer, opened far enough to edit its header block."""

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as fp:
            self.data = fp.read()
        if self.data[:2] != b"MZ":
            raise Unsupported("not a Windows executable")

        at = self.data.find(SIGNATURE)
        if at < 4:
            raise Unsupported("no NSIS header -- this is probably Inno Setup "
                              "or InstallShield; see --emit-shim")
        self.base = at - 4
        (self.flags, _sig1, _sig2, _sig3, _sig4,
         self.header_size, self.total) = struct.unpack_from(
            "<7I", self.data, self.base)

        if self.base + self.total != len(self.data):
            raise Unsupported(
                "the archive claims %d bytes from %#x but the file ends at "
                "%#x -- something has been appended to it"
                % (self.total, self.base, len(self.data)))

        # In a solid archive there is no per-block size: the compressor's own
        # stream starts straight after the first header and runs to the end of
        # the file, header and every packed file together. A size dword whose
        # low bytes are themselves a valid LZMA start is the giveaway.
        self.solid = _looks_like_lzma(
            self.data[self.base + 28:self.base + 40])

        if self.solid:
            self.body_at = self.base + 28
            self.block_size = len(self.data) - self.body_at - self._crc_len()
            self.block_compressed = True
            blob = self.data[self.body_at:self.body_at + self.block_size]
            self.stream, self.codec, self.codec_state = decompress(blob, -1)
            if len(self.stream) < self.header_size:
                raise Unsupported(
                    "the solid stream is %d bytes, shorter than the %d-byte "
                    "header it declares" % (len(self.stream), self.header_size))
            self.header = self.stream[:self.header_size]
            return

        block = struct.unpack_from("<I", self.data, self.base + 28)[0]
        self.block_compressed = bool(block & COMPRESSED)
        self.block_size = block & ~COMPRESSED
        self.body_at = self.base + 32
        self.stream = None
        blob = self.data[self.body_at:self.body_at + self.block_size]

        if self.block_compressed:
            self.header, self.codec, self.codec_state = decompress(
                blob, self.header_size)
        else:
            self.header, self.codec, self.codec_state = blob, "stored", None
        if len(self.header) != self.header_size:
            raise Unsupported(
                "header decompressed to %d bytes, not the %d it declares"
                % (len(self.header), self.header_size))

    def _crc_len(self):
        return 0 if (self.flags & FH_FLAGS_NO_CRC) else 4

    # -- the bit we actually came for -----------------------------------------

    def find(self, needle):
        """Every offset of a NUL-terminated string equal to `needle`.

        Matching is case-insensitive because the registry is, and the shipped
        installers are not consistent about `Microprose` vs `MicroProse`.
        """
        want = needle.lower().encode("latin-1")
        out, at = [], 0
        while True:
            at = self.header.lower().find(want, at)
            if at < 0:
                return out
            # A string-table entry ends at its NUL; anything else is a
            # substring of a longer string and must be left alone.
            if self.header[at + len(want):at + len(want) + 1] == b"\0":
                out.append(at)
            at += 1

    def replace(self, offset, old, new):
        if len(new) != len(old):
            raise Unsupported(
                "%r and %r are different lengths. NSIS string-table offsets "
                "are absolute, so a longer or shorter value would shift every "
                "string after it." % (old, new))
        self.header = (self.header[:offset] + new.encode("latin-1")
                       + self.header[offset + len(old):])

    def _build_solid(self):
        """Rebuild a solid archive: one stream over the header and all files.

        There is nothing to splice here -- the packed files share a compression
        context with the header -- so the whole stream is recompressed. That is
        slow on a large archive and it is the only way to stay valid.
        """
        stream = self.header + self.stream[self.header_size:]
        body = compress(stream, self.codec, self.codec_state)

        check, _, _ = decompress(body, -1)
        if check != stream:
            raise Unsupported("recompressed solid stream did not verify")

        head = bytearray(self.data[self.base:self.body_at])
        struct.pack_into("<I", head, 24,
                         28 + len(body) + self._crc_len())
        out = self.data[:self.base] + bytes(head) + body
        if self._crc_len():
            out += struct.pack("<I", zlib.crc32(out[CRC_SKIP:]) & 0xFFFFFFFF)
        return out

    def build(self):
        """Reassemble the file, fixing the sizes and the CRC as we go."""
        if self.solid:
            return self._build_solid()
        if self.codec == "stored":
            body = self.header
            block = len(body)
        else:
            body = compress(self.header, self.codec, self.codec_state)
            block = len(body) | COMPRESSED

            # Prove the stub will get back what we put in before shipping it.
            check, _, _ = decompress(body, self.header_size)
            if check != self.header:
                raise Unsupported("recompressed header did not verify")

        tail_at = self.body_at + self.block_size
        has_crc = not (self.flags & FH_FLAGS_NO_CRC)
        tail = self.data[tail_at:len(self.data) - (4 if has_crc else 0)]

        total = 32 + len(body) + len(tail) + (4 if has_crc else 0)
        head = bytearray(self.data[self.base:self.body_at])
        struct.pack_into("<I", head, 24, total)
        struct.pack_into("<I", head, 28, block)

        out = self.data[:self.base] + bytes(head) + body + tail
        if has_crc:
            out += struct.pack("<I", zlib.crc32(out[CRC_SKIP:]) & 0xFFFFFFFF)
        return out


# --- driving it --------------------------------------------------------------

def key_variants(version):
    """The spellings of the key that turn up across these installers."""
    return ["Software\\Microprose\\Falcon\\" + version,
            "Microprose\\Falcon\\" + version]


def find_keys(inst, version):
    """Every NUL-terminated Falcon-version key in the header, deduplicated.

    The variants overlap -- the short spelling is a tail of the long one, and
    the match is case-insensitive -- so several patterns land on the same
    string. Group by where the string *ends* and keep the longest.
    """
    best = {}
    for variant in key_variants(version):
        for at in inst.find(variant):
            end = at + len(variant)
            if end not in best or at < best[end][0]:
                best[end] = (at, variant)
    return sorted(best.values())


def process(path, old, new, list_only, suffix):
    try:
        inst = Installer(path)
    except Unsupported as exc:
        print("  %s: %s" % (os.path.basename(path), exc))
        return False

    print("  %s" % os.path.basename(path))
    print("      NSIS header %d bytes, %s, archive %d bytes"
          % (inst.header_size, inst.codec, inst.total))

    hits = find_keys(inst, old)
    if not hits:
        print("      no Falcon %s registry key here -- nothing to do" % old)
        return False

    for at, variant in hits:
        actual = inst.header[at:at + len(variant)].decode("latin-1")
        print("      %#08x  %s" % (at, actual))
    if list_only:
        return False

    # Only the version at the end of the key changes, so the surrounding
    # spelling -- whatever case this installer used -- is left alone.
    for at, variant in hits:
        inst.replace(at + len(variant) - len(old), old, new)

    stem, ext = os.path.splitext(path)
    out_path = stem + suffix + ext
    blob = inst.build()
    with open(out_path, "wb") as fp:
        fp.write(blob)

    # Read our own output back the way the stub will, and refuse to leave a
    # file behind that does not check out.
    try:
        check = Installer(out_path)
        if not find_keys(check, new):
            raise Unsupported("the patched file does not contain the new key")
        if find_keys(check, old):
            raise Unsupported("the old key is still in the patched file")
    except Unsupported:
        os.remove(out_path)
        raise

    print("      wrote %s  (%+d bytes, header block %d -> %d)"
          % (os.path.basename(out_path), len(blob) - len(inst.data),
             inst.block_size, check.block_size))
    return True


SHIM = r"""Windows Registry Editor Version 5.00

; Point Falcon 4.0's key at the FreeFalcon install, so an installer that only
; knows the old key unpacks into the right game. Written by
; tools/installer-patch/patch_installer.py --emit-shim.
;
; This is the blunt instrument: prefer patching the installer itself. While
; this is applied, anything that genuinely wants Falcon 4.0 -- including a real
; Falcon 4.0 install -- will be sent to FreeFalcon instead. Back up the key
; first, and delete this one when the theater is installed.
;
;   reg export "HKLM\SOFTWARE\WOW6432Node\MicroProse\Falcon\4.0" falcon40-backup.reg

[HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\MicroProse\Falcon\4.0]
{values}
"""


def emit_shim(out_path):
    """Copy the live 4.1 values into a .reg that defines 4.0 the same way."""
    try:
        import winreg
    except ImportError:
        raise Unsupported("--emit-shim needs Windows")

    path = r"SOFTWARE\MicroProse\Falcon\4.1"
    for access in (winreg.KEY_READ | winreg.KEY_WOW64_32KEY,
                   winreg.KEY_READ | winreg.KEY_WOW64_64KEY):
        try:
            key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, path, 0, access)
            break
        except OSError:
            continue
    else:
        raise Unsupported("no %s key -- is FreeFalcon installed?" % path)

    lines, i = [], 0
    with key:
        while True:
            try:
                name, value, kind = winreg.EnumValue(key, i)
            except OSError:
                break
            i += 1
            if kind == winreg.REG_SZ:
                lines.append('"%s"="%s"' % (name, str(value).replace("\\", "\\\\")))
    if not lines:
        raise Unsupported("the 4.1 key has no string values to copy")

    with open(out_path, "w", encoding="utf-8", newline="\r\n") as fp:
        fp.write(SHIM.format(values="\n".join(lines)))
    print("wrote %s -- %d values copied from Falcon\\4.1" % (out_path, len(lines)))
    print("apply it with:  reg import %s" % out_path)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("installer", nargs="*",
                    help="installer .exe files, or globs")
    ap.add_argument("--from", dest="old", default="4.0",
                    help="the Falcon version key to look for (default 4.0)")
    ap.add_argument("--to", dest="new", default="4.1",
                    help="the key to point it at (default 4.1)")
    ap.add_argument("--suffix", default=" (FreeFalcon)",
                    help="appended to the patched file's name")
    ap.add_argument("--list", action="store_true",
                    help="report what is in there and change nothing")
    ap.add_argument("--emit-shim", metavar="FILE",
                    help="instead, write a .reg mirroring 4.1 onto the 4.0 key")
    args = ap.parse_args(argv)

    if args.emit_shim:
        try:
            emit_shim(args.emit_shim)
        except Unsupported as exc:
            print("error: %s" % exc, file=sys.stderr)
            return 1
        return 0

    if not args.installer:
        ap.error("give at least one installer, or --emit-shim")
    if len(args.old) != len(args.new):
        ap.error("--from and --to must be the same length: NSIS string "
                 "offsets are absolute")

    paths = []
    for pattern in args.installer:
        found = sorted(glob.glob(pattern))
        paths.extend(found or [pattern])

    print("Falcon %s -> %s in %d file(s)" % (args.old, args.new, len(paths)))
    patched = 0
    for path in paths:
        if not os.path.isfile(path):
            print("  %s: not found" % path)
            continue
        try:
            patched += bool(process(path, args.old, args.new,
                                    args.list, args.suffix))
        except Unsupported as exc:
            print("      failed: %s" % exc)
    print("%d patched" % patched)
    return 0


if __name__ == "__main__":
    sys.exit(main())
