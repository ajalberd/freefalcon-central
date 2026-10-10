// terrainclipmap.cpp -- Artscout - 2026: #78 mesh-shader terrain, data side.
// Only the posts the camera just uncovered are uploaded; the mesh shader builds
// every vertex from them (see ffterrain.hlsl).
#include <windows.h>
#include <math.h>
#include <vector>

#include "ttypes.h"   // FeetPerPost, LEVEL_POST_TO_WORLD
#include "tmap.h"     // TheMap.LastNearTexLOD()
#include "tpost.h"    // Tpost
#include "terrtex.h"  // TheTerrTextures / TheFarTextures
#include "rviewpnt.h" // RViewPoint
#include "dispopts.h" // DisplayOptions -- screen size for the NVG vignette
#include "graphics/dxengine/dxengine.h"
#include "graphics/dxengine/common/irenderer.h"
#include "graphics/include/fflog.h" // mirror the debug stream into FFDebug.log
#include "graphics/include/cloudshadow.h"
#include <stdio.h>
#include <stdarg.h>
#include "terrainclipmap.h"

// Same radius cap the legacy path uses, so both cover the same ground.
static const int TCLIP_MAX_RADIUS = 96;
// The morph band width now comes from cfg (g_nTerrainMorphPosts); this is the
// default that knob ships with.

// Artscout - 2026: the debugger stream, same one R12Log uses -- MonoPrint in FF
// goes somewhere else entirely and never reaches the log.
static void TClipLog(const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    FFDebugLog(buf);
}

// Post info bits -- must match the PI_* defines in ffterrain.hlsl.
static const unsigned int PI_VALID = (1u << 16);
static const unsigned int PI_HASTILE = (1u << 17);
static const unsigned int PI_SLOT_MASK = 0xFFFFu;

// Terrain feature flags -- must match the TF_* defines in ffterrain.hlsl.
static const unsigned int TF_TEXTURED = (1u << 0);
static const unsigned int TF_LIGHTING = (1u << 1);
static const unsigned int TF_FOG = (1u << 2);
static const unsigned int TF_WIREOVERLAY = (1u << 3);
static const unsigned int TF_IRGREY = (1u << 4);
static const unsigned int TF_NVG = (1u << 5);
static const unsigned int TF_SEAMGRAD = (1u << 6); // must match TF_SEAMGRAD in ffterrain.hlsl
static const unsigned int TF_UNDERLAP = (1u << 7); // must match TF_UNDERLAP in ffterrain.hlsl

// Declared OUT here on purpose: inside the anonymous namespace below an extern
// picks up internal linkage and never resolves to its real definition.
extern bool g_bTerrainMeshShader;
extern bool g_bTerrainMeshDebugTint; // flat per-LOD tint, for bring-up
extern int g_terrainRadiusCap; // #DX12 A5: sensor pass shrinks its rings
extern int g_nTerrainMorphPosts;  // cfg: geomorph band width, in posts
extern int g_nTerrainRingRadius;  // cfg: fixed ring radius; 0 = streamed range
extern float g_fFarPlaneKm;       // cfg: world far clip, km (caps every ring)

namespace
{
// One clipmap texel: elevation plus the tile coordinates of this post.
struct ClipPost
{
    float z, u, v, d;
};

struct Level
{
    bool ready;   // filled at least once since the last invalidate
    int lod;      // theater LOD this slice holds
    int originRow; // absolute level post mapped to texel row 0
    int originCol;
    int refreshRow; // rolling re-scan cursor, an ABSOLUTE level post row
    // The band actually drawn (ring + margin), in absolute level posts. Only
    // this is streamed: the ring is a fraction of the window, and every post
    // outside it would burn a GetPost plus a tile activation for nothing.
    int actR0, actR1, actC0, actC1;
    bool hasBand; // actR0.. held a real band last update (so the strips that just entered it can be told)
    std::vector<float> shadowZ; // CPU copy, for the per-chunk bounds
    std::vector<unsigned char> shadowOk;
    // Artscout - 2026 (hole-line investigation): the ABSOLUTE post (row, col) that last wrote each window texel.
    // The window is toroidal, so a texel nobody refreshed still holds the post from 256 posts away: its shadowOk
    // says "valid" and nothing else notices. Comparing the stamp with the post the ring expects there does.
    std::vector<int> stampR, stampC;
};

Level s_level[TCLIP_MAX_LODS];
int s_levels = 0;
int s_baseLod = 0;
bool s_created = false;
int s_chunkCount = 0;
TerrainClipConstants s_cb;

// Scratch upload buffers, kept across frames (the rects are small).
std::vector<ClipPost> s_post;
std::vector<unsigned int> s_info;
std::vector<float> s_bounds; // min/max per tile, all levels

// Status line every few seconds: without it a black ground gives no clue which
// step stalled (no support / no data / no chunks).
unsigned long s_statTick = 0;
unsigned int s_statPosts = 0;
unsigned int s_statRegions = 0;
unsigned int s_statTiled = 0; // posts that resolved to a real tile
// Per level, so it is visible WHICH posts miss: outside the theater's range
// (invalid), or valid but with no tile resident.
unsigned int s_statLvPosts[TCLIP_MAX_LODS] = {};
unsigned int s_statLvValid[TCLIP_MAX_LODS] = {};
unsigned int s_statLvTiled[TCLIP_MAX_LODS] = {};
unsigned int s_statLvNoSrv[TCLIP_MAX_LODS] = {}; // valid post, GetTileSRV = 0
// The sensor pass runs its own Update (cap > 0) and overwrites the shared
// constants, so its numbers have to be tracked apart from the main view's.
unsigned int s_statSensorUpdates = 0;
unsigned int s_statSensorPosts = 0;
unsigned int s_statSensorTiled = 0;
unsigned int s_statSensorFlags = 0;
int s_statSensorChunks = 0;
bool s_statSensorSunOk = false;
float s_statSensorSun[10] = {}; // dir3, color3, ambient3, dayNight

inline int WrapTexel(int v)
{
    const int m = v % TCLIP_TEXELS;
    return (m < 0) ? m + TCLIP_TEXELS : m;
}

inline int FloorDiv(int a, int b)
{
    return (a >= 0) ? (a / b) : -(((-a) + b - 1) / b);
}

// The ring's reach for this LOD, in level posts. 0 = nothing to draw.
// applyCap belongs to DRAWING only: the sensor pass shrinks its rings, but the
// streamed band must stay full-size or the main view loses the posts it needs.
int RingRange(RViewPoint* vp, int lod, bool applyCap)
{
    const int avail = vp->GetAvailablePostRange(lod);
    const int availSafe = (avail > 1) ? avail - 1 : 0;
    if (availSafe <= 2)
        return 0;

    int range = availSafe;
    // Stock: the ring reaches as far as the theater has streamed. That range
    // DROPS whenever a block in the area of interest is still loading and comes
    // back when it lands (TBlockList::ComputeAvailableRange), so the rings --
    // and with them the LOD/texture boundaries and the geomorph band -- resize
    // from frame to frame. Pinning the radius takes that out of the picture.
    if (g_nTerrainRingRadius > 0 && range > g_nTerrainRingRadius)
        range = g_nTerrainRingRadius;
    if (range > TCLIP_MAX_RADIUS)
        range = TCLIP_MAX_RADIUS;
    {
        // Artscout - 2026: nothing past the far plane is drawn (the amplification shader culls it, and the
        // haze is opaque by 0.9 of it), but a ring asked for more used to still be streamed and uploaded.
        // A square box of half-width 0.95 * far plane covers the whole visible disc; add a chunk of slack.
        float km = g_fFarPlaneKm < 20.0f ? 20.0f : (g_fFarPlaneKm > 400.0f ? 400.0f : g_fFarPlaneKm);
        const float stepFt = FeetPerPost * (float)(1 << lod);
        const int reach = (int)(0.95f * km * 3280.84f / stepFt) + TCLIP_CHUNK;
        if (range > reach)
            range = reach;
    }

    // #DX12 A5: the TGP/Maverick sensor renders a SECOND full view into its RTT
    // every frame; it caps the radius so that pass stays cheap.
    if (applyCap && g_terrainRadiusCap > 0 && range > g_terrainRadiusCap)
        range = g_terrainRadiusCap;
    // Keep the ring inside the window it is streamed into.
    if (range > TCLIP_TEXELS / 2 - TCLIP_CHUNK)
        range = TCLIP_TEXELS / 2 - TCLIP_CHUNK;
    return range & ~1; // even reach -> the snapped box stays symmetric
}

// Fill one post from the theater. Out-of-range posts stay invalid: GetPost is
// unsafe past the available range (tviewpnt.h), it does not merely return null.
void ReadPost(RViewPoint* vp, int lod, int row, int col, int centerRow,
              int centerCol, int availSafe, bool useTex, ClipPost& outPost,
              unsigned int& outInfo)
{
    outPost.z = 0.0f;
    outPost.u = outPost.v = outPost.d = 0.0f;
    outInfo = 0;

    if (abs(row - centerRow) > availSafe || abs(col - centerCol) > availSafe)
        return;

    Tpost* p = vp->GetPost(row, col, lod);
    if (!p)
        return;

    outPost.z = p->z;
    outPost.u = p->u;
    outPost.v = p->v;
    outPost.d = p->d;
    outInfo = PI_VALID;

    // Near tiles come from the high-res set; fall back to the theater-wide far
    // texture of the same tile when the near one has not streamed in yet.
    void* srv = useTex ? TheTerrTextures.GetTileSRV((TextureID)p->texID) :
                         TheFarTextures.GetTileSRV((TextureID)p->texID);
    if (!srv)
        srv = TheFarTextures.GetTileSRV((TextureID)p->texID);
    if (!srv)
        return;

    const unsigned int slot = g_pRenderer->BindlessTexIndex(
        (struct ID3D11ShaderResourceView*)srv);
    if (slot <= PI_SLOT_MASK)
        outInfo |= PI_HASTILE | slot;
}

// Upload one seam-free rect, in ABSOLUTE level posts, and refresh the shadow.
void UploadRect(RViewPoint* vp, Level& lv, int level, int r0, int r1, int c0,
                int c1, int centerRow, int centerCol, int availSafe,
                bool useTex)
{
    const int h = r1 - r0, w = c1 - c0;
    if (h <= 0 || w <= 0)
        return;

    s_post.resize((size_t)h * w);
    s_info.resize((size_t)h * w);

    for (int r = 0; r < h; ++r)
    {
        for (int c = 0; c < w; ++c)
        {
            ClipPost p;
            unsigned int info;
            ReadPost(vp, lv.lod, r0 + r, c0 + c, centerRow, centerCol,
                     availSafe, useTex, p, info);
            s_post[(size_t)r * w + c] = p;
            s_info[(size_t)r * w + c] = info;

            if (level >= 0 && level < TCLIP_MAX_LODS)
            {
                ++s_statLvPosts[level];

                if (info & PI_VALID)
                    ++s_statLvValid[level];

                if (info & PI_HASTILE)
                {
                    ++s_statTiled;
                    ++s_statLvTiled[level];
                }
                else if (info & PI_VALID)
                {
                    ++s_statLvNoSrv[level]; // post exists, tile did not resolve
                }
            }

            const int tr = WrapTexel(r0 + r), tc = WrapTexel(c0 + c);
            lv.shadowZ[(size_t)tr * TCLIP_TEXELS + tc] = p.z;
            lv.shadowOk[(size_t)tr * TCLIP_TEXELS + tc] =
                (info & PI_VALID) ? 1 : 0;
            if (!lv.stampR.empty())
            {
                lv.stampR[(size_t)tr * TCLIP_TEXELS + tc] = r0 + r;
                lv.stampC[(size_t)tr * TCLIP_TEXELS + tc] = c0 + c;
            }
        }
    }

    g_pRenderer->UpdateTerrainClipmap(level, WrapTexel(c0), WrapTexel(r0), w, h,
                                      &s_post[0], &s_info[0]);
    s_statPosts += (unsigned int)(w * h);
    ++s_statRegions;
}

// Clip to the drawn band, split on the toroidal seam, then upload the pieces.
void FillRect(RViewPoint* vp, Level& lv, int level, int r0, int r1, int c0,
              int c1, int centerRow, int centerCol, int availSafe, bool useTex)
{
    // Outside the band nothing is ever drawn, and every post there would cost a
    // GetPost plus a slice of the tile-activation budget.
    if (r0 < lv.actR0)
        r0 = lv.actR0;
    if (r1 > lv.actR1)
        r1 = lv.actR1;
    if (c0 < lv.actC0)
        c0 = lv.actC0;
    if (c1 > lv.actC1)
        c1 = lv.actC1;

    if (r1 <= r0 || c1 <= c0)
        return;

    int rowSplit[3], colSplit[3];
    int nRow = 0, nCol = 0;

    rowSplit[nRow++] = r0;
    if (WrapTexel(r0) + (r1 - r0) > TCLIP_TEXELS)
        rowSplit[nRow++] = r0 + (TCLIP_TEXELS - WrapTexel(r0));
    rowSplit[nRow++] = r1;

    colSplit[nCol++] = c0;
    if (WrapTexel(c0) + (c1 - c0) > TCLIP_TEXELS)
        colSplit[nCol++] = c0 + (TCLIP_TEXELS - WrapTexel(c0));
    colSplit[nCol++] = c1;

    for (int i = 0; i + 1 < nRow; ++i)
    {
        for (int j = 0; j + 1 < nCol; ++j)
        {
            UploadRect(vp, lv, level, rowSplit[i], rowSplit[i + 1],
                       colSplit[j], colSplit[j + 1], centerRow, centerCol,
                       availSafe, useTex);
        }
    }
}

// Per-tile elevation bounds straight off the shadow copy, so the amplification
// shader can cull a chunk without reading the post texture.
void RecomputeBounds(const Level& lv, int level)
{
    float* dst = &s_bounds[(size_t)level * TCLIP_TILES * TCLIP_TILES * 2];

    for (int tr = 0; tr < TCLIP_TILES; ++tr)
    {
        for (int tc = 0; tc < TCLIP_TILES; ++tc)
        {
            float lo = 1e30f, hi = -1e30f;
            // +1 post so a chunk's far edge (shared with the next chunk) counts.
            for (int r = 0; r <= TCLIP_CHUNK; ++r)
            {
                for (int c = 0; c <= TCLIP_CHUNK; ++c)
                {
                    const int pr = WrapTexel(tr * TCLIP_CHUNK + r);
                    const int pc = WrapTexel(tc * TCLIP_CHUNK + c);
                    const size_t k = (size_t)pr * TCLIP_TEXELS + pc;
                    if (!lv.shadowOk[k])
                        continue;
                    const float z = lv.shadowZ[k];
                    if (z < lo)
                        lo = z;
                    if (z > hi)
                        hi = z;
                }
            }
            if (lo > hi) // no valid post in this tile
            {
                lo = 0.0f;
                hi = 0.0f;
            }
            const size_t t = (size_t)tr * TCLIP_TILES + tc;
            dst[t * 2 + 0] = lo;
            dst[t * 2 + 1] = hi;
        }
    }
}
} // namespace

void TerrainClipmap_Invalidate()
{
    for (int i = 0; i < TCLIP_MAX_LODS; ++i)
    {
        s_level[i].ready = false;
        s_level[i].hasBand = false;
    }
    s_chunkCount = 0;
    // Force a fresh CreateTerrainClipmap: leaving 3D releases the backend's
    // images, and re-using them would draw through dead descriptors.
    s_created = false;
}

const TerrainClipConstants& TerrainClipmap_Constants()
{
    return s_cb;
}

int TerrainClipmap_ChunkCount()
{
    return s_chunkCount;
}

bool TerrainClipmap_Update(RViewPoint* vp, const float camPos[3],
                           const float eyePos[3], float dayNight)
{
    if (!g_bTerrainMeshShader || !vp || !camPos)
        return false;
    if (!g_pRenderer || !g_pRenderer->MeshTerrainAvailable())
        return false;

    // The vertices are pre-translated by the camera and the view matrix is
    // rotation-only, so this MUST be the position the view matrix was built
    // for -- the same one every object is pre-translated by. camPos is the
    // viewpoint, which trails it by the head lean / 6DOF / turbulence offset;
    // pre-translating by that instead slides the whole ground against the
    // cockpit and the objects every time the head or the airframe moves.
    const float* eye = eyePos ? eyePos : camPos;

    // Sensor pass (cap > 0) shares every static here with the main view, so
    // its share of the counters is taken as a delta over this call.
    const unsigned int postsEnter = s_statPosts;
    const unsigned int tiledEnter = s_statTiled;

    const int hiLOD = vp->GetHighLOD();
    const int loLOD = vp->GetLowLOD();
    int levels = loLOD - hiLOD + 1;
    if (levels < 1)
        return false;
    if (levels > TCLIP_MAX_LODS)
        levels = TCLIP_MAX_LODS;

    if (!s_created || levels != s_levels || hiLOD != s_baseLod)
    {
        if (!g_pRenderer->CreateTerrainClipmap(TCLIP_TEXELS, levels,
                                               TCLIP_TILES))
            return false;
        s_created = true;
        s_levels = levels;
        s_baseLod = hiLOD;
        s_bounds.assign((size_t)levels * TCLIP_TILES * TCLIP_TILES * 2, 0.0f);
        for (int i = 0; i < levels; ++i)
        {
            s_level[i].ready = false;
            s_level[i].hasBand = false;
            s_level[i].lod = hiLOD + i;
            s_level[i].refreshRow = 0;
            s_level[i].shadowZ.assign(TCLIP_TEXELS * TCLIP_TEXELS, 0.0f);
            s_level[i].shadowOk.assign(TCLIP_TEXELS * TCLIP_TEXELS, 0);
            s_level[i].stampR.assign(TCLIP_TEXELS * TCLIP_TEXELS, 0x7FFFFFFF);
            s_level[i].stampC.assign(TCLIP_TEXELS * TCLIP_TEXELS, 0x7FFFFFFF);
        }
        // NOT TerrainClipmap_Invalidate() here: it clears s_created, which would
        // re-create the clipmap every frame and never let a level past L0.
        s_chunkCount = 0;
    }

    // One full refill per frame at most -- a fresh slice is 65k posts.
    bool refilledThisFrame = false;
    bool boundsDirty = false;
    int readyLevels = 0;

    for (int i = 0; i < s_levels; ++i)
    {
        Level& lv = s_level[i];
        const int lod = lv.lod;
        const float step = FeetPerPost * (float)(1 << lod);
        const int centerRow = (int)floorf(camPos[0] / FeetPerPost) >> lod;
        const int centerCol = (int)floorf(camPos[1] / FeetPerPost) >> lod;
        const bool useTex = (lod <= TheMap.LastNearTexLOD());

        int avail = vp->GetAvailablePostRange(lod);
        const int availSafe = (avail > 1) ? avail - 1 : 0;
        if (availSafe <= 2)
            continue; // nothing streamed here yet

        // The band this level streams: the ring plus a margin for the border
        // posts the normals and the geomorph read, and for camera drift.
        const int range = RingRange(vp, lod, false);
        if (range <= 0)
            continue;
        {
            // Artscout - 2026: -G distance investigation. One line per LOD whenever its ring or the streamed
            // range changes: is the ring limited by what has streamed (avail), the TCLIP_MAX_RADIUS cap, or
            // the window? And does its outer edge sit past the 280000 ft far plane (ContextMPR::ZFAR)?
            static int s_lastAvail[TCLIP_MAX_LODS], s_lastRange[TCLIP_MAX_LODS];
            static int s_logged = 0;
            if (lod >= 0 && lod < TCLIP_MAX_LODS &&
                (s_lastAvail[lod] != availSafe || s_lastRange[lod] != range) &&
                s_logged < 300)
            {
                s_lastAvail[lod] = availSafe;
                s_lastRange[lod] = range;
                ++s_logged;
                const char* why = (range == TCLIP_MAX_RADIUS)           ? "MAX_RADIUS cap" :
                                  (range >= availSafe - 1)              ? "streamed range" :
                                  (range == TCLIP_TEXELS / 2 - TCLIP_CHUNK) ? "window" :
                                  (g_nTerrainRingRadius > 0)            ? "cfg radius" :
                                  ((float)range * step >= 0.95f * g_fFarPlaneKm * 3280.84f - 2.0f * step) ? "far plane" :
                                                                          "other";
                const float edgeFt = (float)range * step;
                TClipLog("[TERRAIN-RING] lod=%d avail=%d ring=%d posts (%.1f km, %.0f ft%s) limited by: %s\n",
                         lod, availSafe, range, edgeFt / 3280.84f, edgeFt,
                         edgeFt > g_fFarPlaneKm * 3280.84f ? " > far plane" : "", why);
            }
        }
        // Clamped to what the theater actually has: past availSafe every post
        // comes back invalid, and reading them was over half the work.
        int reach = range + TCLIP_CHUNK * 2;
        if (reach > availSafe)
            reach = availSafe;
        const int oldR0 = lv.actR0, oldR1 = lv.actR1, oldC0 = lv.actC0, oldC1 = lv.actC1;
        const bool hadBand = lv.hasBand;
        lv.actR0 = centerRow - reach;
        lv.actR1 = centerRow + reach + 1;
        lv.actC0 = centerCol - reach;
        lv.actC1 = centerCol + reach + 1;
        lv.hasBand = true;

        // Window origin: the camera sits in the middle of the slice.
        const int wantRow = centerRow - TCLIP_TEXELS / 2;
        const int wantCol = centerCol - TCLIP_TEXELS / 2;

        if (!lv.ready)
        {
            if (refilledThisFrame)
                continue; // next frame; the ring below skips this level
            lv.originRow = wantRow;
            lv.originCol = wantCol;
            FillRect(vp, lv, i, wantRow, wantRow + TCLIP_TEXELS, wantCol,
                     wantCol + TCLIP_TEXELS, centerRow, centerCol, availSafe,
                     useTex);
            lv.ready = true;
            refilledThisFrame = true;
            boundsDirty = true;
            RecomputeBounds(lv, i);
        }
        else
        {
            const int dRow = wantRow - lv.originRow;
            const int dCol = wantCol - lv.originCol;

            if (abs(dRow) >= TCLIP_TEXELS || abs(dCol) >= TCLIP_TEXELS)
            {
                if (refilledThisFrame)
                    continue;
                lv.originRow = wantRow;
                lv.originCol = wantCol;
                FillRect(vp, lv, i, wantRow, wantRow + TCLIP_TEXELS, wantCol,
                         wantCol + TCLIP_TEXELS, centerRow, centerCol,
                         availSafe, useTex);
                refilledThisFrame = true;
                boundsDirty = true;
                RecomputeBounds(lv, i);
            }
            else if (dRow || dCol)
            {
                // Only the strips the window just crossed. Rows first over the
                // NEW column span, then columns over the rows that stay.
                const int newR0 = (dRow > 0) ? lv.originRow + TCLIP_TEXELS :
                                               wantRow;
                const int newR1 = (dRow > 0) ? wantRow + TCLIP_TEXELS :
                                               lv.originRow;
                const int newC0 = (dCol > 0) ? lv.originCol + TCLIP_TEXELS :
                                               wantCol;
                const int newC1 = (dCol > 0) ? wantCol + TCLIP_TEXELS :
                                               lv.originCol;

                if (dRow)
                {
                    FillRect(vp, lv, i, newR0, newR1, wantCol,
                             wantCol + TCLIP_TEXELS, centerRow, centerCol,
                             availSafe, useTex);
                }
                if (dCol)
                {
                    const int keepR0 = (dRow > 0) ? wantRow : newR1;
                    const int keepR1 = (dRow > 0) ? newR0 : wantRow +
                                                                TCLIP_TEXELS;
                    FillRect(vp, lv, i, keepR0, keepR1, newC0, newC1,
                             centerRow, centerCol, availSafe, useTex);
                }

                lv.originRow = wantRow;
                lv.originCol = wantCol;
                boundsDirty = true;
                RecomputeBounds(lv, i);
            }

            // Artscout - 2026: the strips that just ENTERED the band. The ring's edge is at the data-availability
            // limit, so the band has no margin past it: when the ring steps forward, its new outermost columns/rows
            // were never uploaded, and the 8-row rolling re-scan below took ~13 updates to reach them -- a whole
            // edge column of quads dropped (stale posts) for ~50-100 ms every time the ring stepped. Flying along,
            // that is a line at every ring border that flickers on and off and moves with you. Upload what entered
            // NOW, before anything is drawn this update. (The window-edge strips above never cover it: those are
            // at the far side of the 256-post window, outside the band.)
            if (hadBand && (lv.actR0 != oldR0 || lv.actR1 != oldR1 || lv.actC0 != oldC0 || lv.actC1 != oldC1))
            {
                const int rA = lv.actR0, rB = lv.actR1, cA = lv.actC0, cB = lv.actC1;

                // rows newly inside the band, across the whole new column span
                if (rA < oldR0)
                    FillRect(vp, lv, i, rA, (rB < oldR0) ? rB : oldR0, cA, cB, centerRow, centerCol, availSafe, useTex);
                if (rB > oldR1)
                    FillRect(vp, lv, i, (rA > oldR1) ? rA : oldR1, rB, cA, cB, centerRow, centerCol, availSafe, useTex);

                // columns newly inside the band, over the rows the old band already covered (the rest is above)
                const int kr0 = (rA > oldR0) ? rA : oldR0, kr1 = (rB < oldR1) ? rB : oldR1;
                if (kr1 > kr0)
                {
                    if (cA < oldC0)
                        FillRect(vp, lv, i, kr0, kr1, cA, (cB < oldC0) ? cB : oldC0, centerRow, centerCol, availSafe, useTex);
                    if (cB > oldC1)
                        FillRect(vp, lv, i, kr0, kr1, (cA > oldC1) ? cA : oldC1, cB, centerRow, centerCol, availSafe, useTex);
                }

                boundsDirty = true;
                RecomputeBounds(lv, i);
            }

            // Rolling re-scan over the BAND: a tile activates long after its
            // post was uploaded (the budget is a few per frame), so the band is
            // re-read a few rows at a time until every post has found its tile.
            // The cursor is an ABSOLUTE post row, not an offset into the band:
            // the band's origin walks with the camera, so an offset advancing
            // by REFRESH_ROWS while the origin advances by one post skips a row
            // per step -- whole rows that never get re-read, and whose posts
            // keep "no tile" for as long as the drift keeps the same phase.
            // That is the untextured striping that only resolves on a refill.
            const int REFRESH_ROWS = 8;
            if (lv.actR1 > lv.actR0)
            {
                if (lv.refreshRow < lv.actR0 || lv.refreshRow >= lv.actR1)
                    lv.refreshRow = lv.actR0;
                int rr1 = lv.refreshRow + REFRESH_ROWS;
                if (rr1 > lv.actR1)
                    rr1 = lv.actR1;
                FillRect(vp, lv, i, lv.refreshRow, rr1, lv.actC0, lv.actC1,
                         centerRow, centerCol, availSafe, useTex);
                lv.refreshRow = rr1;
                if (lv.refreshRow >= lv.actR1)
                {
                    lv.refreshRow = lv.actR0;
                    boundsDirty = true;
                    RecomputeBounds(lv, i);
                }
            }
        }

        if (lv.ready)
            ++readyLevels;
        (void)step;
    }

    if (!readyLevels)
    {
        // Artscout - 2026: a frame with no terrain at all (whole-screen hole in one eye). Capped log.
        static int s_n = 0;
        if (s_n < 40)
        {
            ++s_n;
            TClipLog("[TERRAIN-SKIP] t=%lu no level ready (levels=%d created=%d) -> terrain NOT drawn this call\n",
                     GetTickCount(), s_levels, (int)s_created);
        }
        return false;
    }

    if (boundsDirty)
    {
        g_pRenderer->UpdateTerrainChunkBounds(
            &s_bounds[0], s_levels * TCLIP_TILES * TCLIP_TILES);
    }

    // ---- rings + chunk spans, tiled exactly like the legacy DrawLodPatch ----
    int chunkId = 0;
    int inB[4] = {0, 0, 0, 0};
    bool hasInner = false;

    for (int i = 0; i < s_levels; ++i)
    {
        Level& lv = s_level[i];
        TerrainClipLevelGpu& g = s_cb.clip[i];
        const int lod = lv.lod;

        g.originPost[0] = lv.originRow;
        g.originPost[1] = lv.originCol;
        g.chunkSpan[0] = chunkId;
        g.chunkSpan[1] = g.chunkSpan[2] = g.chunkSpan[3] = 0;
        g.ringInner[0] = g.ringInner[1] = g.ringInner[2] = 0;
        g.ringInner[3] = -1;

        int avail = vp->GetAvailablePostRange(lod);
        const int availSafe = (avail > 1) ? avail - 1 : 0;
        if (!lv.ready || availSafe <= 2)
        {
            g.ringOuter[0] = g.ringOuter[1] = 0;
            g.ringOuter[2] = g.ringOuter[3] = 0;
            g.originPost[2] = g.originPost[3] = 0;
            continue;
        }

        const int centerRow = (int)floorf(camPos[0] / FeetPerPost) >> lod;
        const int centerCol = (int)floorf(camPos[1] / FeetPerPost) >> lod;

        // Same reach the streaming band used -- one source, no drift.
        const int range = RingRange(vp, lod, true);

        // Outer edges snap to EVEN posts, i.e. the coarse LOD's grid lines.
        int rHi = (centerRow + range) & ~1;
        int rLo = centerRow - range;
        if (rLo & 1)
            ++rLo;
        int cHi = (centerCol + range) & ~1;
        int cLo = centerCol - range;
        if (cLo & 1)
            ++cLo;

        g.ringOuter[0] = rLo;
        g.ringOuter[1] = rHi;
        g.ringOuter[2] = cLo;
        g.ringOuter[3] = cHi;

        if (hasInner)
        {
            g.ringInner[0] = inB[0];
            g.ringInner[1] = inB[1];
            g.ringInner[2] = inB[2];
            g.ringInner[3] = inB[3];
        }

        {
            // Artscout - 2026: flicker-at-a-ring-boundary investigation. Two cheap checks, capped log.
            //  [TERRAIN-NEST]: the finer ring's box (halved, i.e. this ring's inner hole) must sit inside this
            //    ring's outer box, or there is a hole (inner too big) -- or an overlap strip (z-fight).
            //  [TERRAIN-BOX]: a ring box moved. Normal while flying (steps of 2 posts); the PATTERN matters --
            //    a box that flips back and forth between two values on consecutive updates (one per eye, or
            //    camera jitter at a snap boundary) is a ring edge that dances.
            static int s_nestLogged = 0, s_boxLogged = 0;
            static int s_lastBox[TCLIP_MAX_LODS][4];
            static bool s_haveBox[TCLIP_MAX_LODS];

            // [TERRAIN-HOLE]: every post the ring's quads use must be valid AND be the post this texel is
            // supposed to hold. A bad row or column is a straight hole. CPU-side only; capped log.
            static int s_holeLogged = 0;
            static unsigned long s_holeT0 = 0;
            if (!s_holeT0)
                s_holeT0 = GetTickCount();
            // the first few seconds after a (re)fill are legitimately incomplete: don't spend the log on them
            if (s_holeLogged < 120 && !lv.stampR.empty() && GetTickCount() - s_holeT0 > 10000)
            {
                int rowBad[TCLIP_TEXELS] = {0}, colBad[TCLIP_TEXELS] = {0};
                int bad = 0, stale = 0, invalid = 0, firstR = 0, firstC = 0;
                const int hh = rHi - rLo + 1, ww = cHi - cLo + 1;

                if (hh > 0 && ww > 0 && hh <= TCLIP_TEXELS && ww <= TCLIP_TEXELS)
                {
                    for (int r = rLo; r <= rHi; ++r)
                        for (int c = cLo; c <= cHi; ++c)
                        {
                            const size_t ix = (size_t)WrapTexel(r) * TCLIP_TEXELS + WrapTexel(c);
                            const bool st = (lv.stampR[ix] != r || lv.stampC[ix] != c);
                            const bool iv = !lv.shadowOk[ix];

                            if (st || iv)
                            {
                                if (!bad)
                                    firstR = r, firstC = c;
                                ++bad;
                                stale += st;
                                invalid += (!st && iv);
                                ++rowBad[r - rLo];
                                ++colBad[c - cLo];
                            }
                        }
                }

                if (bad > 0)
                {
                    int worstRow = -1, worstRowN = 0, worstCol = -1, worstColN = 0;
                    for (int k = 0; k < hh; ++k)
                        if (rowBad[k] > worstRowN)
                            worstRowN = rowBad[k], worstRow = rLo + k;
                    for (int k = 0; k < ww; ++k)
                        if (colBad[k] > worstColN)
                            worstColN = colBad[k], worstCol = cLo + k;

                    ++s_holeLogged;
                    TClipLog("[TERRAIN-HOLE] t=%lu lod=%d %d bad of %d posts (stale %d, invalid %d) first (%d,%d) | worst row %d: %d/%d | worst col %d: %d/%d%s\n",
                             GetTickCount(), lod, bad, hh * ww, stale, invalid, firstR, firstC, worstRow, worstRowN,
                             ww, worstCol, worstColN, hh,
                             (worstRowN * 2 >= ww || worstColN * 2 >= hh) ? "  <== A LINE" : "");
                }
            }

            if (hasInner && s_nestLogged < 40 &&
                (inB[0] < rLo || inB[1] > rHi || inB[2] < cLo || inB[3] > cHi))
            {
                ++s_nestLogged;
                TClipLog("[TERRAIN-NEST] lod=%d finer box/2 = (%d..%d, %d..%d) NOT inside outer (%d..%d, %d..%d)\n",
                         lod, inB[0], inB[1], inB[2], inB[3], rLo, rHi, cLo, cHi);
            }

            if (i < TCLIP_MAX_LODS)
            {
                const int nb[4] = {rLo, rHi, cLo, cHi};

                if (s_haveBox[i] && s_boxLogged < 120 &&
                    (nb[0] != s_lastBox[i][0] || nb[1] != s_lastBox[i][1] ||
                     nb[2] != s_lastBox[i][2] || nb[3] != s_lastBox[i][3]))
                {
                    ++s_boxLogged;
                    TClipLog("[TERRAIN-BOX] t=%lu lod=%d (%d..%d, %d..%d) was (%d..%d, %d..%d) cam=(%.0f,%.0f)\n",
                             GetTickCount(), lod, nb[0], nb[1], nb[2], nb[3],
                             s_lastBox[i][0], s_lastBox[i][1], s_lastBox[i][2], s_lastBox[i][3],
                             camPos[0], camPos[1]);
                }

                for (int q = 0; q < 4; ++q)
                    s_lastBox[i][q] = nb[q];

                s_haveBox[i] = true;
            }
        }

        // Chunks align to absolute posts that are a multiple of TCLIP_CHUNK, so
        // one chunk maps onto exactly one bounds tile.
        const int chunkR0 = FloorDiv(rLo, TCLIP_CHUNK);
        const int chunkR1 = FloorDiv(rHi - 1, TCLIP_CHUNK);
        const int chunkC0 = FloorDiv(cLo, TCLIP_CHUNK);
        const int chunkC1 = FloorDiv(cHi - 1, TCLIP_CHUNK);
        const int perCol = chunkR1 - chunkR0 + 1;
        const int perRow = chunkC1 - chunkC0 + 1;

        g.originPost[2] = chunkR0 * TCLIP_CHUNK;
        g.originPost[3] = chunkC0 * TCLIP_CHUNK;
        g.chunkSpan[1] = perRow;
        g.chunkSpan[2] = perRow * perCol;
        g.chunkSpan[3] = perCol;
        chunkId += g.chunkSpan[2];

        // The coarser LOD's inner box is this outer box halved (even edges).
        inB[0] = rLo >> 1;
        inB[1] = rHi >> 1;
        inB[2] = cLo >> 1;
        inB[3] = cHi >> 1;
        hasInner = true;
    }

    s_chunkCount = chunkId;

    if (chunkId == 0)
    {
        static int s_n0 = 0;
        if (s_n0 < 40)
        {
            ++s_n0;
            int av[TCLIP_MAX_LODS] = {0};
            for (int i = 0; i < s_levels && i < TCLIP_MAX_LODS; ++i)
                av[i] = vp->GetAvailablePostRange(s_level[i].lod);
            TClipLog("[TERRAIN-SKIP] t=%lu zero chunks (levels=%d ready=%d) avail=%d,%d,%d,%d,%d\n", GetTickCount(),
                     s_levels, readyLevels, av[0], av[1], av[2], av[3], av[4]);
        }
    }

    // Every view slot gets the base matrix, then the backend overwrites them per
    // eye if this pass is view-instanced / multiview (stereo or quad-views).
    for (int v = 0; v < 4; ++v)
    {
        memcpy(s_cb.view[v], (const float*)&CDXEngine::GetObjView(),
               sizeof(s_cb.view[v]));
        memcpy(s_cb.proj[v], (const float*)&CDXEngine::GetObjProjection(),
               sizeof(s_cb.proj[v]));
    }
    const int viewsUsed = g_pRenderer->GetPerViewMatrices(s_cb.view, s_cb.proj);
    s_cb.camPos[0] = eye[0];
    s_cb.camPos[1] = eye[1];
    s_cb.camPos[2] = eye[2];
    s_cb.camPos[3] = 0.0f;
    s_cb.params[0] = FeetPerPost;
    s_cb.params[1] = dayNight;
    // The shader clamps this to >= 1, and alpha hits 1 exactly on the ring's
    // outer edge either way, so 0/1 leaves the seam watertight with no morph
    // inboard of it. See g_nTerrainMorphPosts for why the width matters.
    s_cb.params[2] = (float)((g_nTerrainMorphPosts > 0) ? g_nTerrainMorphPosts :
                                                          1);
    s_cb.params[3] = (float)TCLIP_TEXELS;
    // The sun as SetLights delivered it -- the very lamp that lights the
    // cockpit. CDXEngine::TheSun is a leftover D3D7 light and stays a fallback.
    bool sunOk = false;
    {
        float sd[3] = {0.0f, 0.0f, 1.0f};
        float sc[3] = {1.0f, 1.0f, 1.0f};
        float sa[3] = {0.0f, 0.0f, 0.0f};

        if (g_pRenderer->GetSunLight(sd, sc, sa))
        {
            float len = sqrtf(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]);
            if (len < 1e-6f)
                len = 1.0f;
            s_cb.sunDir[0] = sd[0] / len;
            s_cb.sunDir[1] = sd[1] / len;
            s_cb.sunDir[2] = sd[2] / len;
            s_cb.sunDir[3] = 0.0f;
            s_cb.sunColor[0] = sc[0];
            s_cb.sunColor[1] = sc[1];
            s_cb.sunColor[2] = sc[2];
            s_cb.sunColor[3] = 0.0f;
            // Floor the ambient at the day/night level so a lit slope brightens
            // without the shaded side collapsing to black.
            const float floorAmb = dayNight * 0.5f;
            s_cb.sunAmbient[0] = (sa[0] > floorAmb) ? sa[0] : floorAmb;
            s_cb.sunAmbient[1] = (sa[1] > floorAmb) ? sa[1] : floorAmb;
            s_cb.sunAmbient[2] = (sa[2] > floorAmb) ? sa[2] : floorAmb;
            s_cb.sunAmbient[3] = 0.0f;
            sunOk = true;
        }
    }

    if (!sunOk)
    {
        const D3DVECTOR& d = CDXEngine::TheSun.dvDirection;
        float len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
        if (len < 1e-6f)
            len = 1.0f;
        s_cb.sunDir[0] = d.x / len;
        s_cb.sunDir[1] = d.y / len;
        s_cb.sunDir[2] = d.z / len;
        s_cb.sunDir[3] = 0.0f;

        const D3DCOLORVALUE& sd = CDXEngine::TheSun.dcvDiffuse;
        const D3DCOLORVALUE& sa = CDXEngine::TheSun.dcvAmbient;
        s_cb.sunColor[0] = sd.r;
        s_cb.sunColor[1] = sd.g;
        s_cb.sunColor[2] = sd.b;
        s_cb.sunColor[3] = 0.0f;
        // Floor the ambient at the day/night level so a lit slope brightens
        // without the shaded side collapsing to black.
        s_cb.sunAmbient[0] = (sa.r > dayNight * 0.5f) ? sa.r : dayNight * 0.5f;
        s_cb.sunAmbient[1] = (sa.g > dayNight * 0.5f) ? sa.g : dayNight * 0.5f;
        s_cb.sunAmbient[2] = (sa.b > dayNight * 0.5f) ? sa.b : dayNight * 0.5f;
        s_cb.sunAmbient[3] = 0.0f;
    }

    // #A5/#97: the sensor pass is monochrome and NVG greens the world; the mesh
    // terrain has its own shader, so it must be told what the others are doing.
    s_cb.flags[0] = TF_TEXTURED | TF_LIGHTING;

    if (g_bTerrainMeshDebugTint)
        s_cb.flags[0] |= TF_WIREOVERLAY;

    {
        extern int g_nTerrainSeamFix; // cfg: bit 1 = wrap-corrected pixel gradients
        if (g_nTerrainSeamFix & 2)
            s_cb.flags[0] |= TF_SEAMGRAD;
        // cfg: bit 2 (4) = coarser ring slips a sunk one-quad rim under the finer ring (plugs hairline gaps)
        if (g_nTerrainSeamFix & 4)
            s_cb.flags[0] |= TF_UNDERLAP;
    }

    if (g_pRenderer->IsIRGrey())
        s_cb.flags[0] |= TF_IRGREY;

    if (g_pRenderer->IsNvgMode())
        s_cb.flags[0] |= TF_NVG;

    // Distance haze, as the legacy terrain pass had (FF_FOG): it dissolves the
    // far ground into the horizon and hides the residual LOD-seam shimmer.
    {
        float fs = 0.0f, fe = 0.0f, frgb[3] = {0.0f, 0.0f, 0.0f};

        if (g_pRenderer->GetFogParams(fs, fe, frgb))
        {
            s_cb.flags[0] |= TF_FOG;
            s_cb.fog[0] = fs;
            s_cb.fog[1] = fe;
            s_cb.fogColor[0] = frgb[0];
            s_cb.fogColor[1] = frgb[1];
            s_cb.fogColor[2] = frgb[2];
            s_cb.fogColor[3] = 1.0f;
        }
    }

    s_cb.misc[0] = (float)DisplayOptions.DispWidth;
    s_cb.misc[1] = (float)DisplayOptions.DispHeight;
    s_cb.misc[2] = (float)(GetTickCount() % 100000u) * 0.001f;
    s_cb.misc[3] = 0.0f;

    // Artscout - 2026: cumulus shadows. The mask reaches the terrain PS through
    // the same bindless path as a tile; no slot means no shadow, not a stale one.
    {
        const CloudShadowParams &cs = CloudShadow_Current();
        memcpy(s_cb.cloudSh0, cs.p0, sizeof(s_cb.cloudSh0));
        memcpy(s_cb.cloudSh1, cs.p1, sizeof(s_cb.cloudSh1));
        unsigned int slot = 0xFFFFFFFFu;
        if (cs.mask && cs.p1[3] > 0.0f)
            slot = g_pRenderer->BindlessTexIndex(cs.mask);
        s_cb.cloudShSlot[0] = slot;
        s_cb.cloudShSlot[1] = s_cb.cloudShSlot[2] = s_cb.cloudShSlot[3] = 0;
        if (slot == 0xFFFFFFFFu)
            s_cb.cloudSh1[3] = 0.0f;
    }
    s_cb.flags[1] = (unsigned int)s_levels;
    s_cb.flags[2] = (unsigned int)s_baseLod;
    s_cb.flags[3] = (unsigned int)s_chunkCount;

    // Frustum planes for the amplification shader's cull, in the same
    // camera-relative world space the chunk AABBs use (the object view matrix
    // is rotation-only, so view*proj applies directly).
    {
        // One AS cull serves every view, so the planes must cover ALL of them:
        // per view, keep the smallest offset, which is the widest volume. Eyes
        // look almost the same way, so the normals stay near-parallel.
        static const int col[6] = {0, 0, 1, 1, 2, 2};
        static const float sign[6] = {1.0f, -1.0f, 1.0f, -1.0f, 0.0f, -1.0f};
        const int viewCount = (viewsUsed > 0) ? viewsUsed : 1;

        for (int p = 0; p < 6; ++p)
            s_cb.frustum[p][0] = s_cb.frustum[p][1] = s_cb.frustum[p][2] =
                s_cb.frustum[p][3] = 0.0f;

        // Artscout - 2026: cull against a WIDER frustum than the one drawn. The planes are built from the
        // pose this update ran with, but the headset displays the frame at a LATER predicted time, and the
        // runtime renders from a late-latched pose. A head turn between those two moments swings real
        // geometry in from the side -- chunks culled here were never generated, so they arrive a frame or
        // more late and read as terrain popping in at the edge of vision while looking around.
        // Widening the PROJECTION before extracting the planes widens the side planes only: columns 0 and 1
        // carry x and y, so scaling them down enlarges the horizontal and vertical field of view, while the
        // z and w columns -- and therefore the near and far planes -- are untouched. Doing it here keeps the
        // amplification shader byte-identical, so no shader rebuild is needed.
        extern float g_fTerrainCullPad;
        float padK = 1.0f + ((g_fTerrainCullPad > 0.0f) ? g_fTerrainCullPad : 0.0f);
        if (padK < 1.0f)
            padK = 1.0f;

        for (int v = 0; v < viewCount; ++v)
        {
            // Widened copy of this view's projection -- the DRAWN projection is untouched.
            float wproj[16];
            memcpy(wproj, s_cb.proj[v], sizeof(wproj));
            if (padK > 1.0f)
            {
                const float inv = 1.0f / padK;
                for (int r = 0; r < 4; ++r)
                {
                    wproj[r * 4 + 0] *= inv; // x column -> wider horizontal FOV
                    wproj[r * 4 + 1] *= inv; // y column -> wider vertical FOV
                }
            }

            float mvp[16];
            for (int r = 0; r < 4; ++r)
            {
                for (int c = 0; c < 4; ++c)
                {
                    float sum = 0.0f;
                    for (int k = 0; k < 4; ++k)
                        sum += s_cb.view[v][r * 4 + k] * wproj[k * 4 + c];
                    mvp[r * 4 + c] = sum;
                }
            }

            // Row-vector convention (clip = v * mvp): a plane is a column of
            // mvp combined with the w column. Inside = dot(n, p) + d >= 0.
            for (int p = 0; p < 6; ++p)
            {
                float pl[4];
                for (int i = 0; i < 4; ++i)
                {
                    // p == 4 is the near side (plain z column, reversed-Z too).
                    pl[i] = (p == 4) ? mvp[i * 4 + col[p]] :
                                       mvp[i * 4 + 3] +
                                           sign[p] * mvp[i * 4 + col[p]];
                }
                const float len =
                    sqrtf(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
                if (len <= 1e-6f)
                {
                    // Degenerate matrix -> leave the plane zero, which rejects
                    // nothing. Never cull on a plane we could not build.
                    s_cb.frustum[p][0] = s_cb.frustum[p][1] =
                        s_cb.frustum[p][2] = s_cb.frustum[p][3] = 0.0f;
                    continue;
                }
                const float inv = 1.0f / len;
                if (v == 0 || pl[3] * inv < s_cb.frustum[p][3])
                {
                    for (int i = 0; i < 4; ++i)
                        s_cb.frustum[p][i] = pl[i] * inv;
                }
            }
        }
    }

    {
        // Artscout - 2026: right-eye whole-screen terrain dropout investigation.
        //  * "TerrainNoCull": zero every frustum plane. The amplification shader treats a zero plane as "rejects
        //    nothing", so no chunk is ever culled. If the dropout / the line goes away with this on, the cull
        //    (frustum planes or the per-tile height bounds) is the culprit; if not, it is not the cull.
        //  * [TERRAIN-FRUSTUM]: the far plane's offset should be steady frame to frame; log if it jumps.
        extern bool g_bTerrainNoCull;
        static float s_lastFar = 0.0f;
        static int s_frLogged = 0;
        const float farD = s_cb.frustum[4][3];

        if (s_lastFar != 0.0f && s_frLogged < 60 &&
            fabsf(farD - s_lastFar) > 0.02f * fabsf(s_lastFar))
        {
            ++s_frLogged;
            TClipLog("[TERRAIN-FRUSTUM] t=%lu far plane d %.0f -> %.0f | near %.0f | left (%.3f %.3f %.3f %.0f) right (%.3f %.3f %.3f %.0f)\n",
                     GetTickCount(), s_lastFar, farD, s_cb.frustum[5][3], s_cb.frustum[0][0], s_cb.frustum[0][1],
                     s_cb.frustum[0][2], s_cb.frustum[0][3], s_cb.frustum[1][0], s_cb.frustum[1][1],
                     s_cb.frustum[1][2], s_cb.frustum[1][3]);
        }
        s_lastFar = farD;

        if (g_bTerrainNoCull)
            for (int p = 0; p < 6; ++p)
                s_cb.frustum[p][0] = s_cb.frustum[p][1] = s_cb.frustum[p][2] = s_cb.frustum[p][3] = 0.0f;
    }

    // The sensor pass overwrites every shared static, so keep its own view of
    // the last update apart from the main view's.
    if (g_terrainRadiusCap > 0)
    {
        ++s_statSensorUpdates;
        s_statSensorPosts += s_statPosts - postsEnter;
        s_statSensorTiled += s_statTiled - tiledEnter;
        s_statSensorFlags = s_cb.flags[0];
        s_statSensorChunks = s_chunkCount;
        s_statSensorSunOk = sunOk;
        for (int k = 0; k < 3; ++k)
        {
            s_statSensorSun[k] = s_cb.sunDir[k];
            s_statSensorSun[3 + k] = s_cb.sunColor[k];
            s_statSensorSun[6 + k] = s_cb.sunAmbient[k];
        }
        s_statSensorSun[9] = s_cb.params[1];
    }

    // ---- status line, on a clock (a per-frame line drowns the console) ----
    {
        const unsigned long now = GetTickCount();
        if (!s_statTick)
            s_statTick = now;
        if (now - s_statTick >= 5000)
        {
            TClipLog("[terr-mesh] levels=%d base=%d chunks=%d views=%d | 5s: "
                     "posts=%u regions=%u tiled=%u\n",
                     s_levels, s_baseLod, s_chunkCount, viewsUsed, s_statPosts,
                     s_statRegions, s_statTiled);
            TClipLog("[terr-mesh]  flags=0x%X (tex=%d lit=%d fog=%d ir=%d "
                     "nvg=%d) cap=%d\n",
                     s_cb.flags[0], (int)((s_cb.flags[0] & TF_TEXTURED) != 0),
                     (int)((s_cb.flags[0] & TF_LIGHTING) != 0),
                     (int)((s_cb.flags[0] & TF_FOG) != 0),
                     (int)((s_cb.flags[0] & TF_IRGREY) != 0),
                     (int)((s_cb.flags[0] & TF_NVG) != 0), g_terrainRadiusCap);
            TClipLog("[terr-mesh]  sensor: updates=%u posts=%u tiled=%u "
                     "chunks=%d flags=0x%X (tex=%d lit=%d ir=%d nvg=%d)\n",
                     s_statSensorUpdates, s_statSensorPosts, s_statSensorTiled,
                     s_statSensorChunks, s_statSensorFlags,
                     (int)((s_statSensorFlags & TF_TEXTURED) != 0),
                     (int)((s_statSensorFlags & TF_LIGHTING) != 0),
                     (int)((s_statSensorFlags & TF_IRGREY) != 0),
                     (int)((s_statSensorFlags & TF_NVG) != 0));
            TClipLog("[terr-mesh]  sensor sun src=%s dir=(%.3f %.3f %.3f) "
                     "diff=(%.2f %.2f %.2f) amb=(%.2f %.2f %.2f) "
                     "dayNight=%.2f\n",
                     s_statSensorSunOk ? "lights" : "FALLBACK(TheSun)",
                     s_statSensorSun[0], s_statSensorSun[1], s_statSensorSun[2],
                     s_statSensorSun[3], s_statSensorSun[4], s_statSensorSun[5],
                     s_statSensorSun[6], s_statSensorSun[7], s_statSensorSun[8],
                     s_statSensorSun[9]);
            TClipLog("[terr-mesh]  sun src=%s dir=(%.3f %.3f %.3f) diff=(%.2f "
                     "%.2f %.2f) amb=(%.2f %.2f %.2f) dayNight=%.2f fog=%d "
                     "(%.0f..%.0f)\n",
                     sunOk ? "lights" : "FALLBACK(TheSun)",
                     s_cb.sunDir[0], s_cb.sunDir[1], s_cb.sunDir[2],
                     s_cb.sunColor[0], s_cb.sunColor[1], s_cb.sunColor[2],
                     s_cb.sunAmbient[0], s_cb.sunAmbient[1], s_cb.sunAmbient[2],
                     s_cb.params[1], (int)((s_cb.flags[0] & TF_FOG) != 0),
                     s_cb.fog[0], s_cb.fog[1]);
            for (int i = 0; i < s_levels; ++i)
            {
                const Level& lv = s_level[i];
                const TerrainClipLevelGpu& g = s_cb.clip[i];
                TClipLog("[terr-mesh]  L%d lod=%d ready=%d ring=(%d..%d, "
                         "%d..%d) chunks=%d tex=%d | posts=%u valid=%u "
                         "tiled=%u noSrv=%u\n",
                         i, lv.lod, (int)lv.ready, g.ringOuter[0],
                         g.ringOuter[1], g.ringOuter[2], g.ringOuter[3],
                         g.chunkSpan[2],
                         (int)(lv.lod <= TheMap.LastNearTexLOD()),
                         s_statLvPosts[i], s_statLvValid[i], s_statLvTiled[i],
                         s_statLvNoSrv[i]);
            }
            s_statTick = now;
            s_statPosts = s_statRegions = s_statTiled = 0;
            s_statSensorUpdates = s_statSensorPosts = s_statSensorTiled = 0;
            for (int k = 0; k < TCLIP_MAX_LODS; ++k)
            {
                s_statLvPosts[k] = s_statLvValid[k] = 0;
                s_statLvTiled[k] = s_statLvNoSrv[k] = 0;
            }
        }
    }

    return s_chunkCount > 0;
}
