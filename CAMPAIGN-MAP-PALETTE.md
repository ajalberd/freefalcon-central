# The campaign map's palette

Reconnaissance, not a plan. Everything here was measured with
`tools/terrain/tilesurvey.py` against the shipped Korea data and the installed Israel
theater; re-run it rather than trusting these numbers for a third theater.

---

## The finding

**Israel's `Theater.map` colour table is byte-identical to Korea's.** All 256 entries, no
exceptions. Israel's `.map` is dated 2002; its terrain is 2011 — the author replaced the
ground and never touched the table.

So the campaign map paints desert through a temperate Korean palette: 118 green-dominant
entries against 55 warm ones. Israel's sand tiles average around `[247, 226, 203]`, and
the nearest entries are near-white greys:

| tile | share of theater | mean RGB | nearest table entry |
|---|---|---|---|
| `HDSRT015` | 33.8% | `[247, 226, 203]` | 61 = `[231, 231, 214]` |
| `Hdorr20d` | 6.7% | `[249, 228, 203]` | 62 = `[239, 239, 222]` |
| `HDYNE014` | 4.6% | `[226, 205, 183]` | 71 = `[214, 214, 181]` |

Quantisation error, tile pixels into the theater's own table:

| | mean (of 441) | within 16 | within 32 |
|---|---|---|---|
| Korea | **9.6** | 86.4% | 99.4% |
| Israel | **19.5** | 23.5% | 99.7% |

The zoomed-in map comes out washed pale green. **The base map has the same problem and
always has** — one pixel per post is coarse enough that nobody looked. The tile detail
layer did not introduce this; it made it visible.

The tiles themselves are fine and resolve correctly — Israel's own desert imagery, out of
Israel's own `texture.bin` (259 sets, 2,133 tiles against Korea's 113 and 1,087). The
problem is entirely the table they are forced through.

---

## Fixed first: red and blue were exchanged

Before any of the above is judged, note that the map was also drawing with **red and blue
swapped**, and had been since it was first built from terrain.

`UI95_RGB24Bit` takes a Win32 `COLORREF` — it reads red from bits 3–7, green from 11–15
and blue from 19–23, i.e. `0x00BBGGRR` with **red in the low byte**. Every other caller
hands it one (`PreparePalette(RGB(255, 40, 40))`). `BuildTerrainMapImage` packed
`(r << 16) | (g << 8) | b` instead, so:

- the sea's `[16, 41, 49]` displayed as `[49, 41, 16]` — **brown**, which is what the
  campaign map's water has looked like all along, and
- desert tan displayed as pale blue, which is most of why Israel read as snow rather than
  merely washed out.

Now `UI95_RGB24Bit(RGB(r, g, b))`. It affects Korea as much as Israel; the post-colour map
was coarse enough that it read as an odd palette rather than as a bug, and only became
obvious once real ground imagery went through the same table.

## The banding in open water is the same problem

Korea's open sea is two tiles — `HCOST00F` at 91% and `HCOST73F` at 9%, alternating on
tile-cell boundaries. Their means are `[16, 48, 64]` and `[15, 47, 63]`: **1.7 apart**, a
difference nobody could see. The table's nearest entries are 252 `[24, 49, 66]` and 250
`[16, 41, 49]` — **20.5 apart**.

So quantisation amplifies an invisible difference twelvefold and paints the sea in faint
bands. That is not an averaging artifact to be worked around; it is a sparse palette
turning a rounding decision into a visible step, and it is the clearest small example of
why the table needs replacing. A palette built from the theater's own tiles would put
both water tiles on the same entry, or on two genuinely adjacent ones.

Worth measuring after any palette change: the sea should go flat.

**This does not make the palette work unnecessary.** The quantisation figures above are
measured in RGB space and are unaffected by the packing — Israel's desert still lands on
`[231, 231, 214]`, a warm near-white, instead of a cool one. The ground still reads as
bleached. But judge the remaining problem against a corrected build, not against
screenshots taken before this.

---

## The constraint that shapes any fix

There is exactly **one palette**, and both layers share it.

`C_ScaleBitmap::PreparePalette` derives the sixteen blended overlay palettes from the
*base* image's palette, and the detail image copies that same table verbatim. That is
what keeps the Logistics layers and the FLOT line working over detail pixels. So:

- base image and detail image must agree on the palette, and
- a new palette means the base map's post colours — which are `ColorTable` **indices** —
  have to be remapped into it.

---

## Sketch, in the order it should be done

1. **Preview offline first.** Add a `--palette` mode to `tilesurvey.py` that builds a
   candidate palette and reports the same error table above, for Korea and Israel. If it
   does not move Israel's numbers a long way, stop — the engine work is not worth it.
2. **Choose the sample by area, not by tile count.** The top 50 texIDs cover 90.4% of
   Israel and 84.3% of Korea, so a few dozen decoded tiles describe a theater. Median-cut
   or k-means to 256.
3. **Keep the reserved entries honest.** Index 0 currently means "no colour data", and in
   practice means water — 96.6% of Israel's index-0 posts and 85.2% of Korea's reference
   the coast tile. Something still has to read as "unread block".
4. ~~**Remap the base map.**~~ **Done, and differently.** The base map no longer paints
   the post colour byte at all — `CampMapTileColors` (default on) paints every post from
   the average of the tile it sits on. That byte exists for untextured far terrain and
   describes a theater badly: unpopulated over water, and over Israel's desert it lands
   on the temperate table's greens, so the Negev and the Sinai came out grass-coloured.
   Averaging the tile also puts the base map in exactly the colour the detail layer
   resolves to, so nothing changes hue across the detail threshold. Costs one pass over
   each referenced tile's block headers at first build (~1,500 tiles for Israel, read
   whole rather than eight bytes at a time).

   So when the palette is rebuilt, there is **no `ColorTable` index left to remap** on the
   base map — both layers quantise tile colours through the same lookup, and repointing
   that lookup at a better palette fixes both at once. That is a much smaller change than
   this step originally described.
5. **Mind the ordering.** The palette wants a texID histogram, and the only pass over
   every post is the one that paints the map. Either histogram texIDs in a cheap pre-pass
   over the block data, or build the palette lazily on the first detail build and repaint
   the base image then.
6. **Fall back.** A theater whose tiles cannot be read keeps `ColorTable`, the same way
   the detail layer already declines.

Expect a small improvement for Korea and a large one for Israel.

---

## Two things Israel broke that are already fixed

Recorded here because they are the same class of assumption — "every theater is Korea".

**texID width varies.** Korea encodes res at bits 12–15 with nothing above, so its texIDs
fit in 16 bits (`0x2000`…`0x2706`). Israel widens the set field and pushes res to bits
16–19, so every one of its texIDs is `0x20000` or more. `CampMapDetailTileAverageIndex`
had `if (texID > 0xFFFF) return 0;` and a `new BYTE[0x10000]` table, so it bailed on every
Israeli post and the sea stayed pure white on exactly the theater that needed it most. Now
keyed on the `(set, tile)` pair — bounded at 4096 regardless of how wide the texID is, and
the more accurate key anyway, since two texIDs differing only in resolution bits name the
same file.

Worth knowing: `TextureDB::ExtractSet` is `(texID >> 4) & 0xFF`, eight bits. Israel
declares 259 sets but references only up to 248, so it works by luck. **A theater
referencing set ≥ 256 would silently draw the wrong tile**, in the stock engine as much as
in the map code.

**Nothing invalidated on a theater change.** `s_terrainMapTried` in `SetMapImage` was a
once-per-process static, and the detail layer's caches — tile name table, colour lookup,
decoded tiles, average-index table — inherited that. Loading a second theater in one
session swaps `FalconTerrainDataDir` underneath (`SetNewTheater`, `theaterdef.cpp`) and
none of it noticed, so the campaign map kept showing whichever theater was opened first.
The `(set, tile)` keys collide across theaters rather than missing, so it would have drawn
the wrong ground rather than nothing. Both now key on `FalconTerrainDataDir` and drop
everything when it moves.

The old base image is deliberately leaked on a theater change. `CreateOccupationMap` hangs
it off a `C_Resmgr` whose `Cleanup` also tears down the index the image is registered in,
and guessing wrong about that ownership is a double free in resource code — a poor trade
for a few MB on an event that happens when a player picks a different theater. Reusing the
buffer in place when the dimensions match is the tidy version, if it ever matters.

---

## Where the code is

- `BuildTerrainMapImage`, `SetMapImage`, `UpdateTerrainDetail` —
  `src/ui/src/campaign/cmap.cpp`
- `TileFileName`, `TileKey`, `BuildColorLut`, `ReleaseTheaterCaches`,
  `CampMapDetailTileAverageIndex` — `src/ui/src/campaign/cmapdetail.cpp`
- `PreparePalette`, `SetDetail`, `Draw` — `src/ui95/csclbmp.cpp`
- `TMap::LoadColorTable` — `src/graphics/terrain/tmap.cpp`
- `tools/terrain/tilesurvey.py` — every number above
