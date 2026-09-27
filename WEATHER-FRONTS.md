# Weather fronts

Weather that varies across the theater. **Written and built, not yet flown.** The first
flight's `FFDebug.log` `FRONTS:` lines are the evidence to read before changing anything.

## What was there

One condition for the whole world (1 Sunny .. 4 Inclement), set from Setup → Graphics.
Falcon 4.0 had a per-grid-square weather map; the Nov 2003 Cobra/Jammer rewrite replaced it
and left `WeatherClass::GetCloudCover(x,y)` / `GetCloudLevel` / `Set*` as empty stubs. The
satellite spotting check in `campbase.cpp` that used cloud cover was commented out
(`//JAM 20Nov03 - FIXME`), so no weather ever hid anything. The renderer's 9x9 cells of
~17 km around the viewer (`RealWeather::weatherCellArray`) were only ever random cloud
placement under that one condition.

Two bugs found on the way, both fixed:

- `otwloop.cpp` played the rain sound off the global `int weatherCondition` in
  `winmain.cpp`, which nothing sets, so rain was always silent.
- `eom.cpp` asked for the wind at `&gndNormal` (a normal vector) rather than the aircraft's
  position. Harmless while wind was global, wrong once it is not.

## The model

`src/graphics/include/weatherfronts.h`, `src/graphics/weather/weatherfronts.cpp`. A
**severity** field on the condition scale, continuous, analytic:

    prevailing  +  random patches (value noise, drifts with the wind,
                   re-forms on a 6 h cycle)  +  fronts (strongest up + strongest down)

clamped to 1 .. 4.49. Fronts are cold/warm bands, squall lines, storm cells and clearings,
each a position at birth, heading, speed, size, severity, gust, temperature change, birth
time and life (builds 15%, fades 25%). The whole state is 380 bytes, fixed layout, and
depends only on campaign time. MP clients therefore grow the same weather from it
(`FalconWeatherMessage` now carries it), and the save carries it verbatim.

`ffcamp/weather.py` is a line-for-line Python port for the editor. `selftest.py` checks it
against 13 severities the C++ produced, and it matched to 2e-6 over 1200 points. **Change
one and you must change the other.**

Simulated over 4 days at the defaults (3 fronts/day, patches 0.8, noise normalised ×1.75),
the theater comes out Sunny/Fair/Poor/Inclement % ≈ 82/16/3/0 on a Sunny prevailing,
11/70/16/3 on Fair, and 0/11/70/18 on Poor. A given spot changes condition 4–5 times a day.

## How the game uses it

- **Renderer** (`RealWeather::SampleLocal`, per frame): `weatherCondition` becomes the
  condition *where the viewer is*, with ±0.08 hysteresis at each boundary. The fraction
  shades within the condition: Sunny/Fair `ShadingFactor`; the Poor deck lowers 15,000 →
  8,000 ft into a front; Inclement's `WeatherQuality`. `overcastFade` thins the overcast,
  its lighting drop and its fog for the first 0.35 past the Fair/Poor edge, so crossing a
  front is not one step. A change of condition re-runs the haze setup
  (`RenderOTW::RefreshWeatherHaze`), which was only ever set at renderer setup.
- **Distant cloud**: while the local condition is Sunny or Fair, each 17 km cell asks the
  field for its own severity. Cells at Fair or worse draw cumulus, darker and larger toward
  Poor, so a front shows as a bank of cloud up to ~60 km off.
- **Gameplay**: `GetCloudCover` returns 0..8 from the field. Satellites no longer spot
  through Poor or worse (the restored check). Briefing `CLOUD_TYPE` / `CLOUD_BASE` and
  `#IF_CLEAR_WEATHER` use the target's weather. Ship lights and vehicle dust use the
  unit's own weather. Wind speed and temperature by position add the fronts' gusts and
  temperature change, so the flight model, bombs and chaff feel them.
- **Evolution** (master only, `UpdateWeather` every 30 campaign s): fronts expire, and new
  ones arrive from the upwind edge at `g_fWeatherFrontsPerDay`. Storm cells and squall lines
  form in place. The old whole-map condition random walk is off while fronts are on.
- **Setup's weather choice** sets the *prevailing* condition; the fronts stay.

## Files

`.wth` gains a block after the two zero map dimensions (offset 37) the game has always
written: `"FRNT"`, u32 version 1, u32 active, the 380-byte state, then f32 windHeading
(rad, direction clouds move *to*), windSpeed (km/h), temperature. Old readers stop before
it. **A campaign reads nothing else from `.wth`**, so this block is the only way a
campaign keeps its weather across a save, prevailing condition included. A campaign file
without it behaves as before: the Setup condition, plus freshly seeded fronts.

Three `.wth` layouts exist in the shipped files. The editor keeps each one's bytes unless
told otherwise: Cobra 37 bytes; Cobra with the last two floats swapped (Tacedit writes
that); and Cobra/older followed by Tacedit's 128x128 weather map (32805 bytes), which
nothing has read since 2003 and which is dropped only when a fronts block is written in
its place.

## Knobs (`FFViper.cfg`)

    set g_nWeatherFronts 1          // 0 = one condition everywhere, the old behaviour
    set g_fWeatherFrontsPerDay 3.0  // new fronts per campaign day
    set g_fWeatherNoise 0.8         // random patches: 0 none, 1 ~a third of the map a step off

## Editor

Weather tab (`tools/campaign-editor/web/weather.js`). It shows the field over the map and
previews it up to 48 h ahead (the server runs the model). You can place, aim, size or remove
fronts, generate them the way the game does, reroll the patches, and set the prevailing
condition, wind, temperature and cloud bases.

## Not done / to watch

- Flown by nobody yet. Watch for: haze pop on condition change, the overcast appearing
  under you at a front edge, and dark cumulus banks sitting too low (`frontCumulusZ`
  floors the base at 8,000 ft when the local condition never set one).
- Only cumulus is drawn per cell. Overcast, fog and lighting are still one value at the
  viewer, so from inside Fair you see a Poor area as towering cloud, not as an overcast
  deck.
- AI aircraft far from the player still use the player's `stratusZ` wherever they read it
  directly (turbulence reads it for everyone).
- The weather message grew by ~385 bytes, so a mixed-build multiplayer game will not agree
  on it.
