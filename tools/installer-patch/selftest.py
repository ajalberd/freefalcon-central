#!/usr/bin/env python3
"""Check patch_installer.py against real installers.

The strong test is reversibility: patch 4.0 -> 4.1, patch that back to 4.0,
and require the uncompressed header to come out byte-identical to the
original's. That exercises the whole path -- decompress, edit, recompress,
resize, re-CRC -- and a mistake anywhere in it shows up as a difference.

    python selftest.py <installer.exe> [more.exe ...]
"""

import os
import shutil
import struct
import sys
import tempfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import nsis_entries  # noqa: E402
import patch_exe  # noqa: E402
import patch_installer as P  # noqa: E402
import skip_checks  # noqa: E402

fails = []
checks = 0


def check(cond, msg):
    global checks
    checks += 1
    if not cond:
        fails.append(msg)
        print("  FAIL  " + msg)
    return cond


def test_file(path):
    print(os.path.basename(path))
    try:
        orig = P.Installer(path)
    except P.Unsupported as exc:
        print("  --    %s" % exc)
        return

    print("  %s header, %d bytes uncompressed, archive %d bytes"
          % (orig.codec, orig.header_size, orig.total))

    # The CRC rule the stub uses, confirmed against the shipped file.
    if not (orig.flags & P.FH_FLAGS_NO_CRC):
        stored = struct.unpack_from("<I", orig.data, len(orig.data) - 4)[0]
        check(stored == (zlib.crc32(orig.data[P.CRC_SKIP:-4]) & 0xFFFFFFFF),
              "the original's CRC32 does not match a CRC from offset %d"
              % P.CRC_SKIP)

    # A rebuild that changes nothing must still come out readable, with the
    # same header and the same packed files. This is the only coverage a solid
    # archive gets -- none of the shipped ones carry a Falcon key -- and it is
    # what proves the container work is right independently of the edit.
    rebuilt = orig.build()
    tmp = tempfile.mkdtemp(prefix="ffpatch-")
    try:
        same = os.path.join(tmp, "rebuilt.exe")
        with open(same, "wb") as fp:
            fp.write(rebuilt)
        again = P.Installer(same)
        check(again.header == orig.header,
              "a no-op rebuild changed the header")
        check(again.solid == orig.solid, "a no-op rebuild changed solidity")
        if orig.solid:
            check(again.stream == orig.stream,
                  "a no-op rebuild changed the packed files")
        else:
            check(again.data[again.body_at + again.block_size:-4]
                  == orig.data[orig.body_at + orig.block_size:-4],
                  "a no-op rebuild changed the packed files")
        check(again.base + again.total == len(again.data),
              "a no-op rebuild left the archive length wrong")
        if not (orig.flags & P.FH_FLAGS_NO_CRC):
            stored = struct.unpack_from("<I", again.data,
                                        len(again.data) - 4)[0]
            check(stored == (zlib.crc32(again.data[P.CRC_SKIP:-4])
                             & 0xFFFFFFFF),
                  "a no-op rebuild left the CRC32 wrong")
        print("  ok    no-op rebuild is faithful%s"
              % (" (solid)" if orig.solid else ""))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    hits = P.find_keys(orig, "4.0")
    if not hits:
        print("  --    no Falcon 4.0 key here, nothing further to test")
        return
    print("  %d key(s): %s" % (len(hits), ", ".join(
        orig.header[a:a + len(v)].decode("latin-1") for a, v in hits)))

    tmp = tempfile.mkdtemp(prefix="ffpatch-")
    try:
        work = os.path.join(tmp, os.path.basename(path))
        shutil.copy2(path, work)

        check(P.process(work, "4.0", "4.1", False, " (FF)"),
              "forward patch reported nothing done")
        stem, ext = os.path.splitext(work)
        once = stem + " (FF)" + ext
        if not check(os.path.isfile(once), "forward patch wrote no file"):
            return

        mid = P.Installer(once)
        check(not P.find_keys(mid, "4.0"), "a 4.0 key survived the patch")
        check(len(P.find_keys(mid, "4.1")) == len(hits),
              "expected %d 4.1 keys, found %d"
              % (len(hits), len(P.find_keys(mid, "4.1"))))
        check(mid.header_size == orig.header_size,
              "the uncompressed header changed size")
        check(mid.base == orig.base, "the PE image moved")
        check(mid.data[:orig.base] == orig.data[:orig.base],
              "bytes before the archive changed")
        check(mid.data[mid.body_at + mid.block_size:-4]
              == orig.data[orig.body_at + orig.block_size:-4],
              "the file data after the header changed")
        check(mid.base + mid.total == len(mid.data),
              "the patched archive length does not reach the end of the file")
        if not (mid.flags & P.FH_FLAGS_NO_CRC):
            stored = struct.unpack_from("<I", mid.data, len(mid.data) - 4)[0]
            check(stored == (zlib.crc32(mid.data[P.CRC_SKIP:-4]) & 0xFFFFFFFF),
                  "the patched file's CRC32 is wrong")

        diff = [i for i, (a, b) in enumerate(zip(orig.header, mid.header))
                if a != b]
        check(len(diff) == len(hits),
              "%d key(s) patched but %d header bytes changed"
              % (len(hits), len(diff)))

        # And back again.
        check(P.process(once, "4.1", "4.0", False, " (back)"),
              "reverse patch reported nothing done")
        stem2, ext2 = os.path.splitext(once)
        twice = stem2 + " (back)" + ext2
        if not check(os.path.isfile(twice), "reverse patch wrote no file"):
            return
        back = P.Installer(twice)
        check(back.header == orig.header,
              "the header did not survive a round trip")
        # The CRC legitimately differs: liblzma's encoder is not bit-identical
        # to the one NSIS shipped, so the compressed header comes out a few
        # bytes longer even when its contents match exactly.
        check(back.data[back.body_at + back.block_size:-4]
              == orig.data[orig.body_at + orig.block_size:-4],
              "the file data did not survive a round trip")
        if not (back.flags & P.FH_FLAGS_NO_CRC):
            stored = struct.unpack_from("<I", back.data, len(back.data) - 4)[0]
            check(stored == (zlib.crc32(back.data[P.CRC_SKIP:-4]) & 0xFFFFFFFF),
                  "the round-tripped file's CRC32 is wrong")

        print("  ok    round trip clean, %d byte(s) changed per pass"
              % len(diff))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_skip_checks(path):
    """Bypassing a gate must change one parameter and nothing else."""
    try:
        orig = P.Installer(path)
    except P.Unsupported:
        return

    ents = nsis_entries.Entries(orig)
    gates = skip_checks.find_gates(ents, skip_checks.DEFAULT_PATTERN)

    if not gates:
        print("  --    no prerequisite gates here")
        return

    tmp = tempfile.mkdtemp(prefix="ffpatch-")
    try:
        work = os.path.join(tmp, os.path.basename(path))
        shutil.copy2(path, work)

        check(skip_checks.process(work, skip_checks.DEFAULT_PATTERN, False,
                                  " (nc)"),
              "skip_checks reported nothing done")

        stem, ext = os.path.splitext(work)
        out = stem + " (nc)" + ext
        if not check(os.path.isfile(out), "skip_checks wrote no file"):
            return

        done = P.Installer(out)
        dents = nsis_entries.Entries(done)

        check(dents.count == ents.count,
              "the instruction count changed (%d -> %d)"
              % (ents.count, dents.count))
        check(dents.strings == ents.strings,
              "the string table changed, and it should not have")
        check(done.header_size == orig.header_size,
              "the header changed size")

        # Exactly one dword per gate, and every one of them a branch we named.
        moved = [i for i, (a, b) in enumerate(zip(orig.header, done.header))
                 if a != b]
        expected = set()
        for g in gates:
            at = (dents.entries_at + g["index"] * nsis_entries.ENTRY_SIZE
                  + 4 + g["branch"] * 4)
            expected.update(range(at, at + 4))
        check(set(moved) <= expected,
              "%d byte(s) changed outside the gate branches"
              % len(set(moved) - expected))

        for g in gates:
            _w, params = dents.get(g["index"])
            check(params[g["branch"]] == g["target"] + 1,
                  "gate at %d branches to %d, not %d"
                  % (g["index"], params[g["branch"]], g["target"] + 1))
            # Both ways out now go to the same instruction, which is what makes
            # the condition a no-op rather than an inverted test.
            hold, fail = nsis_entries.CONDITIONALS[dents.get(g["index"])[0]]
            check(params[hold] == params[fail],
                  "gate at %d still branches two different ways" % g["index"])

        check(not skip_checks.find_gates(dents, skip_checks.DEFAULT_PATTERN),
              "a gate survived")
        check(done.base + done.total == len(done.data),
              "the archive length does not reach the end of the file")
        if not (done.flags & P.FH_FLAGS_NO_CRC):
            stored = struct.unpack_from("<I", done.data,
                                        len(done.data) - 4)[0]
            check(stored == (zlib.crc32(done.data[P.CRC_SKIP:-4]) & 0xFFFFFFFF),
                  "the CRC32 is wrong after bypassing gates")

        print("  ok    %d gate(s) bypassed, %d bytes changed"
              % (len(gates), len(moved)))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_plain_exe(path):
    """A plain PE: patch the key, patch it back, require the same bytes.

    No container to rebuild here -- the string is a literal in the image and the
    replacement is the same length -- so a round trip has to be byte-identical
    apart from the checksum being recomputed to the same value.
    """
    try:
        with open(path, "rb") as fp:
            data = fp.read()
        hits = patch_exe.find_keys(data, "4.0")
    except (OSError, patch_exe.Unsupported):
        return

    if not hits:
        return

    print(os.path.basename(path))

    tmp = tempfile.mkdtemp(prefix="ffexe-")
    try:
        work = os.path.join(tmp, os.path.basename(path))
        shutil.copy2(path, work)

        check(patch_exe.process(work, "4.0", "4.1", False, False, " (FF)"),
              "forward patch reported nothing done")
        stem, ext = os.path.splitext(work)
        once = stem + " (FF)" + ext

        if not check(os.path.isfile(once), "forward patch wrote no file"):
            return

        with open(once, "rb") as fp:
            mid = fp.read()

        check(len(mid) == len(data), "the patched file changed size")
        check(not patch_exe.find_keys(mid, "4.0"), "a 4.0 key survived")
        check(len(patch_exe.find_keys(mid, "4.1")) == len(hits),
              "expected %d 4.1 keys" % len(hits))

        csum_at, _sec = patch_exe._pe_offsets(mid)
        stored = struct.unpack_from("<I", data, csum_at)[0]

        if stored:
            got = struct.unpack_from("<I", mid, csum_at)[0]
            check(got == patch_exe.checksum(mid, csum_at),
                  "the PE checksum is wrong after patching")

        # One byte per key, and every one of them inside a key.
        moved = [i for i, (a, b) in enumerate(zip(data, mid)) if a != b]
        allowed = set()

        for at, length in hits:
            allowed.update(range(at + length - 3, at + length))

        if stored:
            allowed.update(range(csum_at, csum_at + 4))

        check(set(moved) <= allowed,
              "%d byte(s) changed outside the keys and the checksum"
              % len(set(moved) - allowed))

        check(patch_exe.process(once, "4.1", "4.0", False, False, " (back)"),
              "reverse patch reported nothing done")
        stem2, ext2 = os.path.splitext(once)
        twice = stem2 + " (back)" + ext2

        with open(twice, "rb") as fp:
            back = fp.read()

        check(back == data, "the file did not survive a round trip")
        print("  ok    round trip byte-identical, %d key(s)" % len(hits))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_refusals():
    """The guards that keep a bad edit from reaching disk."""
    print("refusals")

    class Fake:
        header = b"Software\\Microprose\\Falcon\\4.0\0"
        replace = P.Installer.replace

    try:
        Fake().replace(0, "4.0", "4.01")
        check(False, "a replacement of a different length was accepted")
    except P.Unsupported:
        check(True, "")

    tmp = tempfile.mkdtemp(prefix="ffpatch-")
    try:
        junk = os.path.join(tmp, "notaninstaller.exe")
        with open(junk, "wb") as fp:
            fp.write(b"MZ" + os.urandom(4096))
        try:
            P.Installer(junk)
            check(False, "a file with no NSIS header was accepted")
        except P.Unsupported:
            check(True, "")

        notexe = os.path.join(tmp, "plain.dat")
        with open(notexe, "wb") as fp:
            fp.write(b"not an executable at all")
        try:
            P.Installer(notexe)
            check(False, "a non-executable was accepted")
        except P.Unsupported:
            check(True, "")

        print("  ok    a resized value, a non-NSIS exe and a non-exe are "
              "all refused")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    test_refusals()
    for path in argv:
        if os.path.isfile(path):
            test_file(path)
            test_skip_checks(path)
            test_plain_exe(path)
        else:
            print("%s: not found" % path)
    print("\n%d checks, %d failures" % (checks, len(fails)))
    for f in fails:
        print("  - " + f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
