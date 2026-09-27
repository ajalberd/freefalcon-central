// Artscout - 2026: GT7 tone mapping for the D3D12 HDR scene (see RENDER-LIGHTING.md, "HDR + GT7").
//
// One fullscreen program, two jobs, picked by gMode:
//   gMode 1 -- TONE MAP IN PLACE. The FP16 scene is copied aside and this pass writes the tone-mapped
//              image back into the scene target, so the 2D overlays drawn after it (HUD, 2D pit, text)
//              land on display-referred values and are never curved themselves.
//   gMode 0 -- OUTPUT. FP16 scene -> the 8-bit back buffer / XR eye image, clamped. A frame whose tone
//              map never ran (splash, menu viewer, view instancing) therefore looks exactly as it did
//              before the HDR target existed: values above 1 simply clip, as the 8-bit target did.
//
// Units. The engine shades in sRGB-ENCODED space (textures, vertex colours and lights are all gamma
// values; the XR blit comment in ffxrblit.hlsl says the same). So the curve decodes to linear first,
// runs on linear Rec.709 lifted into Rec.2020 (the space the GT7 operator is defined in), and re-encodes.
// gExposure already carries the paper-white factor: scene 1.0 lands on GT7's SDR paper white (250 nits,
// 2.5 frame-buffer units), which keeps mid-tones on the curve's linear section -- i.e. the current look
// is preserved below ~0.7 and only highlights change.
//
// The operator is a direct port of Polyphony Digital's reference implementation:
//   gt7_tone_mapping.cpp v1.0 (2025-08-10), s2025 PBS course supplemental, ICtCp UCS.
//   MIT License, Copyright (c) 2025 Polyphony Digital Inc.
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
//   associated documentation files (the "Software"), to deal in the Software without restriction,
//   including without limitation the rights to use, copy, modify, merge, publish, distribute,
//   sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
//   furnished to do so, subject to the following conditions: The above copyright notice and this
//   permission notice shall be included in all copies or substantial portions of the Software.
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.
// Parameters are the reference defaults (curve 0.25 / 0.538 / 0.444 / 1.280, blend 0.6, fade
// 0.98..1.16); do not "tune" them here -- exposure is the knob (g_fToneMapExposure).
#pragma once

static const char* kGT7ToneMapHlsl = R"HLSL(
Texture2D<float4> gScene : register(t0);

cbuffer ToneMapCB : register(b0)
{
    float gExposure; // scene-linear -> GT7 frame-buffer units (paper white * user exposure)
    float gMode;     // 1 = GT7 in place, 0 = clamp-only output
    float2 gPad;
};

struct VSOut
{
    float4 pos : SV_POSITION;
};

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

// --- sRGB transfer, extended above 1 (the FP16 scene holds encoded values that may exceed 1) ---
float3 SrgbToLinear(float3 s)
{
    s = max(s, 0.0f);
    float3 lo = s / 12.92f;
    float3 hi = pow((s + 0.055f) / 1.055f, 2.4f);
    return (s <= 0.04045f) ? lo : hi;
}

float3 LinearToSrgb(float3 l)
{
    l = saturate(l);
    float3 lo = l * 12.92f;
    float3 hi = 1.055f * pow(l, 1.0f / 2.4f) - 0.055f;
    return (l <= 0.0031308f) ? lo : hi;
}

// --- Rec.709 <-> Rec.2020 (linear) ---
float3 Rec709ToRec2020(float3 c)
{
    return float3(dot(c, float3(0.627404f, 0.329283f, 0.043313f)),
                  dot(c, float3(0.069097f, 0.919541f, 0.011362f)),
                  dot(c, float3(0.016391f, 0.088013f, 0.895595f)));
}

float3 Rec2020ToRec709(float3 c)
{
    return float3(dot(c, float3(1.660491f, -0.587641f, -0.072850f)),
                  dot(c, float3(-0.124551f, 1.132900f, -0.008349f)),
                  dot(c, float3(-0.018151f, -0.100579f, 1.118730f)));
}

// --- GT7 reference (frame-buffer units: 1.0 = 100 cd/m^2) ---
static const float kRefLum = 100.0f;
static const float kSdrPaperWhite = 250.0f;
static const float kPeak = kSdrPaperWhite / kRefLum; // SDR: curve peak = paper white = 2.5

float SmoothStepGT(float x, float e0, float e1)
{
    if (x < e0) return 0.0f;
    if (x > e1) return 1.0f;
    float t = (x - e0) / (e1 - e0);
    return t * t * (3.0f - 2.0f * t);
}

// GTToneMappingCurveV2::evaluateCurve with initializeCurve(kPeak, 0.25, 0.538, 0.444, 1.280).
float GTCurve(float x)
{
    const float alpha = 0.25f, midPoint = 0.538f, linearSection = 0.444f, toeStrength = 1.280f;
    const float k  = (linearSection - 1.0f) / (alpha - 1.0f);
    const float kA = kPeak * linearSection + kPeak * k;
    const float kB = -kPeak * k * exp(linearSection / k);
    const float kC = -1.0f / (k * kPeak);
    if (x < 0.0f)
        return 0.0f;
    float weightLinear = SmoothStepGT(x, 0.0f, midPoint);
    float weightToe = 1.0f - weightLinear;
    if (x < linearSection * kPeak)
    {
        float toeMapped = midPoint * pow(x / midPoint, toeStrength);
        return weightToe * toeMapped + weightLinear * x;
    }
    return kA + kB * exp(x * kC);
}

// ST 2084 (PQ), frame-buffer scale in and out.
float EotfSt2084(float n)
{
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f,
                c2 = 18.8515625f, c3 = 18.6875f, pqC = 10000.0f;
    n = saturate(n);
    float np = pow(n, 1.0f / m2);
    float l = max(np - c1, 0.0f);
    l = l / (c2 - c3 * np);
    l = pow(l, 1.0f / m1);
    return l * pqC / kRefLum;
}

float InverseEotfSt2084(float v)
{
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f,
                c2 = 18.8515625f, c3 = 18.6875f, pqC = 10000.0f;
    float y = max(v * kRefLum / pqC, 0.0f);
    float ym = pow(y, m1);
    return exp2(m2 * (log2(c1 + c2 * ym) - log2(1.0f + c3 * ym)));
}

float3 RgbToICtCp(float3 rgb) // linear Rec.2020 in
{
    float l = (rgb.r * 1688.0f + rgb.g * 2146.0f + rgb.b * 262.0f) / 4096.0f;
    float m = (rgb.r * 683.0f + rgb.g * 2951.0f + rgb.b * 462.0f) / 4096.0f;
    float s = (rgb.r * 99.0f + rgb.g * 309.0f + rgb.b * 3688.0f) / 4096.0f;
    float lPQ = InverseEotfSt2084(l);
    float mPQ = InverseEotfSt2084(m);
    float sPQ = InverseEotfSt2084(s);
    return float3((2048.0f * lPQ + 2048.0f * mPQ) / 4096.0f,
                  (6610.0f * lPQ - 13613.0f * mPQ + 7003.0f * sPQ) / 4096.0f,
                  (17933.0f * lPQ - 17390.0f * mPQ - 543.0f * sPQ) / 4096.0f);
}

float3 ICtCpToRgb(float3 ictcp) // linear Rec.2020 out
{
    float l = ictcp.x + 0.00860904f * ictcp.y + 0.11103f * ictcp.z;
    float m = ictcp.x - 0.00860904f * ictcp.y - 0.11103f * ictcp.z;
    float s = ictcp.x + 0.560031f * ictcp.y - 0.320627f * ictcp.z;
    float lLin = EotfSt2084(l);
    float mLin = EotfSt2084(m);
    float sLin = EotfSt2084(s);
    return max(float3(3.43661f * lLin - 2.50645f * mLin + 0.0698454f * sLin,
                      -0.79133f * lLin + 1.9836f * mLin - 0.192271f * sLin,
                      -0.0259499f * lLin - 0.0989137f * mLin + 1.12486f * sLin),
               0.0f);
}

// GT7ToneMapping::applyToneMapping, initializeAsSDR(). Output in [0, 1] linear.
float3 GT7ToneMapSdr(float3 rgb)
{
    const float blendRatio = 0.6f, fadeStart = 0.98f, fadeEnd = 1.16f;
    const float sdrCorrection = 1.0f / kPeak;
    // UCS luminance of (kPeak, kPeak, kPeak): the LMS rows each sum to 1, so it is PQ(kPeak).
    const float targetUcs = InverseEotfSt2084(kPeak);

    float3 ucs = RgbToICtCp(rgb);
    float3 skewedRgb = float3(GTCurve(rgb.r), GTCurve(rgb.g), GTCurve(rgb.b));
    float3 skewedUcs = RgbToICtCp(skewedRgb);
    float chromaScale = 1.0f - SmoothStepGT(ucs.x / targetUcs, fadeStart, fadeEnd);
    float3 scaledUcs = float3(skewedUcs.x, ucs.y * chromaScale, ucs.z * chromaScale);
    float3 scaledRgb = ICtCpToRgb(scaledUcs);
    float3 blended = (1.0f - blendRatio) * skewedRgb + blendRatio * scaledRgb;
    return sdrCorrection * min(blended, kPeak);
}

float4 PSMain(VSOut i) : SV_TARGET
{
    float4 c = gScene.Load(int3(i.pos.xy, 0));
    if (gMode < 0.5f)
        return saturate(c);

    float3 lin = SrgbToLinear(c.rgb) * gExposure;
    float3 tm = GT7ToneMapSdr(Rec709ToRec2020(lin));
    return float4(LinearToSrgb(max(Rec2020ToRec709(tm), 0.0f)), saturate(c.a));
}
)HLSL";
