// Artscout - 2026: JHMCS-style helmet-mounted cueing system. See hmcs.h for the knobs.
//
// Replaces the 2001 S.G. "HMS" as far as the player in the 3D pit is concerned. That one drew a
// circle at the centre of the screen while an AIM-9 was uncaged and turned OFF the uncaged seeker's
// own search, relying on the padlock view to hand the missile a target -- so in VR, where nobody
// padlocks, an uncaged AIM-9 on an HMS aircraft could never lock anything, and the circle (a flat
// 2D overlay at the viewport centre) sat at the wrong depth in each eye.
//
// Here the helmet line of sight is the 3D pit's own view direction (OTWDriver.headMatrix), so the
// same code serves the HMD, TrackIR and mouse look. Everything is drawn as a DIRECTION pushed out to
// optical infinity and projected with the renderer's current (per-eye) camera: no IPD term is
// needed at infinity, so both eyes agree by construction -- which is also where a real JHMCS
// focuses its symbology.

#include "stdhdr.h"
#include "hmcs.h"
#include "graphics/include/fflog.h"
#include "graphics/include/renderow.h"
#include "otwdrive.h"
#include "simdrive.h"
#include "aircrft.h"
#include "airframe.h"
#include "entity.h"
#include "classtbl.h"
#include "fcc.h"
#include "sms.h"
#include "missile.h"
#include "radar.h"
#include "sensclas.h"
#include "object.h" // SimObjectType
#include "hud.h"
#include "phyconst.h"

int g_nHmcs = 1; // 0 off, 1 HMS-flagged aircraft, 2 all aircraft
// The HMCS knob (left console, below CMDS): 0 = off, 1..HMCS_LEVELS-1 = symbology brightness.
// The cfg value is where the knob sits at launch.
int g_nHmcsLevel = 3;
static const int HMCS_LEVELS = 4;
static int s_lastOnLevel = 3; // what SimHmcsToggle turns back on to
bool g_bHmcsSlaveSeeker = true;
bool g_bHmcsSlaveRadar = true;
bool g_bHmcsHudBlank = true;
bool g_bHmcsLog = false;
float g_fHmcsHudHalfWidth = 12.5f; // deg; F-16 HUD total field of view, roughly
float g_fHmcsHudTop = 6.0f;
float g_fHmcsHudBottom = -16.0f;
float g_fHmcsScale = 1.0f;
float g_fHmcsTextScale = 1.5f; // glyph size; the font ladder tops out small for an eye buffer

extern float g_fTextScaleOverride;

// Class-table flag S.G. used for "this vehicle has an HMS" (see mislfcc.cpp / padlock.cpp).
static const unsigned long HMS_EQUIPPED_FLAG = 0x20000000;

// Last time the radar reported pointing ACM BORE down the helmet line of sight (ms, vuxRealTime).
static VU_TIME s_radarSlavedAt = 0;

void Hmcs_NoteRadarSlaved()
{
    s_radarSlavedAt = vuxRealTime;
}

void Hmcs_StepKnob(int dir)
{
    g_nHmcsLevel = max(0, min(HMCS_LEVELS - 1, g_nHmcsLevel + dir));

    if (g_nHmcsLevel > 0)
        s_lastOnLevel = g_nHmcsLevel;
}

void Hmcs_Toggle()
{
    if (g_nHmcsLevel > 0)
    {
        s_lastOnLevel = g_nHmcsLevel;
        g_nHmcsLevel = 0;
    }
    else
        g_nHmcsLevel = max(1, min(HMCS_LEVELS - 1, s_lastOnLevel));
}

bool Hmcs_Equipped(SimBaseClass* platform)
{
    if (not platform or g_nHmcs <= 0)
        return false;

    if (g_nHmcs >= 2)
        return true;

    VehicleClassDataType* vc =
        (VehicleClassDataType*)Falcon4ClassTable[platform->Type() -
                                                 VU_LAST_ENTITY_TYPE]
            .dataPtr;
    return vc and (vc->Flags bitand HMS_EQUIPPED_FLAG);
}

bool Hmcs_Cueing(SimBaseClass* platform)
{
    if (g_nHmcs <= 0 or g_nHmcsLevel <= 0)
        return false;

    AircraftClass* pa = SimDriver.GetPlayerAircraft();

    // Only the player wears the helmet, and only the 3D pit has a helmet line of sight to read.
    if (not pa or (platform and platform not_eq pa))
        return false;

    if (pa->mainPower == AircraftClass::MainPowerOff)
        return false;

    if (OTWDriver.GetOTWDisplayMode() not_eq OTWDriverClass::Mode3DCockpit)
        return false;

    return Hmcs_Equipped(pa);
}

bool Hmcs_GetLos(SimBaseClass* platform, float* az, float* el)
{
    if (not Hmcs_Cueing(platform))
        return false;

    // headMatrix columns are the head's [forward | right | up] in the body frame (x fwd, y right,
    // z down) -- see VCock_HeadCalc, which loads the HMD basis straight into them.
    const Trotation& h = OTWDriver.headMatrix;
    float fx = h.M11, fy = h.M21, fz = h.M31;
    float horiz = sqrtf(fx * fx + fy * fy);

    if (horiz < 1e-6f and fabsf(fz) < 1e-6f)
        return false;

    *az = (float)atan2(fy, fx);
    *el = (float)atan2(-fz, horiz);
    return true;
}

bool Hmcs_InHud(float az, float el)
{
    if (not g_bHmcsHudBlank)
        return false;

    float a = az * RTD, e = el * RTD;
    return fabsf(a) < g_fHmcsHudHalfWidth and e < g_fHmcsHudTop and
           e > g_fHmcsHudBottom;
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------

namespace
{
Tpoint V(float x, float y, float z)
{
    Tpoint p;
    p.x = x;
    p.y = y;
    p.z = z;
    return p;
}

Tpoint Add(const Tpoint& a, const Tpoint& b, float s)
{
    return V(a.x + b.x * s, a.y + b.y * s, a.z + b.z * s);
}

float Dot(const Tpoint& a, const Tpoint& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Tpoint Cross(const Tpoint& a, const Tpoint& b)
{
    return V(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
             a.x * b.y - a.y * b.x);
}

Tpoint Norm(const Tpoint& a)
{
    float n = sqrtf(Dot(a, a));
    return (n > 1e-6f) ? V(a.x / n, a.y / n, a.z / n) : a;
}

Tpoint AzEl(float az, float el)
{
    float ce = cosf(el);
    return V(ce * cosf(az), ce * sinf(az), -sinf(el));
}

void DirAzEl(const Tpoint& d, float* az, float* el)
{
    *az = (float)atan2(d.y, d.x);
    *el = (float)atan2(-d.z, sqrtf(d.x * d.x + d.y * d.y));
}

struct HmcsPainter
{
    RenderOTW* r;
    Tpoint fwd, right, up; // head basis in the body frame

    // A body-frame direction -> this eye's pixels. Pushed far out so it sits at infinity; the
    // camera position is ignored by TransformCameraCentricPoint anyway.
    bool Project(const Tpoint& d, float* px, float* py) const
    {
        Tpoint n = Norm(d);

        if (Dot(n, fwd) < 0.2f) // behind or at the edge of the eye: the 1/z blows up
            return false;

        Tpoint far_ = V(n.x * 10000.0f, n.y * 10000.0f, n.z * 10000.0f);
        ThreeDVertex v;
        r->TransformCameraCentricPoint(&far_, &v);
        *px = v.x;
        *py = v.y;
        return true;
    }

    // Local axes around any direction c, aligned with the head so symbols stay upright to the pilot.
    void Axes(const Tpoint& c, Tpoint* rt, Tpoint* upv) const
    {
        *rt = Norm(Add(right, c, -Dot(right, c)));
        *upv = Cross(*rt, c);
    }

    // Offset u deg right and v deg up of direction c.
    Tpoint At(const Tpoint& c, float u, float v) const
    {
        Tpoint rt, upv;
        Axes(c, &rt, &upv);
        float s = g_fHmcsScale * DTR;
        return Add(Add(c, rt, tanf(u * s)), upv, tanf(v * s));
    }

    void Line(const Tpoint& a, const Tpoint& b) const
    {
        float x1, y1, x2, y2;

        if (Project(a, &x1, &y1) and Project(b, &x2, &y2))
            r->Render2DLine(x1, y1, x2, y2);
    }

    void Seg(const Tpoint& c, float u1, float v1, float u2, float v2) const
    {
        Line(At(c, u1, v1), At(c, u2, v2));
    }

    void Circle(const Tpoint& c, float radDeg, int segs = 24,
                bool dashed = false) const
    {
        for (int i = 0; i < segs; i++)
        {
            if (dashed and (i bitand 1))
                continue;

            float a0 = 2.0f * PI * i / segs, a1 = 2.0f * PI * (i + 1) / segs;
            Seg(c, radDeg * cosf(a0), radDeg * sinf(a0), radDeg * cosf(a1),
                radDeg * sinf(a1));
        }
    }

    // align: -1 left edge at the point, 0 centred, +1 right edge at the point. Vertically centred.
    void Text(const Tpoint& d, const char* s, int align) const
    {
        float x, y;

        if (not Project(d, &x, &y))
            return;

        float w = (float)VirtualDisplay::ScreenTextWidth(s);
        float h = (float)VirtualDisplay::ScreenTextHeight();

        if (align == 0)
            x -= w * 0.5f;
        else if (align > 0)
            x -= w;

        r->ScreenText(x, y - h * 0.5f, s);
    }
};

const char* WeaponMnemonic(WeaponType t)
{
    switch (t)
    {
    case wtAim9:
        return "SRM";

    case wtAim120:
        return "MRM";

    case wtGuns:
        return "GUN";

    default:
        return "";
    }
}
} // namespace

void Hmcs_Draw(RenderOTW* renderer, const Trotation* headMatrix)
{
    AircraftClass* pa = SimDriver.GetPlayerAircraft();

    if (not renderer or not headMatrix or not pa or not Hmcs_Cueing(pa))
        return;

    HmcsPainter p;
    p.r = renderer;
    p.fwd = Norm(V(headMatrix->M11, headMatrix->M21, headMatrix->M31));
    p.right = Norm(V(headMatrix->M12, headMatrix->M22, headMatrix->M32));
    // Up from right x forward rather than the third column: with z down in the body frame that is
    // the one sign that cannot come out flipped whatever convention the column was loaded with.
    p.up = Cross(p.right, p.fwd);

    float losAz, losEl;
    DirAzEl(p.fwd, &losAz, &losEl);
    const bool losInHud = Hmcs_InHud(losAz, losEl);

    FireControlComputer* fcc = pa->FCC;
    SMSClass* sms = pa->Sms;
    RadarClass* radar = (RadarClass*)FindSensor(pa, SensorClass::Radar);
    SimObjectType* tgt = radar ? radar->CurrentTarget() : NULL;

    if (not tgt and fcc)
        tgt = fcc->TargetPtr();

    // Knob brightness: scale the HUD colour's RGB, keep its alpha.
    {
        static const float bright[HMCS_LEVELS] = {0.0f, 0.4f, 0.7f, 1.0f};
        DWORD col = TheHud ? TheHud->GetHudColor() : 0xFF00FF00;
        float k = bright[max(0, min(HMCS_LEVELS - 1, g_nHmcsLevel))];
        DWORD r = (DWORD)((col bitand 0xFF) * k), g = (DWORD)(((col >> 8) bitand 0xFF) * k),
              b = (DWORD)(((col >> 16) bitand 0xFF) * k);
        renderer->SetColor((col bitand 0xFF000000) bitor (b << 16) bitor (g << 8) bitor r);
    }
    const float savedTextScale = g_fTextScaleOverride;
    g_fTextScaleOverride = g_fHmcsTextScale;
    char buf[32];

    // ---- Head-stabilised: aiming cross + data block. Blanked while looking through the HUD. ----
    if (not losInHud)
    {
        const Tpoint& c = p.fwd;
        p.Seg(c, 0.6f, 0.0f, 2.0f, 0.0f);
        p.Seg(c, -0.6f, 0.0f, -2.0f, 0.0f);
        p.Seg(c, 0.0f, 0.6f, 0.0f, 2.0f);
        p.Seg(c, 0.0f, -0.6f, 0.0f, -2.0f);

        // Heading, top centre.
        float hdg = pa->Yaw() * RTD;

        while (hdg < 0.5f)
            hdg += 360.0f;

        while (hdg >= 360.5f)
            hdg -= 360.0f;

        sprintf_s(buf, "%03d", (int)(hdg + 0.5f));
        p.Text(p.At(c, 0.0f, 8.0f), buf, 0);

        // Airspeed and G / Mach, left.
        sprintf_s(buf, "%d", (int)(pa->GetKias() + 0.5f));
        p.Text(p.At(c, -8.0f, 3.0f), buf, -1);

        if (pa->af)
        {
            sprintf_s(buf, "%.1fG", pa->af->nzcgb);
            p.Text(p.At(c, -8.0f, -3.0f), buf, -1);
            sprintf_s(buf, "M%.2f", pa->af->mach);
            p.Text(p.At(c, -8.0f, -5.0f), buf, -1);
        }

        // Altitude, weapon and master arm, right.
        sprintf_s(buf, "%d", (int)(-pa->ZPos() + 0.5f));
        p.Text(p.At(c, 8.0f, 3.0f), buf, 1);

        if (sms)
        {
            const char* mn = WeaponMnemonic(sms->curWeaponType);

            if (*mn)
            {
                if (sms->curWeaponType == wtGuns)
                    sprintf_s(buf, "%s", mn);
                else
                    sprintf_s(buf, "%s %d", mn, sms->NumCurrentWpn());

                p.Text(p.At(c, 8.0f, -3.0f), buf, 1);
            }

            const char* arm = (sms->MasterArm() == SMSClass::Arm) ? "ARM" :
                              (sms->MasterArm() == SMSClass::Sim) ? "SIM" :
                                                                    "SAFE";
            p.Text(p.At(c, 8.0f, -5.0f), arm, 1);
        }

        if (tgt and tgt->localData)
        {
            sprintf_s(buf, "%.1f", tgt->localData->range * FT_TO_NM);
            p.Text(p.At(c, 0.0f, -8.0f), buf, 0);
        }

        // ACM BORE pointed down the helmet line of sight: the radar's cue circle round the cross.
        if (not tgt and vuxRealTime - s_radarSlavedAt < 250)
            p.Circle(c, 3.0f, 24, true);
    }

    // ---- World-stabilised: target designator box / locator line, AIM-9 seeker circle. ----
    if (tgt and tgt->localData)
    {
        float taz = tgt->localData->az, tel = tgt->localData->el;
        Tpoint t = AzEl(taz, tel);
        float off = acosf(max(-1.0f, min(1.0f, Dot(t, p.fwd)))) * RTD;

        if (off < 10.0f * g_fHmcsScale)
        {
            if (not Hmcs_InHud(taz, tel))
            {
                const float b = 1.2f;
                p.Seg(t, -b, -b, b, -b);
                p.Seg(t, b, -b, b, b);
                p.Seg(t, b, b, -b, b);
                p.Seg(t, -b, b, -b, -b);
            }
        }
        else if (not losInHud)
        {
            // Off the display: a line from the cross pointing at the target, and how far off it is.
            float u = Dot(t, p.right), v = Dot(t, p.up);
            float n = sqrtf(u * u + v * v);

            if (n > 1e-4f)
            {
                u /= n;
                v /= n;
                p.Seg(p.fwd, u * 2.5f, v * 2.5f, u * 7.0f, v * 7.0f);
                sprintf_s(buf, "%d", (int)(off + 0.5f));
                p.Text(p.At(p.fwd, u * 8.5f, v * 8.5f), buf, 0);
            }
        }
    }

    if (sms and fcc and sms->curWeaponType == wtAim9)
    {
        MissileClass* msl = (MissileClass*)sms->GetCurrentWeapon();
        float saz = fcc->missileSeekerAz, sel = fcc->missileSeekerEl;

        if (msl and not Hmcs_InHud(saz, sel))
        {
            Tpoint s = AzEl(saz, sel);
            p.Circle(s, 1.5f, 20);

            // Tracking: a steady inner ring, flashing while still caged (tone, but not yet uncaged).
            if (msl->targetPtr and (not msl->isCaged or (vuxRealTime bitand 0x100)))
                p.Circle(s, 0.6f, 12);
        }
    }

    g_fTextScaleOverride = savedTextScale;

    if (g_bHmcsLog)
    {
        static VU_TIME lastLog = 0;

        if (vuxRealTime - lastLog > 1000)
        {
            lastLog = vuxRealTime;
            char line[256];
            sprintf_s(line,
                      "HMCS los az=%.1f el=%.1f inHud=%d tgt=%d seeker az=%.1f el=%.1f radarSlaved=%d\n",
                      losAz * RTD, losEl * RTD, (int)losInHud, tgt ? 1 : 0,
                      fcc ? fcc->missileSeekerAz * RTD : 0.0f,
                      fcc ? fcc->missileSeekerEl * RTD : 0.0f,
                      (int)(vuxRealTime - s_radarSlavedAt < 250));
            FFDebugLog(line);
        }
    }
}
