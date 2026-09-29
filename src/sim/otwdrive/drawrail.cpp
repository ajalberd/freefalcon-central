// Artscout - 2026: the railway track, drawn as a strip on the ground near the camera.
//
// Pilot for option 2 in RAIL.md ("Drawing the track in 3D"): plain colours, no texture. The
// routes are the ones the trains run on (railnet.cpp, rail.txt). Each frame the segments within
// RailTrackRangeKm of the camera are cut into short pieces that follow the ground -- heights from
// OTWDriver.GetGroundLevel, the lookup ground vehicles sit on -- and drawn as a ballast quad with
// two darker rail quads on top. One drawable with a huge radius, so the display list always
// visits it in the nearest ring, after the terrain.
//
// The quads go through the DX engine's DX2D world-quad path (TheDXEngine.DX2D_AddQuad, what smoke
// trails and the particle fallback use): world-space, flushed through the GPU renderer's
// BeginDynamic2D pipeline -- depth-tested, per eye in VR. The first cut used the legacy
// Render3D path (TransformPoint + DrawSquare) that DrawableTrail's dead branch still has; under
// D3D12 those come out as a flat overlay: left eye only and drawn over everything, cockpit
// included (seen 2026-09-28). DX2D does not write depth, which suits a ground strip: whatever is
// drawn later (vehicles, cockpit) still covers it.
//
// Off unless RailTrack. RailTrackLog reports what was drawn and how far the strip sits from the
// ground, for the flicker hunt if the terrain mesh and the height lookup disagree.

#include <math.h>
#include <stdio.h>
#include <vector>
#include "otwdrive.h"
#include "drawobj.h"
#include "renderow.h"
#include "tod.h"
#include "fflog.h"
#include "graphics/dxengine/dxengine.h"
#include "graphics/dxengine/openxrbackend.h" // which eye a pass is for, for RailTrackLog
#include "railnet.h"

extern bool g_bRailTrack;
extern bool g_bRailTrackLog;
extern int g_nRailTrackRangeKm;

namespace
{
const float FT_PER_KM = 3279.98F;
const float STEP_FT = 100.0F;      // piece length: short enough to follow the ground
const float BALLAST_W = 16.0F;     // ballast bed, about 5 m
const float GAUGE = 4.71F;         // standard gauge
const float RAIL_W = 0.6F;         // exaggerated so the rails survive a few hundred feet up
const float LIFT_FT = 1.5F;        // above the ground lookup (sim z is down: subtract)
const float RAIL_LIFT_FT = 2.0F;

class DrawableRail : public DrawableObject
{
public:
    DrawableRail() : DrawableObject(1.0F), loaded(false), lastLog(0)
    {
        radius = 1.0e9F; // always "distance 0": drawn in the nearest ring every frame
        position.x = position.y = position.z = 0.0F;

        for (int i = 0; i < 5; i++)
            drawsPerEye[i] = 0;
    }

    virtual void Draw(class RenderOTW *renderer, int LOD);

private:
    struct P
    {
        float x, y;
    };

    std::vector<std::vector<P>> routes;
    bool loaded;
    DWORD lastLog;
    int drawsPerEye[5]; // flat, then VR views 0..3

    static void Collect(void *ctx, int route, int index, float x, float y)
    {
        std::vector<std::vector<P>> &r = *static_cast<std::vector<std::vector<P>> *>(ctx);

        if (index == 0 or route >= (int)r.size())
            r.resize(route + 1);

        P p = {x, y};
        r[route].push_back(p);
    }

    // One quad of the strip between two pieces, offset `off` feet sideways, `w` wide, handed to
    // DX2D as corners relative to their centre (sim axes: x north, y east, z down). No texture:
    // the GPU renderer's default white, so the vertex colour is the colour. DX2D frustum-culls
    // each quad on a sphere of `radius` round its centre.
    static void Quad(const Tpoint &a, const Tpoint &b, float px, float py, float off, float w,
                     float lift, float za0, float za1, float zb0, float zb1, DWORD colour)
    {
        Tpoint c[4];
        const float o0 = off - w * 0.5F, o1 = off + w * 0.5F;
        c[0].x = a.x + px * o0; c[0].y = a.y + py * o0; c[0].z = za0 - lift;
        c[1].x = a.x + px * o1; c[1].y = a.y + py * o1; c[1].z = za1 - lift;
        c[2].x = b.x + px * o1; c[2].y = b.y + py * o1; c[2].z = zb1 - lift;
        c[3].x = b.x + px * o0; c[3].y = b.y + py * o0; c[3].z = zb0 - lift;

        D3DXVECTOR3 centre;
        centre.x = (c[0].x + c[1].x + c[2].x + c[3].x) * 0.25F;
        centre.y = (c[0].y + c[1].y + c[2].y + c[3].y) * 0.25F;
        centre.z = (c[0].z + c[1].z + c[2].z + c[3].z) * 0.25F;

        D3DDYNVERTEX q[4];
        float r2 = 0.0F;

        for (int i = 0; i < 4; i++)
        {
            q[i].pos.x = c[i].x - centre.x;
            q[i].pos.y = c[i].y - centre.y;
            q[i].pos.z = c[i].z - centre.z;
            q[i].dwColour = colour;
            q[i].dwSpecular = 0;
            q[i].tu = q[i].tv = 0.0F;
            const float d2 = q[i].pos.x * q[i].pos.x + q[i].pos.y * q[i].pos.y +
                             q[i].pos.z * q[i].pos.z;
            r2 = d2 > r2 ? d2 : r2;
        }

        TheDXEngine.DX2D_AddQuad(LAYER_GROUND, 0, &centre, q, sqrtf(r2), 0);
    }

    static DWORD Colour(float r, float g, float b)
    {
        const DWORD R = (DWORD)(r * 255.0F), G = (DWORD)(g * 255.0F), B = (DWORD)(b * 255.0F);
        return 0xFF000000 | (R << 16) | (G << 8) | B; // D3DCOLOR: ARGB
    }
};

void DrawableRail::Draw(RenderOTW *renderer, int)
{
    if (not g_bRailTrack)
        return;

    if (not loaded)
    {
        RailVisitRoutes(Collect, &routes);
        loaded = true;
    }

    const float cx = renderer->X(), cy = renderer->Y();
    const int km = g_nRailTrackRangeKm < 1 ? 1 : (g_nRailTrackRangeKm > 30 ? 30 : g_nRailTrackRangeKm);
    const float range = km * FT_PER_KM;
    const float light = TheTimeOfDay.GetLightLevel();

    // Ballast: grey-brown; rails: dark steel. Scaled by the light so night is dark.
    const DWORD ballast = Colour(0.46F * light, 0.42F * light, 0.36F * light);
    const DWORD steel = Colour(0.16F * light, 0.15F * light, 0.14F * light);

    // Which VR eye (or -1 for flat) this pass is for -- tallied for RailTrackLog, since the
    // first cut showed in one eye only.
    const int eye = g_pOpenXRBackend ? g_pOpenXRBackend->CurrentEye() : -1;
    drawsPerEye[(eye >= -1 and eye <= 3) ? eye + 1 : 0]++;

    int pieces = 0;
    double offSum = 0.0;
    float offMax = 0.0F;

    for (const std::vector<P> &route : routes)
    {
        for (size_t i = 0; i + 1 < route.size(); i++)
        {
            const P &a = route[i], &b = route[i + 1];
            const float dx = b.x - a.x, dy = b.y - a.y;
            const float len = sqrtf(dx * dx + dy * dy);

            if (len < 1.0F)
                continue;

            // Skip segments whose nearest point is out of range.
            float t = ((cx - a.x) * dx + (cy - a.y) * dy) / (len * len);
            t = t < 0.0F ? 0.0F : (t > 1.0F ? 1.0F : t);
            const float nx = a.x + dx * t - cx, ny = a.y + dy * t - cy;

            if (nx * nx + ny * ny > range * range)
                continue;

            const float ux = dx / len, uy = dy / len;
            const float px = -uy, py = ux; // sideways, in the ground plane
            const int n = (int)(len / STEP_FT) + 1;
            Tpoint prev;
            float pz0 = 0, pz1 = 0, pzc = 0;

            for (int k = 0; k <= n; k++)
            {
                Tpoint cur;
                const float s = len * k / n;
                cur.x = a.x + ux * s;
                cur.y = a.y + uy * s;
                const float ex = cur.x - cx, ey = cur.y - cy;
                const bool inRange = ex * ex + ey * ey <= range * range;

                // Ground under both edges and the centre: on a hillside a flat strip would dig
                // into the uphill side and float on the other.
                const float hw = BALLAST_W * 0.5F;
                const float z0 = OTWDriver.GetGroundLevel(cur.x - px * hw, cur.y - py * hw);
                const float z1 = OTWDriver.GetGroundLevel(cur.x + px * hw, cur.y + py * hw);
                const float zc = OTWDriver.GetGroundLevel(cur.x, cur.y);
                cur.z = zc;

                if (k > 0 and inRange)
                {
                    Quad(prev, cur, px, py, 0.0F, BALLAST_W, LIFT_FT, pz0, pz1, z0, z1, ballast);
                    Quad(prev, cur, px, py, -GAUGE * 0.5F, RAIL_W, RAIL_LIFT_FT, pzc, pzc, zc, zc,
                         steel);
                    Quad(prev, cur, px, py, GAUGE * 0.5F, RAIL_W, RAIL_LIFT_FT, pzc, pzc, zc, zc,
                         steel);
                    pieces++;

                    if (g_bRailTrackLog)
                    {
                        // How far the approximate ground (what far terrain LODs are closer to)
                        // is from the exact lookup the strip uses.
                        const float d = fabsf(OTWDriver.GetApproxGroundLevel(cur.x, cur.y) - zc);
                        offSum += d;
                        offMax = d > offMax ? d : offMax;
                    }
                }

                prev = cur;
                pz0 = z0;
                pz1 = z1;
                pzc = zc;
            }
        }
    }

    if (g_bRailTrackLog)
    {
        const DWORD now = GetTickCount();

        if (now - lastLog > 10000)
        {
            lastLog = now;
            char ln[300];
            // Draw calls in the last ~10 s by eye: flat (-1), then VR views 0..3. Both eyes
            // should be close to equal; one near zero means the strip is only visited once a frame.
            sprintf(ln, "RAILTRACK: %d routes, %d pieces within %d km (this pass, eye %d); exact vs "
                        "approx ground %.1f ft mean, %.1f ft max; draws by eye flat/0/1/2/3 = "
                        "%d/%d/%d/%d/%d\n",
                    (int)routes.size(), pieces, km, eye, pieces ? offSum / pieces : 0.0, offMax,
                    drawsPerEye[0], drawsPerEye[1], drawsPerEye[2], drawsPerEye[3],
                    drawsPerEye[4]);
            FFDebugLog(ln);

            for (int i = 0; i < 5; i++)
                drawsPerEye[i] = 0;
        }
    }
}

DrawableRail *gRailDrawable = nullptr;
} // namespace

// OTWDriverClass::Enter, once the viewpoint exists.
void DrawRailCreate()
{
    if (not g_bRailTrack or gRailDrawable)
        return;

    gRailDrawable = new DrawableRail;
    OTWDriver.InsertObject(gRailDrawable);
}

// OTWDriverClass::Exit, before the viewpoint is cleaned up: the display lists only unlink their
// objects, the owner frees them.
void DrawRailDestroy()
{
    if (not gRailDrawable)
        return;

    OTWDriver.RemoveObject(gRailDrawable, TRUE);
    gRailDrawable = nullptr;
}
