// ffobject.hlsl -- Artscout - 2026: object/terrain pass, HLSL for both backends.
// Ported from the GLSL pair it replaces: bindings, UBO layout and behaviour are
// unchanged. Lighting is PER-VERTEX (legacy D3D7 gouraud), as the PS expects.
#include "ffcommon.hlsli"
//============================== Resources ====================================

// Paired image+sampler on one binding: DXC folds them into a combined
// descriptor, which is what the existing sets already hold.
[[vk::combinedImageSampler]][[vk::binding(1, 0)]] Texture2D gTex0;
[[vk::combinedImageSampler]][[vk::binding(1, 0)]] SamplerState gTex0Samp;
[[vk::combinedImageSampler]][[vk::binding(2, 0)]] Texture2D gTex1;
[[vk::combinedImageSampler]][[vk::binding(2, 0)]] SamplerState gTex1Samp;

// #107 bindless terrain: the tile atlas as one unbounded array. An array cannot
// be a combined descriptor, so image and sampler are separate here.
[[vk::binding(0, 1)]] Texture2D gBindless[];
[[vk::binding(3, 0)]] SamplerState gBindlessSamp;

//============================ Stage plumbing =================================

struct VSIn
{
    [[vk::location(0)]] float3 pos      : POSITION;
    [[vk::location(1)]] float3 normal   : NORMAL;
    [[vk::location(2)]] float4 color    : COLOR0;    // diffuse + ambient material
    [[vk::location(3)]] float4 emissive : COLOR1;    // EMISSIVE material, not a highlight
    [[vk::location(4)]] float2 uv       : TEXCOORD0;
    [[vk::location(5)]] uint   texIndex : TEXCOORD1; // bindless tile slot
};

struct VSOut
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv    : TEXCOORD0;
    [[vk::location(2)]] float2 uv1   : TEXCOORD1;
    [[vk::location(3)]] float3 spec  : TEXCOORD2;
    [[vk::location(4)]] float  fogF  : TEXCOORD3;
    [[vk::location(5)]] nointerpolation uint texIndex : TEXCOORD4;
    // Artscout - 2026: per-pixel lighting inputs (FF_PIXELLIGHT); the PS normalizes both.
    [[vk::location(6)]] float3 nrm   : TEXCOORD5;
    [[vk::location(7)]] float3 view  : TEXCOORD6;
    [[vk::location(8)]] float3 wpos  : TEXCOORD7;
    [[vk::location(9)]] float3 emis  : TEXCOORD8;
};

// The VS writes one field more than the PS reads: PointSize is a builtin, takes
// no location, and must be written or a POINT_LIST pipeline is invalid.
struct VSOutVs
{
    float4 pos : SV_Position;
    [[vk::builtin("PointSize")]] float psize : PSIZE;
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv    : TEXCOORD0;
    [[vk::location(2)]] float2 uv1   : TEXCOORD1;
    [[vk::location(3)]] float3 spec  : TEXCOORD2;
    [[vk::location(4)]] float  fogF  : TEXCOORD3;
    [[vk::location(5)]] nointerpolation uint texIndex : TEXCOORD4;
    [[vk::location(6)]] float3 nrm   : TEXCOORD5;
    [[vk::location(7)]] float3 view  : TEXCOORD6;
    [[vk::location(8)]] float3 wpos  : TEXCOORD7;
    [[vk::location(9)]] float3 emis  : TEXCOORD8;
};

//============================== Lighting =====================================

// Artscout - 2026: the object light model in ONE place, used per-vertex (legacy Gouraud) and
// per-pixel (FF_PIXELLIGHT) exactly like the D3D12 twin's FFObjectLighting -- the two backends
// must not drift. N is the world-space normal (normalized), wpos the camera-relative world
// position, viewVec the unnormalized camera->point vector.
void FFObjectLighting(float3 N, float3 wpos, float3 viewVec, out float3 lit, out float3 spec)
{
    float ambScale = 1.0f;
    const float sunScale = Has(FF_COCKPIT) ? 1.25f : 1.0f;
    if (Has(FF_COCKPIT))
    {
        const float sunLvl =
            (gNumLights.x > 0u) ?
                dot(gLights[0].color.rgb, float3(0.299f, 0.587f, 0.114f)) :
                1.0f;
        ambScale = clamp(sunLvl, 0.06f, 1.0f);
    }

    lit = gAmbient.rgb * ambScale;
    // Artscout - 2026: the pit's flood/instrument fill -- what the two cockpit light knobs add on
    // top of the environment (the environment is already in gAmbient). FF_COCKPIT only, and LOCAL:
    // |wpos| is the distance from the pilot's eye (the pit is drawn camera-relative), so the fill
    // falls off from there instead of lighting the whole model. gCockpitFill.w = the reach.
    if (Has(FF_COCKPIT))
        lit += gCockpitFill.rgb *
               saturate(1.0f - length(wpos) / max(gCockpitFill.w, 1.0e-3f));
    for (uint l = 0u; l < gNumLights.x && l < 8u; ++l)
    {
        float3 Ldir;
        float atten = 1.0f;
        if (gLights[l].params.y < 0.5f) // directional (sun)
        {
            Ldir = -normalize(gLights[l].direction.xyz);
        }
        else // point/spot (muzzle flashes / explosions / lamps)
        {
            const float3 toL = gLights[l].position.xyz - wpos;
            const float dist = length(toL);
            Ldir = toL / max(dist, 1e-3f);
            if (gLights[l].params.y < 1.5f) // point lamp (see the D3D12 twin's note)
            {
                // Artscout - 2026: the AUTHORED D3D7 attenuation when the CPU supplies it
                // (params.z = a0, params.w = a1), cut at the light's range; the port's hard ramp
                // (1 - d/range) is the fallback and still bounds a1 == 0 data (the tail strobe).
                atten = (gLights[l].params.z > 0.0f)
                            ? ((dist <= gLights[l].params.x)
                                   ? min(1.0f, 1.0f / max(gLights[l].params.z +
                                                              gLights[l].params.w * dist,
                                                          1.0e-4f))
                                   : 0.0f)
                            : saturate(1.0f - dist / max(gLights[l].params.x, 1.0f));
            }
            else // spot cone
            {
                atten = saturate(1.0f - dist / max(gLights[l].params.x, 1.0f));
                const float cosA = dot(Ldir, -normalize(gLights[l].direction.xyz));
                atten *= saturate((cosA - gLights[l].params.z) /
                                  max(gLights[l].params.w - gLights[l].params.z, 1e-4f));
            }
        }
        const float lScale = (gLights[l].params.y < 0.5f) ? sunScale : 1.0f;
        lit += gLights[l].color.rgb * max(dot(N, Ldir), 0.0f) * atten * lScale;
        // Artscout - 2026: the light's OWN AMBIENT (D3D7: matAmbient * lightAmbient * atten), as a
        // scale on its colour in color.w -- see CDXLight::UpdateDynamicLights and the D3D12 twin.
        // NO N.L on purpose: this lights a lamp's own housing, the skin it sits flush against, and
        // POINTLIST lamps (zero vertex normal). The sun leaves color.w at 0, so light 0 is inert.
        lit += gLights[l].color.rgb * gLights[l].color.w * atten;
    }
    if (Has(FF_FULLBRIGHT))
        lit = float3(1.0f, 1.0f, 1.0f);

    spec = float3(0.0f, 0.0f, 0.0f);
    float specPow = gSpec.w;
    float3 specCol = gSpec.rgb;
    if (Has(FF_COCKPIT) && specPow <= 0.0f)
    {
        specPow = 20.0f;
        specCol = float3(0.10f, 0.10f, 0.10f);
    }
    if (specPow > 0.0f && gNumLights.x > 0u)
    {
        const float3 V  = normalize(viewVec);
        const float3 Ls = (gLights[0].params.y < 0.5f) ?
                              -normalize(gLights[0].direction.xyz) :
                              normalize(gLights[0].position.xyz - wpos);
        const float3 H  = normalize(Ls + V);
        spec = specCol * gLights[0].color.rgb *
               pow(max(dot(N, H), 0.0f), specPow);
    }
}

//============================== Vertex =======================================

// PointSize is required for a POINT_LIST pipeline (BSP nav-lights): without it
// the driver rejects the pipeline and the device is lost on the first such frame.
VSOutVs VS_Object(VSIn i, uint viewId : SV_ViewID)
{
    VSOutVs o;
    o.psize = 1.0f;

    const float4 wp = mul(float4(i.pos, 1.0f), gWorld);
    o.pos = mul(mul(wp, gView[viewId]), gProj[viewId]);
    // Artscout - 2026: per-pixel lighting inputs (FF_PIXELLIGHT); cheap even when unused.
    o.nrm  = mul(i.normal, (float3x3)gWorld);
    o.view = gCamPos.xyz - wp.xyz;
    o.wpos = wp.xyz;
    o.emis = i.emissive.rgb; // PS needs it for FF_EMISSIVE (added to the lit material)

    // COLORVERTEX is always on in the D3D7 object path, so the vertex colour
    // applies unconditionally -- dropping it blows the cockpit panels white.
    float4 col = i.color;
    float3 spec = float3(0.0f, 0.0f, 0.0f);

    if (Has(FF_AFTERBURNER))
    {
        col.rgb = float3(1.0f, 1.0f, 1.0f); // the PS maps brightness to a gradient
    }
    else if (Has(FF_EMISSIVE))
    {
        // Artscout - 2026: D3D7 semantics -- emissive ADDS to the lit material, it does not replace
        // it (see the D3D12 twin; the old shortcut made the F-16's light strips flat red/green panels).
        if (Has(FF_PIXELLIGHT))
        {
            // the PS lights the surface and adds i.emis afterwards
        }
        else
        {
            const float3 N = normalize(mul(i.normal, (float3x3)gWorld));
            float3 lit;
            // Artscout - 2026: carry the highlight, like the FF_LIGHTING branch. Dropping it was
            // invisible while FF_EMISSIVE meant a handful of SwEmissive surfaces; with D3D7's
            // emissive default restored this is the legacy path for nearly every surface.
            FFObjectLighting(N, wp.xyz, gCamPos.xyz - wp.xyz, lit, spec);
            col.rgb = i.color.rgb * saturate(lit) + i.emissive.rgb;
        }
    }
    else if (Has(FF_LIGHTING))
    {
        // Artscout - 2026: FF_PIXELLIGHT -> the PS runs the same model per pixel from
        // o.nrm/o.view; leave the vertex colour unlit here (bit-identical legacy path otherwise).
        if (!Has(FF_PIXELLIGHT))
        {
            const float3 N = normalize(mul(i.normal, (float3x3)gWorld));
            float3 lit;
            FFObjectLighting(N, wp.xyz, gCamPos.xyz - wp.xyz, lit, spec);
            col.rgb *= saturate(lit);
        }
    }

    o.color = col;
    o.uv = i.uv;
    o.uv1 = i.uv; // stage 1 shares coordinates (terrain day/night)
    o.spec = spec;
    o.texIndex = i.texIndex;
    // Fog distance is clip.w: this engine's view forward axis is X, not view.z.
    o.fogF = Has(FF_FOG) ?
                 saturate((gFogParams.y - o.pos.w) /
                          max(gFogParams.y - gFogParams.x, 1e-4f)) :
                 1.0f;
    return o;
}

//============================== Pixel ========================================

float4 PS_Object(VSOut i) : SV_Target
{
    // FF_GLOC: G-force vignette on a fullscreen quad -- before any texture work.
    if (Has(FF_GLOC))
    {
        const float r = length(i.uv - float2(0.5f, 0.5f)) * 2.0f;
        const float v = smoothstep(gGloc.y, gGloc.z, r) * gGloc.x;
        return float4(gMaterialColor.rgb, saturate(v));
    }

    float4 c = gMaterialColor * i.color;

    // Artscout - 2026: per-PIXEL object lighting (FF_PIXELLIGHT) -- same model as the D3D12 twin.
    // The VS left the vertex colour unlit; run the loop here from the interpolated world
    // normal/position/view so a lamp's range falls off across a surface, not between vertices.
    float3 pixSpec = float3(0.0f, 0.0f, 0.0f);
    if (Has(FF_LIGHTING) && Has(FF_PIXELLIGHT) && !Has(FF_AFTERBURNER))
    {
        const float3 N = normalize(i.nrm);
        float3 lit, spec;
        FFObjectLighting(N, i.wpos, i.view, lit, spec);
        c.rgb *= saturate(lit);
        // Artscout - 2026: D3D7 order -- texture * (matDiffuse*lit + matEmissive). Added to the LIT
        // MATERIAL here (not multiplied by the vertex colour, which is black on a lamp lens), so the
        // texture stage below modulates the pair. Past the texture it was a flat unmodulated term
        // and washed textured lamps white. See the D3D12 twin's note.
        if (Has(FF_EMISSIVE) && !Has(FF_AFTERBURNER))
            c.rgb += i.emis;
        pixSpec = spec;
    }

    float texA = 1.0f; // chroma is baked to a=0 at load time

    if (Has(FF_TEXTURE0))
    {
        // FF_BINDLESS routes the base sample through the array by per-vertex
        // slot; ~0 means "not resident" and falls back to gTex0 (white here).
        const bool bindless = Has(FF_BINDLESS) && i.texIndex != 0xFFFFFFFFu;
        const float4 t0 =
            bindless ?
                gBindless[NonUniformResourceIndex(i.texIndex)].Sample(
                    gBindlessSamp, i.uv) :
                gTex0.Sample(gTex0Samp, i.uv);
        texA = t0.a;

        if (Has(FF_TEXCOLORDIFFUSE))
        {
            // D3D7 TexColorDiffuse (HUD/DED text): the glyph is in the ALPHA and
            // its RGB is not black, so cut by alpha and keep the vertex colour.
            if (texA < 0.5f)
                discard;
        }
        else if (Has(FF_RTTSOFT))
        {
            // #7 additive emissive composite of the RTT atlas: symbology is
            // ADDED, so no chroma cut and no rim on the text.
            c.rgb *= t0.rgb * 1.5f;
            c.a = i.color.a;
        }
        else
        {
            if (Has(FF_CHROMAKEY))
            {
                const float3 d = abs(t0.rgb - gChromaKey.rgb);
                if (max(max(d.r, d.g), d.b) <= gChromaKey.a)
                    discard;
            }
            c *= t0;
        }

        // Terrain day/night: the D3D7 stage 1 op is CURRENT ADD TEXTURE -- it
        // ADDS. A multiply here is what used to give a black ground.
        if (Has(FF_TEXTURE1))
            c.rgb += gTex1.Sample(gTex1Samp, i.uv1).rgb;

        if (Has(FF_MODULATE2X))
            c.rgb *= 2.0f;
    }

    // Artscout - 2026: the emissive is now folded into the lit material above (D3D7 order) so the
    // texture modulates it; it used to be added here, past the texture. Both paths agree again --
    // the legacy per-vertex branch in VS_Object has always done `color*lit + emissive` pre-texture.

    // #49 afterburner: scale colour BY texture brightness, with time-animated
    // turbulence and flicker so the plume licks instead of sitting frozen.
    if (Has(FF_AFTERBURNER))
    {
        const float tAB = gFogParams.w;
        const float2 uvAB = i.uv;
        float turb = 0.5f
                   + 0.30f * sin(uvAB.y * 15.0f - tAB * 11.0f + uvAB.x * 6.0f)
                   + 0.16f * sin(uvAB.y * 29.0f - tAB * 19.0f - uvAB.x * 10.0f + 1.7f)
                   + 0.08f * sin(uvAB.x * 22.0f + tAB * 7.0f);
        turb = saturate(turb);

        const float b = max(c.r, max(c.g, c.b));
        const float bMod = b * lerp(0.60f, 1.30f, turb);

        const float3 cool = float3(1.0f, 0.30f, 0.06f);
        const float3 hot = float3(1.0f, 0.95f, 0.82f);
        const float3 core = float3(0.70f, 0.82f, 1.00f);
        float3 tint = lerp(cool, hot, saturate(bMod * 1.4f));
        tint = lerp(tint, core, saturate((bMod - 0.85f) * 3.0f) * 0.45f);

        // In daylight a real plume is nearly invisible -- scale by darkness.
        const float amb = saturate(max(gAmbient.r, max(gAmbient.g, gAmbient.b)));
        const float intensity = lerp(0.45f, 2.6f, 1.0f - amb);
        const float flick = 0.85f + 0.15f * sin(tAB * 42.0f) * (0.6f + 0.4f * turb);
        c.rgb = tint * bMod * intensity * flick;
    }

    if (Has(FF_ALPHATEST))
    {
        // Key on TEXTURE alpha so per-vertex alpha cannot discard opaque geometry.
        const float aTest = Has(FF_TEXTURE0) ? texA : c.a;
        if (aTest < gFogParams.z)
            discard;
    }

    c.rgb += (Has(FF_PIXELLIGHT) ? pixSpec : i.spec); // D3D7-style specular, on top of the texture, before fog

    // #12 water: a UV+time effect (the terrain has no world normal here).
    if (Has(FF_WATER))
    {
        const float t = gFogParams.w;
        const float2 p = i.uv * 7.0f;
        const float w = sin(p.x + t * 0.6f)
                      + sin(p.y * 1.3f - t * 0.5f)
                      + sin((p.x + p.y) * 0.8f + t * 0.9f) * 0.5f;
        c.rgb *= float3(0.80f, 0.92f, 1.06f);
        c.rgb += w * 0.025f * float3(0.6f, 0.7f, 0.85f);
    }

    if (Has(FF_FOG))
        c.rgb = lerp(gFogColor.rgb, c.rgb, i.fogF);

    // #72 cockpit: brighten gently and let the clamp take the top end.
    if (Has(FF_COCKPIT))
        c.rgb = saturate(c.rgb * 1.08f);

    // #A5 sensor pass (TGP/Maverick/FLIR) -> Rec.601 luma.
    if (Has(FF_IRGREY))
        c.rgb = dot(c.rgb, float3(0.299f, 0.587f, 0.114f)).xxx;

    // #97 NVG: green phosphor with tube gain, scanlines, grain and vignette.
    if (Has(FF_NVG))
    {
        const float kNvgGain = 4.0f;
        float lum = dot(c.rgb, float3(0.30f, 0.59f, 0.11f));
        lum = 1.0f - exp(-lum * kNvgGain);

        const float scan = 0.92f + 0.08f * sin(i.pos.y * 3.14159f);
        const float2 np = i.pos.xy + gFogParams.w * 37.0f;
        const float grain =
            frac(sin(dot(np, float2(12.9898f, 78.233f))) * 43758.5453f);
        const float noise = 0.91f + 0.09f * grain;
        const float2 vc =
            i.pos.xy / max(gParams.yz, float2(1.0f, 1.0f)) - 0.5f;
        const float vig = saturate(1.0f - dot(vc, vc) * 1.35f);

        lum *= scan * noise * vig;
        c.rgb = float3(0.10f, 1.0f, 0.28f) * lum;
    }

    return c;
}
