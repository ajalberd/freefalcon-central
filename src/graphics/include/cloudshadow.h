/***************************************************************************\
    cloudshadow.h
    Artscout - 2026

    Cumulus shadows on the ground, on objects and in the pit.

    RealWeather::UpdateCloudShadow rasterises the cumulus puffs it draws into a
    top-down cover mask (weather-local, so the wind moves only the origin and
    the mask is rebuilt only when the cells change), and publishes it here.
    The terrain pass reads it through a bindless slot (ffterrain.hlsl,
    TerrainCloudShadow); the object pass through t7 (ffemu.hlsl,
    CloudSunShadow). Both darken the SUN term only.
\***************************************************************************/
#ifndef _CLOUDSHADOW_H_
#define _CLOUDSHADOW_H_

struct ID3D11ShaderResourceView;

struct CloudShadowParams
{
    // xy = the mask's world origin (feet), z = the cloud plane's world z
    // (z DOWN), w = 1 / the mask's world size (feet).
    float p0[4];
    // xyz = unit vector toward the sun (world, z DOWN), w = strength
    // (0 = no cloud shadow this frame; the shaders then never read the mask).
    float p1[4];
    ID3D11ShaderResourceView *mask;
};

// This frame's parameters. Never null; p1[3] == 0 when there is nothing to cast.
const CloudShadowParams &CloudShadow_Current();

#endif // _CLOUDSHADOW_H_
