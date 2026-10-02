"""Reproduce the SimToGrid round-trip bug (src/campaign/camplib/find.cpp).

GridToSim(x) = x * FEET_PER_KM + FEET_PER_KM / 2      (centre of the cell, single precision)
old SimToGrid = FloatToInt32(sim / FEET_PER_KM)        (round to nearest, ties to even)

The quotient is exactly x + 0.5 up to float error, so whether it rounded up was a coin toss
per coordinate. Needs numpy only.
"""
import numpy as np

f = np.float32
G = f(3279.98)            # FEET_PER_KM, graphics/include/constant.h
OFF = f(G / f(2.0))       # OffsetToMiddle


def grid_to_sim(x):
    return f(f(f(x) * G) + OFF)


def old_sim_to_grid(s):
    return int(np.rint(f(s / G)))


def new_sim_to_grid(s):
    return int(np.floor(f(s / G)))


bad_old = [x for x in range(1024) if old_sim_to_grid(grid_to_sim(x)) != x]
bad_new = [x for x in range(1024) if new_sim_to_grid(grid_to_sim(x)) != x]
print("old: %d of 1024 coordinates read back wrong (all +1: %s)" % (
    len(bad_old), all(old_sim_to_grid(grid_to_sim(x)) == x + 1 for x in bad_old)))
print("new: %d of 1024 coordinates read back wrong" % len(bad_new))
print("first wrong coordinates:", bad_old[:20])
