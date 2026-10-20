"""Supply research trials (2026-10-18): what supply mechanisms do to the h_bar war. Deterministic campsim (det=1) and startups launched one at a
time (simultaneous startups still race on something shared and part at hour 1), so a mechanism is compared with ITS OWN seed's baseline.

    python supply_trials.py run [round]       queue the arms of a round, 3 seeds each
    python supply_trials.py report [round]    paired table from supply/results.jsonl

Per run: the end (event@hour), Blue's share of objectives at the end and its peak, Blue's loss of objectives from the peak (a setback), China's
join hour, Blue's share of time on the offensive after the join and its mean initiative, Blue SPEARHEAD supply (mean supply of Blue battalions
within 40 km of Blue's northern edge) on days 3 / 5 / 7, the North's front supply the same way, and the SXDIAG line (sources out of reach)."""
import hashlib
import json
import os
import re
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "supply")
RES = os.path.join(OUT, "results.jsonl")
PAR = int(os.environ.get("TRIAL_JOBS", "9"))
SEEDS = tuple(int(x) for x in os.environ.get("TRIAL_SEEDS", "1,2,3").split(","))
HBAR = ("usecfg=1 tri=chn_appr2 alliedcounter=1:8:80 noprod=1 prodsup=0.75 usemult=2 chinaown=1 neutralnouse=1 neutralhold=1 initindep=1 "
        "turnmode=4 teambar=1:50,2:50,3:50,5:30,6:15 det=1")
lock = threading.Lock()
launch = threading.Lock()
SMODEL = "sxrear=72 sxhops=12 sxloss=10 sxdelay=10 sxwheeled=1 teambar=1:40,2:40,3:40,5:30,6:15"
# campaigns: key -> (save, theater stem, base settings, endgame event that is a Blue / Israeli win). An arm is (knobs, round) for chdd, or
# (knobs, round, campaign key). The base for the stock campaigns is just the cfg + deterministic replay.
GEN = "usecfg=1 det=1"
CAMPS = {"chdd": ("chdd", "", HBAR, 17), "k1": ("save1", "", GEN, 17), "k2": ("save2", "", GEN, 17), "i2": ("save2", "Israel2007", GEN, 6)}
RAIL = ("raillines=Gyeongbu,Gyeongui,Hambuk,Honam,Kangwon,Khasan,Manpo,Meiji,Pukbunaeryuk,Pyongbu,Pyongdok,Pyongra,Pyongui,Shendan,Shenji "
        "railload=240")
# the light package: no hard reach, 5% per road node, convoy time, captured stores usable after 12 h and from 50% status
LIGHT = "sxloss=5 sxdelay=10 sxrear=12 sxsrcmin=50 sxwheeled=1"
BAR15 = "teambar=1:15,2:15,3:15,5:15,6:15"


def camp(arm):
    a = ARMS[arm]
    return CAMPS[a[2] if len(a) > 2 else "chdd"]

# arm -> (extra knobs, round)
ARMS = {
    "base": ("", 1),
    "base_rep": ("", 1),                      # the same again: must match "base" exactly
    "own": ("sxown=1", 1),                     # only sources the team holds
    "rear24": ("sxrear=24", 1),                # a captured source counts after 24 h
    "rear72": ("sxrear=72", 1),
    "home": ("sxrear=9999", 1),                # only sources held since the start
    "hops6": ("sxhops=6", 1),                  # reach: 6 links
    "hops10": ("sxhops=10", 1),
    "loss5": ("sxloss=5", 1),                  # every road node costs 5% (stock 2)
    "loss5r0": ("sxloss=5 sxrail=0", 1),       # ... but rail is free: rail as the artery
    "home_loss5r0": ("sxrear=9999 sxloss=5 sxrail=0", 1),
    # round 2 (det clock build): can anything starve a spearhead? extremes first
    "b2": ("", 2),
    "loss10": ("sxloss=10", 2),
    "loss10r1": ("sxloss=10 sxrail=1", 2),
    "home_h8_l10": ("sxrear=9999 sxhops=8 sxloss=10", 2),
    "use4": ("usemult=4", 2),
    "use4_home_l10": ("usemult=4 sxrear=9999 sxloss=10", 2),
    # round 3: reach with forward depots after a delay (setback, then recovery?), and the engaged / convoy-time mechanisms
    "b3": ("", 3),
    "r_home": ("sxrear=9999 sxhops=8 sxloss=10", 3),
    "r48": ("sxrear=48 sxhops=8 sxloss=10", 3),
    "r72": ("sxrear=72 sxhops=8 sxloss=10", 3),
    "r120": ("sxrear=120 sxhops=8 sxloss=10", 3),
    "r72h12": ("sxrear=72 sxhops=12 sxloss=10", 3),
    "r72l5": ("sxrear=72 sxhops=8 sxloss=5", 3),
    "front0": ("sxfront=0", 3),
    "front25": ("sxfront=25", 3),
    "delay5": ("sxdelay=5", 3),
    "delay10": ("sxdelay=10", 3),
    # round 4: supply-weighted initiative (an existing switch) so a resupplied Blue can take the initiative back; and the player's strike choices,
    # held down from hour 48 (blow=...:hold), with and without the reach rules
    "b4": ("", 4),
    "is": ("initsupply=1", 4),
    "R": ("sxrear=72 sxhops=12 sxloss=10", 4),
    "R_is": ("sxrear=72 sxhops=12 sxloss=10 initsupply=1", 4),
    "R48_is": ("sxrear=48 sxhops=8 sxloss=10 initsupply=1", 4),
    "b_bridgeN": ("blow=bridge:6:hold blowat=48", 4),
    "b_depotN": ("blow=depot:6:hold blow=armybase:6:hold blowat=48", 4),
    "R_is_bridgeN": ("sxrear=72 sxhops=12 sxloss=10 initsupply=1 blow=bridge:6:hold blowat=48", 4),
    "R_is_depotN": ("sxrear=72 sxhops=12 sxloss=10 initsupply=1 blow=depot:6:hold blow=armybase:6:hold blowat=48", 4),
    "R_is_factN": ("sxrear=72 sxhops=12 sxloss=10 initsupply=1 blow=factory:6:hold blowat=48", 4),
    "R_is_bridgeB": ("sxrear=72 sxhops=12 sxloss=10 initsupply=1 blow=bridge:2:hold blowat=72", 4),
    "d5": ("sxdelay=5", 4),
    "d10": ("sxdelay=10", 4),
    # round 5: the comeback (Blue's bar 40 instead of 50), reach + convoy time together, and strikes under convoy time
    "R_bar40": ("sxrear=72 sxhops=12 sxloss=10 teambar=1:40,2:40,3:40,5:30,6:15", 5),
    "R_bar40_is": ("sxrear=72 sxhops=12 sxloss=10 initsupply=1 teambar=1:40,2:40,3:40,5:30,6:15", 5),
    "R_d10": ("sxrear=72 sxhops=12 sxloss=10 sxdelay=10", 5),
    "R_d10_bar40": ("sxrear=72 sxhops=12 sxloss=10 sxdelay=10 teambar=1:40,2:40,3:40,5:30,6:15", 5),
    "d10_depotN": ("sxdelay=10 blow=depot:6:hold blow=armybase:6:hold blowat=48", 5),
    "d10_bridgeN": ("sxdelay=10 blow=bridge:6:hold blowat=48", 5),
    "d10_factN": ("sxdelay=10 blow=factory:6:hold blowat=48", 5),
    # round 6: supply by truck (stock routes it on Foot, round a blown bridge) -- does bridge interdiction bite then?
    "b6": ("", 6),
    "w": ("sxwheeled=1", 6),
    "w_bridgeN": ("sxwheeled=1 blow=bridge:6:hold blowat=48", 6),
    "w_d10": ("sxwheeled=1 sxdelay=10", 6),
    "w_d10_bridgeN": ("sxwheeled=1 sxdelay=10 blow=bridge:6:hold blowat=48", 6),
    "w_R_bridgeN": ("sxwheeled=1 sxrear=72 sxhops=12 sxloss=10 blow=bridge:6:hold blowat=48", 6),
    "w_bridgeB": ("sxwheeled=1 blow=bridge:2:hold blowat=72", 6),
    "w_d10_bridgeB": ("sxwheeled=1 sxdelay=10 blow=bridge:2:hold blowat=72", 6),
    # round 7: the player's choices on the candidate model S = reach (depots after 72 h, 12 links, 10%/node) + convoy time + Blue bar 40,
    # with supply by truck: which strike, and when
    "S": (SMODEL, 7),
    "S_depotN": (SMODEL + " blow=depot:6:hold blow=armybase:6:hold blowat=48", 7),
    "S_depotN96": (SMODEL + " blow=depot:6:hold blow=armybase:6:hold blowat=96", 7),
    "S_bridgeN": (SMODEL + " blow=bridge:6:hold blowat=48", 7),
    "S_factN": (SMODEL + " blow=factory:6:hold blowat=48", 7),
    "S_portN": (SMODEL + " blow=port:6:hold blowat=48", 7),
    "S_bridgeB": (SMODEL + " blow=bridge:2:hold blowat=72", 7),
    "S_depotB": (SMODEL + " blow=depot:2:hold blow=armybase:2:hold blowat=72", 7),
    # round 8: logistics hubs only (no city is a supply source), so supply has to travel over roads and bridges
    "nc": ("sxnocity=1", 8),
    "nc_bridgeN": ("sxnocity=1 sxwheeled=1 blow=bridge:6:hold blowat=48", 8),
    "N": (SMODEL + " sxnocity=1", 8),
    "N_bridgeN": (SMODEL + " sxnocity=1 blow=bridge:6:hold blowat=48", 8),
    "N_depotN": (SMODEL + " sxnocity=1 blow=depot:6:hold blow=armybase:6:hold blowat=48", 8),
    "N_bridgeB": (SMODEL + " sxnocity=1 blow=bridge:2:hold blowat=72", 8),
    "N_depotB": (SMODEL + " sxnocity=1 blow=depot:2:hold blow=armybase:2:hold blowat=72", 8),
    # round 9: player-sized strikes on model S (blow=TYPE:TEAM:hold:N -- the N of the type nearest the enemy, held down)
    "S9": (SMODEL, 9),
    "S_depot10": (SMODEL + " blowlim=depot:6:10 blowlim=armybase:6:10 blowat=48", 9),
    "S_bridge10": (SMODEL + " blowlim=bridge:6:10 blowat=48", 9),
    "S_port5": (SMODEL + " blowlim=port:6:5 blowat=48", 9),
    "S_fact10": (SMODEL + " blowlim=factory:6:10 blowat=48", 9),
    "S_bridgeB10": (SMODEL + " blowlim=bridge:2:10 blowat=72", 9),
    "S_depotB10": (SMODEL + " blowlim=depot:2:10 blowlim=armybase:2:10 blowat=72", 9),
    # round 10: determinism check (S again, vs S9), and convoy interdiction (sxconvoyhit=T:P, with sxdelay): the player's sorties on the
    # North's lines, or the North's on Blue's
    "S_rep": (SMODEL, 10),
    "S_convN5": (SMODEL + " sxconvoyhit=6:5", 10),
    "S_convN15": (SMODEL + " sxconvoyhit=6:15", 10),
    "S_convB10": (SMODEL + " sxconvoyhit=2:10", 10),
    "S_depot10_rep": (SMODEL + " blowlim=depot:6:10 blowlim=armybase:6:10 blowat=48", 10),  # the new key reproduces round 9?
    # round 11 (zeroed allocations under det=1): exact replays -- each must match its twin frame for frame
    "det_S_a": (SMODEL, 11),
    "det_S_b": (SMODEL, 11),
    "det_h_a": ("", 11),
    "det_h_b": ("", 11),
    # round 12: paired same-seed comparisons on model S (6 seeds), each arm against "P_S" of the same seed
    "P_S": (SMODEL, 12),
    "P_depot10": (SMODEL + " blowlim=depot:6:10 blowlim=armybase:6:10 blowat=48", 12),
    "P_fact10": (SMODEL + " blowlim=factory:6:10 blowat=48", 12),
    "P_bridge10": (SMODEL + " blowlim=bridge:6:10 blowat=48", 12),
    "P_bridgeB10": (SMODEL + " blowlim=bridge:2:10 blowat=72", 12),
    "P_convN15": (SMODEL + " sxconvoyhit=6:15", 12),
    "P_convB10": (SMODEL + " sxconvoyhit=2:10", 12),
    "P_h": ("", 12),
    # round 13 (China-supply fix in): the follow-up ideas and the map model, exact pairs against P_S, 6 seeds
    # 1: trains on every line (the cfg already runs one on the Pyongbu Line), China's from Shenyang / Dandong too, twice the load
    "P_rail": (SMODEL + " raillines=Gyeongbu,Gyeongui,Hambuk,Honam,Kangwon,Khasan,Manpo,Meiji,Pukbunaeryuk,Pyongbu,Pyongdok,Pyongra,Pyongui,Shendan,Shenji railload=240", 13),
    "P_rail_bridgeRear": (SMODEL + " raillines=Gyeongbu,Gyeongui,Hambuk,Honam,Kangwon,Khasan,Manpo,Meiji,Pukbunaeryuk,Pyongbu,Pyongdok,Pyongra,Pyongui,Shendan,Shenji railload=240 blowsel=bridge:6:10:10:1 blowat=48", 13),
    "P_att3": (SMODEL + " sxattackuse=3", 13),                               # 3: attacking uses 3x the supply
    "P_pocket": (SMODEL + " sxpocket=12", 13),                               # 4: cut off 12 h -> morale falls, surrender
    "P_bridgeRear": (SMODEL + " blowsel=bridge:6:10:10:1 blowat=48", 13),    # 5: supply-line bridges behind the front (11th-20th nearest)
    "P_bridgeOnce": (SMODEL + " blowsel=bridge:6:0:10:0 blowat=48", 13),     # 5: front bridges struck once, then repaired
    "P_bar15": (SMODEL + " teambar=1:15,2:15,3:15,5:15,6:15", 13),           # 7: supply alone sets the rhythm (cfg bar 15 for all)
    "P_bar50": (SMODEL + " teambar=1:50,2:50,3:50,5:30,6:15", 13),           # 7: the h_bar bars kept
    "P_map": (SMODEL + " sxmap=1", 13),                                      # map model: producers -> stores -> shipments -> units
    "P_map_fact10": (SMODEL + " sxmap=1 blowlim=factory:6:10 blowat=48", 13),
    "P_map_depot10": (SMODEL + " sxmap=1 blowlim=depot:6:10 blowlim=armybase:6:10 blowat=48", 13),
    "P_map_bridgeRear": (SMODEL + " sxmap=1 blowsel=bridge:6:10:10:1 blowat=48", 13),
    "P_map_convN15": (SMODEL + " sxmap=1 sxconvoyhit=6:15", 13),             # 2: the few convoys (shipments) of the North hit
    "P_map_convB10": (SMODEL + " sxmap=1 sxconvoyhit=2:10", 13),             # 2: Blue's shipments hit
    "P_hmap": ("sxmap=1", 13),                                               # the map model on plain h_bar
    # round 14: softer attack use, pockets as true encirclement, the map model with a 50-link shipping reach and 50% shipments (+ rail)
    "P_att2": (SMODEL + " sxattackuse=2", 14),
    "P_pocket2": (SMODEL + " sxpocket=12", 14),
    "P_map50": (SMODEL + " sxmap=1 sxmapreach=50 sxmapship=50", 14),
    "P_map50_rail": (SMODEL + " sxmap=1 sxmapreach=50 sxmapship=50 raillines=Gyeongbu,Gyeongui,Hambuk,Honam,Kangwon,Khasan,Manpo,Meiji,Pukbunaeryuk,Pyongbu,Pyongdok,Pyongra,Pyongui,Shendan,Shenji railload=240", 14),
    "P_map50_fact10": (SMODEL + " sxmap=1 sxmapreach=50 sxmapship=50 blowlim=factory:6:10 blowat=48", 14),
    "P_map50_depot10": (SMODEL + " sxmap=1 sxmapreach=50 sxmapship=50 blowlim=depot:6:10 blowlim=armybase:6:10 blowat=48", 14),
    "P_map50_bridgeRear": (SMODEL + " sxmap=1 sxmapreach=50 sxmapship=50 blowsel=bridge:6:10:10:1 blowat=48", 14),
    "P_map50_convN15": (SMODEL + " sxmap=1 sxmapreach=50 sxmapship=50 sxconvoyhit=6:15", 14),
    "P_map50_convB10": (SMODEL + " sxmap=1 sxmapreach=50 sxmapship=50 sxconvoyhit=2:10", 14),
    "P_map50_depotB10": (SMODEL + " sxmap=1 sxmapreach=50 sxmapship=50 blowlim=depot:2:10 blowlim=armybase:2:10 blowat=72", 14),
    # round 15: the map model with the stock 2% node loss (model S's 10% compounds over long shipping routes), and the strikes on it
    "P_M": ("sxrear=72 sxhops=12 sxdelay=10 sxwheeled=1 teambar=1:40,2:40,3:40,5:30,6:15 sxmap=1 sxmapreach=50 sxmapship=50 sxmapair=10", 15),
    "P_M_depot10": ("sxrear=72 sxhops=12 sxdelay=10 sxwheeled=1 teambar=1:40,2:40,3:40,5:30,6:15 sxmap=1 sxmapreach=50 sxmapship=50 sxmapair=10 blowlim=depot:6:10 blowlim=armybase:6:10 blowat=48", 15),
    "P_M_fact10": ("sxrear=72 sxhops=12 sxdelay=10 sxwheeled=1 teambar=1:40,2:40,3:40,5:30,6:15 sxmap=1 sxmapreach=50 sxmapship=50 sxmapair=10 blowlim=factory:6:10 blowat=48", 15),
    "P_M_bridgeRear": ("sxrear=72 sxhops=12 sxdelay=10 sxwheeled=1 teambar=1:40,2:40,3:40,5:30,6:15 sxmap=1 sxmapreach=50 sxmapship=50 sxmapair=10 blowsel=bridge:6:10:10:1 blowat=48", 15),
    "P_M_convN15": ("sxrear=72 sxhops=12 sxdelay=10 sxwheeled=1 teambar=1:40,2:40,3:40,5:30,6:15 sxmap=1 sxmapreach=50 sxmapship=50 sxmapair=10 sxconvoyhit=6:15", 15),
    "P_M_depotB10": ("sxrear=72 sxhops=12 sxdelay=10 sxwheeled=1 teambar=1:40,2:40,3:40,5:30,6:15 sxmap=1 sxmapreach=50 sxmapship=50 sxmapair=10 blowlim=depot:2:10 blowlim=armybase:2:10 blowat=72", 15),
    "P_M_convB10": ("sxrear=72 sxhops=12 sxdelay=10 sxwheeled=1 teambar=1:40,2:40,3:40,5:30,6:15 sxmap=1 sxmapreach=50 sxmapship=50 sxmapair=10 sxconvoyhit=2:10", 15),
    # round 16: posture follows supply (sxposture=P:R) -- a team consolidates while more than P% of its front battalions are starved
    "P_post30": (SMODEL + " sxposture=30:15", 16),
    "P_post40": (SMODEL + " sxposture=40:25", 16),
    "P_post50": (SMODEL + " sxposture=50:35", 16),
    "P_post40_bar15": (SMODEL + " sxposture=40:25 teambar=1:15,2:15,3:15,5:15,6:15", 16),
    # round 17: the global levers one at a time on plain h_bar (paired against P_h) -- what lowers a unit's supply, and where
    "L_ratio25": ("maxratio=0.25", 17),          # a delivery covers at most 25% of the shortfall (stock 50%)
    "L_ratio10": ("maxratio=0.1", 17),
    "L_intOff120": ("gndsupoff=120", 17),        # attacking units resupplied every 2 h (stock 20 min)
    "L_intAll": ("gndsupoff=180 gndsupdef=360", 17),
    "L_use3": ("usemult=3", 17),                 # (base is 2)
    "L_prod40": ("prodsup=0.4", 17),             # production (base 0.75)
    "L_loss5": ("sxloss=5", 17),                 # 5% per road node (stock 2%): distance costs
    "L_nocity": ("sxnocity=1", 17),              # only depots / army bases / ports are sources: fewer, further sources
    "L_nocity_loss5": ("sxnocity=1 sxloss=5", 17),
    "P_S12": (SMODEL.replace("sxrear=72", "sxrear=12"), 17),   # model S with captured depots usable after 12 h (vs P_S)
    # round 18: the delay comes from repair, not a timer -- stores wrecked when lost (sxscorch), sources count from 50% status (sxsrcmin)
    "P_scorch60": (SMODEL.replace("sxrear=72", "sxscorch=60 sxsrcmin=50"), 18),
    "P_scorch80": (SMODEL.replace("sxrear=72", "sxscorch=80 sxsrcmin=50"), 18),
    "P_srcmin50": (SMODEL.replace("sxrear=72", "sxsrcmin=50"), 18),   # status gate only (bombed sources stop counting)
    # round 19: sxrear alone on plain h_bar (paired against P_h), and with hubs only (no city is a source)
    "L_rear12": ("sxrear=12", 19),
    "L_rear72": ("sxrear=72", 19),
    "L_rear12_nocity": ("sxrear=12 sxnocity=1", 19),
    "L_rear72_nocity": ("sxrear=72 sxnocity=1", 19),
    # round 20: the light package (no hard reach) on Korea chdd, against P_h / P_S12; variants drop or add one piece
    "LP": (LIGHT + " " + BAR15 + " " + RAIL, 20),
    "LP_bar40": (LIGHT + " teambar=1:40,2:40,3:40,5:30,6:15 " + RAIL, 20),
    "LP_hbar": (LIGHT + " " + RAIL, 20),                     # h_bar's bars kept
    "LP_norail": (LIGHT + " " + BAR15, 20),
    "LP_nodelay": (LIGHT.replace("sxdelay=10 ", "") + " " + BAR15 + " " + RAIL, 20),
    "LP_loss10": (LIGHT.replace("sxloss=5", "sxloss=10") + " " + BAR15 + " " + RAIL, 20),
    "LP_hops12": (LIGHT + " sxhops=12 " + BAR15 + " " + RAIL, 20),
    "LP_post40": (LIGHT + " " + BAR15 + " " + RAIL + " sxposture=40:25", 20),
    # round 21: the other campaigns -- stock settings, + shared offensives, + the light package (Korea also with rail)
    "K1_gen": ("", 21, "k1"),
    "K1_t4": ("turnmode=4", 21, "k1"),
    "K1_light": ("turnmode=4 " + LIGHT + " " + RAIL, 21, "k1"),
    "K1_light_norail": ("turnmode=4 " + LIGHT, 21, "k1"),
    "K2_gen": ("", 21, "k2"),
    "K2_t4": ("turnmode=4", 21, "k2"),
    "K2_light": ("turnmode=4 " + LIGHT + " " + RAIL, 21, "k2"),
    "K2_light_norail": ("turnmode=4 " + LIGHT, 21, "k2"),
    "I2_gen": ("", 21, "i2"),
    "I2_t4": ("turnmode=4", 21, "i2"),
    "I2_light": ("turnmode=4 " + LIGHT, 21, "i2"),
    "I2_light_t0": (LIGHT, 21, "i2"),                        # the package without shared offensives
    # round 22: controls for the light package -- how much is the bar, how much is supply?
    "L_bar15": (BAR15, 22),                                  # only the bars changed (vs P_h)
    "L_bar15_rail": (BAR15 + " " + RAIL, 22),                # bars + rail, no supply rules
    "LP_nosx": (BAR15 + " " + RAIL + " sxwheeled=1", 22),    # = LP without the supply rules
    # round 23: Blue's offensive bar 25-50 (China 30, North 15), with and without the light supply rules, no rail boost
    "B25": ("teambar=1:25,2:25,3:25,5:30,6:15", 23),
    "B25_light": (LIGHT + " teambar=1:25,2:25,3:25,5:30,6:15", 23),
    "B30": ("teambar=1:30,2:30,3:30,5:30,6:15", 23),
    "B30_light": (LIGHT + " teambar=1:30,2:30,3:30,5:30,6:15", 23),
    "B35": ("teambar=1:35,2:35,3:35,5:30,6:15", 23),
    "B35_light": (LIGHT + " teambar=1:35,2:35,3:35,5:30,6:15", 23),
    "B40": ("teambar=1:40,2:40,3:40,5:30,6:15", 23),
    "B40_light": (LIGHT + " teambar=1:40,2:40,3:40,5:30,6:15", 23),
    "B45": ("teambar=1:45,2:45,3:45,5:30,6:15", 23),
    "B45_light": (LIGHT + " teambar=1:45,2:45,3:45,5:30,6:15", 23),
    "B50": ("teambar=1:50,2:50,3:50,5:30,6:15", 23),
    "B50_light": (LIGHT + " teambar=1:50,2:50,3:50,5:30,6:15", 23),
    # round 24: can the player's choices turn bar 45 + light (2/6 Blue wins)? Target-priority sliders (objpri, Blue's teams) and direct strikes
    "O_pol": (LIGHT + " teambar=1:45,2:45,3:45,5:30,6:15 objpri=pol:100", 24),
    "O_depot": (LIGHT + " teambar=1:45,2:45,3:45,5:30,6:15 objpri=depot:100,armybase:100", 24),
    "O_prod": (LIGHT + " teambar=1:45,2:45,3:45,5:30,6:15 objpri=prod:100", 24),
    "O_roads": (LIGHT + " teambar=1:45,2:45,3:45,5:30,6:15 objpri=roads:100", 24),
    "O_supply": (LIGHT + " teambar=1:45,2:45,3:45,5:30,6:15 objpri=pol:100,prod:100,depot:100,armybase:100,roads:100", 24),
    "O_supplyonly": (LIGHT + " teambar=1:45,2:45,3:45,5:30,6:15 objpri=pol:100,prod:100,depot:100,armybase:100,roads:100,airbase:0,airstrip:0,radar:0", 24),
    "O_depot10": (LIGHT + " teambar=1:45,2:45,3:45,5:30,6:15 blowlim=depot:6:10 blowlim=armybase:6:10 blowat=48", 24),
    "O_fact10": (LIGHT + " teambar=1:45,2:45,3:45,5:30,6:15 blowlim=factory:6:10 blowat=48", 24),
    "O_convN15": (LIGHT + " teambar=1:45,2:45,3:45,5:30,6:15 sxconvoyhit=6:15", 24),
    "O_supply_nolight": ("teambar=1:45,2:45,3:45,5:30,6:15 objpri=pol:100,prod:100,depot:100,armybase:100,roads:100", 24),
    # round 25: real convoys (sxconvoys), A = h_bar (before model S) vs B = Blue bar 45 + the light supply rules (after); fresh baselines, same build
    "CV_A0": ("", 25),
    "CV_A1": ("sxconvoys=6", 25),
    "CV_A1_4": ("sxconvoys=4", 25),
    "CV_B0": (LIGHT + " teambar=1:45,2:45,3:45,5:30,6:15", 25),
    "CV_B1": (LIGHT + " teambar=1:45,2:45,3:45,5:30,6:15 sxconvoys=6", 25),
    "CV_B1_4": (LIGHT + " teambar=1:45,2:45,3:45,5:30,6:15 sxconvoys=4", 25),
    # round 26: round 25 again with columns that keep their route, divert when the destination falls, and rally (no stalls); same build
    "CF_A0": ("", 26),
    "CF_A1": ("sxconvoys=6", 26),
    "CF_A1_4": ("sxconvoys=4", 26),
    "CF_B0": (LIGHT + " teambar=1:45,2:45,3:45,5:30,6:15", 26),
    "CF_B1": (LIGHT + " teambar=1:45,2:45,3:45,5:30,6:15 sxconvoys=6", 26),
    "CF_B1_4": (LIGHT + " teambar=1:45,2:45,3:45,5:30,6:15 sxconvoys=4", 26),
    # round 27: rail on (bar 45 + light); rail bridge objectives, cut penalties (transload 12 h / 50%, 4 h detrain on open track)
    "RB_0": (LIGHT + " teambar=1:45,2:45,3:45,5:30,6:15 " + RAIL, 27),
    "RB_obj": (LIGHT + " teambar=1:45,2:45,3:45,5:30,6:15 " + RAIL + " railbridgeobjs=1", 27),
    "RB_pen": (LIGHT + " teambar=1:45,2:45,3:45,5:30,6:15 " + RAIL + " railcut=12:50 raildetrain=240", 27),
    "RB_both": (LIGHT + " teambar=1:45,2:45,3:45,5:30,6:15 " + RAIL + " railbridgeobjs=1 railcut=12:50 raildetrain=240", 27),
    # round 28: round 27 again with the rail bridge objectives player-only (no AI strikes) and not standing in for road bridges
    "RP_0": (LIGHT + " teambar=1:45,2:45,3:45,5:30,6:15 " + RAIL, 28),
    "RP_obj": (LIGHT + " teambar=1:45,2:45,3:45,5:30,6:15 " + RAIL + " railbridgeobjs=1", 28),
    "RP_pen": (LIGHT + " teambar=1:45,2:45,3:45,5:30,6:15 " + RAIL + " railcut=12:50 raildetrain=240", 28),
    "RP_both": (LIGHT + " teambar=1:45,2:45,3:45,5:30,6:15 " + RAIL + " railbridgeobjs=1 railcut=12:50 raildetrain=240", 28),
    # round 29: back to basics -- the North's supply production. Base = the h_bar campaign as campsim runs it (stores do not
    # produce, DataRateModSup 0.75); "game" = as the installed game produces (stores produce, Falcon4.AII DataRateModSup 1.5)
    "PC_0": ("", 29),
    "PC_game": ("noprod=0 prodsup=1.5", 29),
    "PC_75": ("prodcut=6:75", 29),
    "PC_50": ("prodcut=6:50", 29),
    "PC_25": ("prodcut=6:25", 29),
    # round 30: simple levers to have the North low on supply by the time China joins (h_bar; base = PC_0 of round 29)
    "SL_s50": ("startsup=6:50", 30),                         # North battalions start the war at 50% supply
    "SL_s30": ("startsup=6:30", 30),
    "SL_u3": ("useteam=6:3", 30),                            # North battalions use 3x the supply moving and firing
    "SL_s50p50": ("startsup=6:50 prodcut=6:50", 30),         # start at 50% and make half the supply
    "SL_s30p50": ("startsup=6:30 prodcut=6:50", 30),
    "SL_port": ("portcargo=24", 30),                         # ports produce only after a tanker/cargo ship docks
    # round 31: (a) every DPRK power station (power + nuclear plants) knocked out at hour 0 -- held down / struck once then repaired;
    # (b) initiative weighted by supply -- overall (1) or overall x the front's mean (2), alone and with a 50% start
    "PW_hold": ("blow=grid:6:hold blowat=0", 31),
    "PW_once": ("blow=grid:6:once blowat=0", 31),
    "IS_1": ("initsupply=1", 31),
    "IS_2": ("initsupply=2", 31),
    "IS_2_s50": ("initsupply=2 startsup=6:50", 31),
    # round 32: settle the start level -- North battalions at 60% / 70% (50% and 30% are round 30)
    "SL_s60": ("startsup=6:60", 32),
    "SL_s70": ("startsup=6:70", 32),
    # round 33: confirmation on fresh seeds (TRIAL_SEEDS=7..12), one build: base, the North at 50%, and that plus its power held down
    "CF2_0": ("", 33),
    "CF2_s50": ("startsup=6:50", 33),
    "CF2_s50pw": ("startsup=6:50 blow=grid:6:hold blowat=0", 33),
}


def runname(arm, seed):
    save, th, _, _ = camp(arm)
    return "%s%s-sx-%s-s%d" % (save, "-" + th if th else "", arm, seed)


def frames(path):
    out = []
    for l in open(path):
        if l.startswith('{"t"'):
            out.append(json.loads(l))
    return out


def edge_supply(f, teams, north):
    """mean supply of the team's battalions within 40 km of its forward edge (Blue: the northernmost; the North: the southernmost)"""
    bns = [u for u in f["u"] if u[2] in teams and u[1] == 0]
    if not bns:
        return None
    ys = sorted(u[4] for u in bns)
    edge = sum(ys[-10:]) / len(ys[-10:]) if north else sum(ys[:10]) / len(ys[:10])
    near = [u[11] for u in bns if abs(u[4] - edge) <= 40]
    return round(sum(near) / len(near), 1) if near else None


def analyse(arm, seed):
    base = os.path.join(HERE, "runs", runname(arm, seed))
    fr = frames(base + ".jsonl")
    log = open(base + ".log", errors="replace").read()
    ev = {int(a): int(c) / 60.0 for a, c in re.findall(r"EVENT (\d+) FIRED at min (\d+)", log)}
    res = re.findall(r"RESULT seed=\d+ endgame=(\d+) engine=\d+ campaign_h=([\d.]+)", log)
    m = {"arm": arm, "seed": seed, "endgame": int(res[-1][0]) if res else None, "end_h": float(res[-1][1]) if res else round(fr[-1]["t"] / 60, 1)}
    m["run"] = runname(arm, seed)
    m["blue_win"] = m["endgame"] == camp(arm)[3]
    share = lambda f: 100.0 * f["s"][2][4] / max(1, f["s"][2][4] + f["s"][6][4] + f["s"][5][4])
    m["share_end"] = round(share(fr[-1]), 1)
    peak = max(fr, key=lambda f: f["s"][2][4])
    after = [f for f in fr if f["t"] >= peak["t"]]
    m["blue_peak_objs"] = peak["s"][2][4]
    m["blue_peak_h"] = round(peak["t"] / 60)
    m["blue_drop_from_peak"] = peak["s"][2][4] - min(f["s"][2][4] for f in after)
    m["blue_objs_end"] = fr[-1]["s"][2][4]
    j = ev.get(11)
    m["china_in_h"] = round(j) if j else None
    if j:
        aj = [f for f in fr if f["t"] / 60 >= j]
        if aj:
            m["blue_off_after_join"] = round(100 * sum(1 for f in aj if f["a"][2][0] == 4) / len(aj))
            m["blue_init_after_join"] = round(sum(f["a"][2][3] for f in aj) / len(aj), 1)
    for d in (3, 5, 7):
        f = min(fr, key=lambda q: abs(q["t"] / 60 - d * 24))
        if abs(f["t"] / 60 - d * 24) < 2:
            m["blue_spear_sup_d%d" % d] = edge_supply(f, (1, 2, 3), True)
            m["north_front_sup_d%d" % d] = edge_supply(f, (5, 6), False)
            m["blue_low_sup_d%d" % d] = sum(1 for u in f["u"] if u[2] in (1, 2, 3) and u[1] == 0 and u[11] < 50)
    sx = re.findall(r"SXDIAG h=\d+ team (\d+) searches (\d+), no source in reach (\d+), refused (\d+), mean links ([\d.]+)", log)
    for t in ("2", "6"):
        last = [x for x in sx if x[0] == t]
        if last:
            m["sx_t%s" % t] = [int(last[-1][1]), int(last[-1][2]), int(last[-1][3]), float(last[-1][4])]
    m["blue_objs_daily"] = [min(fr, key=lambda q: abs(q["t"] / 60 - h))["s"][2][4] for h in range(0, int(fr[-1]["t"] / 60) + 1, 24)]
    m["north_front_daily"] = [edge_supply(min(fr, key=lambda q: abs(q["t"] / 60 - h)), (5, 6), False) for h in range(0, int(fr[-1]["t"] / 60) + 1, 24)]
    m["blue_spear_daily"] = [edge_supply(min(fr, key=lambda q: abs(q["t"] / 60 - h)), (1, 2, 3), True) for h in range(0, int(fr[-1]["t"] / 60) + 1, 24)]
    m["sig"] = hashlib.md5(json.dumps(fr[-1]["u"]).encode()).hexdigest()[:8]
    return m


def run_one(arm, seed):
    with lock:
        if os.path.exists(RES) and any(json.loads(l)["arm"] == arm and json.loads(l)["seed"] == seed for l in open(RES)):
            return
    knobs = ARMS[arm][0]
    save, th, base, _ = camp(arm)
    name = runname(arm, seed)
    logp = os.path.join(HERE, "runs", name + ".log")
    cmd = [sys.executable, os.path.join(HERE, "run_batch.py"), "--save", save, "--days", "10", "--seeds", str(seed), "--jobs", "1",
           "--tag", "sx-" + arm] + (["--theater", th] if th else []) + ["--set"] + (base + " " + knobs).split()
    t0 = time.time()
    with launch:  # one startup at a time: release once this run is in its tick loop
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=HERE)
        time.sleep(1)
        while p.poll() is None:
            try:
                if os.path.getmtime(logp) >= t0 and "campsim: save=" in open(logp, errors="replace").read():
                    break
            except OSError:
                pass
            time.sleep(0.5)
        time.sleep(1)
    out = p.communicate()[0]
    try:
        m = analyse(arm, seed)
    except Exception as ex:
        m = {"arm": arm, "seed": seed, "error": str(ex), "tail": out[-300:]}
    m["wall_s"] = round(time.time() - t0)
    with lock:
        os.makedirs(OUT, exist_ok=True)
        open(RES, "a").write(json.dumps(m) + "\n")
    print("done %-28s end %s h%s share %s" % (name, m.get("endgame"), m.get("end_h"), m.get("share_end", m.get("error"))), flush=True)


def load():
    seen = {}
    if os.path.exists(RES):
        for l in open(RES):
            r = json.loads(l)
            if "error" not in r:
                seen[(r["arm"], r["seed"])] = r
    return seen


def f0(v):
    return "-" if v is None else ("%.0f" % v if isinstance(v, (int, float)) else str(v))


def report(rnd=None):
    rows = load()
    print("arm            seed | end      share(d vs base) peak@h drop | China in, Blue off%/init after | Blue spearhead supply d3/d5/d7 (low<50 d5) | North front d3/d5/d7 | SX t2 [search, out of reach, refused, links]")
    for arm, (knobs, r, *_) in ARMS.items():
        if rnd and r != rnd:
            continue
        for s in SEEDS:
            x = rows.get((arm, s))
            if not x:
                continue
            b = rows.get(("base", s))
            d = (x["share_end"] - b["share_end"]) if b else None
            same = " =base" if b and arm != "base" and b.get("sig") == x.get("sig") else ""
            print("%-14s %d | %s@%-4s %4.1f(%+5.1f) %4d@%-3d %3d | %3s, %3s/%4s | %s/%s/%s (%s) | %s/%s/%s | %s%s" % (
                arm, s, x.get("endgame"), f0(x.get("end_h")), x["share_end"], d if d is not None else 0, x["blue_peak_objs"], x["blue_peak_h"],
                x["blue_drop_from_peak"], f0(x.get("china_in_h")), f0(x.get("blue_off_after_join")), f0(x.get("blue_init_after_join")),
                f0(x.get("blue_spear_sup_d3")), f0(x.get("blue_spear_sup_d5")), f0(x.get("blue_spear_sup_d7")), f0(x.get("blue_low_sup_d5")),
                f0(x.get("north_front_sup_d3")), f0(x.get("north_front_sup_d5")), f0(x.get("north_front_sup_d7")), x.get("sx_t2", "-"), same))


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "report"
    rnd = int(sys.argv[2]) if len(sys.argv) > 2 else None
    os.makedirs(OUT, exist_ok=True)
    if cmd == "run":
        items = [(a, s) for s in SEEDS for a, (_, r, *_) in ARMS.items() if not rnd or r == rnd]
        print("queue: %d runs, %d at a time" % (len(items), PAR), flush=True)
        with ThreadPoolExecutor(max_workers=PAR) as ex:
            list(ex.map(lambda a: run_one(*a), items))
    elif cmd == "reanalyse":   # recompute the metrics of finished runs (results.jsonl rewritten)
        rows = [json.loads(l) for l in open(RES)]
        out = []
        for r in rows:
            try:
                m = analyse(r["arm"], r["seed"])
                m["wall_s"] = r.get("wall_s")
                out.append(m)
            except Exception:
                out.append(r)
        open(RES, "w").write("".join(json.dumps(m) + "\n" for m in out))
        print("reanalysed", len(out))
    elif cmd == "daily":       # Blue objectives per day, per arm and seed
        rows = load()
        for arm, (knobs, r, *_) in ARMS.items():
            if rnd and r != rnd:
                continue
            for s in SEEDS:
                x = rows.get((arm, s))
                if x and x.get("blue_objs_daily"):
                    print("%-14s %d objs %s | spear %s | north front %s" % (arm, s, x["blue_objs_daily"], x.get("blue_spear_daily"), x.get("north_front_daily")))
    else:
        report(rnd)


if __name__ == "__main__":
    main()
