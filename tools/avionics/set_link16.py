"""Set or clear the Link 16 terminal flag (VEH_HAS_LINK16, 0x10000000) on vehicle classes in FALCON4.VCD.

The HSD's Link 16 "lite" layer (FireControlComputer::DrawFriendlyFlights) shows other friendly flights
only to an aircraft whose vehicle class carries this flag, and only those flights whose aircraft carry it
too. The flag lives in the vehicle table, so which blocks have a terminal is plain data.

  python set_link16.py --list    <VCD> [<VCD> ...]            show every aircraft and its flag
  python set_link16.py --apply   <VCD> [<VCD> ...]            set the flag on the default names (below)
  python set_link16.py --apply --add "F-16AM-B" <VCD> ...     ... plus extra names (repeatable)
  python set_link16.py --remove  <VCD> ...                    clear it on the default names

Default names: the F-16 blocks that can carry MIDS -- Block 50/52 (F-16CJ, F-16DJ), Block 52+ (F-16C+), the
Korean Block 52s (KF-16C, KF-16D) and the MLU F-16AM-B/AM-D -- plus F-15K, F/A-18E/F and the E-3 and E-2C
AWACS so they share the picture. Not in the list, deliberately: F-16A/B, F-16C/CG/DG (Block 30 to 42), F-16XL,
the Greek F-16C/D HAF and F-16 AGSR -- pass them with --add if wanted.

A backup `<file>.bak-pre-link16` is written once, before the first change. Both VCD framings are handled
(plain count first, or 0 first with the count at the end); the flag word is 4 bytes at record offset 4.
"""
import argparse
import shutil
import struct
import sys

RECORD = 160
FLAG = 0x10000000
DEFAULT_NAMES = ["F-16CJ", "F-16DJ", "F-16C+", "KF-16C", "KF-16D",
                 "F-16AM-B", "F-16AM-D", "F-15K", "F/A-18E", "F/A-18F", "E-3", "E-2C"]


def records(raw):
    lead = struct.unpack_from("<h", raw, 0)[0]
    count = struct.unpack_from("<h", raw, len(raw) - 2)[0] if lead == 0 else lead
    if 2 + count * RECORD not in (len(raw), len(raw) - 2):
        sys.exit("size does not match %d records of %d bytes" % (count, RECORD))
    for i in range(count):
        base = 2 + i * RECORD
        name = raw[base + 8:base + 23].split(b"\0")[0].decode("latin1")
        yield i, base, name


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--list", action="store_true")
    mode.add_argument("--apply", action="store_true")
    mode.add_argument("--remove", action="store_true")
    ap.add_argument("--add", action="append", default=[], help="extra vehicle name (exact, case-sensitive)")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    names = set(DEFAULT_NAMES + a.add)

    for path in a.files:
        raw = bytearray(open(path, "rb").read())
        changed = 0

        for i, base, name in records(raw):
            flags = struct.unpack_from("<I", raw, base + 4)[0]

            if a.list:
                if name.startswith(("F-", "KF-", "F/A", "E-3", "EF-", "CF-")):
                    print("%s  %4d  %-16s %08x%s" % (path, i, name, flags, "  LINK16" if flags & FLAG else ""))
            elif name in names:
                new = (flags | FLAG) if a.apply else (flags & ~FLAG)

                if new != flags:
                    struct.pack_into("<I", raw, base + 4, new)
                    changed += 1
                    print("%s  %4d  %-16s %08x -> %08x" % (path, i, name, flags, new))

        if changed:
            backup = path + ".bak-pre-link16"

            try:
                open(backup, "rb").close()
            except OSError:
                shutil.copyfile(path, backup)

            open(path, "wb").write(bytes(raw))

        if not a.list:
            print("%s: %d record(s) changed" % (path, changed))


if __name__ == "__main__":
    main()
