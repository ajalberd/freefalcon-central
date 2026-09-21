#!/usr/bin/env python3
r"""Switch off an installer's prerequisite gates.

These theater installers refuse to run unless they recognise the install they
are pointed at -- FFViper.exe must be present, and a registry version must
compare new enough. On a FreeFalcon build that is not the one the installer was
written against, a gate can fail for reasons that have nothing to do with
whether the theater will work.

The shape of a gate is always the same three instructions:

    N     a conditional whose FAILING branch is 0 (fall through)
    N+1   MessageBox  "FreeFalcon5 not installed. Aborting..."
    N+2   Abort / Quit

so switching one off is a single parameter: point that failing branch past the
Abort instead of into the message. Nothing is deleted, no instruction moves, and
every other branch target in the file stays valid -- which matters, because they
are absolute indices.

    python skip_checks.py "ITO2 V4c (FreeFalcon).exe" --list
    python skip_checks.py "ITO2 V4c (FreeFalcon).exe"

The original is never modified; the result is written beside it.
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import nsis_entries  # noqa: E402
import patch_installer as P  # noqa: E402

# What a prerequisite complaint sounds like. Deliberately narrow: this must not
# match "are you sure you want to uninstall".
DEFAULT_PATTERN = (r"not installed|requires|must be installed|"
                   r"unable to read|could not find|is required|"
                   r"wrong version|older version")

ABORTS = ("Abort", "Quit")


def find_gates(ents, pattern):
    """Every conditional whose failing branch runs into a complaint and a stop."""
    rx = re.compile(pattern, re.IGNORECASE)
    gates = []

    for i in range(ents.count - 2):
        which, params = ents.get(i)

        if which not in nsis_entries.CONDITIONALS:
            continue
        if ents.op(i + 1) != "MessageBox" or ents.op(i + 2) not in ABORTS:
            continue

        message = ents.text(ents.get(i + 1)[1][1]) or ""

        if not rx.search(message):
            continue

        # Which branch falls into the message? Exactly one should be 0.
        hold, fail = nsis_entries.CONDITIONALS[which]
        branch = None

        if params[fail] == 0 and params[hold] != 0:
            branch = fail
        elif params[hold] == 0 and params[fail] != 0:
            branch = hold

        if branch is None:
            continue

        gates.append(dict(index=i, branch=branch, message=message,
                          op=ents.op(i), stop=ents.op(i + 2),
                          target=i + 3))

    return gates


def process(path, pattern, list_only, suffix):
    inst = P.Installer(path)
    ents = nsis_entries.Entries(inst)

    print("  %s" % os.path.basename(path))
    print("      %d instructions, %d strings" %
          (ents.count, len(ents.strings)))

    gates = find_gates(ents, pattern)

    if not gates:
        print("      no prerequisite gates matched -- nothing to do")
        return False

    for g in gates:
        print("      %4d  %-13s -> %-11s %r"
              % (g["index"], g["op"], g["stop"], g["message"][:54]))

    if list_only:
        return False

    for g in gates:
        # Branch targets are stored as index + 1; 0 means fall through.
        ents.set_param(g["index"], g["branch"], g["target"] + 1)

    stem, ext = os.path.splitext(path)
    out_path = stem + suffix + ext

    with open(out_path, "wb") as fp:
        fp.write(inst.build())

    # Read it back and confirm every gate now jumps past its own Abort.
    check = P.Installer(out_path)
    cents = nsis_entries.Entries(check)
    left = find_gates(cents, pattern)

    if left:
        os.remove(out_path)
        raise P.Unsupported("%d gate(s) survived the edit" % len(left))

    for g in gates:
        _w, params = cents.get(g["index"])
        if params[g["branch"]] != g["target"] + 1:
            os.remove(out_path)
            raise P.Unsupported("gate at %d did not take the new branch"
                                % g["index"])

    print("      wrote %s  (%d gate(s) bypassed)"
          % (os.path.basename(out_path), len(gates)))
    return True


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("installer", nargs="+")
    ap.add_argument("--list", action="store_true",
                    help="report the gates and change nothing")
    ap.add_argument("--pattern", default=DEFAULT_PATTERN,
                    help="regex the complaint text must match")
    ap.add_argument("--suffix", default=" (no checks)")
    args = ap.parse_args(argv)

    done = 0
    for path in args.installer:
        if not os.path.isfile(path):
            print("  %s: not found" % path)
            continue
        try:
            done += bool(process(path, args.pattern, args.list, args.suffix))
        except P.Unsupported as exc:
            print("      failed: %s" % exc)

    print("%d patched" % done)
    return 0


if __name__ == "__main__":
    sys.exit(main())
