FreeFalcon / FFViper -- VR stereo convergence + clickable-cockpit aim patch
===========================================================================

WHAT THIS IS
------------
A patch for an EXISTING, WORKING FreeFalcon installation. It is not a game
download -- no Falcon 4.0 data is included, and none can be legally
redistributed. If you do not already have FreeFalcon installed and running,
this will not do anything for you.

Fixes a set of VR bugs that affect PLAIN-STEREO headsets (2-view
PRIMARY_STEREO) -- e.g. Meta Quest, most PC HMDs. Quad-views hardware
(Varjo-class) was already correct and is unaffected.


WHAT IT FIXES
-------------
1. Nothing converges in the 3D pit. Both the cockpit and the distant terrain
   appeared doubled at every depth, and no IPD tweak helped. Plain stereo was
   rendering a symmetric frustum while the runtime reports strongly asymmetric
   per-eye FOV (each eye canted ~7 deg outward), leaving the two eye images
   diverging by ~14 deg. Now renders and submits the runtime's true off-axis
   per-eye frustum, as the quad-views path always did.

2. Stereo drifted when looking off-centre. The per-eye IPD was applied along
   the AIRFRAME's right axis instead of the HEAD's, so it was only correct
   while looking straight ahead -- worst looking down-left/down-right at the
   aux panels.

3. Clickable cockpit was unusable with a mouse: two cursors that would not
   fuse, and clicks landing on the wrong switch (worse toward the lower
   corners). Three causes -- the hit-test projected with a different frustum
   than the render, the cursor was drawn through yet another projection, and
   the cursor's stereo offset used the airframe axis rather than the head's.


INSTALL
-------
1. Back up your existing FFViper.exe.
2. Copy FFViper.exe into your FreeFalcon folder, overwriting.
3. Copy the DLLs alongside it ONLY IF you do not already have them:
      OpenAL32.dll, openxr_loader.dll, dxcompiler.dll, dxil.dll, nvtt30205.dll
   If your install already runs the current dev build, you already have these
   and only need the exe.

Do NOT delete ST48W.dll, ST80W.dll or dbghelp.dll from your install -- they
are stock FreeFalcon files, deliberately not included here.


CONFIGURATION
-------------
All three fixes are ON by default. Each can be reverted individually in
FFViper.cfg if it misbehaves on your hardware:

    set g_bVrTrueEyeFov 0         # revert fix 1
    set g_bVrHeadRelIpd 0         # revert fix 2
    set g_bVrHeadRelCursorIpd 0   # revert fix 3

Recommended for the clickable cockpit -- set this to your cockpit panel depth
in button units (~814 on an F-16 pit at default seat position). The free-aim
cursor sits at this depth, so a wrong value makes it split in two:

    set g_fVrRayReach 814

IMPORTANT -- view instancing must stay OFF on plain-stereo hardware:

    set g_bVrViewInstancing 0
    set g_bVrD3D12ViCockpit 0

The view-instanced world pass still uses the old symmetric FOV, so enabling it
reintroduces bug 1. Porting the fix there is listed as known remaining work.


KNOWN REMAINING ISSUES
----------------------
- View instancing needs the same off-axis FOV fix (see above).
- External views (9/0) have no head tracking at all -- every HMD consumer is
  gated to cockpit modes, so head movement there only produces compositor
  reprojection of a head-unaware camera.


NO AUDIO? (not caused by this patch, but commonly hit)
------------------------------------------------------
This build routes DirectSound through OpenAL Soft, which reads alsoft.ini from
%APPDATA% and from the exe's folder. If a global alsoft.ini contains a driver
exclusion WITHOUT a trailing comma, e.g.

    drivers=-dsound

then OpenAL Soft has zero usable backends and opens no device at all -- total
silence. Per its own documentation, unlisted backends are only considered if
the list ENDS WITH A COMMA. The correct form is:

    drivers=-dsound,

Also check volume-adjust / output-limiter: a large positive volume-adjust with
output-limiter=false clips hard and sounds like constant crackling.


REGISTRY -- IMPORTANT IF YOU ALREADY RUN FREEFALCON (registry/ folder)
----------------------------------------------------------------------
This build reads its data paths from the Falcon "4.1" registry key instead of
"4.0". The reason: a stock Falcon 4.0 / GOG install owns the "4.0" key, so
sharing it makes one of the two games read the other's directories. Giving
FreeFalcon its own "4.1" key lets both live on the same machine.

*** If FreeFalcon already worked for you, your paths are under 4.0 and this
    exe will not find them until you migrate. Symptoms: missing terrain, no
    theater, or a failure to start. ***

Fix it with either (both need administrator rights -- they write to HKLM):

  registry\install-registry.bat   (recommended)
      Put it next to FFViper.exe, right-click -> "Run as administrator".
      - If you already have a 4.0 key, it COPIES 4.0 -> 4.1 and leaves 4.0
        completely untouched, so your existing install keeps working and a
        stock Falcon 4.0 is unaffected.
      - If you have no existing key, it writes 4.1 using its own folder as the
        install path, so it works wherever you installed.
      - Re-run with /force to overwrite the 4.1 paths with this folder
        (use this if your 4.0 key belongs to stock Falcon 4.0 rather than
        FreeFalcon, or if you moved your install).

  registry\FreeFalcon6-registry.reg
      Manual alternative. Assumes C:\FreeFalcon6 -- EDIT THE PATHS FIRST if
      your install is elsewhere, then double-click to import.

Both target the 32-bit registry view (WOW6432Node). That is intentional: the
game opens the key with KEY_WOW64_32KEY even though the exe is 64-bit. Do not
"correct" it to the native path or it will not find its data.

Nothing here deletes or edits your 4.0 key.


LICENSING
---------
See THIRD-PARTY-NOTICES.txt and ./licenses/. FreeFalcon itself is BSD 2-Clause,
which permits binary redistribution with the copyright notice and disclaimer
reproduced (both are included).
