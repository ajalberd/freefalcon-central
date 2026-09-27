#!/usr/bin/env python3
"""AI-upscale terrain tiles into a JSGME mod (never touches the install's own tiles).

Pipeline, per day tile:
  1. read the DDS (DXT3/5 converted to DXT1 losslessly by bms_tiles_mod), decode the top level;
  2. reflect-pad 16 px so the model sees context instead of a hard border, run a 4x spandrel model
     (any OpenModelDB .pth/.safetensors), crop the pad back off;
  3. resize to --size with an antialiased area filter (256 -> 1024 -> 512 is a 2x gain with the
     model's detail; a 512 source is detail-enhanced at the same size);
  4. back-project: nudge the result until its downsample matches the source tile. That keeps every
     colour -- and above all every EDGE, where a tile meets its neighbours -- where the original art
     put it, so the model adds texture but cannot move a coastline or open a seam;
  5. encode DXT1 with NVTT 3 (nvcompress -bc1 -production), full mip chain.

Night tiles (*N.dds) are copied unchanged: the loader sizes them separately from the day tile.

WHY 512 BY DEFAULT: TextureHandle::Load (graphics/texture/tex.cpp) builds the CPU mip chain the terrain
needs only for tiles <= 512 px; a 1024 tile is uploaded single-mip and shimmers at range. Larger sizes
need that gate (or a BC mip-chain upload) changed first.

Runs in its own venv (torch CUDA + spandrel), see WIP-NOTES "Terrain tile upscaling":
    C:/Users/Andrew/.ffupscale/venv/Scripts/python.exe tile_upscale.py
        --src  "C:/FreeFalcon6/MODS/BMS 4.32 Korea Tiles/terrdata/korea/texture/texture"
        --model C:/Users/Andrew/.ffupscale/models/4xNomos2_realplksr_dysample.pth
        --out  "C:/FreeFalcon6/MODS/Upscaled Korea Tiles/terrdata/korea/texture/texture"
        [--names HCOST043,HFARMD99 | --names-file list.txt] [--size 512] [--preview DIR]

--preview also writes each result as PNG (full model resolution, before the resize) for comparisons.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

import numpy as np
import torch
import torch.nn.functional as F

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bms_tiles_mod as bm  # noqa: E402
import tilesurvey as ts  # noqa: E402

NVCOMPRESS = r"C:\Users\Andrew\.ffupscale\nvtt\nvcompress.exe"
PAD = 16


def read_tile(path):
    """-> float32 (h, w, 3) in 0..1 from any DXT1/3/5 tile."""
    data, _ = bm.convert_to_dxt1(open(path, "rb").read())
    h, w = struct.unpack_from("<II", data, 12)
    return ts.decode_dxt1(data[128:], w, h).astype(np.float32) / 255.0


def load_model(path, device):
    from spandrel import ModelLoader
    desc = ModelLoader().load_from_file(path)
    if desc.scale != 4:
        raise SystemExit("%s is %dx; this tool expects a 4x model" % (path, desc.scale))
    model = desc.model.eval().to(device)
    half = bool(getattr(desc, "supports_half", False)) and device.type == "cuda"
    if half:
        model = model.half()
    print("model: %s (%s, fp16=%s)" % (os.path.basename(path), desc.architecture.name, half))
    return model, half


@torch.inference_mode()
def upscale4(model, half, img, device, chunk=320):
    """(h, w, 3) -> (4h, 4w, 3) tensor on device; reflect-padded; chunked with overlap for big inputs."""
    x = torch.from_numpy(img).permute(2, 0, 1)[None].to(device)
    x = F.pad(x, (PAD, PAD, PAD, PAD), mode="reflect")
    if half:
        x = x.half()
    _, _, H, W = x.shape
    if max(H, W) <= chunk:
        y = model(x)
    else:
        y = torch.zeros((1, 3, H * 4, W * 4), device=device, dtype=x.dtype)
        step = chunk - 2 * PAD
        for top in range(0, H - 2 * PAD, step):
            for left in range(0, W - 2 * PAD, step):
                t0, l0 = min(top, max(0, H - chunk)), min(left, max(0, W - chunk))
                part = model(x[:, :, t0:t0 + chunk, l0:l0 + chunk])
                # keep the centre of each chunk (its own PAD margin is context only)
                it, il = (PAD if t0 > 0 else 0), (PAD if l0 > 0 else 0)
                y[:, :, (t0 + it) * 4:(t0 + chunk) * 4, (l0 + il) * 4:(l0 + chunk) * 4] = \
                    part[:, :, it * 4:, il * 4:]
    y = y.float()[:, :, PAD * 4:-PAD * 4, PAD * 4:-PAD * 4]
    return y.clamp(0, 1)


def back_project(y, src, size, iters=3):
    """Resize y to size x size, then correct it so area-downsampling to the source size gives the source."""
    s = torch.from_numpy(src).permute(2, 0, 1)[None].to(y.device)
    out = F.interpolate(y, size=(size, size), mode="bicubic", antialias=True, align_corners=False)
    if size == s.shape[-1]:
        return out.clamp(0, 1)  # same size: nothing to project against beyond the model's own fidelity
    for _ in range(iters):
        down = F.interpolate(out, size=s.shape[-2:], mode="area")
        out = (out + F.interpolate(s - down, size=(size, size), mode="bicubic", align_corners=False)).clamp(0, 1)
    return out


def to_png(t, path):
    a = (t[0].permute(1, 2, 0).cpu().numpy() * 255.0 + 0.5).clip(0, 255).astype(np.uint8)
    ts.write_png(path, a)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", required=True, help="source tile folder (DDS)")
    ap.add_argument("--model", required=True)
    ap.add_argument("--out", required=True, help="output tile folder (inside a MODS/ tree)")
    ap.add_argument("--size", type=int, default=512)
    ap.add_argument("--names", help="comma list of tile base names (default: every day tile in --src)")
    ap.add_argument("--names-file")
    ap.add_argument("--preview", help="also write full-resolution PNGs here")
    ap.add_argument("--copy-night", action="store_true", help="copy the matching *N.dds night tiles")
    args = ap.parse_args(argv)

    if os.path.normcase(os.path.abspath(args.out)).startswith(os.path.normcase(os.path.abspath(args.src))):
        raise SystemExit("--out must not be inside --src")
    src = {n.lower(): n for n in os.listdir(args.src) if n.lower().endswith(".dds")}
    if args.names or args.names_file:
        want = (args.names.split(",") if args.names else
                [l.strip() for l in open(args.names_file) if l.strip()])
        day = [src[w.split(".")[0].lower() + ".dds"] for w in want if w.split(".")[0].lower() + ".dds" in src]
    else:
        day = [n for k, n in src.items() if not (k.endswith("n.dds") and k[:-5] + ".dds" in src)]
    day = sorted(set(day))

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    model, half = load_model(args.model, device)
    os.makedirs(args.out, exist_ok=True)
    if args.preview:
        os.makedirs(args.preview, exist_ok=True)

    tmp = tempfile.mkdtemp(prefix="tileup_")
    t0 = time.time()
    try:
        for i, n in enumerate(day):
            img = read_tile(os.path.join(args.src, n))
            if img.shape[1] >= 2 * args.size:
                # already at least twice the target: the model would only be averaged away again
                y = torch.from_numpy(img).permute(2, 0, 1)[None].to(device)
            else:
                y = upscale4(model, half, img, device)
            if args.preview:
                to_png(y, os.path.join(args.preview, n[:-4] + ".png"))
            out = back_project(y, img, min(args.size, y.shape[-1]))
            to_png(out, os.path.join(tmp, n[:-4] + ".png"))
            if (i + 1) % 50 == 0 or i + 1 == len(day):
                print("  %d/%d tiles, %.1fs" % (i + 1, len(day), time.time() - t0), flush=True)

        # one nvcompress call over the folder (it keeps base names, writes .dds)
        r = subprocess.run([NVCOMPRESS, "-silent", "-color", "-noalpha", "-bc1", "-production",
                            "-mipfilter", "kaiser", tmp, args.out], capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit("nvcompress failed: %s %s" % (r.stdout[-2000:], r.stderr[-2000:]))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # nvcompress writes lower-case .dds names from the PNG names -- restore the source spelling, check headers
    made = {m.lower(): m for m in os.listdir(args.out)}
    for n in day:
        m = made.get(n.lower())
        if m is None:
            raise SystemExit("missing output for " + n)
        if m != n:
            os.replace(os.path.join(args.out, m), os.path.join(args.out, n))
        d = open(os.path.join(args.out, n), "rb").read(128)
        if d[84:88] != b"DXT1":
            raise SystemExit("%s: nvcompress wrote %r, not DXT1" % (n, d[84:88]))

    if args.copy_night:
        for n in day:
            night = src.get(n[:-4].lower() + "n.dds")
            if night:
                shutil.copy2(os.path.join(args.src, night), os.path.join(args.out, night))
    print("wrote %d tiles to %s in %.0fs" % (len(day), args.out, time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
