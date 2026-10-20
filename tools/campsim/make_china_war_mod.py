"""Build the "Korea China War" JSGME mod: the campaign setup settled in campsim (SUPPLY-RESEARCH.md, rounds 23-36).

    python make_china_war_mod.py [--exe PATH]

Korea save0 (NEW campaigns), on top of Korea Rail War (the campsim tests ran with it enabled -- usecfg=1 reads
config\\mods\\*.cfg like the game). Needs FFViper-campaign.exe (branch campsim-gpu, f830facc or later): the cfg keys below
do not exist in older builds, which ignore them.

  - save0.cam = campaign\\SAVE\\chdd.cam: China's ground battalions staged at Dandong / the Pyongbu line's Yalu crossing
    (make_china_stage.py).
  - save0.tri = chn_appr2.tri: China joins when the ROK holds Sariwon, Hwangju, Anbyon or Wonsan; Blue wins on
    Pyongyang + Wonsan's group (event 17), the North on Seoul or Pusan.
  - Falcon4.AII = the installed (Rail War) one with DataRateModSup 0.75, as campsim ran (prodsup=0.75).
  - config\\mods\\Korea China War.cfg = the h_bar knobs + Blue's bar 40 + the North starting at 70% supply.

Writes C:/FreeFalcon6/MODS/Korea China War/ (not enabled: enable it in JSGME AFTER Korea Rail War). With the exe given,
copies it to C:/FreeFalcon6/FFViper-campaign.exe (FFViper.exe is never touched).
"""
import argparse
import os
import re
import shutil

GAME = r"C:\FreeFalcon6"
SAVE = os.path.join(GAME, "campaign", "SAVE")
MOD = os.path.join(GAME, "MODS", "Korea China War")

CFG = """// Korea China War (JSGME mod): settings for FFViper-campaign.exe, read after FFViper.cfg. Disable the mod in JSGME to remove
// this file. Settled in campsim, 12 seeds: Blue wins 12/12, median day 6.4; China's entry slows Blue for 2-4 days
// (tools/campsim SUPPLY-RESEARCH.md "Latest (2026-10-19/20, rounds 23-36)").

// The h_bar campaign (CAMPAIGN-CHINA-EXPERIMENTS.md): China a team of its own, every team its own initiative,
// offensives shared, a neutral team holds still and uses no supply
set g_bChinaOwnTeam 1
set g_bInitIndependent 1
set g_nTurnMode 4
set g_bNeutralHold 1
set g_bNeutralNoUse 1

// Allied counteroffensive: China/Russia battalions at the front (8 within 80 km) launch a full offensive
set g_bAlliedCounterattack 1
set g_nAlliedCounterMin 8
set g_nAlliedCounterKm 80

// Initiative an offensive needs: Blue 40 (h_bar 50 -- at 40 it can resume after China's push), China 30, DPRK 15
set g_sTeamOffenseBars "1:40,2:40,3:40,5:30,6:15"

// Supply: depots and army bases do not produce (factories and ports do), use x2 moving and firing,
// and the North's battalions begin the war at 70%
set g_bStoresNoProd 1
set g_nSupplyUseMult 2
set g_nStartSupplyTeam 6
set g_nStartSupplyPct 70
"""

README = """Korea China War (JSGME mod)
============================

The Korea campaign (save0, "Tiger Spirit") as tested in campsim for a longer war with China in it:
China's army staged at the Yalu, China joining when the ROK reaches Sariwon, Hwangju, Anbyon or Wonsan,
the North starting the war at 70% supply, and Blue needing less initiative (40) to attack again after
China's counterpush. In 12 simulated wars Blue won all 12, on day 6.4 at the median, with China's entry
slowing the advance for 2-4 days before Blue broke through.

Requirements
  - FFViper-campaign.exe (installed next to FFViper.exe). Start the game with it; FFViper.exe ignores the settings.
  - Korea Rail War enabled FIRST; enable this mod after it (it replaces save0.cam, save0.tri and Falcon4.AII).
  - Start a NEW campaign: the starting supply is applied only when a campaign begins, not to saves.

Files
  campaign/SAVE/save0.cam     China staged forward (campsim save "chdd")
  campaign/SAVE/save0.tri     the China-entry trigger script (campsim "chn_appr2")
  campaign/SAVE/Falcon4.AII   DataRateModSup 0.75 (stock install 1.5)
  config/mods/Korea China War.cfg   the campaign settings

Not compatible with Korea Escalation (both replace save0.cam / save0.tri / Falcon4.AII).
Built by tools/campsim/make_china_war_mod.py (branch campsim-gpu).
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", help="FFViper.exe built from campsim-gpu, installed as FFViper-campaign.exe")
    a = ap.parse_args()

    dst = os.path.join(MOD, "campaign", "SAVE")
    os.makedirs(dst, exist_ok=True)
    os.makedirs(os.path.join(MOD, "config", "mods"), exist_ok=True)

    shutil.copyfile(os.path.join(SAVE, "chdd.cam"), os.path.join(dst, "save0.cam"))
    shutil.copyfile(os.path.join(SAVE, "chn_appr2.tri"), os.path.join(dst, "save0.tri"))

    aii = open(os.path.join(SAVE, "Falcon4.AII"), encoding="latin-1").read()
    aii, n = re.subn(r"(?m)^(DataRateModSup\s*=\s*)[0-9.]+", r"\g<1>0.75", aii)
    if n != 1:
        raise SystemExit("Falcon4.AII: expected one DataRateModSup line, found %d" % n)
    with open(os.path.join(dst, "Falcon4.AII"), "w", encoding="latin-1", newline="") as f:
        f.write(aii)

    with open(os.path.join(MOD, "config", "mods", "Korea China War.cfg"), "w", newline="\r\n") as f:
        f.write(CFG)
    with open(os.path.join(MOD, "Korea China War README.txt"), "w", newline="\r\n") as f:
        f.write(README)

    if a.exe:
        out = os.path.join(GAME, "FFViper-campaign.exe")
        shutil.copyfile(a.exe, out)
        print("installed", out)

    for root, _, files in os.walk(MOD):
        for fn in files:
            p = os.path.join(root, fn)
            print("%9d  %s" % (os.path.getsize(p), os.path.relpath(p, MOD)))


if __name__ == "__main__":
    main()
