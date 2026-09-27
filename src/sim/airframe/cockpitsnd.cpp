// Artscout - 2026: the player's continuous cockpit sounds.
//
// ECS hum, pilot breathing, buffet, the wheel brakes and the ILS marker beacons. All but the ECS
// start/end takes are LOOPED sounds, and a looped voice in this engine plays only while it is re-armed
// every frame (mlrVoice::Exec stops it on the first frame it is not). So each sound below is simply
// "re-arm while the condition holds": there is no stop call anywhere, and nothing can be left running.
// The recordings and their table levels are BMS 4.38's (f4sndtbl.txt ids 300..311).

#include "stdhdr.h"
#include "airframe.h"
#include "aircrft.h"
#include "simdrive.h"
#include "fsound.h"
#include "soundfx.h"
#include "navsystem.h"

extern int g_nPilotBreathing;
extern float g_fSeatTravel;

// Seat height. g_nSeatMove is set by the SimSeatUp/SimSeatDown commands (-1 down, 0 stopped, +1 up);
// g_fSeatHeight is the result in FEET above neutral, read by VCock_HeadCalc to raise the eyepoint.
// Globals rather than airframe members because the seat outlives the jet: it keeps its setting from one
// flight to the next, as your own seat would.
int g_nSeatMove = 0;
float g_fSeatHeight = 0.0f;

namespace
{
    float Clamp01(float v)
    {
        return (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
    }

    // Amplitude 0..1 to the sound table's hundredths of a dB (0 = full, -10000 = silent).
    float AmpToVol(float amp)
    {
        return (amp <= 0.01f) ? -4000.0f : 2000.0f * (float)log10(amp);
    }
}

void AirframeClass::CockpitSounds(void)
{
    if (not platform or platform not_eq SimDriver.GetPlayerEntity() or not SFX_DEF)
        return;

    const float dt = SimLibMajorFrameTime;
    const bool inAir = IsSet(InAir);

    // ECS: bleed air from a running engine, through an air source that feeds the pit. The start take
    // is about a second long, so the loop takes over just before it ends; switching the air off (or
    // the engine stopping) plays the spool-down. The knob itself only clicks -- see 3dbuttons.dat.
    const bool ecsOn = (airSource == AS_NORM or airSource == AS_DUMP) and
                       not IsSet(EngineStopped) and rpm >= 0.6f;

    if (ecsOn and not ecsSoundOn)
    {
        platform->SoundPos.Sfx(SFX_ECS_START);
        ecsSoundTime = 0.0f;
    }
    else if (not ecsOn and ecsSoundOn)
    {
        platform->SoundPos.Sfx(SFX_ECS_END);
    }

    if (ecsOn)
    {
        ecsSoundTime += dt;

        if (ecsSoundTime >= 0.9f)
            platform->SoundPos.Sfx(SFX_ECS_LOOP);
    }

    ecsSoundOn = ecsOn;

    // Pilot breathing through the mask. PilotBreathing 0 = off, 1 = only under G (default), 2 = calm
    // breathing all the time as well. Fast from about 4 G, straining from about 6.5, each with a
    // little hysteresis so a pull hovering on a threshold does not flip between recordings.
    if (g_nPilotBreathing > 0)
    {
        const float g = nzcgb;
        int level = breathSoundLevel;

        if (g >= 6.5f)
            level = 3;
        else if (g >= 4.0f and level < 2)
            level = 2;
        else if (level == 3 and g < 6.0f)
            level = 2;
        else if (level == 2 and g < 3.5f)
            level = 1;

        if (level < 1)
            level = 1;

        breathSoundLevel = level;

        if (level == 3)
            platform->SoundPos.Sfx(SFX_BREATH_STRAIN);
        else if (level == 2)
            platform->SoundPos.Sfx(SFX_BREATH_FAST);
        else if (g_nPilotBreathing >= 2)
            platform->SoundPos.Sfx(SFX_BREATH_CALM);
    }

    // Buffet: sets in around 13 degrees of AoA and is heavy by 22, and is stronger the more air there
    // is over the wing -- next to nothing at 100 knots, full from about 250.
    if (inAir and vcas > 80.0f)
    {
        const float aoa = Clamp01((alpha - 13.0f) / 9.0f);
        const float q = Clamp01(0.3f + 0.7f * (vcas - 100.0f) / 150.0f);
        const float amp = aoa * q;

        if (amp > 0.02f)
            platform->SoundPos.Sfx(SFX_BUFFET, 0, 1.0f, AmpToVol(amp));
    }

    // Wheel brakes: the rumble of the brakes working on the roll-out and while taxiing. Louder and a
    // little higher with speed; the tyre squeal above 80 knots (eom.cpp) still plays on top.
    if (not inAir and IsSet(WheelBrakes) and not IsSet(GearBroken) and gearPos >= 0.8f)
    {
        const float kts = vt / KNOTS_TO_FTPSEC;

        if (kts > 3.0f)
        {
            const float s = Clamp01(kts / 100.0f);
            platform->SoundPos.Sfx(SFX_WHEEL_BRAKE, 0, 0.8f + 0.4f * s,
                                   AmpToVol(0.25f + 0.75f * s));
        }
    }

    // Seat height: about an inch a second (the ACES II covers its 5 in in ~5.5 s, which is also how long
    // BMS's motor recordings run), stopping at the ends of its travel. The motor sounds while it moves.
    if (g_nSeatMove not_eq 0)
    {
        const float travel = ((g_fSeatTravel > 0.0f) ? g_fSeatTravel : 0.0f) / 12.0f;
        const float rate = (5.0f / 12.0f) / 5.5f;
        float h = g_fSeatHeight + (float)g_nSeatMove * rate * dt;

        if (h >= travel or h <= -travel)
        {
            h = (h > 0.0f) ? travel : -travel;
            g_nSeatMove = 0; // end stop
        }
        else
        {
            platform->SoundPos.Sfx((g_nSeatMove > 0) ? SFX_SEAT_UP : SFX_SEAT_DOWN);
        }

        g_fSeatHeight = h;
    }

    // Airflow, BMS 4.38's cockpit layers. FF6 plays its wind (SFX_WIND) only in the outside views, so the
    // pit was silent of air noise altogether. Thresholds follow BMS's own table notes: the low-speed AoA
    // layer is "audible over Mach .6 at 10 deg", the high-speed one "over Mach .8 at 8 deg".
    {
        const float kts = vt / KNOTS_TO_FTPSEC;

        // dynamic pressure on the canopy: in from about 80 kt, full by 500; pitch rises with it
        const float q = Clamp01((vcas - 80.0f) / 420.0f);

        if (q > 0.02f)
            platform->SoundPos.Sfx(SFX_AIRFLOW_CANOPY, 0, 0.85f + 0.3f * q, AmpToVol(q));

        if (inAir)
        {
            const float lowAoa = Clamp01((alpha - 6.0f) / 6.0f) * Clamp01((mach - 0.45f) / 0.2f);
            const float highAoa = Clamp01((alpha - 4.0f) / 5.0f) * Clamp01((mach - 0.72f) / 0.12f);

            if (lowAoa > 0.02f)
                platform->SoundPos.Sfx(SFX_AOA_LOWSPEED, 0, 1.0f, AmpToVol(lowAoa));

            if (highAoa > 0.02f)
                platform->SoundPos.Sfx(SFX_AOA_HIGHSPEED, 0, 1.0f, AmpToVol(highAoa));

            // the gear in the airstream: scales with how far it is down and with airspeed
            const float gear = Clamp01(gearPos) * Clamp01((vcas - 80.0f) / 170.0f);

            if (gear > 0.02f)
                platform->SoundPos.Sfx(SFX_GEAR_WIND, 0, 1.0f, AmpToVol(gear));
        }

        // canopy up: the wind in the pit, from a fast taxi upward
        if (canopyState)
        {
            const float w = Clamp01(kts / 120.0f);

            if (w > 0.02f)
                platform->SoundPos.Sfx(SFX_WIND_CANOPY_OPEN, 0, 1.0f, AmpToVol(w));
        }
    }

    // Marker beacons, placed on the tuned ILS (NavigationSystem::GetMarkerBeacon).
    if (inAir and gNavigationSys)
    {
        const int marker = gNavigationSys->GetMarkerBeacon();

        if (marker == 1)
            platform->SoundPos.Sfx(SFX_OUTER_MARKER);
        else if (marker == 2)
            platform->SoundPos.Sfx(SFX_MIDDLE_MARKER);
    }
}
