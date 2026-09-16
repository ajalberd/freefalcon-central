/***************************************************************************\
    CmapDetail.cpp
    Artscout - 2026

    Campaign map detail on zoom: draw the visible patch of map out of the GROUND
    TILES the sim flies over, instead of magnifying the one-pixel-per-post base map.

    Why this is possible at all
    --------------------------
    Every terrain post carries a `texID` alongside its colour byte, and at LOD 0 four
    posts across share one -- DiskblockToMemblock computes u as
    `((i << LOD) & 0x3) * 0.25`, so one texture spans four post-quads. The theater data
    bears that out exactly: over a sample of 16,384 four-by-four post groups, every
    single one carried a single shared texID. One tile therefore covers 4 x 819.995 ft
    = 3,280 ft of ground, and the tile behind it is 256x256 (sometimes 512x512) DXT1.
    That is 12.8 ft per pixel against the base map's 820 -- sixty-four times the linear
    detail, and it is already on disk.

    Why it can stay in the existing 8-bit pipeline
    ---------------------------------------------
    C_ScaleBitmap blends its overlay through sixteen palettes that PreparePalette
    derives from the BASE image's palette, so a truecolour detail layer would break
    every Logistics layer and the FLOT line. It does not have to be truecolour.
    Quantising decoded tile pixels into the theater's own 256-entry TMap::ColorTable --
    the very table the base map is painted with -- costs a mean RGB error of 9.6 out of
    441 across all 1,087 Korea tiles, with 86% of pixels inside 16. Measured, not
    assumed: `tools/terrain/tilesurvey.py` reports it for any theater. So the detail
    image is 8-bit, shares the base image's palette verbatim, and the whole
    ScaleUp8Overlay path is reused untouched.

    Why it is a stand-in rather than a bigger map
    --------------------------------------------
    At tile resolution Korea is 262,144 pixels square -- 68.7 gigapixels. It cannot be
    one bitmap, so the detail has to be built for the visible window only. But making
    the map image itself a window would change what "map pixel" means, and MapRect_,
    CenterX_, scale_, FEET_PER_PIXEL, the zoom clamps and every icon position are all
    expressed in whole-theater map pixels. So the base image, its size and its
    coordinate system are left exactly as they are, and this hands C_ScaleBitmap a
    separate image to blit in its place for the current source rect
    (C_ScaleBitmap::SetDetail -> O_Output::Blend4BitDetail). Nothing re-projects,
    because nothing moves.

    Reading the files directly, as BuildTerrainMapImage already does for the posts: the
    texture bank hands out GPU handles and only keeps CPU pixels on its non-DDS path,
    and in DDS mode it drops the palette entirely. A 2D blit needs neither of those.
\***************************************************************************/
#include <ciso646>
#include <windows.h>
#include <stdio.h>
#include <string.h>
// chandler.h before ui95_ext.h, the same order cmap.cpp uses: the UI95 headers are not
// self-contained and pull their base classes in through it.
#include "chandler.h"
#include "ui95_ext.h"
#include "tmap.h"
#include "ttypes.h"
#include "fflog.h"

extern char FalconTerrainDataDir[];
extern IMAGE_RSC *CreateOccupationMap(long ID, long w, long h, long palsize);

// This module's own surface, declared up front so the definitions below can call each
// other in whatever order reads best.
bool CampMapDetailBeginGrid(long mapW, long mapH, int lod);
void CampMapDetailSetCell(long cellX, long cellY, DWORD texID);
long CampMapDetailPostsPerCell();
bool CampMapDetailHaveGrid();
void CampMapDetailReset();
void CampMapDetailOverlayChanged();
bool CampMapDetailBuild(const UI95_RECT *mapRect, long destW, long destH,
                        const BYTE *baseOverlay, long baseW, long baseH,
                        const WORD *basePalette);
IMAGE_RSC *CampMapDetailImage();
BYTE *CampMapDetailOverlay();
long *CampMapDetailRows();
long *CampMapDetailCols();
BYTE CampMapDetailTileAverageIndex(DWORD texID);

extern bool g_bCampMapDetail;
extern int g_nCampMapDetailTiles;
extern bool g_bLogCampMapDetail;
extern bool g_bCampMapFlipNS, g_bCampMapFlipEW;

// Every tile is normalised to this on load. 256 is the smaller of the two sizes the
// Korea data ships and is far more than the zoom floor can show: the closest zoom puts
// 64 posts across the window, which even on a 2048 px window is 32 detail pixels per
// post, against the 64 a 256 tile supplies. A 512 tile is sampled down by two.
#define DETAIL_TILE 256
#define DETAIL_TILE_PIXELS (DETAIL_TILE * DETAIL_TILE)

// Cap on the detail image. It is sized to the map window, so this only ever bites on a
// very large one; past it the base map is used, which is the correct fallback anyway.
#define DETAIL_MAX_PIXELS (4 * 1024 * 1024)

// Most detail pixels per post. Beyond this the tile has no more texels to give.
#define DETAIL_MAX_SUBDIV 64

namespace
{

/*--------------------------------------------------------------- the tile grid ---*/

// One texID per tile cell, in MAP-IMAGE space -- already carrying whatever
// CampMapFlipNS / CampMapFlipEW did to the base image, so a cell index here lines up
// with the base map pixel above it with no further arithmetic.
DWORD *s_cells = NULL;
long s_cellsWide = 0;
long s_cellsHigh = 0;
long s_postsPerCell = 0;   // 4 at LOD 0, 2 at LOD 1
bool s_flipNS = false;     // the flips the grid was captured under
bool s_flipEW = false;

/*----------------------------------------------------------- colour quantisation --*/

// RGB555 -> nearest ColorTable index. 32 KB, built once from the theater's own table,
// then one lookup per decoded texel. The base map image is painted from the same table,
// so an index here means the same colour there.
BYTE *s_lut = NULL;

bool BuildColorLut()
{
    if (s_lut)
        return true;

    s_lut = new BYTE[32768];

    if (not s_lut)
        return false;

    long ctr[256], ctg[256], ctb[256];

    for (int i = 0; i < 256; i++)
    {
        const Tcolor &c = TheMap.ColorTable[i];
        long r = (long)(c.r * 255.0f), g = (long)(c.g * 255.0f),
             b = (long)(c.b * 255.0f);
        ctr[i] = (r < 0) ? 0 : (r > 255) ? 255 : r;
        ctg[i] = (g < 0) ? 0 : (g > 255) ? 255 : g;
        ctb[i] = (b < 0) ? 0 : (b > 255) ? 255 : b;
    }

    for (long k = 0; k < 32768; k++)
    {
        // The 5-bit channels expand the same way the DXT1 decode below expands them, so
        // the table is indexed by exactly the values that will be looked up in it.
        const long r = (((k >> 10) bitand 0x1F) * 255 + 15) / 31;
        const long g = (((k >> 5) bitand 0x1F) * 255 + 15) / 31;
        const long b = ((k bitand 0x1F) * 255 + 15) / 31;

        long best = 0, bestD = 0x7FFFFFFF;

        for (int i = 0; i < 256; i++)
        {
            const long dr = r - ctr[i], dg = g - ctg[i], db = b - ctb[i];
            const long d = dr * dr + dg * dg + db * db;

            if (d < bestD)
            {
                bestD = d;
                best = i;

                if (not d)
                    break;
            }
        }

        s_lut[k] = (BYTE)best;
    }

    return true;
}

inline BYTE Quantise(long r5, long g6, long b5)
{
    // g is six bits in DXT1's 565; the lookup table is 555, so drop the low bit.
    return s_lut[(r5 << 10) bitor ((g6 >> 1) << 5) bitor b5];
}

/*--------------------------------------------------------------- the tile cache --*/

struct DetailTile
{
    DWORD texID;
    DWORD lastUsed;
    BYTE *pixels;   // DETAIL_TILE x DETAIL_TILE ColorTable indices
    bool tried;     // a tile that failed to load is not retried every rebuild
};

DetailTile *s_tiles = NULL;
long s_tileCount = 0;
DWORD s_tileClock = 0;

// texture.bin's set/tile table, flattened to the filename each texID names.
// TextureDB holds the same thing but keeps it protected, and in DDS mode it has thrown
// the palette away by the time anyone could ask.
char (*s_tileNames)[20] = NULL;
long *s_setFirstTile = NULL;
long *s_setTileCount = NULL;
long s_numSets = 0;

bool LoadTileTable()
{
    if (s_tileNames)
        return true;

    char fn[MAX_PATH];
    sprintf(fn, "%s/texture/texture.bin", FalconTerrainDataDir);
    FILE *fp = fopen(fn, "rb");

    if (not fp)
        return false;

    long numSets = 0, totalTiles = 0;

    if (fread(&numSets, sizeof(long), 1, fp) not_eq 1 or
        fread(&totalTiles, sizeof(long), 1, fp) not_eq 1 or numSets < 1 or
        numSets > 4096 or totalTiles < 1 or totalTiles > 65536)
    {
        fclose(fp);
        return false;
    }

    s_tileNames = (char (*)[20]) new char[totalTiles][20];
    s_setFirstTile = new long[numSets];
    s_setTileCount = new long[numSets];

    if (not s_tileNames or not s_setFirstTile or not s_setTileCount)
    {
        fclose(fp);
        return false;
    }

    long written = 0;
    bool ok = true;

    for (long i = 0; i < numSets and ok; i++)
    {
        long nTiles = 0;
        BYTE terrainType = 0;

        if (fread(&nTiles, sizeof(long), 1, fp) not_eq 1 or
            fread(&terrainType, 1, 1, fp) not_eq 1 or nTiles < 0 or
            written + nTiles > totalTiles)
        {
            ok = false;
            break;
        }

        s_setFirstTile[i] = written;
        s_setTileCount[i] = nTiles;

        for (long j = 0; j < nTiles and ok; j++)
        {
            long nAreas = 0, nPaths = 0;

            if (fread(s_tileNames[written], 1, 20, fp) not_eq 20 or
                fread(&nAreas, sizeof(long), 1, fp) not_eq 1 or
                fread(&nPaths, sizeof(long), 1, fp) not_eq 1 or nAreas < 0 or
                nPaths < 0)
            {
                ok = false;
                break;
            }

            // TexArea is 16 bytes, TexPath 24; neither is needed to find the bitmap.
            if (fseek(fp, nAreas * 16 + nPaths * 24, SEEK_CUR) not_eq 0)
            {
                ok = false;
                break;
            }

            written++;
        }
    }

    fclose(fp);

    if (not ok)
    {
        delete[] s_tileNames;
        s_tileNames = NULL;
        delete[] s_setFirstTile;
        s_setFirstTile = NULL;
        delete[] s_setTileCount;
        s_setTileCount = NULL;
        return false;
    }

    s_numSets = numSets;
    return true;
}

// texID layout is TextureDB's: tile in the low four bits, set in the next eight,
// resolution above that. Only the set and tile pick the file -- the resolution selects
// which of the H/M/L variants the sim streams, and the Korea data ships only H.
const char *TileFileName(DWORD texID)
{
    const long set = (texID >> 4) bitand 0xFF;
    const long tile = texID bitand 0xF;

    if (not s_tileNames or set >= s_numSets or tile >= s_setTileCount[set])
        return NULL;

    return s_tileNames[s_setFirstTile[set] + tile];
}

/*-------------------------------------------------------------------- DXT1 read --*/

// Decode one 4x4 DXT1 block straight into ColorTable indices. Alpha is irrelevant here:
// terrain tiles are opaque ground, and the one-bit-alpha variant of the block layout
// (c0 <= c1) only changes how the two interpolated colours are derived.
void DecodeDxt1Block(const BYTE *src, BYTE *dst, long stride)
{
    const WORD c0 = (WORD)(src[0] bitor (src[1] << 8));
    const WORD c1 = (WORD)(src[2] bitor (src[3] << 8));

    long r[4], g[4], b[4];
    r[0] = (c0 >> 11) bitand 0x1F;
    g[0] = (c0 >> 5) bitand 0x3F;
    b[0] = c0 bitand 0x1F;
    r[1] = (c1 >> 11) bitand 0x1F;
    g[1] = (c1 >> 5) bitand 0x3F;
    b[1] = c1 bitand 0x1F;

    if (c0 > c1)
    {
        r[2] = (2 * r[0] + r[1]) / 3;
        g[2] = (2 * g[0] + g[1]) / 3;
        b[2] = (2 * b[0] + b[1]) / 3;
        r[3] = (r[0] + 2 * r[1]) / 3;
        g[3] = (g[0] + 2 * g[1]) / 3;
        b[3] = (b[0] + 2 * b[1]) / 3;
    }
    else
    {
        r[2] = (r[0] + r[1]) / 2;
        g[2] = (g[0] + g[1]) / 2;
        b[2] = (b[0] + b[1]) / 2;
        r[3] = g[3] = b[3] = 0;
    }

    BYTE idx[4];

    for (int i = 0; i < 4; i++)
        idx[i] = Quantise(r[i], g[i], b[i]);

    const DWORD bits = (DWORD)src[4] bitor ((DWORD)src[5] << 8) bitor
                       ((DWORD)src[6] << 16) bitor ((DWORD)src[7] << 24);

    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++)
            dst[row * stride + col] =
                idx[(bits >> ((row * 4 + col) * 2)) bitand 3];
}

// Read a tile's .dds and expand it to DETAIL_TILE x DETAIL_TILE ColorTable indices.
// Only DXT1 is handled -- it is what every terrain tile in the shipped data is, and the
// caller falls back to the post-colour map for anything else rather than guessing.
BYTE *LoadTilePixels(DWORD texID)
{
    const char *name = TileFileName(texID);

    if (not name)
        return NULL;

    char base[64];
    strncpy(base, name, sizeof(base) - 1);
    base[sizeof(base) - 1] = 0;

    char *dot = strchr(base, '.');

    if (dot)
        *dot = 0;

    char fn[MAX_PATH];
    sprintf(fn, "%s/texture/texture/%s.dds", FalconTerrainDataDir, base);
    FILE *fp = fopen(fn, "rb");

    if (not fp)
        return NULL;

    BYTE head[128];

    if (fread(head, 1, 128, fp) not_eq 128 or memcmp(head, "DDS ", 4) not_eq 0)
    {
        fclose(fp);
        return NULL;
    }

    const DWORD h = *(const DWORD *)(head + 12);
    const DWORD w = *(const DWORD *)(head + 16);

    if (memcmp(head + 84, "DXT1", 4) not_eq 0 or w not_eq h or w < 4 or
        w > 2048 or (w bitand (w - 1)))
    {
        fclose(fp);
        return NULL;
    }

    const long blocks = (long)(w / 4);
    const size_t surface = (size_t)blocks * blocks * 8;
    BYTE *raw = new BYTE[surface];

    if (not raw)
    {
        fclose(fp);
        return NULL;
    }

    const size_t got = fread(raw, 1, surface, fp);
    fclose(fp);

    if (got not_eq surface)
    {
        delete[] raw;
        return NULL;
    }

    // Decode at the tile's own size, then take every Nth texel to reach DETAIL_TILE.
    // Both shipped sizes are powers of two, so the step is exact.
    BYTE *full = new BYTE[(size_t)w * w];

    if (not full)
    {
        delete[] raw;
        return NULL;
    }

    for (long by = 0; by < blocks; by++)
        for (long bx = 0; bx < blocks; bx++)
            DecodeDxt1Block(raw + ((size_t)by * blocks + bx) * 8,
                            full + ((size_t)by * 4) * w + bx * 4, (long)w);

    delete[] raw;

    if (w == DETAIL_TILE)
        return full;

    BYTE *out = new BYTE[DETAIL_TILE_PIXELS];

    if (not out)
    {
        delete[] full;
        return NULL;
    }

    if (w > DETAIL_TILE)
    {
        const long step = (long)w / DETAIL_TILE;

        for (long y = 0; y < DETAIL_TILE; y++)
            for (long x = 0; x < DETAIL_TILE; x++)
                out[y * DETAIL_TILE + x] = full[(size_t)(y * step) * w + x * step];
    }
    else
    {
        const long step = DETAIL_TILE / (long)w;

        for (long y = 0; y < DETAIL_TILE; y++)
            for (long x = 0; x < DETAIL_TILE; x++)
                out[y * DETAIL_TILE + x] = full[(size_t)(y / step) * w + x / step];
    }

    delete[] full;
    return out;
}

// Least-recently-used over a fixed pool. A handful of tiles carry most of the theater --
// one covers 49% of Korea's ground and the top two hundred cover 95% -- so even a small
// pool almost never evicts something that is about to be asked for again.
const BYTE *GetTile(DWORD texID)
{
    long freeSlot = -1, oldest = 0;

    for (long i = 0; i < s_tileCount; i++)
    {
        if (s_tiles[i].pixels or s_tiles[i].tried)
        {
            if (s_tiles[i].texID == texID)
            {
                s_tiles[i].lastUsed = ++s_tileClock;
                return s_tiles[i].pixels;
            }
        }
        else if (freeSlot < 0)
            freeSlot = i;

        if (s_tiles[i].lastUsed < s_tiles[oldest].lastUsed)
            oldest = i;
    }

    const long slot = (freeSlot >= 0) ? freeSlot : oldest;

    if (s_tiles[slot].pixels)
    {
        delete[] s_tiles[slot].pixels;
        s_tiles[slot].pixels = NULL;
    }

    s_tiles[slot].texID = texID;
    s_tiles[slot].lastUsed = ++s_tileClock;
    s_tiles[slot].pixels = LoadTilePixels(texID);
    s_tiles[slot].tried = true;

    return s_tiles[slot].pixels;
}

/*------------------------------------------------------------- the detail window -*/

// One image, allocated at the largest size the map window can ever ask for and then
// described smaller per build. Re-creating an IMAGE_RSC on every zoom step would mean
// tearing down the C_Resmgr that owns its pixels each time, and the size changes on
// every zoom step -- subdiv is derived from the zoom. The pixel buffer is the same
// memory either way, so only the header has to move.
IMAGE_RSC *s_image = NULL;
BYTE *s_overlay = NULL;
long *s_rows = NULL;
long *s_cols = NULL;
long s_imageCap = 0;    // pixels the image and overlay buffers can hold
long s_capW = 0, s_capH = 0;
long s_rampW = 0, s_rampH = 0;

// What the current buffers were built for, so a redraw that changes nothing rebuilds
// nothing. The overlay generation is bumped by the map whenever it restamps its layers.
UI95_RECT s_builtRect = {0, 0, 0, 0};
long s_builtSubdiv = 0;
long s_builtDestW = 0, s_builtDestH = 0;
DWORD s_builtOverlayGen = 0xFFFFFFFF;
DWORD s_overlayGen = 0;

// Per detail column: which cell it falls in, and which texel of that cell's tile.
long *s_colCell = NULL;
long *s_colTexel = NULL;
long *s_rowCell = NULL;
long *s_rowTexel = NULL;
long s_axisAlloc = 0;

bool EnsureAxis(long n)
{
    if (s_axisAlloc >= n)
        return true;

    delete[] s_colCell;
    delete[] s_colTexel;
    delete[] s_rowCell;
    delete[] s_rowTexel;
    s_colCell = new long[n];
    s_colTexel = new long[n];
    s_rowCell = new long[n];
    s_rowTexel = new long[n];
    s_axisAlloc = (s_colCell and s_colTexel and s_rowCell and s_rowTexel) ? n : 0;
    return s_axisAlloc >= n;
}

// Fill one axis' ramps. `start` is the first map-image pixel of the window, `subdiv` the
// detail pixels per map pixel. A flipped axis has to walk the tile backwards: the base
// map mirrored whole cells, so post k of a cell sits where post P-1-k's ground is, and
// the sub-pixels within it reverse too -- hence mirroring the combined index rather than
// the texel, which would be a pixel or two out.
void FillAxis(long *cell, long *texel, long count, long start, long subdiv,
              bool flip)
{
    const long perCell = s_postsPerCell * subdiv;

    for (long i = 0; i < count; i++)
    {
        const long mapPixel = start + i / subdiv;
        cell[i] = mapPixel / s_postsPerCell;

        long k = (mapPixel % s_postsPerCell) * subdiv + (i % subdiv);

        if (flip)
            k = perCell - 1 - k;

        texel[i] = (k * DETAIL_TILE) / perCell;
    }
}

// The row axis runs the other way up. DiskblockToMemblock computes
//
//     u = start + ((i << LOD) & 3) * minStep        -- increases with the column
//     v = stop  - ((i >> 4) << LOD & 3) * minStep   -- DECREASES with the row
//
// so the first post row of a cell sits at the BOTTOM of the tile, not the top. Sampling
// rows the same way as columns mirrors every cell against its neighbours: forest blocks
// come out cut into horizontal bands and a road crossing a cell boundary breaks into
// stair-steps. Measured on the shipped data, against the column axis as a control --
// adjacent cells usually carry different tiles, so a perfect seam is not available and
// the column figure is what "as good as it gets" looks like:
//
//     column seams (no inversion needed)      2.10x interior detail
//     row seams, sampled like columns         2.81x
//     row seams, inverted                     1.98x   <- matches the control
//
// Inverting is therefore the same operation `flip` performs, applied to the row axis
// whenever the north-south flip is NOT in force -- hence the negation at the call site.
#define DETAIL_ROW_AXIS_INVERTED(flipNS) (not(flipNS))

// Size the buffers for the destination rect -- the detail image is never larger than
// that -- then describe the image at whatever this build actually needs. The palette
// sits after the pixels, so paletteoffset moves with the size and the palette has to be
// rewritten; the caller does that anyway to pick up the base image's table.
bool EnsureImage(long capW, long capH, long w, long h)
{
    if (not s_image or capW > s_capW or capH > s_capH)
    {
        // The map window is a fixed rectangle out of the .scf, so the cap is set once
        // and never grows. If it somehow did, decline detail for now rather than tear
        // down the C_Resmgr that owns the pixels -- the base map is the right fallback.
        if (s_image)
            return false;

        s_image = CreateOccupationMap(5551301, capW, capH, 256);

        if (not s_image)
            return false;

        s_overlay = new BYTE[(size_t)capW * capH];

        if (not s_overlay)
            return false;

        s_capW = capW;
        s_capH = capH;
        s_imageCap = capW * capH;
    }

    if (w * h > s_imageCap)
        return false;

    s_image->Header->w = (short)w;
    s_image->Header->h = (short)h;
    s_image->Header->imageoffset = 0;
    s_image->Header->paletteoffset = (int)(w * h);
    return true;
}

bool EnsureRamps(long destW, long destH)
{
    if (s_rows and s_rampW >= destW + 2 and s_rampH >= destH + 2)
        return true;

    delete[] s_rows;
    delete[] s_cols;
    s_rows = new long[destH + 2];
    s_cols = new long[destW + 2];

    if (not s_rows or not s_cols)
    {
        s_rampW = s_rampH = 0;
        return false;
    }

    s_rampW = destW + 2;
    s_rampH = destH + 2;
    return true;
}

} // namespace

/***************************************************************************\
    Capture the tile grid while the base map is being built.

    Called once per post block from BuildTerrainMapImage, which is already walking every
    block and already knows where each one lands after the north-south / east-west
    flips. Storing one texID per CELL rather than per post is not an optimisation that
    assumes anything: the cell IS the unit the format textures, and the first post of
    each cell is by construction the one whose texID the 3D engine maps across it.
\***************************************************************************/
bool CampMapDetailBeginGrid(long mapW, long mapH, int lod)
{
    CampMapDetailReset();

    if (not g_bCampMapDetail)
        return false;

    // Above LOD 1 a tile covers a single post and the posts index the FAR texture set
    // instead, which is a different file and a different question. Detail is for the
    // zoomed-in case anyway, which is what LOD 0 is for.
    if (lod < 0 or lod > 1)
        return false;

    const long perCell = 4 >> lod;

    if (mapW < perCell or mapH < perCell or (mapW % perCell) or (mapH % perCell))
        return false;

    s_cellsWide = mapW / perCell;
    s_cellsHigh = mapH / perCell;
    s_cells = new DWORD[(size_t)s_cellsWide * s_cellsHigh];

    if (not s_cells)
    {
        s_cellsWide = s_cellsHigh = 0;
        return false;
    }

    memset(s_cells, 0, sizeof(DWORD) * (size_t)s_cellsWide * s_cellsHigh);
    s_postsPerCell = perCell;
    s_flipNS = g_bCampMapFlipNS;
    s_flipEW = g_bCampMapFlipEW;
    return true;
}

// cellX / cellY are in map-image cell space, i.e. after the flips. A flip maps whole
// cells to whole cells, because the map is a whole number of cells wide and high.
void CampMapDetailSetCell(long cellX, long cellY, DWORD texID)
{
    if (not s_cells or cellX < 0 or cellY < 0 or cellX >= s_cellsWide or
        cellY >= s_cellsHigh)
        return;

    s_cells[cellY * s_cellsWide + cellX] = texID;
}

long CampMapDetailPostsPerCell()
{
    return s_postsPerCell;
}

bool CampMapDetailHaveGrid()
{
    return s_cells not_eq NULL;
}

void CampMapDetailReset()
{
    delete[] s_cells;
    s_cells = NULL;
    s_cellsWide = s_cellsHigh = 0;
    s_postsPerCell = 0;
    s_builtSubdiv = 0;
    s_builtOverlayGen = 0xFFFFFFFF;
}

// The map calls this whenever it restamps its overlay, so the detail copy of it is
// known stale without having to compare sixteen megabytes.
void CampMapDetailOverlayChanged()
{
    s_overlayGen++;
}

/***************************************************************************\
    Build (or reuse) the detail window for the map's current source rect.

    Returns false whenever the base map should be drawn instead: detail switched off, no
    tile grid, a zoom too far out to gain anything, or a tile that could not be read.
\***************************************************************************/
bool CampMapDetailBuild(const UI95_RECT *mapRect, long destW, long destH,
                        const BYTE *baseOverlay, long baseW, long baseH,
                        const WORD *basePalette)
{
    if (not g_bCampMapDetail or not s_cells or not mapRect)
        return false;

    const long srcW = mapRect->right - mapRect->left;
    const long srcH = mapRect->bottom - mapRect->top;

    if (srcW < 1 or srcH < 1 or destW < 1 or destH < 1)
        return false;

    if (mapRect->left < 0 or mapRect->top < 0 or mapRect->right > baseW or
        mapRect->bottom > baseH)
        return false;

    // Detail pixels per map pixel. One means the base map already has a pixel per
    // destination pixel and there is nothing to add. Taken from whichever axis is
    // tighter, so the result is never larger than the destination on either -- the blit
    // only walks the scale-up path, and rounding the two axes independently would
    // occasionally overshoot one of them and lose the detail layer altogether.
    long subdiv = destW / srcW;

    if (destH / srcH < subdiv)
        subdiv = destH / srcH;

    if (subdiv > DETAIL_MAX_SUBDIV)
        subdiv = DETAIL_MAX_SUBDIV;

    while (subdiv > 1 and (double)srcW * subdiv * (double)srcH * subdiv >
                              (double)DETAIL_MAX_PIXELS)
        subdiv--;

    if (subdiv < 2)
        return false;

    const long w = srcW * subdiv;
    const long h = srcH * subdiv;

    const bool sameWindow =
        s_image and s_builtSubdiv == subdiv and s_builtDestW == destW and
        s_builtDestH == destH and s_builtRect.left == mapRect->left and
        s_builtRect.top == mapRect->top and s_builtRect.right == mapRect->right and
        s_builtRect.bottom == mapRect->bottom;

    if (sameWindow and s_builtOverlayGen == s_overlayGen)
        return true;

    if (not BuildColorLut() or not LoadTileTable())
        return false;

    if (not s_tiles)
    {
        s_tileCount = g_nCampMapDetailTiles;

        if (s_tileCount < 16)
            s_tileCount = 16;

        if (s_tileCount > 2048)
            s_tileCount = 2048;

        s_tiles = new DetailTile[s_tileCount];

        if (not s_tiles)
        {
            s_tileCount = 0;
            return false;
        }

        memset(s_tiles, 0, sizeof(DetailTile) * s_tileCount);
    }

    if (not basePalette or not EnsureImage(destW, destH, w, h) or
        not EnsureRamps(destW, destH) or not EnsureAxis((w > h) ? w : h))
        return false;

    // The base image's palette verbatim, so an index means the same colour in both and
    // the blended palettes C_ScaleBitmap derives from it apply to detail pixels unchanged.
    // It has to be rewritten each build because paletteoffset moves with the image size.
    {
        WORD *pal = s_image->GetPalette();

        if (not pal)
            return false;

        memcpy(pal, basePalette, sizeof(WORD) * 256);
    }

    BYTE *img = (BYTE *)s_image->GetImage();

    if (not img)
        return false;

    FillAxis(s_colCell, s_colTexel, w, mapRect->left, subdiv, s_flipEW);
    FillAxis(s_rowCell, s_rowTexel, h, mapRect->top, subdiv,
             DETAIL_ROW_AXIS_INVERTED(s_flipNS));

    long misses = 0;

    for (long y = 0; y < h; y++)
    {
        const DWORD *cellRow = s_cells + (size_t)s_rowCell[y] * s_cellsWide;
        const long v = s_rowTexel[y] * DETAIL_TILE;
        BYTE *dst = img + (size_t)y * w;

        long lastCell = -1;
        const BYTE *tile = NULL;

        for (long x = 0; x < w; x++)
        {
            const long cx = s_colCell[x];

            if (cx not_eq lastCell)
            {
                lastCell = cx;
                tile = GetTile(cellRow[cx]);

                if (not tile)
                    misses++;
            }

            // A tile that will not load leaves the post's own colour showing through,
            // which is the base map -- a gap reads as coarse ground rather than a hole.
            dst[x] = tile ? tile[v + s_colTexel[x]] : 0;
        }
    }

    // The overlay follows the same ramps, sampled at post resolution: the Logistics
    // layers and the FLOT are stamped one byte per base map pixel and stay exactly where
    // they were stamped. Zero-filled when no layer is up, which resolves to palette 0.
    if (baseOverlay)
    {
        for (long y = 0; y < h; y++)
        {
            const BYTE *src =
                baseOverlay + (size_t)(mapRect->top + y / subdiv) * baseW;
            BYTE *dst = s_overlay + (size_t)y * w;

            for (long x = 0; x < w; x++)
                dst[x] = src[mapRect->left + x / subdiv];
        }
    }
    else
        memset(s_overlay, 0, (size_t)w * h);

    // Nearest-neighbour ramps from the detail image onto the destination rect. One extra
    // entry because ScaleUp8Overlay reads one row past the one it is filling.
    for (long i = 0; i <= destH; i++)
    {
        long r = (i * h) / destH;
        s_rows[i] = (r < h) ? r : h - 1;
    }

    s_rows[destH + 1] = s_rows[destH];

    for (long i = 0; i <= destW; i++)
    {
        long c = (i * w) / destW;
        s_cols[i] = (c < w) ? c : w - 1;
    }

    s_cols[destW + 1] = s_cols[destW];

    s_builtRect = *mapRect;
    s_builtSubdiv = subdiv;
    s_builtDestW = destW;
    s_builtDestH = destH;
    s_builtOverlayGen = s_overlayGen;

    if (g_bLogCampMapDetail)
    {
        // Generous, and bounded. The worst case of the format below is 195 characters
        // against the 200 this used to carry -- five to spare on a stack buffer written
        // by an unbounded sprintf, which is not a margin worth having. A smash here would
        // not crash here either: it would corrupt the frame and fault somewhere
        // unrelated, and only ever with logging switched on.
        char ln[512];
        _snprintf(ln, sizeof(ln) - 1,
                "[MAPDETAIL] src=%ldx%ld dest=%ldx%ld subdiv=%ld detail=%ldx%ld "
                "ftPerPixel=%.1f postsPerCell=%ld tileMisses=%ld\n",
                srcW, srcH, destW, destH, subdiv, w, h,
                // A tile always spans four LOD-0 posts, however many map pixels
                // that is at the LOD the base image was built from.
                (FeetPerPost * 4.0f) / (float)(s_postsPerCell * subdiv),
                s_postsPerCell, misses);
        ln[sizeof(ln) - 1] = 0;   // _snprintf does not terminate on truncation
        FFDebugLog(ln);
    }

    return true;
}

/***************************************************************************\
    Artscout - 2026: the ColorTable index a whole tile averages to.

    For the base map, not the detail layer. A terrain post over water carries colour
    index 0 -- measured at 99.9% of the posts whose tile is the sea tile -- and
    ColorTable[0] is pure white, so the post-colour map paints every ocean a flat
    white field. That is roughly half of Korea. The colour byte is simply not
    populated for water; the sim never needs it, because water is drawn from its
    texture.

    The tile is populated, though, and we are already decoding tiles. Averaging one
    gives the base map the same colour the detail layer resolves to for that ground,
    so the two agree and zooming across the detail threshold does not change the
    colour of the sea.

    Cached by texID in a flat 64K table: this is called once per post while the map
    image is built, and the theater references about a thousand distinct tiles, half
    of its ground being a single one.
\***************************************************************************/
BYTE CampMapDetailTileAverageIndex(DWORD texID)
{
    static BYTE *s_avg = NULL;      // 0 = not computed yet
    static bool s_failed = false;

    if (texID > 0xFFFF or s_failed)
        return 0;

    if (not s_avg)
    {
        if (not BuildColorLut() or not LoadTileTable())
        {
            s_failed = true;
            return 0;
        }

        s_avg = new BYTE[0x10000];

        if (not s_avg)
        {
            s_failed = true;
            return 0;
        }

        memset(s_avg, 0, 0x10000);
    }

    if (s_avg[texID])
        return s_avg[texID];

    const char *name = TileFileName(texID);

    if (not name)
        return 0;

    // Average in RGB and quantise the result, rather than averaging palette indices,
    // which would be meaningless arithmetic on a lookup table.
    char base[64];
    strncpy(base, name, sizeof(base) - 1);
    base[sizeof(base) - 1] = 0;
    char *dot = strchr(base, '.');

    if (dot)
        *dot = 0;

    char fn[MAX_PATH];
    sprintf(fn, "%s/texture/texture/%s.dds", FalconTerrainDataDir, base);
    FILE *fp = fopen(fn, "rb");

    if (not fp)
        return 0;

    BYTE head[128];

    if (fread(head, 1, 128, fp) not_eq 128 or memcmp(head, "DDS ", 4) not_eq 0 or
        memcmp(head + 84, "DXT1", 4) not_eq 0)
    {
        fclose(fp);
        return 0;
    }

    const DWORD w = *(const DWORD *)(head + 16);
    const DWORD h = *(const DWORD *)(head + 12);

    if (w not_eq h or w < 4 or w > 2048)
    {
        fclose(fp);
        return 0;
    }

    // Only the two endpoint colours of each block are needed for an average this
    // coarse -- the four interpolants all lie between them -- so the index bits can
    // be skipped entirely and the whole tile costs one pass over its block headers.
    const long blocks = (long)(w / 4);
    double sr = 0, sg = 0, sb = 0;
    long n = 0;
    BYTE blk[8];

    for (long i = 0; i < blocks * blocks; i++)
    {
        if (fread(blk, 1, 8, fp) not_eq 8)
            break;

        for (int e = 0; e < 2; e++)
        {
            const WORD c = (WORD)(blk[e * 2] bitor (blk[e * 2 + 1] << 8));
            sr += (((c >> 11) bitand 0x1F) * 255 + 15) / 31;
            sg += (((c >> 5) bitand 0x3F) * 255 + 31) / 63;
            sb += ((c bitand 0x1F) * 255 + 15) / 31;
            n++;
        }
    }

    fclose(fp);

    if (not n)
        return 0;

    const long r = (long)(sr / n), g = (long)(sg / n), b = (long)(sb / n);
    BYTE idx = s_lut[((r >> 3) << 10) bitor ((g >> 3) << 5) bitor (b >> 3)];

    // 0 doubles as the "not computed yet" marker and is the index being replaced, so a
    // tile averaging to it would loop forever and substitute nothing. Nothing does --
    // entry 0 is pure white and no ground tile averages to white -- so this is a guard,
    // not a colour choice.
    if (not idx)
        idx = 1;

    s_avg[texID] = idx;
    return idx;
}

IMAGE_RSC *CampMapDetailImage()
{
    return s_image;
}

BYTE *CampMapDetailOverlay()
{
    return s_overlay;
}

long *CampMapDetailRows()
{
    return s_rows;
}

long *CampMapDetailCols()
{
    return s_cols;
}
