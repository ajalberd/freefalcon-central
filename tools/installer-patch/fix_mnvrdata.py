#!/usr/bin/env python3
r"""Swap a theater's transposed maneuver tables back inside its Simdata.zip.

Some theater packages ship the 2002 maneuver table under the 2010 name --
`Sim\ACDATA\BRAIN\mnvrdata.dat` is 12793 bytes beginning "A#", where the stock
file is 16543 bytes beginning "#New FF5.5.1 July 21, 2010" -- and no
`mnvrdata5.dat` at all.  `DigitalBrain::ReadManeuverData` dispatches on the
first byte, so that theater loads no maneuver data: the AI has no dogfight
decisions at all, and before `FreeManeuverData` zeroed its counts the next
theater switch walked Korea's counts off a NULL in `WvrEngage`.

For a non-Korea theater the zip wins over anything loose on disk
(`theaterdef.cpp` attaches it with `replace == TRUE`), so the repair has to go
inside the zip.  This rewrites it in place:

    mnvrdata.dat   <- the 16543-byte 2010 table, taken from stock Zips/Simdata.zip
    mnvrdata5.dat  <- the 12793-byte 2002 table the package shipped

Every stored entry name, its order, and its compression method are preserved;
only the two maneuver-table entries change.  The original is kept as
`<zip>.bak-mnvrswap`, and re-running on a fixed archive verifies it and changes
nothing.

    python fix_mnvrdata.py "C:\FreeFalcon6\ZipsTaiwan\Simdata.zip"
    python fix_mnvrdata.py ZipsTaiwan\Simdata.zip --stock "Zips\Simdata.zip" --list
"""

import argparse
import os
import shutil
import sys
import zipfile

TARGET = "sim/acdata/brain/mnvrdata.dat"
FIVE = "sim/acdata/brain/mnvrdata5.dat"
LEGACY_SIZE = 12793


def norm(name):
    return name.replace("\\", "/").lower()


def find(zf, wanted):
    """The single member whose normalised name matches, or None."""
    hits = [i for i in zf.infolist() if norm(i.filename) == wanted]
    return hits[0] if len(hits) == 1 else None


def inspect(theater_zip, stock_zip):
    """Return (state, detail) without changing anything."""
    with zipfile.ZipFile(stock_zip) as st:
        stock = find(st, TARGET)
        if stock is None:
            return "bad-stock", "no %s in %s" % (TARGET, stock_zip)
        stock_dat = st.read(stock)

    with zipfile.ZipFile(theater_zip) as tw:
        target = find(tw, TARGET)
        if target is None:
            return "bad-theater", "no %s in %s" % (TARGET, theater_zip)
        current = tw.read(target)
        five = find(tw, FIVE)

    if current.startswith(b"#"):
        if five is None:
            return "half-fixed", "ASCII table but no mnvrdata5.dat"
        return "fixed", (len(current), five.file_size)
    if len(current) != LEGACY_SIZE:
        return "unexpected", "theater table is %d bytes" % len(current)
    return "broken", (len(current), len(stock_dat), stock_dat)


def fix(theater_zip, stock_zip, stock_dat):
    with zipfile.ZipFile(theater_zip) as tw:
        members = tw.infolist()
        target = find(tw, TARGET)
        legacy = tw.read(target)
        five_name = target.filename[: -len("mnvrdata.dat")] + "mnvrdata5.dat"
        assert norm(five_name) == FIVE, five_name

        tmp = theater_zip + ".new"

        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as out:
            for info in members:
                data = stock_dat if info.filename == target.filename else tw.read(info)
                zi = zipfile.ZipInfo(info.filename, info.date_time)
                zi.compress_type = info.compress_type
                zi.external_attr = info.external_attr
                zi.internal_attr = info.internal_attr
                zi.create_system = info.create_system
                out.writestr(zi, data)

            zi = zipfile.ZipInfo(five_name, target.date_time)
            zi.compress_type = target.compress_type
            zi.external_attr = target.external_attr
            zi.internal_attr = target.internal_attr
            zi.create_system = target.create_system
            out.writestr(zi, legacy)

    backup = theater_zip + ".bak-mnvrswap"
    if not os.path.exists(backup):
        shutil.copy2(theater_zip, backup)
    os.replace(tmp, theater_zip)
    return legacy, len(members), backup


def verify(theater_zip, stock_dat, legacy, members):
    """Re-open the result and require exactly the intended change."""
    with zipfile.ZipFile(theater_zip) as z:
        bad = z.testzip()
        if bad is not None:
            raise AssertionError("zip integrity failure at %r" % bad)
        if z.read(find(z, TARGET)) != stock_dat:
            raise AssertionError("mnvrdata.dat did not take the stock table")
        if z.read(find(z, FIVE)) != legacy:
            raise AssertionError("mnvrdata5.dat did not keep the 2002 table")
        names = [norm(i.filename) for i in z.infolist()]
        if len(names) != members + 1:
            raise AssertionError("entry count changed by more than the new file")
        if norm(find(z, FIVE).filename) == norm(find(z, TARGET).filename):
            raise AssertionError("the two tables collapsed onto one name")


def process(theater_zip, stock_zip, list_only):
    print("  %s" % theater_zip)
    state, detail = inspect(theater_zip, stock_zip)

    if state == "fixed":
        print("      already fixed: mnvrdata.dat %d bytes, mnvrdata5.dat %d"
              % detail)
        return False
    if state in ("bad-stock", "bad-theater", "half-fixed"):
        print("      skipped: %s" % detail)
        return False
    if state == "unexpected":
        print("      skipped: %s -- not the transposition this tool knows"
              % detail)
        return False

    legacy_size, stock_size, stock_dat = detail
    print("      mnvrdata.dat   %5d -> %5d bytes (2010 ASCII table)"
          % (legacy_size, stock_size))
    print("      mnvrdata5.dat  %5d bytes (2002 table kept)" % legacy_size)

    if list_only:
        return False

    legacy, members, backup = fix(theater_zip, stock_zip, stock_dat)
    verify(theater_zip, stock_dat, legacy, members)
    print("      %d entries, backup %s" % (members + 1, os.path.basename(backup)))
    return True


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("theater_zip", nargs="+",
                    help="a theater's Zips<Name>/Simdata.zip")
    ap.add_argument("--stock", default=None,
                    help="stock Zips/Simdata.zip (default: beside the theater zip)")
    ap.add_argument("--list", action="store_true",
                    help="report what would change and change nothing")
    args = ap.parse_args(argv)

    done = 0
    for path in args.theater_zip:
        if not os.path.isfile(path):
            print("  %s: not found" % path)
            continue
        stock = args.stock
        if stock is None:
            # ZipsTaiwan/Simdata.zip -> <install>/Zips/Simdata.zip
            install = os.path.dirname(os.path.dirname(os.path.abspath(path)))
            stock = os.path.join(install, "Zips", "Simdata.zip")
        if not os.path.isfile(stock):
            print("  %s: no stock archive at %s (use --stock)" % (path, stock))
            continue
        done += bool(process(path, stock, args.list))

    print("%d patched" % done)
    return 0


if __name__ == "__main__":
    sys.exit(main())
