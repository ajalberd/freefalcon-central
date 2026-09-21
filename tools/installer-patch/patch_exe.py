#!/usr/bin/env python3
r"""Retarget a plain Falcon utility at FreeFalcon's registry key.

`patch_installer.py` handles NSIS installers, where the registry path lives in a
compressed header behind a CRC. The utilities that ship *inside* FreeFalcon have
the same problem and none of that structure: `TheaterAdd.exe`,
`SeasonSwitcher.exe` and `WeatherEditor.exe` all read

    HKLM\Software\MicroProse\Falcon\4.0   baseDir

while FFViper.exe itself reads `...\4.1`. On a machine that also has a real
Falcon 4.0 install, `TheaterAdd -a` writes its `theater.lst` into *that* game --
which is why a theater can install perfectly and still never appear in the
theater list.

The fix is the same one byte, and here it is genuinely just a byte: the string
is a literal in the image, `4.0` and `4.1` are the same length, so nothing moves
and no offset is invalidated. The only bookkeeping is the PE checksum, which is
recomputed when the file carried one.

    python patch_exe.py "C:\FreeFalcon6\Utilities\TheaterAdd.exe" --list
    python patch_exe.py "C:\FreeFalcon6\Utilities\*.exe" --in-place

`--in-place` keeps a `.bak-falcon40` copy beside each file, because these are
called by name from installers and a renamed copy would never be used.
"""

import argparse
import glob
import os
import struct
import sys


class Unsupported(Exception):
    pass


def _pe_offsets(data):
    """(checksum field offset, security directory entry offset) or (None, None)."""
    if data[:2] != b"MZ":
        raise Unsupported("not a Windows executable")

    pe = struct.unpack_from("<I", data, 0x3C)[0]

    if data[pe:pe + 4] != b"PE\0\0":
        raise Unsupported("no PE header")

    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]

    if magic == 0x10B:      # PE32
        dirs = opt + 96
    elif magic == 0x20B:    # PE32+
        dirs = opt + 112
    else:
        raise Unsupported("unknown optional header magic %#x" % magic)

    # The security directory is entry 4; a non-zero size means Authenticode.
    return opt + 64, dirs + 4 * 8


def checksum(data, csum_at):
    """The PE image checksum, computed the way the loader does.

    A 16-bit ones-complement sum over the whole file with the checksum field
    itself read as zero, folded to 16 bits, plus the file length.
    """
    total = 0

    for i in range(0, len(data) - 1, 2):
        if i == csum_at:
            continue            # the field reads as zero
        total += struct.unpack_from("<H", data, i)[0]
        total = (total & 0xFFFF) + (total >> 16)

    if len(data) & 1:
        total += data[-1]
        total = (total & 0xFFFF) + (total >> 16)

    total = (total & 0xFFFF) + (total >> 16)
    return (total + len(data)) & 0xFFFFFFFF


def find_keys(data, version):
    """Offsets of NUL-terminated Falcon registry keys ending in `version`."""
    out = []

    for spelling in (b"software\\microprose\\falcon\\",):
        want = spelling + version.encode("latin-1")
        low = data.lower()
        at = 0

        while True:
            at = low.find(want, at)

            if at < 0:
                break

            # Must be a whole string, not a fragment of a longer path.
            if data[at + len(want):at + len(want) + 1] == b"\0":
                out.append((at, len(want)))

            at += 1

    return sorted(set(out))


def process(path, old, new, list_only, in_place, suffix):
    with open(path, "rb") as fp:
        data = fp.read()

    csum_at, sec_at = _pe_offsets(data)
    stored = struct.unpack_from("<I", data, csum_at)[0]
    signed = struct.unpack_from("<I", data, sec_at + 4)[0] != 0

    hits = find_keys(data, old)
    print("  %s" % os.path.basename(path))

    if not hits:
        print("      no Falcon %s key here -- nothing to do" % old)
        return False

    for at, length in hits:
        print("      %#08x  %s" % (at, data[at:at + length].decode("latin-1")))

    if signed:
        print("      NOTE: this file is Authenticode signed; patching it "
              "invalidates the signature")

    if list_only:
        return False

    out = bytearray(data)

    for at, length in hits:
        # Only the version at the end of the key changes, so whatever spelling
        # and case the file used is left alone.
        start = at + length - len(old)
        out[start:start + len(old)] = new.encode("latin-1")

    if stored:
        struct.pack_into("<I", out, csum_at, 0)
        struct.pack_into("<I", out, csum_at, checksum(bytes(out), csum_at))

    blob = bytes(out)

    if in_place:
        backup = path + ".bak-falcon" + old.replace(".", "")

        if not os.path.exists(backup):
            with open(backup, "wb") as fp:
                fp.write(data)

        out_path = path
    else:
        stem, ext = os.path.splitext(path)
        out_path = stem + suffix + ext

    with open(out_path, "wb") as fp:
        fp.write(blob)

    # Read back and confirm.
    with open(out_path, "rb") as fp:
        check = fp.read()

    if find_keys(check, old) or not find_keys(check, new):
        raise Unsupported("the patched file does not read back correctly")

    if len(check) != len(data):
        raise Unsupported("the patched file changed size")

    moved = sum(1 for a, b in zip(data, check) if a != b)
    print("      wrote %s  (%d byte(s) changed%s)"
          % (os.path.basename(out_path), moved,
             ", checksum updated" if stored else ""))
    return True


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target", nargs="+", help="executables, or globs")
    ap.add_argument("--from", dest="old", default="4.0")
    ap.add_argument("--to", dest="new", default="4.1")
    ap.add_argument("--list", action="store_true",
                    help="report and change nothing")
    ap.add_argument("--in-place", action="store_true",
                    help="patch the file itself, keeping a .bak copy")
    ap.add_argument("--suffix", default=" (FreeFalcon)")
    args = ap.parse_args(argv)

    if len(args.old) != len(args.new):
        ap.error("--from and --to must be the same length; the string is "
                 "patched where it sits")

    paths = []

    for pattern in args.target:
        paths.extend(sorted(glob.glob(pattern)) or [pattern])

    print("Falcon %s -> %s in %d file(s)" % (args.old, args.new, len(paths)))
    done = 0

    for path in paths:
        if not os.path.isfile(path):
            print("  %s: not found" % path)
            continue
        try:
            done += bool(process(path, args.old, args.new, args.list,
                                 args.in_place, args.suffix))
        except Unsupported as exc:
            print("      failed: %s" % exc)

    print("%d patched" % done)
    return 0


if __name__ == "__main__":
    sys.exit(main())
