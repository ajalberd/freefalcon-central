# Retargeting old theater installers at FreeFalcon

Falcon 4.0 recorded where it was installed in

```
HKLM\SOFTWARE\WOW6432Node\MicroProse\Falcon\4.0    baseDir = D:\GOG Games\Falcon 4.0
```

FreeFalcon is a separate install and uses the neighbouring key:

```
HKLM\SOFTWARE\WOW6432Node\MicroProse\Falcon\4.1    baseDir = C:\FreeFalcon6
                                                   FFVer   = 6.0
```

Community theater installers written against plain Falcon 4 read the `4.0`
key. On a machine that has both, they unpack into the wrong game; and one that
also checks `FFVer` — which only exists under `4.1` — aborts with *"Unable to
read FreeFalcon version"* however the game is installed.

`patch_installer.py` writes a copy of the installer that reads `4.1` instead.

## Using it

```bash
python patch_installer.py "ITO2 V4c.exe"
```

```
Falcon 4.0 -> 4.1 in 1 file(s)
  ITO2 V4c.exe
      NSIS header 26883 bytes, lzma, archive 351056 bytes
      0x003892  Software\Microprose\Falcon\4.0
      wrote ITO2 V4c (FreeFalcon).exe  (+14 bytes, header block 6224 -> 6238)
1 patched
```

The original is never touched. Other forms:

```bash
python patch_installer.py "ITO2 V4c.exe" --list      # report, change nothing
python patch_installer.py *.exe                      # a folder of installers
python patch_installer.py setup.exe --from 4.0 --to 4.1 --suffix " (FF)"
python patch_installer.py --emit-shim falcon41.reg   # for installers it can't read
```

Keep the patched installer next to the original's data files — these
installers read a `Source.dat` from their own directory and check its MD5, so
moving the `.exe` on its own will fail authentication.

## Why it is only one byte, and why that still needs a rebuild

The registry path is a literal string in the installer's NSIS header. `4.0`
and `4.1` are the same length and the registry is case-insensitive, so the
string can be rewritten where it sits — NSIS string-table offsets are
absolute, and a longer or shorter replacement would shift every string after
it. The script refuses a different-length value for that reason.

The work is in the container around it. An NSIS file is the stub `.exe`, then
a first header carrying `0xDEADBEEF "NullsoftInst"` and two sizes, then a
sequence of blocks each prefixed by a size whose top bit means *compressed*.
Patching one byte means: decompress the header block, edit, recompress,
rewrite the block size and the archive length, and recompute the CRC32 — which
the stub checks over the file **from offset 512**, skipping the DOS stub. Get
any of those wrong and the installer refuses to start.

LZMA, bzip2, deflate and stored header blocks are all handled; the codec and
its exact parameters are read from the file and reused, so the output is a
stream the same stub can read. Before the tool writes anything it decompresses
its own output and compares, and after writing it re-opens the result and
checks that the new key is present and the old one is gone — a file that fails
either check is deleted rather than left around.

Solid archives — where the header and every packed file share one compression
stream — are handled too, by decompressing the whole stream, patching, and
recompressing it. That is slow on a large archive and there is no way around
it: the packed files share a compression context with the header, so there is
nothing to splice.

The patched file usually comes out a handful of bytes larger. liblzma's
encoder is not bit-identical to the one NSIS shipped, so the same header
content compresses to a slightly different size. Nothing downstream cares —
every size and the CRC are recomputed from what was actually written.

## What it does not handle

Inno Setup and InstallShield installers have no NSIS header. For those, or for
anything else the script cannot read, `--emit-shim` reads the live `4.1` values
and writes a `.reg` that defines `4.0` the same way:

```bash
python patch_installer.py --emit-shim falcon41.reg
reg import falcon41.reg
```

That makes every old installer find FreeFalcon, but it is the blunt
instrument: while it is applied, anything that genuinely wants Falcon 4.0 — a
real Falcon 4.0 install included — is sent to FreeFalcon too. Back the key up
first and remove the shim when the theater is in:

```bash
reg export "HKLM\SOFTWARE\WOW6432Node\MicroProse\Falcon\4.0" falcon40-backup.reg
```

## The utilities have the same bug

FreeFalcon ships tools that read the key too, and they read the **old** one:

```
Utilities\TheaterAdd.exe       Software\MicroProse\Falcon\4.0
Utilities\SeasonSwitcher.exe   Software\MicroProse\Falcon\4.0
Utilities\WeatherEditor.exe    Software\MicroProse\Falcon\4.0
F4Info.exe, hostidx.exe, mpsinfo.exe     SOFTWARE\MicroProse\Falcon\4.0
FFViper.exe                     Software\MicroProse\Falcon\4.1
```

This is how a theater installs perfectly and still never appears in the theater
list. Every ITO installer's last step is three `TheaterAdd.exe -a` calls, and
`TheaterAdd` resolves `theater.lst` from `4.0`'s `baseDir` -- so on a machine
with a real Falcon 4.0 install it appends to *that* game's list and FreeFalcon
never hears about it. Nothing reports an error.

`patch_exe.py` fixes a plain PE. There is no container here, so it really is one
byte; the only bookkeeping is the PE checksum, recomputed when the file carried
one:

```bash
python patch_exe.py "C:\FreeFalcon6\Utilities\*.exe" --list
python patch_exe.py "C:\FreeFalcon6\Utilities\*.exe" --in-place
```

`--in-place` keeps a `.bak-falcon40` beside each file, which is what you want
here: these are launched by name from installers, so a renamed copy would never
be the one that runs. The tool refuses a replacement of a different length and
warns if a file is Authenticode signed (none of these are).

`TheaterAdd` is idempotent -- re-running it for a theater already in the list
adds nothing -- so it is safe to run by hand to repair a list:

```bash
cd /c/FreeFalcon6 && ./Utilities/TheaterAdd.exe -a "theaters\Israel\Israeli.tdf"
```

## When the gate is the problem, not the registry

Retargeting the key is not always enough. These installers also refuse to run
unless they recognise the install: ITO checks for four files under `$INSTDIR`
and then compares a registry `FFVer` against a literal `"6.0"`. A gate can fail
for reasons that have nothing to do with whether the theater will work.

`skip_checks.py` switches them off:

```bash
python skip_checks.py "ITO2 V4c (FreeFalcon).exe" --list   # report only
python skip_checks.py "ITO2 V4c (FreeFalcon).exe"
```

```
  ITO2 V4c (FreeFalcon).exe
      455 instructions, 12117 strings
       376  IfFileExists  -> Quit    'FreeFalcon5 not installed. Aborting...'
       389  StrCmp        -> Abort   'Unable to read FreeFalcon version'
       396  StrCmp        -> Abort   'ITO2 V4c requires FreeFalcon 6.0 or later'
      wrote ITO2 V4c (FreeFalcon) (no checks).exe  (6 gate(s) bypassed)
```

`--pattern` widens or narrows what counts as a gate. The default is deliberately
narrow -- it will not touch a re-install guard, because that one usually exists
for a reason:

```bash
python skip_checks.py setup.exe --pattern "not installed|requires|already installed"
```

An "already installed" guard is the one to think twice about. ITO's runs
`theateradd.exe -a` three times just after it, and that appends to
`theater.lst`; bypassing it on top of a **complete** install risks duplicate
theater entries. Bypassing it to finish an install that was interrupted during
extraction is safe, because none of the appending has happened yet -- check
`theater.lst` and whether `UnInstall_ITO2.exe` exists before deciding.

A gate is always the same three instructions — a conditional whose *failing*
branch is 0 (fall through), then a `MessageBox`, then `Abort` or `Quit` — so
switching one off is a single parameter: point that branch past the `Abort`
instead of into the message. Both ways out then reach the same instruction,
which makes the test a no-op rather than an inverted test that fails the other
way round.

Nothing is deleted and no instruction moves, which matters: NSIS branch targets
are **absolute indices stored as index + 1**, so inserting or removing even one
instruction would silently redirect every jump after it. The self-test asserts
that the only bytes that changed are inside the branch parameters it named.

`nsis_entries.py` is the disassembler underneath — the instruction block, the
string table and its inline variable references (`0xFD` plus a two-byte index,
which is how `$INSTDIR` survives into a message). Use it to read a gate before
deciding to remove it:

```python
import patch_installer as P, nsis_entries
ents = nsis_entries.Entries(P.Installer("setup.exe"))
print("\n".join(ents.dump(380, 400)))
```

It is also how to instrument one. Message text is an ordinary string-table entry
and can hold a variable reference, so a **same-length** rewrite of a complaint
into something like `READ [$var35] WANTED [6.0]` makes the installer report the
value it actually read instead of leaving you to infer it.

## Checking the result

`selftest.py` patches an installer, patches it back, and requires the header to
come out byte-identical to the original — along with checking that the file
data, the PE image, the sizes and the CRC are all what they should be:

```bash
python selftest.py "F:\...\ITO V4c\ITO2 V4c.exe"
```

It also rebuilds each file without changing anything and requires the result
to carry the same header and the same packed files — which is the only
coverage a solid archive gets, since none of the shipped ones happen to carry
a Falcon key — and it runs `skip_checks` over the same file, asserting that the
instruction count, the string table and the header size are all untouched and
that every changed byte lies inside a branch parameter it named.

Across the 66 installers on hand, 15 are readable NSIS, 8 carry a Falcon 4.0
key, and the other 51 are Inno Setup or InstallShield and need `--emit-shim`.

7-Zip is a useful second opinion, since it reads NSIS archives with an
implementation that shares no code with this one:

```bash
7z t "ITO2 V4c (FreeFalcon).exe"
7z l "ITO2 V4c (FreeFalcon).exe"
```
