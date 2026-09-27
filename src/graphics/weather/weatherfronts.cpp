/***************************************************************************\
    WeatherFronts.cpp
    Artscout - 2026 (FRONTS)

    See weatherfronts.h. Nothing here touches the renderer or the campaign:
    it is the field and nothing else, so both can use it.
\***************************************************************************/
#include <ciso646>
#include <math.h>
#include <string.h>
#include "weatherfronts.h"

WeatherFrontMap::WeatherFrontMap()
{
    active = false;
    Clear();
}

void WeatherFrontMap::Clear(float prevailing)
{
    memset(&s, 0, sizeof(s));
    s.prevailing = prevailing;
    s.noiseAmp = 0.f;
    s.noiseScale = 400000.f;
}

int WeatherFrontMap::Condition(float severity)
{
    int c = (int)floor(severity + 0.5f);

    if (c < 1)
        c = 1;

    if (c > 4)
        c = 4;

    return c;
}

int WeatherFrontMap::Cover(float severity)
{
    // 1 -> 0, 2 -> 3, 3 -> 6, 4 -> 8
    float c = (severity - 1.f) * 3.f;

    if (c > 8.f)
        c = 8.f;

    if (c < 0.f)
        c = 0.f;

    return (int)(c + 0.5f);
}

float WeatherFrontMap::LifeStrength(const WeatherFront &f, unsigned int now)
{
    if (f.life == 0 or now < f.born)
        return 0.f;

    float t = (float)(now - f.born) / (float)f.life;

    if (t >= 1.f)
        return 0.f;

    // Builds over the first 15%, holds, dies away over the last 25%.
    if (t < 0.15f)
        return t / 0.15f;

    if (t > 0.75f)
        return (1.f - t) / 0.25f;

    return 1.f;
}

void WeatherFrontMap::CorePosition(const WeatherFront &f, unsigned int now,
                                   float *x, float *y) const
{
    double secs = (now > f.born) ? (double)(now - f.born) * 0.001 : 0.0;
    *x = (float)(f.x + cos(f.heading) * f.speed * secs);
    *y = (float)(f.y + sin(f.heading) * f.speed * secs);
}

// 0..1: how much of this front's core reaches the point.
float WeatherFrontMap::Profile(const WeatherFront &f, float x, float y,
                               unsigned int now) const
{
    float life = LifeStrength(f, now);

    if (life <= 0.f)
        return 0.f;

    float cx, cy;
    CorePosition(f, now, &cx, &cy);

    float dx = x - cx, dy = y - cy;
    float ch = (float)cos(f.heading), sh = (float)sin(f.heading);
    float u = dx * ch + dy * sh; // along the direction of travel
    float v = -dx * sh + dy * ch; // along the band

    float w = f.halfWidth;

    if (w < 1000.f)
        w = 1000.f;

    if (f.halfLength <= 0.f)
    {
        // Round: a cell or a clearing.
        float r2 = (u * u + v * v) / (w * w);

        if (r2 > 9.f)
            return 0.f;

        return life * (float)exp(-r2);
    }

    // Band, asymmetric across it. A cold front is steep ahead and drags its
    // weather behind; a warm front is the other way round, its cloud spreading
    // far ahead of the surface line. The sign of tempDelta says which: a
    // front that cools is a cold front.
    float ahead = (f.tempDelta <= 0.f) ? 0.6f : 1.6f;
    float behind = (f.tempDelta <= 0.f) ? 1.5f : 0.7f;
    float k = (u >= 0.f) ? w * ahead : w * behind;
    float a = u / k;

    if (a * a > 9.f)
        return 0.f;

    float across = (float)exp(-a * a);

    // Taper past the ends.
    float e = (float)fabs(v) - f.halfLength;
    float along = 1.f;

    if (e > 0.f)
    {
        float b = e / w;

        if (b > 3.f)
            return 0.f;

        along = (float)exp(-b * b);
    }

    return life * across * along;
}

// Smooth value noise, two octaves, -1..1. Hash of integer lattice points.
static inline float Lattice(int ix, int iy, unsigned int seed)
{
    unsigned int h = (unsigned int)ix * 374761393u +
                     (unsigned int)iy * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xffff) / 32767.5f - 1.f;
}

static float ValueNoise(double x, double y, unsigned int seed)
{
    double fx = floor(x), fy = floor(y);
    int ix = (int)fx, iy = (int)fy;
    float tx = (float)(x - fx), ty = (float)(y - fy);

    // Smoothstep so there are no creases along the lattice lines.
    tx = tx * tx * (3.f - 2.f * tx);
    ty = ty * ty * (3.f - 2.f * ty);

    float a = Lattice(ix, iy, seed), b = Lattice(ix + 1, iy, seed);
    float c = Lattice(ix, iy + 1, seed), d = Lattice(ix + 1, iy + 1, seed);

    float ab = a + (b - a) * tx;
    float cd = c + (d - c) * tx;
    return ab + (cd - ab) * ty;
}

float WeatherFrontMap::Noise(float x, float y, unsigned int now) const
{
    if (s.noiseAmp <= 0.f or s.noiseScale <= 0.f)
        return 0.f;

    // The patches ride the wind. Over hours they also change shape: the
    // field blends between two seeds on a six hour cycle, so the weather over
    // a spot does not only arrive from upwind, it also forms and dissolves.
    double secs = (double)now * 0.001;
    double px = ((double)x - s.driftX * secs) / s.noiseScale;
    double py = ((double)y - s.driftY * secs) / s.noiseScale;

    double cycle = secs / (6.0 * 3600.0);
    unsigned int epoch = (unsigned int)floor(cycle);
    float m = (float)(cycle - floor(cycle));
    m = m * m * (3.f - 2.f * m);

    float n0 = ValueNoise(px, py, s.seed + epoch) * 0.7f +
               ValueNoise(px * 2.3, py * 2.3, s.seed + epoch + 101) * 0.3f;
    float n1 = ValueNoise(px, py, s.seed + epoch + 1) * 0.7f +
               ValueNoise(px * 2.3, py * 2.3, s.seed + epoch + 102) * 0.3f;

    // The blend of octaves and seeds leaves a spread of about 0.29; scale it
    // so noiseAmp reads as roughly half a standard deviation per unit, i.e.
    // at 1.0 about a third of the map sits a condition away from prevailing.
    return (n0 + (n1 - n0) * m) * s.noiseAmp * 1.75f;
}

float WeatherFrontMap::Severity(float x, float y, unsigned int now) const
{
    float sev = s.prevailing;

    if (active)
    {
        sev += Noise(x, y, now);

        // Fronts combine as the strongest of each sign rather than a sum, so
        // two fronts crossing do not stack into a permanent Inclement.
        float up = 0.f, down = 0.f;

        for (unsigned int i = 0; i < s.count and i < FRONTS_MAX; i++)
        {
            const WeatherFront &f = s.fronts[i];
            float p = Profile(f, x, y, now);

            if (p <= 0.f)
                continue;

            float add = f.severity * p;

            if (add > up)
                up = add;

            if (add < down)
                down = add;
        }

        sev += up + down;
    }

    if (sev < 1.f)
        sev = 1.f;

    if (sev > 4.49f)
        sev = 4.49f;

    return sev;
}

void WeatherFrontMap::Extras(float x, float y, unsigned int now,
                             float *windKts, float *tempC) const
{
    float w = 0.f, t = 0.f;

    if (active)
    {
        for (unsigned int i = 0; i < s.count and i < FRONTS_MAX; i++)
        {
            const WeatherFront &f = s.fronts[i];
            float p = Profile(f, x, y, now);

            if (p <= 0.f)
                continue;

            if (f.windBoost * p > w)
                w = f.windBoost * p;

            t += f.tempDelta * p;
        }
    }

    if (windKts)
        *windKts = w;

    if (tempC)
        *tempC = t;
}

void WeatherFrontMap::Expire(unsigned int now)
{
    unsigned int n = 0;

    for (unsigned int i = 0; i < s.count and i < FRONTS_MAX; i++)
    {
        const WeatherFront &f = s.fronts[i];

        if (now >= f.born and now - f.born >= f.life)
            continue;

        s.fronts[n++] = f;
    }

    for (unsigned int i = n; i < FRONTS_MAX; i++)
        memset(&s.fronts[i], 0, sizeof(WeatherFront));

    s.count = n;
}

bool WeatherFrontMap::Add(const WeatherFront &f)
{
    if (s.count >= FRONTS_MAX)
        return false;

    s.fronts[s.count++] = f;
    return true;
}
