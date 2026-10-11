"""Build the FFViper HUD stroke font atlas at 3x from BMS's Hud_Font_d atlas.

The cockpit displays draw glyph quads magnified (render2d.cpp ScreenText leaves the UVs alone), so a
256x256 atlas gets bilinear-smeared. BMS draws the same font ~1 atlas pixel per screen pixel. This
rebuilds the atlas at SCALE x the resolution: smooth upscale, threshold back to 1-bit (rounded corners,
no staircase), then a symmetric dilation for stroke weight. The .rct stays in the original 256 layout --
ReadFontMetrics divides by 256 to get UVs, and UVs are fractions, so a bigger atlas with the same
layout needs no loader change.

Usage:
    python make_hud_font.py <Hud_Font_d.gif> <Hud_Font_d.rct> <out_dir> [--lead N] [--grow N]

Writes <out_dir>/hud_font.gif and hud_font.rct. Needs numpy and ffmpeg on PATH (or FFMPEG env var).
Neither BMS file is stored in this repo; point it at your own BMS install.
"""
import argparse
import os
import subprocess
import tempfile

import numpy as np

SCALE = 3
BASE = 256


def ffmpeg():
    return os.environ.get("FFMPEG", "ffmpeg")


def run(args):
    subprocess.run([ffmpeg(), "-y", "-loglevel", "error"] + args, check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gif")
    ap.add_argument("rct")
    ap.add_argument("out_dir")
    ap.add_argument("--lead", type=int, default=1, help="extra advance per glyph, in base (256) pixels")
    ap.add_argument("--grow", type=int, default=1, help="dilation radius at the hi-res scale (1 = +2 px stroke)")
    a = ap.parse_args()

    os.makedirs(a.out_dir, exist_ok=True)
    big = BASE * SCALE
    with tempfile.TemporaryDirectory() as t:
        raw_in = os.path.join(t, "in.raw")
        raw_big = os.path.join(t, "big.raw")
        raw_out = os.path.join(t, "out.raw")

        run(["-i", a.gif, "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "gray", raw_in])
        run(["-f", "rawvideo", "-pix_fmt", "gray", "-s", "%dx%d" % (BASE, BASE), "-i", raw_in,
             "-vf", "scale=%d:%d:flags=bicubic" % (big, big), "-f", "rawvideo", "-pix_fmt", "gray", raw_big])

        m = np.fromfile(raw_big, np.uint8).reshape(big, big) > 127

        for _ in range(a.grow):
            g = m.copy()
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    g |= np.roll(np.roll(m, dy, 0), dx, 1)
            m = g

        (m.astype(np.uint8) * 255).tofile(raw_out)
        # Same 2-colour palette convention as the stock atlases: index 0 black (the chroma key), rest white.
        run(["-f", "rawvideo", "-pix_fmt", "gray", "-s", "%dx%d" % (big, big), "-i", raw_out,
             "-vf", "split[a][b];[a]palettegen=max_colors=2:reserve_transparent=0[p];[b][p]paletteuse=dither=none",
             "-frames:v", "1", os.path.join(a.out_dir, "hud_font.gif")])

    lines = []
    for ln in open(a.rct).read().splitlines():
        f = ln.split()
        if len(f) == 7:
            f[5] = str(int(f[5]) + a.lead)
            ln = " ".join(f)
        lines.append(ln)
    open(os.path.join(a.out_dir, "hud_font.rct"), "w", newline="\r\n").write("\n".join(lines) + "\n")
    print("wrote", a.out_dir, "(%dx%d atlas)" % (big, big))


if __name__ == "__main__":
    main()
