/***************************************************************************\
    WeatherFronts.h
    Artscout - 2026 (FRONTS)

    Weather that varies across the theater.

    Falcon 4.0 kept a weather map per grid square; the 2003 rewrite replaced
    it with a single condition for the whole world and left GetCloudCover(x,y)
    behind as stubs. This puts the spatial part back, as a field rather than a
    map: a "severity" value at any point, computed from

        prevailing condition  +  drifting random patches  +  moving fronts

    Severity is on the condition scale (1 Sunny .. 4 Inclement), continuous,
    so the renderer can shade toward the next condition before it switches.

    The field is analytic: it needs only the parameters below and the
    campaign time, so every machine in a multiplayer game grows the same
    weather from one small message, and the save file carries a few hundred
    bytes rather than a grid.

    Coordinates are sim feet (x north, y east). Times are campaign ms.
\***************************************************************************/
#ifndef _WEATHERFRONTS_H_
#define _WEATHERFRONTS_H_

#pragma pack(push, 1)

enum
{
    FRONT_COLD = 0, // long band, sharp leading edge, rain behind it
    FRONT_WARM, // long band, wide and shallow, overcast ahead of it
    FRONT_SQUALL, // short line of storms
    FRONT_CELL, // round storm cell
    FRONT_HIGH, // clearing: a patch of better weather
    FRONT_NUM_KINDS
};

// 44 bytes, fixed layout: it is written to .wth files and sent in the weather
// message as is. Add fields only at the end, and bump FRONTS_VERSION.
struct WeatherFront
{
    float x, y; // centre at the moment it was born (sim ft)
    float heading; // direction of travel, radians, sim frame
    float speed; // ft/s
    float halfWidth; // across the band, ft (the direction of travel)
    float halfLength; // along the band, ft; 0 = round
    float severity; // added at the core, condition steps (negative clears)
    float windBoost; // knots added at the core
    float tempDelta; // deg C added at the core
    unsigned int born; // campaign ms
    unsigned int life; // ms
};

#define FRONTS_MAX 8
#define FRONTS_VERSION 1

// The whole state. Also fixed layout, for the same reason.
struct WeatherFrontState
{
    float prevailing; // 1..4, the condition the theater leans to
    float noiseAmp; // strength of the random patches, condition steps
    float noiseScale; // size of a patch, ft
    unsigned int seed; // picks the random patches
    float driftX, driftY; // ft/s the random patches move at (the wind)
    unsigned int count;
    WeatherFront fronts[FRONTS_MAX];
};

#pragma pack(pop)

class WeatherFrontMap
{
public:
    WeatherFrontMap();

    void Clear(float prevailing = 1.f);

    // Severity 1..4.49 at a point. Fractions matter: the renderer uses them.
    float Severity(float x, float y, unsigned int now) const;

    // Extra wind (knots) and temperature (deg C) the fronts put at a point.
    void Extras(float x, float y, unsigned int now, float *windKts,
                float *tempC) const;

    // Discrete condition for a severity, with no hysteresis.
    static int Condition(float severity);

    // 0 (clear) .. 8 (solid) for a severity: Falcon's old cloud-cover scale.
    static int Cover(float severity);

    // Where a front's core is now.
    void CorePosition(const WeatherFront &f, unsigned int now, float *x,
                      float *y) const;

    // 0..1 how far through its life, and its strength at this moment.
    static float LifeStrength(const WeatherFront &f, unsigned int now);

    // Drop fronts that have run their course.
    void Expire(unsigned int now);

    bool Add(const WeatherFront &f);

public:
    WeatherFrontState s;
    bool active; // false: the field is flat at 'prevailing'

private:
    float Profile(const WeatherFront &f, float x, float y,
                  unsigned int now) const;
    float Noise(float x, float y, unsigned int now) const;
};

#endif // _WEATHERFRONTS_H_
