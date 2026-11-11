# Installing third-party theaters

Findings from getting the ITO2 (Israel) package into FreeFalcon 6. Three
separate faults, each of which fails silently, and each of which will hit the
next theater package too.

## 1. Everything except the game reads the wrong registry key

`FFViper.exe` reads `HKLM\SOFTWARE\WOW6432Node\MicroProse\Falcon\4.1`. The
bundled tools and every old theater installer read **`4.0`**, which on a machine
that also has a real Falcon 4.0 install points at that game instead:

```
Utilities\TheaterAdd.exe, SeasonSwitcher.exe, WeatherEditor.exe    4.0
F4Info.exe, hostidx.exe, mpsinfo.exe                               4.0
FFViper.exe                                                        4.1
```

`TheaterAdd.exe` is the one that matters. Every theater installer finishes with

```
TheaterAdd.exe -a theaters\<Name>\<Name>.tdf
```

and TheaterAdd resolves `theater.lst` from `4.0`'s `baseDir`. So a theater
extracts three gigabytes perfectly, writes its uninstaller, reports success, and
never appears in the theater list — because the list it appended to belongs to a
different game. Nothing logs an error.

**Check `theater.lst` before believing an install failed.**
`tools/installer-patch/` fixes both halves: `patch_installer.py` for the NSIS
installer, `patch_exe.py --in-place` for the utilities. `TheaterAdd -a` is
idempotent, so re-running it by hand to repair a list is safe.

## 2. Non-Korea theaters let the zip override loose files

`theaterdef.cpp` attaches each theater's zips differently:

```c
if (not strnicmp(td->m_name, "Korea", 5))
    ResAttach(FalconDataDirectory, tmpPath, FALSE);   // HD files win
else
    ResAttach(FalconDataDirectory, tmpPath, TRUE);    // the ZIP wins
```

and `omni.h`'s resource-manager guide is explicit: `replace == FALSE` means the
zip will **not** replace hard-drive files of the same name. So the stock
`Zips/sim/...` loose tree works for Korea and **cannot** be used to override
anything in a third-party theater — for those, the zip is authoritative and a
fix has to go inside it.

Worth knowing before trying to patch theater data the easy way.

## 3. ITO2 ships the 2002 maneuver table under the 2010 name

This one crashes the sim. The ITO package's `ZipsITO/Simdata.zip` has two files
transposed against the stock `Zips/Simdata.zip`:

| | `mnvrdata.dat` | `mnvrdata5.dat` |
| --- | --- | --- |
| stock | 16543, `#New FF5.5.1 July 21, 2010` | 12793, 2002 |
| ITO2 | **12793, 2002** | 16543, 2010 |

`DigitalBrain::ReadManeuverData` (`digimain.cpp`) dispatches on the **first byte
of the file**: `'#'` is the ASCII table, `'B'` is binary, anything else is
rejected. The 2002 file begins `A#`, so it is rejected — and `ShiWarning` is
silent in Release, so nothing says so.

That alone would only make the AI passive. The crash came from a second defect:
`FreeManeuverData` nulled `intercept`, `merge` and `spikeReact` but **left
`numIntercepts`, `numMerges` and `numReacts` at their old values**. Every reader
gates on the count and then indexes the array with no null check:

```c
numChoices = theIntercept->numMerges;

if (numChoices)
{
    myChoice = rand() % numChoices;
    switch (theIntercept->merge[myChoice])   // wvrengage.cpp:174
```

So: play Korea (table loads), switch to Israel (table freed, reload silently
declines), fly Instant Action, and the first merge dereferences a NULL with
Korea's counts still in place. The crash lands at
`DigitalBrain::WvrEngage+0x202`, `mov edx, [rcx+r8*4]` with `rcx` NULL —
"Read from location 0000000c".

**Fixed both ends.** `FreeManeuverData` now zeroes the counts with the pointers
and the rejection path calls it and logs the offending byte to `FFDebugLog`, so
a bad file means "no maneuvers" instead of a crash two theaters later. While in
there, the frees were `delete` on `new[]` allocations — corrected to `delete[]`;
it only ever appeared to work because every element type is POD.

The data is fixed by swapping the two entries back inside
`ZipsITO/Simdata.zip` (original kept as `Simdata.zip.bak-mnvrswap`). The correct
file was already in the package, just under the wrong name — the 16543-byte
entry is byte-identical to stock.

## Second package: Taiwan FF6-3 (2026-10-25)

`FF6_Taiwan3\TaiwanFF6-3` is the same installer family — `Taiwan 6.exe` and an
865,633,402-byte `Source.dat` whose MD5 the stub checks
(`641613b66e4791651d2659d54784da1e`) — and it failed in the same three ways.
Nothing here is new; it is the evidence that the list above is a property of
these packages and not of ITO.

**Key.** One `Software\Microprose\Falcon\4.0` at `0x00385a`, patched in place
into `Taiwan 6 (FreeFalcon).exe` (+4 bytes). That build still aborted before
extraction — exit code 2, nothing written. All six gates are present: four
`IfFileExists` on `$INSTDIR` (`FFViper.exe`, `FFViper Config Editor.exe`,
`terrdata\objects\KoreaObj.Dxh` and `.Dxl`), the `FFVer` read, and the
"requires FreeFalcon 6.0 or later" comparison. `skip_checks.py` bypassed all
six into `(no checks).exe`, which installed. Which of the six fired is not
pinned down; the same-length message rewrite in `installer-patch/README.md` is
how to find out if it ever matters.

**Install.** `/S /D=C:\FreeFalcon6`, exit 0, 84 s, 20,271 files (3.17 GB). A
name-by-name comparison against `7z l` of `Source.dat` finds no file missing
and none extra, and the archive carries only `Theaters\Taiwan` and `ZipsTaiwan`,
so nothing outside the theater is touched. `theater.lst` gained
`theaters\Taiwan\Taiwan.tdf`, and `UnInstall_Taiwan.exe` plus the
`...\Uninstall\Taiwan` key were written.

**Maneuver table.** Same transposition, same silence: `ZipsTaiwan\Simdata.zip`
held `Sim\ACDATA\BRAIN\mnvrdata.dat` at 12,793 bytes beginning `A#`
(byte-identical to stock `mnvrdata5.dat`) and no `mnvrdata5.dat`.
`tools/installer-patch/fix_mnvrdata.py` puts the stock 16,543-byte table in its
place and keeps the 2002 file under `mnvrdata5.dat`, changing only those two
entries — names, order and per-entry compression are preserved, original kept
as `Simdata.zip.bak-mnvrswap`.

Its other Simdata differences are theater content and hang together: the
package's `radtypes.lst` has 329 slots to stock's 169, and the 52 distinct
radar types it names all ship inside the package, so the stock radar files it
omits are simply not referenced. Same for its older `Sim\ACDATA\*.dat` set,
`formdat.fil` and `actypes.lst`.

## Order of operations for the next package

1. Patch the installer's registry key, and its prerequisite gates if they trip:
   `patch_installer.py`, then `skip_checks.py`.
2. Make sure the bundled utilities read `4.1` — `patch_exe.py --in-place` —
   **before** running the installer, or its `TheaterAdd` step is wasted.
3. After installing, check `theater.lst` actually gained the `.tdf` lines.
4. Diff the theater's `Zips*/Simdata.zip` against stock `Zips/Simdata.zip` for
   files whose sizes have swapped. `sim/ACDATA/BRAIN/mnvrdata.dat` is the known
   one — `tools/installer-patch/fix_mnvrdata.py` does the swap — and the same
   package also ships differing `formdat.fil`, `radtypes.lst` and
   `actypes.lst`, which are legitimate theater content rather than mistakes.
