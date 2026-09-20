#include <math.h>
#include "../include/objectinstance.h"
#include "dxdefines.h"
#include "dxvbmanager.h"
#include "mmsystem.h"
#include "dxengine.h"
#include "../include/objectlod.h"
#include "dxtools.h"
#include "dxlightengine.h"
#include "common/irenderer.h" // #28: GpuLightCPU + SetLights (per-object dynamic light)
extern bool
    g_bUseGpu; // #DX12 п.4: dynamic object lighting on the active renderer
// #28: current-frame sun+ambient (filled in CDXEngine::FlushBuffers).
extern GpuLightCPU g_d3d11Sun;
extern float g_d3d11Amb[4];

#ifndef DEBUG_ENGINE

#endif


/////////////////////////////////////////// LIGHT ENGINE \\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\

CDXLight TheLightEngine;

_MM_ALIGN16 CDXLightElement CDXLight::LightList[MAX_DYNAMIC_LIGHTS];
// #34 C1: CDXLight::m_pD3DD/m_pD3D (D3D7 device) removed.
LightIndexType CDXLight::SwitchedList[MAX_SAMETIME_LIGHTS];
DWORD CDXLight::LightID;
DWORD CDXLight::DynamicLights;
float CDXLight::MaxRange;
bool CDXLight::LightsToOn[MAX_DYNAMIC_LIGHTS];
bool CDXLight::LightsLoaded;


void CDXLight::Setup()
{
    // #34 C1: no D3D7 device to store.
    // clear the light list
    memset(LightList, 0x00, sizeof(LightList));

    // Reset the Light List
    ResetLightsList();
}


// Thi function resets and inits the lights list parameters
void CDXLight::ResetLightsList(void)
{

#ifdef LIGHT_ENGINE_DEBUG
    REPORT_VALUE("LIGHTS", DynamicLights);
#endif

    for (int idx = 0; idx < MAX_DYNAMIC_LIGHTS; idx++)
    {
        // setup max distance
        LightList[idx].CameraDistance = DYNAMIC_LIGHT_INSIDE_RANGE + 1;
        // and if light is on switch it off
        // Artscout - 2026: #34 dropped the dead D3D7 m_pD3DD->LightEnable (D3D11 uploads lights
        // per-object via SetLights; there are no fixed-function light slots).
        LightList[idx].On = false;
        LightsToOn[idx] = false;
    }

    // no lights in the list
    DynamicLights = 0;
    // reset max light distance
    MaxRange = DYNAMIC_LIGHT_INSIDE_RANGE + 1;
    // no lights loaded in DX
    LightsLoaded = false;
}


// This function adds a light in the Dynamic Lights List
DWORD CDXLight::AddDynamicLight(DWORD ID, DXLightType *Light,
                                D3DXMATRIX *RotMatrix, D3DVECTOR *Pos,
                                float Range)
{
    DWORD Index = 0;
    float ActualRange = 0.0f;
    bool Assigned = false;

    // Do not add static lights... they r just use to pre compute emissive colours
    if (Light->Flags.Static)
        return NULL;


    //if already over the higer available range of lights in list, return
    if (Range >= MaxRange)
        return NULL;

    // else look where to place it
    while (Index < MAX_DYNAMIC_LIGHTS)
    {
        // if found the Light at te Max Range
        if (LightList[Index].CameraDistance == MaxRange and (not Assigned))
        {
            // substitute with new light
            LightList[Index].Light = Light->Light;
            // apply the owning of the light
            LightList[Index].Flags = Light->Flags;
            // apply the ID of the light
            LightList[Index].LightID = ID;
            // transform the Direction as a direction, not a point: the legacy TransformCoord added
            // the object's translation to it (the matrix is applied as v*M, so translation is the
            // fourth row), which corrupted the spot axis. Only the 3x3 part applies here.
            {
                D3DXVECTOR3 d = *(D3DXVECTOR3 *)&LightList[Index].Light.dvDirection;
                LightList[Index].Light.dvDirection.x =
                    d.x * RotMatrix->m00 + d.y * RotMatrix->m10 + d.z * RotMatrix->m20;
                LightList[Index].Light.dvDirection.y =
                    d.x * RotMatrix->m01 + d.y * RotMatrix->m11 + d.z * RotMatrix->m21;
                LightList[Index].Light.dvDirection.z =
                    d.x * RotMatrix->m02 + d.y * RotMatrix->m12 + d.z * RotMatrix->m22;
            }
            // transform the Position
            D3DXVec3TransformCoord(
                (D3DXVECTOR3 *)&LightList[Index].Light.dvPosition,
                (D3DXVECTOR3 *)&LightList[Index].Light.dvPosition, RotMatrix);
            // and translate it
            LightList[Index].Light.dvPosition.x += Pos->x;
            LightList[Index].Light.dvPosition.y += Pos->y;
            LightList[Index].Light.dvPosition.z += Pos->z;

            // Calcualtions for the Cone if SPOT Light
            if (Light->Light.dltType == D3DLIGHT_SPOT)
            {
                // The Light Cone Angle
                LightList[Index].phi = Light->Light.dvPhi / 2.0f;
                LightList[Index].alphaX =
                    atan2(LightList[Index].Light.dvDirection.x,
                          LightList[Index].Light.dvDirection.y);
                LightList[Index].alphaY =
                    atan2(Light->Light.dvDirection.z,
                          sqrtf(LightList[Index].Light.dvDirection.y *
                                    LightList[Index].Light.dvDirection.y +
                                LightList[Index].Light.dvDirection.x *
                                    LightList[Index].Light.dvDirection.x));
            }

            //update record XMM position
            *(D3DVECTOR *)&LightList[Index].Pos.d3d =
                LightList[Index].Light.dvPosition;
            // assign new distance
            LightList[Index].CameraDistance = Range;
            // Light has been assigned
            Assigned = true;
        }

        // if new Long range, assign it
        if (LightList[Index].CameraDistance > ActualRange)
            ActualRange = LightList[Index].CameraDistance;

        // if already out of range exit here
        if (ActualRange > DYNAMIC_LIGHT_INSIDE_RANGE)
            break;

        // next light
        Index++;
    }

    // number of lights
    DynamicLights = Index;

    // REPORT_VALUE("Dynamic Lights", DynamicLights);
    // set up the longest range left in list
    MaxRange = ActualRange;
    // return the light ID for this object
    return LightID;
}


#ifdef DEBUG_LOD_ID
extern DWORD gDebugLodID;
extern char TheLODNames[10000][32];
#endif

// This function switch on the nearest lights to an object of a certain radius
void CDXLight::UpdateDynamicLights(DWORD ID, D3DVECTOR *pos, float Radius)
{
    if (g_bUseGpu)
    {
        // #28 D3D11: the per-object light set = the sun (light 0) + the nearest active
        // point lamps (flashes/explosions) within their range of the object. Attenuation
        // is computed by the shader (Params.x=range). Then SetLights -> cbLights for this object.
        if (not g_pRenderer)
            return;
        const int MAXL = 8; // = MAX_LIGHTS in FFEmu.hlsl
        GpuLightCPU lights[MAXL];
        int n = 0;
        lights[n++] = g_d3d11Sun; // sun

        for (DWORD i = 0; i < DynamicLights and n < MAXL; ++i)
        {
            // Artscout - 2026: honour the model's per-light flags, which the D3D7 light engine used and
            // the port dropped. OwnLight = the light may only light the object that owns it (the F-16's
            // nav/formation lights: their glow must not spill onto the stores next to them);
            // NotSelfLight = it must NOT light its own object (the anti-collision strobe, muzzle-flash
            // and explosion particle lights). Ignoring them lit the F-16 with its own strobe -- the
            // external model flashes white with every strobe pulse -- and let wingtip lights bleed onto
            // whatever sits beside them. g_bObjLightMasks = 0 restores the old (flag-blind) behaviour.
            {
                extern bool g_bObjLightMasks;
                if (g_bObjLightMasks)
                {
                    const bool self = (LightList[i].LightID == ID);
                    // Artscout - 2026: the 3D PIT is a separate object from the player's aircraft, so
                    // every one of the jet's own lights (nav/formation/strobe) is "not self" to it and
                    // OwnLight kept them out of the cockpit entirely. They should spill in a little --
                    // the pit is the same physical aircraft. Let OwnLight through when the RECEIVER is
                    // the pit; NotSelfLight (the landing light, muzzle flashes) stays excluded, and the
                    // light's own range/falloff keeps the spill small. g_bObjLightMasks = 0 still
                    // disables the whole mask.
                    const bool pitReceiver = TheDXEngine.GetPitMode();
                    if ((LightList[i].Flags.OwnLight and not self and not pitReceiver) or
                        (LightList[i].Flags.NotSelfLight and self))
                        continue;
                }
            }

            D3DLIGHT7 &L = LightList[i].Light;
            const float dx = L.dvPosition.x - pos->x;
            const float dy = L.dvPosition.y - pos->y;
            const float dz = L.dvPosition.z - pos->z;
            const float range = L.dvRange + Radius;
            if (dx * dx + dy * dy + dz * dz > range * range)
                continue; // too far

            GpuLightCPU &g = lights[n++];
            ZeroMemory(&g, sizeof(g));
            g.Position[0] = L.dvPosition.x;
            g.Position[1] = L.dvPosition.y;
            g.Position[2] = L.dvPosition.z;
            g.Color[0] = L.dcvDiffuse.r;
            g.Color[1] = L.dcvDiffuse.g;
            g.Color[2] = L.dcvDiffuse.b;
            g.Params[0] =
                (L.dvRange > 1.0f) ? L.dvRange : 1.0f; // range (attenuation)
            g.Params[1] = 1.0f; // point
            // Artscout - 2026: hand over the AUTHORED D3D7 attenuation (Params.z = a0, Params.w = a1;
            // a2 is 0 in every shipped light). The shader then falls off the way the models were lit
            // and cuts at the range, instead of the port's hard ramp (1 - d/range). That ramp is what
            // made the pit's own flood/instrument lamps read as dead: they are authored with a 2.2-unit
            // range inside a 22-unit pit. Params.z/w are free for POINT lights; spots use them for the
            // cone cosines.
            //   ONLY when a1 > 0. A light with no falloff term (the F-16 tail strobe is authored
            // a0=1.01, a1=0) degenerates under the curve to a FLAT ~0.99 blob out to its range -- it
            // lit the whole tail as a hard-edged disc and read as "the light is under the plane".
            // Those keep the ramp, which fades them the way they always did.
            {
                extern bool g_bLightFalloffD3D7;
                if (g_bLightFalloffD3D7 and L.dvAttenuation1 > 0.0f)
                {
                    g.Params[2] = L.dvAttenuation0;
                    g.Params[3] = L.dvAttenuation1;
                }
            }

            // Artscout - 2026: spot cones. The port uploaded every dynamic light as a point, so the
            // F-16's anti-collision beacon (D3DLIGHT_SPOT, range 1500 ft, a ~5-degree beam pointing aft)
            // lit the whole aircraft and every store within 1500 ft as if it were an omni lamp.
            // Params.y = 2 = spot, Params.z = cos(outer half-angle), Params.w = cos(inner half-angle).
            // g_bObjSpotCones = 0 restores the old point approximation.
            {
                extern bool g_bObjSpotCones;
                if (g_bObjSpotCones and L.dltType == D3DLIGHT_SPOT)
                {
                    const float dxn = L.dvDirection.x, dyn = L.dvDirection.y,
                                dzn = L.dvDirection.z;
                    const float dl = sqrtf(dxn * dxn + dyn * dyn + dzn * dzn);
                    if (dl > 1e-6f)
                    {
                        const float pi = 3.14159265f;
                        float outer = 0.5f * L.dvPhi;
                        float inner = 0.5f * L.dvTheta;
                        if (outer > pi)
                            outer = pi;
                        if (outer < 0.0f)
                            outer = 0.0f;
                        if (inner > outer)
                            inner = outer;
                        g.Direction[0] = dxn / dl;
                        g.Direction[1] = dyn / dl;
                        g.Direction[2] = dzn / dl;
                        g.Params[1] = 2.0f;
                        g.Params[2] = cosf(outer);
                        g.Params[3] = cosf(inner);
                    }
                }
            }
        }
        g_pRenderer->SetLights(g_d3d11Amb, n, lights, sizeof(lights[0]));
        return;
    }
}


void CDXLight::EnableMappedLights(void)
{
    // Artscout - 2026: #34 D3D11 uploads per-object lights via SetLights; there are no
    // fixed-function light slots to enable/disable. Kept as a no-op for external callers.
}


// Artscout - 2026: #34 removed the dead D3D7 CDXEngine::AddDynamicLight(D3DLIGHT7*) (3-arg)
// and RemoveDynamicLights -- no callers (the live path is CDXLight::AddDynamicLight, 5-arg,
// D3D11 #28). They used the D3D7 device (m_pD3DD->SetLight/LightEnable).


// *** NO MORE USED  ***
// This function is the HardCoding for the PIT of the Taxi Spotlight
void CDXEngine::DrawOwnSpot(Trotation *Rotation)
{
    /* DXLightType OwnSpot;

     D3DXMATRIX RotMatrix;
    #ifndef DEBUG_ENGINE
     AssignPmatrixToD3DXMATRIX(&RotMatrix, Rotation);
    #endif


     // initialize it
     memset(&OwnSpot, 0, sizeof(OwnSpot));

     OwnSpot.Light.dcvDiffuse.r=OwnSpot.Light.dcvDiffuse.g=OwnSpot.Light.dcvDiffuse.b=1.0f;
     OwnSpot.Light.dcvSpecular.r=OwnSpot.Light.dcvSpecular.g=OwnSpot.Light.dcvSpecular.b=1.0f;
     OwnSpot.Light.dvRange=1500.0f;
     OwnSpot.Light.dvAttenuation0=0.1f;
     OwnSpot.Light.dvAttenuation1=0.01f;
     OwnSpot.Light.dltType=D3DLIGHT_SPOT;
     OwnSpot.Light.dvTheta=0.1f;
     OwnSpot.Light.dvPhi=1.3f;
     OwnSpot.Light.dvFalloff=1.0f;
     OwnSpot.Light.dvDirection.z=0.23f;
     OwnSpot.Light.dvDirection.y=0.0f;
     OwnSpot.Light.dvDirection.x=1.0f;

     OwnSpot.Flags.OwnLight=false;


     D3DVECTOR Pos(4, 0, 5);
     D3DXVec3TransformCoord((D3DXVECTOR3*)&Pos, (D3DXVECTOR3*)&Pos, &RotMatrix);
     TheLightEngine.AddDynamicLight(0, &OwnSpot, &RotMatrix, &Pos, 0);
    */
}
