#include <ciso646>
#include "time.h"
#include <math.h>
#include "../include/objectinstance.h"
#include "dxdefines.h"
#include "dxvbmanager.h"
#include "mmsystem.h"
#include "../include/texbank.h"
#ifndef DEBUG_ENGINE
#include "../include/realweather.h"
#endif
#include "dxengine.h"
#include "../include/fflog.h" // Artscout - 2026: menu texture diagnostic
#include "../include/objectlod.h"
#include "dxtools.h"
#include "../include/tod.h"
#include "../../falclib/include/fakerand.h"
#include "../../include/comsup.h"
#include "common/irenderer.h" // PHASE 4: D3D11 object path
#include "openxrbackend.h"   // temp VR stereo diag
#include <stdio.h>
extern bool
    g_bUseGpu; // #DX12 п.4: GPU mode (D3D11 || D3D12) -- the object pass runs on the active renderer

// #34: world matrix -> the shader cbObject (D3D11). The dead D3D7 m_pD3DD->SetTransform else-branch
// was removed.
#define DX_SET_WORLD(M)                                                        \
    do                                                                         \
    {                                                                          \
        if (g_pRenderer)                                                       \
            g_pRenderer->SetWorld((const float *)&(M));                        \
    } while (0)

// This variable is the Model ID presently under draw
DWORD gDebugLodID;

extern bool g_bGreyMFD;
extern bool bNVGmode;
extern int TheObjectLODsCount;

#ifdef DEBUG_LOD_ID
extern char TheLODNames[10000][32];
#endif

// ********************************** STATIC GLOBAL VARIABLES ***************************************

CDXEngine TheDXEngine;

// #34 C1: CDXEngine D3D7 device statics (m_pD3DD/m_pD3D/m_pDD) removed.

D3DXMATRIX CDXEngine::State, CDXEngine::DofTransformation,
    CDXEngine::AppliedState;
DWORD CDXEngine::StateStackLevel;
D3DXMATRIX CDXEngine::CameraView;
D3DXMATRIX CDXEngine::BBMatrix;
D3DVECTOR CDXEngine::CameraPos;
D3DVECTOR CDXEngine::LightDir;
D3DXMATRIX CDXEngine::Projection;
D3DXMATRIX CDXEngine::World;

// #28: current-frame sun+ambient -- for per-object dynamic lighting (UpdateDynamicLights
// in dxlightengine.cpp builds the 'sun + nearby dynamic lamps' set and calls SetLights).
GpuLightCPU g_d3d11Sun = {};
float g_d3d11Amb[4] = {0.45f, 0.45f, 0.45f, 1.0f};
D3DVIEWPORT7 CDXEngine::ViewPort;
_MM_ALIGN16 XMMVector
    CDXEngine::XMMCamera; // the Camera position compatible with XMM Math
DWORD CDXEngine::m_TexID, CDXEngine::m_LastTexID;
DXFlagsType CDXEngine::m_LastFlags;
DWORD CDXEngine::m_LastZBias;
float CDXEngine::m_LODBiasCx;
D3DMATERIAL7 CDXEngine::TheMaterial;
D3DXMATRIX CDXEngine::StateStack[128];
DWORD CDXEngine::m_TexUsed[256];
DWORD CDXEngine::m_LastSpecular;
float CDXEngine::m_FogLevel;
float CDXEngine::m_BlipIntensity;
float CDXEngine::m_LinearFogLevel;
D3DCOLORVALUE CDXEngine::m_FogColor;
DWORD CDXEngine::m_AlphaTextureStage;

D3DLIGHT7 CDXEngine::TheSun, CDXEngine::TheNVG, CDXEngine::TheTV;
D3DCOLORVALUE CDXEngine::TheSunColour;

SurfaceStackType CDXEngine::m_AlphaStack;
SurfaceStackType CDXEngine::m_SolidStack;
#ifdef DEBUG_ENGINE
SurfaceStackType CDXEngine::m_FrameStack;
#endif
bool CDXEngine::DrawPoints, CDXEngine::DrawLines;
bool CDXEngine::m_LinearFog;

VBItemType CDXEngine::m_VB;
NodeScannerType CDXEngine::m_NODE;
ObjectInstance *CDXEngine::m_TheObjectInstance;
ObjectInstance *CDXEngine::m_LastObjectInstance;
TextureHandle *CDXEngine::ZeroTex;
StencilModeType CDXEngine::m_StencilMode;
DWORD CDXEngine::m_StencilRef;
bool CDXEngine::m_PitMode;
bool CDXEngine::m_SurfacePit;
// Artscout - 2026: cockpit sun shadow replay state (see the header + RenderPitShadowMap).
bool CDXEngine::m_ShadowWalk = false;
bool CDXEngine::m_PitShadowDone = false;

DX_StateType CDXEngine::m_RenderState;
DWORD CDXEngine::m_StatesStackLevel;
DX_StatesStackType CDXEngine::m_StatesStack[DX_MAX_NESTED_STATES];

#ifdef DATE_PROTECTION
bool DateOff = true;
#define PROTECTION_MONTH 1
#define PROTECTION_YEAR 2008
#endif

// ********************************* THIS SECTION IS THE REAL ENGINE ********************************
CDXEngine::CDXEngine(void)
{
    DxEngineStateHandle = NULL;
    ZeroTex = NULL;
    TexturesList = NULL;
    m_LinearFog = false;
    m_StatesStackLevel = 0;
    m_RenderState = DX_OTW;

#ifdef DATE_PROTECTION

    time_t t;
    struct tm *today;

    t = time(NULL);
    today = localtime(&t);

    if (today->tm_mon > PROTECTION_MONTH or today->tm_year > PROTECTION_YEAR)
        DateOff = true;
    else
        DateOff = false;


#endif
}

CDXEngine::~CDXEngine(void)
{
    CleanUpTexturesOnDevice();
    ReleaseTextures();
    // #34: DxEngineStateHandle is never set under D3D11 (StoreSetupState is a no-op); dead D3D7
    // DeleteStateBlock removed.
}

// The Default engine states for the renderer
// This state must be sampled at D3DD CREATION PHASE, to keep it independent by following
// BSP engine state changes
void CDXEngine::StoreSetupState(void)
{
    // #34 D3D11: D3D7 state-block save/restore replaced by D3D11 state objects (FFStateMap); no-op.
}

void CDXEngine::SetFogLevel(float FogLevel)
{
    m_FogLevel = m_LinearFog ? 1.0f : FogLevel;
}


void CDXEngine::SetCamera(D3DXMATRIX *Settings, D3DVECTOR Pos, D3DXMATRIX *BB)
{
    CameraView = *Settings;
    CameraPos = Pos;
#ifdef EDIT_ENGINE
    CameraView.m30 = CameraPos.x;
    CameraView.m31 = CameraPos.y;
    CameraView.m32 = CameraPos.z;
#endif
    // #34 D3D11: view matrix into the shader cbuffer (dead D3D7 SetTransform else removed)
    if (g_pRenderer)
        g_pRenderer->SetView((const float *)&CameraView);

    // The BB Stuff
    BBMatrix = *BB;
    BBCx[0].d3d.x = BB->m00, BBCx[0].d3d.y = BB->m10, BBCx[0].d3d.z = BB->m20,
    BBCx[0].d3d.Flags.Word = 0;
    BBCx[1].d3d.x = BB->m01, BBCx[1].d3d.y = BB->m11, BBCx[1].d3d.z = BB->m21,
    BBCx[1].d3d.Flags.Word = 0;
    BBCx[2].d3d.x = BB->m02, BBCx[2].d3d.y = BB->m12, BBCx[2].d3d.z = BB->m22,
    BBCx[2].d3d.Flags.Word = 0;

    // set the XMM Camera
    *((D3DVECTOR *)&XMMCamera.d3d) = Pos;
}


// Artscout - 2026: set by C_3dViewer while a menu 3D viewer is mid-draw, so the diagnostic below
// can report on those surfaces without drowning in the sim's.
bool g_bMenuViewerDrawing = false;

VOID CDXEngine::SelectTexture(GLint texID)
{
    // eventually select other textures for NVG/TV

    // Artscout - 2026 (x64): texID is a small bank index, but the handle/SRV it resolves to are
    // pointer-sized. Use a DWORD_PTR local so the pointer isn't truncated (GLint dropped the high 32 bits).
    const DWORD_PTR bankHandle = (texID not_eq -1) ?
                                     TheTextureBank.GetHandle(texID) :
                                     (DWORD_PTR)ZeroTex;
    DWORD_PTR h = bankHandle;

    if (h)
        h = (DWORD_PTR)((TextureHandle *)h)->m_pDDS;

    // Artscout - 2026: why menu 3D models render untextured. Three values, and whichever is zero
    // names the stage that failed: texID -1 means the geometry never asked for a texture at all,
    // a null bank handle means the bank has no entry loaded for it, and a null gpu means it was
    // loaded but never made resident. Capped so a long look at one screen cannot fill the log.
    {
        extern bool g_bLogMenuTextures;
        extern bool g_bMenuViewerDrawing;
        static int s_logged = 0;

        if (g_bLogMenuTextures and g_bMenuViewerDrawing and s_logged < 12)
        {
            s_logged++;
            char buf[192];
            sprintf(buf,
                    "[MENUTEX] texID=%d bankHandle=%p gpu=%p useGpu=%d "
                    "renderer=%p\n",
                    (int)texID, (void *)bankHandle, (void *)h, (int)g_bUseGpu,
                    (void *)g_pRenderer);
            FFDebugLog(buf);
        }
    }

    if (g_bUseGpu) // PHASE 4/#DX12: m_pDDS holds the GPU texture handle (D3D11 SRV or D3D12Texture*)
    {
        if (g_pRenderer)
            g_pRenderer->SetTexture(0, (struct ID3D11ShaderResourceView *)h);
        return;
    }
    // #34 dead D3D7 SetTexture stages removed (D3D11 returns above)
}


// The View Port setting function
// The passed parameters are in Screen Pixels
void CDXEngine::SetViewport(DWORD l, DWORD t, DWORD r, DWORD b)
{
    ViewPort.dwX = l;
    ViewPort.dwY = t;
    ViewPort.dwWidth = r - l;
    ViewPort.dwHeight = b - t;
    ViewPort.dvMinZ = 0.0f;
    ViewPort.dvMaxZ = 1.0f;
}


// The Engine initialization Function
void CDXEngine::Setup()
{
    // #34 C1: no D3D7 device to store.
    m_LastFlags.w = 0;
    m_TexID = m_LastTexID = -1;
    INIT_S_STACK(m_AlphaStack, MAX_ALPHA_SURFACES);
    INIT_S_STACK(m_SolidStack, MAX_SOLID_SURFACES);
#ifdef DEBUG_ENGINE
    INIT_S_STACK(m_FrameStack, 5000);
#endif
    m_bCullEnable = true;
    m_bDofMove = false;
    m_LastFlags.w = 0;
#ifdef DEBUG_ENGINE
    UseZBias = true;
#endif

    // Initializes the 2D Engine
    DX2D_Init();

    // Initialize the Light engine
    TheLightEngine.Setup(); // #34 C1: D3D7 device args removed

    ZeroMemory(&TheMaterial, sizeof(TheMaterial));
    TheMaterial.ambient.r = TheMaterial.ambient.g = TheMaterial.ambient.b =
        1.0f;
    TheMaterial.diffuse.r = TheMaterial.diffuse.g = TheMaterial.diffuse.b =
        1.0f;
    TheMaterial.specular.r = TheMaterial.specular.g = TheMaterial.specular.b =
        1.0f;
    TheMaterial.dvPower = 6.8f;

    /////////// Initializes the Environmental Light Object to DEFAULT VALUES ///////////////////////
    ZeroMemory(&TheSun, sizeof(TheSun));
    TheSun.dltType = D3DLIGHT_DIRECTIONAL;
    TheSun.dcvAmbient.r = TheSun.dcvAmbient.g = TheSun.dcvAmbient.b = 1.0f;
    TheSun.dcvDiffuse.r = TheSun.dcvDiffuse.g = TheSun.dcvDiffuse.b = 1.0f;
    TheSun.dcvSpecular.r = TheSun.dcvSpecular.g = TheSun.dcvSpecular.b = 1.0f;
    TheSunColour.r = TheSunColour.g = TheSunColour.b = 1.0f;
    ////////////////////////////////////////////////////////////////////////////////////////////////

    //////////////////////////////// The NVG Mode used Light ///////////////////////////////////////
    ZeroMemory(&TheNVG, sizeof(TheNVG));
    TheNVG.dltType = D3DLIGHT_DIRECTIONAL;
    TheNVG.dcvAmbient.r = 0.52f;
    TheNVG.dcvAmbient.g = 0.52f;
    TheNVG.dcvAmbient.b = 0.52f;

    TheNVG.dcvDiffuse.r = 0.04f;
    TheNVG.dcvDiffuse.g = 0.04f;
    TheNVG.dcvDiffuse.b = 0.04f;

    TheNVG.dcvSpecular.r = 0.05f;
    TheNVG.dcvSpecular.g = 0.05f;
    TheNVG.dcvSpecular.b = 0.05f;

    ////////////////////////////////////////////////////////////////////////////////////////////////

    //////////////////////////////// The TV/IR Mode used Light ///////////////////////////////////////
    ZeroMemory(&TheTV, sizeof(TheTV));
    TheTV.dltType = D3DLIGHT_DIRECTIONAL;
    TheTV.dcvAmbient.r = 0.52f;
    TheTV.dcvAmbient.g = 0.52f;
    TheTV.dcvAmbient.b = 0.52f;

    TheTV.dcvDiffuse.r = 0.04f;
    TheTV.dcvDiffuse.g = 0.04f;
    TheTV.dcvDiffuse.b = 0.04f;

    TheTV.dcvSpecular.r = 0.05f;
    TheTV.dcvSpecular.g = 0.05f;
    TheTV.dcvSpecular.b = 0.05f;

    ////////////////////////////////////////////////////////////////////////////////////////////////


    // No Light added for now...
    LightsNumber = 0;

#ifdef EDIT_ENGINE
    m_FrameDrawMode = false;
    m_ScriptsOn = false;
#endif

    // Store the SETUP STATE for the renderer
    //Thsi state has to be stored at D3DD creation phase
    StoreSetupState();
    m_LinearFog = false;
}


// **** CREATION OF THE ZERO TEXTURE - SUCH TEXTURE IS USED BY TEXTURE STAGES IN PRESENCE OF UNTEXTURE4S SURFACES,
// TO RENDER THE APPROPRIATE WAY THE NVG VIEW
void CDXEngine::CreateZeroTexture(void)
{
    // Create the Zero Texture
    ZeroTex = new TextureHandle();
    ZeroTex->Create("", 0, 32, 64, 64, TextureHandle::FLAG_MATCHPRIMARY);

    // PHASE 5: in D3D11 fill ZeroTex with a real WHITE texture (previously skipped ->
    // m_pDDS=NULL -> polygons with texID=-1 sampled nothing -> white/broken). ZeroTex is needed
    // as a neutral white texture for untextured polygons (result = white * vertexcolor).
    if (g_bUseGpu) // #DX12/#104: bake via Load (no-op stub on any GPU backend); skip the dead DDraw Blt path
    {
        static DWORD s_white[64 * 64];
        for (int i = 0; i < 64 * 64; ++i)
            s_white[i] = 0xFFFFFFFF;
        ZeroTex->Load(0, 0,
                      (BYTE *)s_white); // bakes a white 64x64 -> valid SRV
        return;
    }

    // Artscout - 2026: [DX7-PURGE] DDraw surface GetPixelFormat/Blt colour-fill removed
    // (GPU path above bakes ZeroTex white via Load and returns).
}


void CDXEngine::Release(void)
{
    // #34 C1: no D3D7 device / state block to release.
    // Release the 2D Engine items
    DX2D_Release();

    // Release the Zero Texture
    if (ZeroTex)
    {
        delete ZeroTex;
        ZeroTex = NULL;
    }
}


// Setup the Environmental light properties
void CDXEngine::SetSunLight(float Ambient, float Diffuse, float Specular)
{
    // Artscout - 2026: SetSunLight is the once-per-frame light refresh (Render3D::StartDraw -> it runs
    // before the eye loop / the cockpit draw), so this is where the cockpit shadow replay is re-armed
    // for the new frame: its map is model-space and view-independent, and only the sun moves it.
    m_PitShadowDone = false;

    TheSun.dcvAmbient.r = TheSunColour.r * Ambient;
    TheSun.dcvAmbient.g = TheSunColour.g * Ambient;
    TheSun.dcvAmbient.b = TheSunColour.b * Ambient;

    TheSun.dcvDiffuse.r = TheSunColour.r * Diffuse;
    TheSun.dcvDiffuse.g = TheSunColour.g * Diffuse;
    TheSun.dcvDiffuse.b = TheSunColour.b * Diffuse;

    TheSun.dcvSpecular.r = TheSunColour.r * Specular;
    TheSun.dcvSpecular.g = TheSunColour.g * Specular;
    TheSun.dcvSpecular.b = TheSunColour.b * Specular;

#ifndef DEBUG_ENGINE
    TheTimeOfDay.GetLightDirection((Tpoint *)&LightDir);
    LightDir.x = -LightDir.x;
    LightDir.y = -LightDir.y;
    LightDir.z = -LightDir.z;
#endif

    // PHASE 6: port the directional sun light to D3D11 (object path, VS_Object
    // computes col = dwColour * saturate(ambient + sum N.L)). One directional source
    // (sun) + TOD ambient. Previously SetLights was not called -> FF_LIGHTING was off.
    if (g_bUseGpu and g_pRenderer)
    {
        float amb[4] = {TheSun.dcvAmbient.r, TheSun.dcvAmbient.g,
                        TheSun.dcvAmbient.b, 1.0f};
        GpuLightCPU sun;
        memset(&sun, 0, sizeof(sun));
        // Artscout - 2026: LightDir is ALREADY the ray direction (negated right
        // after GetLightDirection above) and every shader takes Ldir =
        // -Direction. Negating here as well lit the whole world from BELOW.
        sun.Direction[0] = LightDir.x;
        sun.Direction[1] = LightDir.y;
        sun.Direction[2] = LightDir.z;
        sun.Color[0] = TheSun.dcvDiffuse.r;
        sun.Color[1] = TheSun.dcvDiffuse.g;
        sun.Color[2] = TheSun.dcvDiffuse.b;
        sun.Params[1] = 0.0f; // directional
        g_pRenderer->SetLights(amb, 1, &sun, sizeof(sun));
        // Artscout - 2026: keep the per-object copy in step. Only FlushBuffers
        // used to fill it, so on the path that ends here UpdateDynamicLights
        // rebuilt every object's light set around a ZERO sun.
        g_d3d11Sun = sun;
        g_d3d11Amb[0] = amb[0];
        g_d3d11Amb[1] = amb[1];
        g_d3d11Amb[2] = amb[2];
        g_d3d11Amb[3] = amb[3];
    }
}


void CDXEngine::EnableCull(bool Status)
{
    m_bCullEnable = Status;
}

void CDXEngine::MoveDof(bool Status)
{
    m_bDofMove = Status;
}


extern DWORD gDebugTextureID;

// * Function referencing and loading textures given an Object Instance *
void CDXEngine::LoadTextures(DWORD ID)
{

    // Fetch the VB Data of this Model
    VBItemType VB;
    TheVbManager.GetModelData(VB, ID);

    gDebugLodID = ID;

    // Get the Textures Offsets
    DWORD *texOffset = VB.Texs;

    // Register each texture for the Model ( and load it if not available ) and setup local Textures List
    for (DWORD a = 0; a < VB.NTex; a++)
    {
#ifndef DEBUG_ENGINE
        gDebugTextureID = *texOffset;
#endif
        TheTextureBank.Reference(*texOffset++);
    }

    gDebugLodID = -1;
#ifndef DEBUG_ENGINE
    gDebugTextureID = -1;
#endif
}


// * Function Dereferencing textures given an Object Instance *
void CDXEngine::UnLoadTextures(DWORD ID)
{

    // Fetch the VB Data of this Model
    VBItemType VB;

    TheVbManager.GetModelData(VB, ID);

    // Consistency check
    if (not VB.Valid)
        return;

    // Get the Textures Offsets
    DWORD *texOffset = VB.Texs;


    // DeRegister each texture for the Model
    for (DWORD a = 0; a < VB.NTex; a++)
        TheTextureBank.Release(*texOffset++);
}


// Stenciling Functions
DWORD CDXEngine::SetStencilMode(DWORD Stencil)
{
    DWORD LastMode = (DWORD)m_StencilMode;

    // #34 D3D11: 3D-cockpit stencil mask via D3D11 state objects (dead D3D7 switch removed).
    m_StencilMode = (StencilModeType)Stencil;
    if (g_pRenderer)
    {
        switch (Stencil)
        {
        case STENCIL_WRITE:
            m_StencilRef++;
            g_pRenderer->SetStencil(2, m_StencilRef); // cockpit writes ref
            break;
        case STENCIL_CHECK:
            if (m_StencilRef)
                g_pRenderer->SetStencil(3, m_StencilRef); // world: ref>stencil
            else
                g_pRenderer->SetStencil(0, 0); // ref==0 -> ALWAYS
            break;
        case STENCIL_OFF:
        default:
            g_pRenderer->SetStencil(0, 0);
            break;
        }
    }
    return LastMode;
}


// * This Function just resets any Feature/lag fro a drawing
void CDXEngine::ResetFeatures(void)
{
    m_LastFlags.w = 0xffffffff;
    DXFlagsType Spare;
    Spare.w = 0x00;
    SetRenderState(m_LastFlags, Spare, DISABLE);
    m_LastFlags.w = 0;

    SelectTexture(-1);
    m_TexID = -1;
    LastTexID = 0xcccccccc;
}


// ********************* SURFACES STACK MANAGEMENT ***************************
// function Pushing in a surface in Surface stack
DWORD CDXEngine::PushSurface(SurfaceStackType *Stack, D3DXMATRIX *State)
{
    DWORD Level = Stack->StackLevel;

    // if enough space Stores the surface Data
    if (Stack->StackLevel < Stack->StackMax)
    {
        DWORD l = Stack->StackLevel++;
        Stack->Stack[l].Vb = m_VB;
        Stack->Stack[l].State = *State;
        Stack->Stack[l].Surface = m_NODE.BYTE;
        Stack->Stack[l].TexID = m_TexID;
        Stack->Stack[l].ObjInst = m_TheObjectInstance;
        Stack->Stack[l].FogLevel = m_FogLevel;
        Stack->Stack[l].Pit =
            m_PitMode; // Artscout - 2026: carry the pit flag to the deferred flush
        memcpy(Stack->Stack[l].LightMap, TheLightEngine.LightsToOn,
               sizeof(Stack->Stack[l].LightMap));
    }

    return Level;
}


// Function pushing a surface into stack, and putting it into sorting lop
bool CDXEngine::PushSurfaceIntoSort(SurfaceStackType *Stack, D3DXMATRIX *State)
{
    DWORD Level;

    Level = PushSurface(Stack, State);
    D3DXVECTOR3 Pos;
    Pos.x = State->m30, Pos.y = State->m31, Pos.z = State->m32;
    DX2D_AddObject(Level, LAYER_AUTO, Stack, &Pos);
    return true;
}


// function Popping out a surface from Surface stack
bool CDXEngine::PopSurface(SurfaceStackType *Stack, D3DXMATRIX *State)
{
    // if stack not empty the assign variables with stacked data
    if (Stack->StackLevel)
    {
        DWORD l = --Stack->StackLevel;
        m_VB = Stack->Stack[l].Vb;
        *State = Stack->Stack[l].State;
        m_NODE.BYTE = Stack->Stack[l].Surface;
        m_TexID = Stack->Stack[l].TexID;
        m_TheObjectInstance = Stack->Stack[l].ObjInst;
        m_FogLevel = Stack->Stack[l].FogLevel;
        m_SurfacePit =
            Stack->Stack[l].Pit; // Artscout - 2026: pit flag for DrawSurface's cull decision
        memcpy(TheLightEngine.LightsToOn, Stack->Stack[l].LightMap,
               sizeof(TheLightEngine.LightsToOn));
        return true;
    }

    return false;
}


// function Getting out a surface from Surface stack
bool CDXEngine::GetSurface(DWORD Level, SurfaceStackType *Stack,
                           D3DXMATRIX *State)
{
    // if stack not empty the assign variables with stacked data
    if (Level < Stack->StackLevel)
    {
        m_VB = Stack->Stack[Level].Vb;
        *State = Stack->Stack[Level].State;
        m_NODE.BYTE = Stack->Stack[Level].Surface;
        m_TexID = Stack->Stack[Level].TexID;
        m_TheObjectInstance = Stack->Stack[Level].ObjInst;
        m_FogLevel = Stack->Stack[Level].FogLevel;
        m_SurfacePit =
            Stack->Stack[Level].Pit; // Artscout - 2026: pit flag for DrawSurface's cull decision
        memcpy(TheLightEngine.LightsToOn, Stack->Stack[Level].LightMap,
               sizeof(TheLightEngine.LightsToOn));
        return true;
    }

    return false;
}


// Function pushing the DX render state into the states stack
void CDXEngine::SaveState(void)
{
    m_StatesStack[m_StatesStackLevel].RenderState = m_RenderState;

    if (m_StatesStackLevel < DX_MAX_NESTED_STATES)
        m_StatesStackLevel++;
}


void CDXEngine::RestoreState(void)
{
    if (m_StatesStackLevel)
    {
        m_RenderState = m_StatesStack[--m_StatesStackLevel].RenderState;
    }
    else
        m_RenderState = DX_OTW;
}


// funcion pushing in the Matrix Stack a Matrix
inline void CDXEngine::PushMatrix(D3DXMATRIX *p)
{
    StateStack[StateStackLevel] = *p;
    StateStackLevel++;
}

// funcion popping out the Matrix Stack a Matrix
inline void CDXEngine::PopMatrix(D3DXMATRIX *p)
{
    if (StateStackLevel)
        *p = StateStack[--StateStackLevel];
}


// Function Selecting Normal View Mode, no NVG, no TV
void CDXEngine::SetViewMode(void)
{
    // #34 D3D11: texture stages / NVG/TV modes are emulated by the FFEmu shader.
    m_AlphaTextureStage = 0;
}


// Function switching the renderer State
void CDXEngine::SetRenderState(DXFlagsType Flags, DXFlagsType NewFlags,
                               bool Enable)
{
    // #34 D3D11: per-surface render flags are D3D11 state objects (FFStateMap); no-op.
}


// *************************************** DRAW SECTION **********************************************

extern DWORD VCounter;
DWORD D3DErroCount;


// ********************************
// * the SURFACE DRAWING Function *
// ********************************
void CDXEngine::DrawSurface()
{
    // Artscout - 2026: the cockpit shadow replay draws DEPTH ONLY. Skipping the state block is not an
    // optimisation -- the texture/material/specular/emissive writes below would leak state into the
    // normal pit draws that run right after. The backend's shadow PSO has no pixel shader, so all the
    // replay needs is the SAME vertex/index issue as the main path (kept in step with the block at the
    // bottom of this function on purpose -- that is the only geometry source).
    if (m_ShadowWalk)
    {
        extern bool g_bUseD3D12;
        extern bool g_bUseVulkan;
        void *vbh = g_bUseD3D12 ? m_VB.VbD3D12 :
                    (g_bUseVulkan ? m_VB.VbVulkan : (void *)m_VB.VbD3D11);
        if (g_pRenderer and vbh)
        {
            void *idxPtr = m_NODE.BYTE + sizeof(DxSurfaceType);
            if (m_NODE.SURFACE->dwPrimType == D3DPT_POINTLIST)
                g_pRenderer->DrawObjectStrip(
                    m_NODE.SURFACE->dwPrimType, vbh, VERTEX_STRIDE,
                    (int)((DWORD) * ((Int16 *)idxPtr)),
                    (int)m_NODE.SURFACE->dwVCount);
            else
                g_pRenderer->DrawObjectIndexed(
                    m_NODE.SURFACE->dwPrimType, vbh, VERTEX_STRIDE, 0,
                    (unsigned short *)idxPtr, (int)m_NODE.SURFACE->dwVCount);
        }
        return;
    }

#ifdef DEBUG_ENGINE
    DXDrawCalls++;
    DXDrawVertices += m_NODE.SURFACE->dwVCount;
#endif


    DXFlagsType NewFlags;
    NewFlags.w = m_NODE.SURFACE->dwFlags.w;

    // #34: dead D3D7 emissive-source SetRenderState removed (D3D11 emissive via FF_EMISSIVE shader, #49).


    ////////////////////// Test if any change in rendering mode /////////////
    //if(NewFlags.StateFlags not_eq m_LastFlags.StateFlags){

#ifdef DEBUG_ENGINE
    DXStateChanges++;
#endif

    // Selects changed Flags
    DXFlagsType ChangedFlags, DisabledFlags, EnabledFlags;
    ChangedFlags.w = m_LastFlags.w xor NewFlags.w;
    DisabledFlags.w = ChangedFlags.w bitand (compl NewFlags.w);
    EnabledFlags.w = ChangedFlags.w bitand NewFlags.w;


    // Check for changes in lags affecting RENDERER STATE
    if (DisabledFlags.StateFlags)
        SetRenderState(DisabledFlags, NewFlags, DISABLE);

    /*if(EnabledFlags.StateFlags)*/
    SetRenderState(NewFlags, NewFlags, ENABLE);

    m_LastFlags.w = NewFlags.w;
    //}


    /////////////////////// TEXTURE CHANGE Feature //////////////////////////
    if (m_TexID not_eq LastTexID)
    {
        SelectTexture(m_TexID);
#ifdef DEBUG_ENGINE
        DXTexSwitches++;
#endif
        LastTexID = m_TexID;
    }


    ////////////////////// ZBIAS Checking done every time ////////////////////
#ifdef DEBUG_ENGINE

    if (UseZBias and m_LastZBias not_eq m_NODE.SURFACE->dwzBias)
    {
        m_LastZBias = m_NODE.SURFACE->dwzBias;
        m_pD3DD->SetRenderState(D3DRENDERSTATE_ZBIAS, m_LastZBias);
    }

#else

    if (m_LastZBias not_eq m_NODE.SURFACE->dwzBias)
    {
        m_LastZBias = m_NODE.SURFACE->dwzBias;
        // Artscout - 2026: this used to end here -- the value was read, cached, and thrown away ("#34:
        // dead D3D7 ZBIAS removed"), so every surface drew at the pass-wide bias. The models do use it:
        // ~10% of the shipped surfaces carry a non-zero dwzBias, which is what keeps coplanar detail
        // (decals, panel plates, thin fins) off the surface underneath. Without it they z-fight and
        // flicker as the camera moves. Bucket it -- the data is overwhelmingly 0 or 1 with a short tail
        // out to 16 -- and hand it to the backend, which folds it into its pipeline state.
        if (g_pRenderer)
        {
            const DWORD zb = m_LastZBias;
            const int level = (zb == 0) ? 0 : (zb <= 2) ? 1 : (zb <= 7) ? 2 : 3;
            g_pRenderer->SetObjectDepthBias(level);
        }
    }

#endif


    ///////////////// Bill Boarded Surfaces Management - START //////////////
    if (NewFlags.b.BillBoard)
    {
        // Apply the BillBoard Transformation
        D3DXMATRIX R = BBMatrix;
        R.m30 = AppliedState.m30;
        R.m31 = AppliedState.m31;
        R.m32 = AppliedState.m32;
        R.m33 = 1.0f;
        DX_SET_WORLD(R);
    }


    ////////////////////// Surface SPECULARITY  management ///////////////////////////
    if (TheMaterial.power not_eq m_NODE.SURFACE->SpecularIndex or
        m_LastSpecular not_eq m_NODE.SURFACE->DefaultSpecularity)
    {
        TheMaterial.power = m_NODE.SURFACE->SpecularIndex;
        m_LastSpecular = m_NODE.SURFACE->DefaultSpecularity;
        TheMaterial.dcvSpecular.r =
            (float)((m_LastSpecular >> 16) bitand 0xff) / 255.0f;
        TheMaterial.dcvSpecular.g =
            (float)((m_LastSpecular >> 8) bitand 0xff) / 255.0f;
        TheMaterial.dcvSpecular.b =
            (float)(m_LastSpecular bitand 0xff) / 255.0f;
        // #34: dead D3D7 SetMaterial removed (D3D11 material via shader, #29)
        // #29 D3D11: surface specular -> shader (Blinn-Phong from light 0). power=SpecularIndex,
        // color=dcvSpecular (from DefaultSpecularity). power=0 or color=0 -> no highlight.
        if (g_pRenderer)
            g_pRenderer->SetMaterialSpecular(
                TheMaterial.dcvSpecular.r, TheMaterial.dcvSpecular.g,
                TheMaterial.dcvSpecular.b,
                (float)m_NODE.SURFACE->SpecularIndex);
    }


#ifdef EDIT_ENGINE

    /////////////////////////////////THIS IS THE EDIT ENGINE CALL \\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\

    CheckHR(m_pD3DD->DrawPrimitive(m_NODE.SURFACE->dwPrimType, D3DFVF_MANAGED,
                                   m_NODE.BYTE + sizeof(DxSurfaceType),
                                   m_NODE.SURFACE->dwVCount, 0));

    //////////////////////////////////// END EDIT ENGINE CALL \\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\

#else

    //////////////////////////////////THIS IS THE GAME ENGINE CALL \\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\

    HRESULT hr;

    ///////////////////////// Draw the Primitive /////////////////////////////////
#ifdef INDEXED_MODE_ENGINE

    if (g_bUseGpu)
    {
        // PHASE 4/#DX12 п.4: draw from the per-model GPU mirror VB (D3D11 buffer or D3D12 resource).
        hr = 0;
        extern bool g_bUseD3D12;
        extern bool g_bUseVulkan;
        void *vbh = g_bUseD3D12 ?
                        m_VB.VbD3D12 :
                        (g_bUseVulkan ? m_VB.VbVulkan : (void *)m_VB.VbD3D11);
        if (g_pRenderer and vbh)
        {
            // Artscout - 2026: D3D7-parity back-face culling for object surfaces (see g_nObjCullMode).
            // The models carry two coincident windings on every thin fin; unculled, both rasterise at
            // the same depth and the tie-break speckles the fin between the two opposite-shaded sides.
            // The pit path (m_SurfacePit) covers the 3D pit AND the stores attached to it -- the
            // player's own wing missiles ride it, which is why the fins kept flickering while every
            // other object was already culled. g_bObjCullPit=0 keeps the old no-cull for that path.
            {
                extern int g_nObjCullMode;
                extern bool g_bObjCullPit;
                g_pRenderer->SetObjectCull((m_SurfacePit and not g_bObjCullPit) ? 0 : g_nObjCullMode);
            }

            // per-model buffer: vertices from 0, baseVertex=0, indices 0-based as is.
            void *idxPtr = m_NODE.BYTE + sizeof(DxSurfaceType);

            // Alpha-test (chroma cutout) -- strictly like D3D7: enable ONLY for
            // surfaces with the ChromaKey flag (see context.cpp/SetRenderState,
            // ALPHATESTENABLE is set only in the MPR_SE_CHROMA branch). Opaque
            // surfaces draw without cutout (dark texture RGB, even at alpha=0).
            g_pRenderer->SetAlphaTestEnabled(
                m_NODE.SURFACE->dwFlags.b.ChromaKey != 0);

            // Self-illuminated surfaces (emissive material = vertex COLOR2 / dwSpecular).
            // Artscout - 2026: this gate was INVERTED. D3D7 armed the emissive source for EVERY
            // surface in the object pass and only DISARMED it for a SwEmissive surface whose
            // switch is off:
            //     SetRenderState(EMISSIVEMATERIALSOURCE, D3DMCS_COLOR2);          // always
            //     if (SwitchValues && SwEmissive && !(SwitchValues[n] & mask))
            //         SetRenderState(EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);    // no glow
            // (dxengine.cpp of the D3D7 tree, commit 35b1e813; the same COLOR2 default is in the
            // pass state block and in ModelInit.) The port enabled it ONLY for SwEmissive
            // surfaces -- so every lamp the models author on a PLAIN surface lost its glow.
            // That is what the black lamps are: the F-16CJ's intake nav lamps are ONE untextured
            // Alpha triangle-fan pair (node 55576) whose centre vertices are diffuse 0xFF000000
            // with emissive 0xFFFF0000 / 0xFF00FF00 and whose rim fades to alpha 0. The colour is
            // ENTIRELY in COLOR2; the diffuse is black on purpose. Drop the emissive and the fan
            // draws as an opaque BLACK star in the middle of its own red glow. The landing-light
            // beam cone (node 74864) and the whole afterburner plume (nodes 75776..91152) are
            // authored the same way, and every one of the 3D pit's 396 surfaces carries a COLOR2.
            // g_bObjEmissiveAll = 0 restores the port's behaviour.
            bool afterburner =
                false; // #49 hoisted: also used to wrap the draw in additive blend
            {
                extern bool g_bObjEmissiveAll;
                extern bool g_bPitEmissive;
                // Artscout - 2026: the blanket COLOR2 emissive is right for the WORLD models and
                // wrong for the 3D pit, and the model data says why. In the external F-16 a bright
                // COLOR2 always comes with a BLACK diffuse -- the lamp-lens signature, the colour
                // lives in COLOR2 because there is nowhere else for it to live. The pit's big
                // untextured tub (LOD 4105 node@120, 9720 indices) carries COLOR2 on almost every
                // vertex, and only the 915 brightest of them are black-diffuse lamps (the caution
                // panel and its red warnings, in one 2.6 x 1.7 x 2.1 cluster). The 1345 in the band
                // below -- 0x9A9E9E, 0x7F7F7F, 0x656565, spread over the WHOLE tub -- sit on
                // ordinary painted structure: that is baked shading, not self-illumination, and
                // adding 0.4-0.6 of it unmodulated lights the cockpit up with every lamp switched
                // off. (An earlier session hit the same wall from the other side and concluded
                // COLOR2 is "a subtle specular" in pit models; half right -- it is both, and the
                // diffuse tells them apart.) Default off for the pit; PitEmissive 1 to see it.
                // A SwEmissive pit surface is an explicit authoring signal and still obeys its
                // switch below -- LOD 4105 has none, so this costs nothing today.
                //   ...but "pit" is the whole PIT PATH, which carries the player's own wings and
                // stores for the view out of the canopy, and their lamps must keep glowing. The
                // pit LOD has its own: node@364640 is the wingtip pair (y +/-15.3) and node@364368
                // the tail strobe, both Alpha|ChromaKey|**Textured**, dark-red diffuse with a white
                // COLOR2 -- the colour is in the texture. The baked-shading band that caused the
                // bright cockpit is all in the UNTEXTURED tub (node@120). So exempt textured
                // surfaces: D3D7 modulates the emissive by the texture, which bounds it by the art,
                // and it is only on an untextured pit surface that COLOR2 is both unbounded and
                // not a lamp. Without this the wing lamps went black when seen from the cockpit.
                const bool textured =
                    NewFlags.b.Texture and m_NODE.SURFACE->TexID[0] not_eq 0xFFFFFFFFu;
                bool emissive =
                    g_bObjEmissiveAll and
                    (g_bPitEmissive or not m_SurfacePit or textured);

                if (NewFlags.b.SwEmissive)
                {
                    if (m_TheObjectInstance->SwitchValues)
                        emissive =
                            (m_TheObjectInstance
                                 ->SwitchValues[m_NODE.SURFACE->SwitchNumber] &
                             m_NODE.SURFACE->SwitchMask) != 0;
                    else
                        emissive =
                            true; // no switch table -> D3D7 default keeps COLOR2 (glow)
                }

                g_pRenderer->SetEmissive(emissive);

                // #49 afterburner cone: among emissive surfaces, the AB cone is the one whose
                // switch is COMP_AB (0) / COMP_AB2 (30) -- exterior lights use other switch numbers
                // (tail strobe 7, nav 8, land 9). Flag it so the shader applies the warm flame
                // gradient (reference real_af.png) instead of the model's stylized blue emissive.
                // Require the Alpha flag: the AB cone is an Alpha surface (drawn in the alpha pass).
                // Restricting to Alpha keeps the additive blend flip INSIDE the alpha pass, where
                // restoring BLEND_ALPHA is correct. Without this, an opaque emissive surface with
                // switch 0 in the SOLID pass would leave alpha-blend + no-depth-write set for the
                // rest of the pass -> the whole aircraft turned translucent (interior showed through).
                //   Artscout - 2026: and require SwEmissive. SwitchNumber is only meaningful on a
                // SwEmissive surface -- on every other one it is simply 0, so once the emissive
                // gate above defaults to ON this test would have claimed every plain Alpha surface
                // in the model as the afterburner cone. The F-16CJ alone has 44 of them (the
                // exhaust glow shells, the landing-light beam), and each would have been recoloured
                // by the flame gradient and drawn additive.
                afterburner =
                    emissive && NewFlags.b.SwEmissive && NewFlags.b.Alpha &&
                    (m_NODE.SURFACE->SwitchNumber == 0 // COMP_AB
                     || m_NODE.SURFACE->SwitchNumber == 30); // COMP_AB2
                g_pRenderer->SetAfterburner(afterburner);
            }

            // #49 the afterburner cone is an Alpha surface (drawn in the alpha pass, BLEND_ALPHA ->
            // translucent/dull). Flip it to pure additive so it glows bright (then restore alpha
            // for the surrounding translucent surfaces e.g. canopy glass).
            if (afterburner)
                g_pRenderer->SetObjectAdditiveBlend(true);

            if (m_NODE.SURFACE->dwPrimType == D3DPT_POINTLIST)
                g_pRenderer->DrawObjectStrip(m_NODE.SURFACE->dwPrimType, vbh,
                                             VERTEX_STRIDE,
                                             (int)((DWORD) * ((Int16 *)idxPtr)),
                                             (int)m_NODE.SURFACE->dwVCount);
            else
                g_pRenderer->DrawObjectIndexed(
                    m_NODE.SURFACE->dwPrimType, vbh, VERTEX_STRIDE, 0,
                    (unsigned short *)idxPtr, (int)m_NODE.SURFACE->dwVCount);

            if (afterburner)
                g_pRenderer->SetObjectAdditiveBlend(
                    false); // restore alpha-pass blend
        }
    }
    // #34: dead D3D7 DrawPrimitiveVB/DrawIndexedPrimitiveVB else-branches removed (D3D11 draws above)


#else
    CheckHR(m_pD3DD->DrawPrimitiveVB(
        m_NODE.SURFACE->dwPrimType, m_VB.Vb,
        (DWORD) * ((Int16 *)(m_NODE.BYTE + sizeof(DxSurfaceType))) +
            m_VB.BaseOffset,
        m_NODE.SURFACE->dwVCount, 0));
#endif

    //////////////////////////////////// END GAME ENGINE CALL \\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\

#endif


#ifdef STAT_DX_ENGINE
    VCounter += m_NODE.SURFACE->dwVCount;
    COUNT_PROFILE("*** DX Draws ");

    if (hr)
        COUNT_PROFILE("*** DX ERRORS ");

#endif


    ///////////////// Bill Boarded Surfaces Management - END ////////////////////
    if (NewFlags.b.BillBoard)
    {
        // Get back to original transformation
        DX_SET_WORLD(AppliedState);
    }
}


// ********************************
// * DOF Process as in FreeFalcon code*
// ********************************

float CDXEngine::Process_DOFRot(float dofrot, int dofNumber, int flags,
                                float min, float max, float multiplier,
                                float unused)
{
    // Negated DOF
    if (flags bitand XDOF_NEGATE)
        dofrot = -dofrot;

    // DOF Limits
    if (flags bitand XDOF_MINMAX)
    {
        if (dofrot < min)
            dofrot = min;

        if (dofrot > max)
            dofrot = max;
    }

    // Scaled 0-1 DOF
    if (flags bitand XDOF_SUBRANGE and min not_eq max)
    {
        dofrot -= min;
        dofrot /= max - min;

        // Angular DOF
        if (flags bitand XDOF_ISDOF)
            dofrot *= (float)(3.14159 / 180.0);
    }

    // Final Scaling
    return (dofrot *= multiplier);
}


void CDXEngine::AssignDOFRotation(D3DXMATRIX *R)
{
    float DofRot;

    // ************ NORMAL ROTATION DOF **************
    if (m_NODE.DOF->Type == ROTATE)
    {
        DofRot = m_TheObjectInstance->DOFValues[m_NODE.DOF->dofNumber].rotation;
        // Apply DOF Rotation on X axis
        D3DXMatrixRotationX(R, DofRot);
        // Apply DOF transformation
        D3DXMatrixMultiply(R, R, &m_NODE.DOF->rotation);
    }


    // ************ EXTENDED ROTATION DOF **************
    if (m_NODE.DOF->Type == XROTATE)
    {
        DofRot = Process_DOFRot(
            m_TheObjectInstance->DOFValues[m_NODE.DOF->dofNumber].rotation,
            m_NODE.DOF->dofNumber, m_NODE.DOF->flags, m_NODE.DOF->min,
            m_NODE.DOF->max, m_NODE.DOF->multiplier, m_NODE.DOF->future);
        // Apply DOF Rotation on X axis
        D3DXMatrixRotationX(R, DofRot);
        // Apply DOF transformation
        D3DXMatrixMultiply(R, R, &m_NODE.DOF->rotation);
    }


    // ************ TRANSLATION DOF - NO ROTATION ******
    if (m_NODE.DOF->Type == TRANSLATE)
        D3DXMatrixIdentity(R);


    // *** SCALING DOF - ROTATION MATRIX USED TO SCALE ***
    if (m_NODE.DOF->Type == SCALE)
    {
        DofRot = Process_DOFRot(
            m_TheObjectInstance->DOFValues[m_NODE.DOF->dofNumber].rotation,
            m_NODE.DOF->dofNumber, m_NODE.DOF->flags, m_NODE.DOF->min,
            m_NODE.DOF->max, m_NODE.DOF->multiplier, m_NODE.DOF->future);

        // Apply Scaling at the destination Matrix
        ZeroMemory(R, sizeof(D3DXMATRIX));
        R->m00 = 1.0f - (1.0f - m_NODE.DOF->scale.x) * DofRot;
        R->m11 = 1.0f - (1.0f - m_NODE.DOF->scale.y) * DofRot;
        R->m22 = 1.0f - (1.0f - m_NODE.DOF->scale.z) * DofRot;
        R->m33 = 1.0f;
    }
}


void CDXEngine::AssignDOFTranslation(D3DXMATRIX *T)
{
    float DofRot;

    // *** NORMAL ROTATION DOF ***
    if (m_NODE.DOF->Type == ROTATE)
        D3DXMatrixTranslation(
            T,
            m_NODE.DOF->translation.x +
                m_TheObjectInstance->DOFValues[m_NODE.DOF->dofNumber]
                    .translation,
            m_NODE.DOF->translation.y, m_NODE.DOF->translation.z);

    // *** EXTENDED ROTATION DOF ***
    if (m_NODE.DOF->Type == XROTATE)
        D3DXMatrixTranslation(
            T,
            m_NODE.DOF->translation.x +
                m_TheObjectInstance->DOFValues[m_NODE.DOF->dofNumber]
                    .translation,
            m_NODE.DOF->translation.y, m_NODE.DOF->translation.z);

    // *** TRANSLATION DOF - NO ROTATION ***
    if (m_NODE.DOF->Type == TRANSLATE)
    {
        DofRot = Process_DOFRot(
            m_TheObjectInstance->DOFValues[m_NODE.DOF->dofNumber].rotation,
            m_NODE.DOF->dofNumber, m_NODE.DOF->flags, m_NODE.DOF->min,
            m_NODE.DOF->max, m_NODE.DOF->multiplier, m_NODE.DOF->future);
        // Get DOF base translation
        Ppoint P = m_NODE.DOF->translation;
        // Apply DOF Scaling
        P.x *= DofRot;
        P.y *= DofRot;
        P.z *= DofRot;
        // Andcreate translation Matrix
        D3DXMatrixTranslation(T, P.x, P.y, P.z);
    }

    // *** SCALING DOF ***
    if (m_NODE.DOF->Type == SCALE)
        D3DXMatrixTranslation(T, m_NODE.DOF->translation.x,
                              m_NODE.DOF->translation.y,
                              m_NODE.DOF->translation.z);
}


// ********************************
// * the NORMAL DOF Function      *
// ********************************
void CDXEngine::DOF(void)
{
    D3DXMATRIX R, T;

#ifdef DEBUG_ENGINE

    if (m_bDofMove)
    {
        float rot = sinf((float)timeGetTime() / 1500.0f);
        m_TheObjectInstance->DOFValues[m_NODE.DOF->dofNumber].rotation =
            ((float)PI / 6.0f) * rot;
    }

#endif

#ifndef DEBUG_ENGINE

    // * CONSISTENCY CHECK  *
    if (m_NODE.DOF->dofNumber >= m_TheObjectInstance->ParentObject->nDOFs)
        return;

#endif
    // **** CALCULATE THE DOF IMPOSED ROTATION ****
    AssignDOFRotation(&R);

    // **** CALCULATE THE DOF IMPOSED TRANSLATION ****
    AssignDOFTranslation(&T);

    // Mix All and set to Actual Applied State
    D3DXMatrixMultiply(&R, &R, &T);
    D3DXMatrixMultiply(&AppliedState, &R, &AppliedState);
    DX_SET_WORLD(AppliedState);
}


// ********************************
// * the DOF MANAGEMENT Function  *
// ********************************
void CDXEngine::DOFManage()
{

#ifdef EDIT_ENGINE
    m_DofLevel++;

    if (m_SkipSwitch)
        return;

#endif

    // Select the DOF Type
    switch (m_NODE.DOF->Type)
    {

    case NO_DOF:
        break;

        // * POSITIONAL DOF MANAGEMENT *
    case ROTATE:
    case XROTATE:
    case TRANSLATE:
    case SCALE:
        PushMatrix(&AppliedState);
#ifdef DEBUG_ENGINE
        //if(NODE.SURFACE->dwFlags.b.Disable) break;
#endif

        DOF();
        break;

    case SWITCH:
    case XSWITCH:
        SWITCHManage();
        break;
    }
}


// ***********************************
// * the switch MANAGEMENT Function  *
// ***********************************
void CDXEngine::SWITCHManage()
{

    //Consistency check
    if (not m_TheObjectInstance->SwitchValues)
    {
        // If no switches then skip the switch
        m_NODE.BYTE += m_NODE.DOF->dwDOFTotalSize;
        //and return
        return;
    }

    // Gets the Switch Number and value
    DWORD SWNumber = m_NODE.DOF->SwitchNumber;
    DWORD Value = m_TheObjectInstance->SwitchValues[SWNumber];
    BYTE *LastAddr = m_NODE.BYTE;

    if (m_NODE.DOF->Type == XSWITCH)
        Value = compl Value;

    // Traverse the Switch Items
    while (m_NODE.DOF->SwitchNumber == SWNumber and
           (m_NODE.DOF->Type == SWITCH or m_NODE.DOF->Type == XSWITCH))
    {
        // If value found then Exit here pointing the SWITCH, next to it is the SURFACE
        if (Value bitand (1 << m_NODE.DOF->SwitchBranch))
        {
            PushMatrix(&AppliedState);
            return;
        }

#ifdef EDIT_ENGINE
        m_SkipSwitch = true;
        m_DofLevel = 1;
        return;
#else
        m_NODE.BYTE += m_NODE.DOF->dwDOFTotalSize;
#endif
    }
}


// * This Function just Transformates the Object and pass it to the VB Manager for later Drawing *
// The 'CameraSpace' flag is used for child items from an undergoing draw, as the position is already relative to the camera
// and so need no camera relative calculations
// #16/#26 forward decl (falclib/include/isbad.h) for guarding dangling reads in the lights loop.
extern bool F4IsBadReadPtr(const void *lp, unsigned int ucb);

void CDXEngine::DrawObject(ObjectInstance *objInst, D3DXMATRIX *RotMatrix,
                           const Ppoint *Pos, const float sx, const float sy,
                           const float sz, const float scale, bool CameraSpace,
                           DWORD LightOwner)
{
    D3DXMATRIX Scale, State;
    D3DVECTOR p;
    bool Visible = false;
    DWORD Liter;
    DxDbHeader *Model;

#ifdef DEBUG_LOD_ID
    // Debug pahse of LODs, clear any label
    LodLabel[0] = 0;
#endif;

    // Consistency Check
    if (not objInst->ParentObject)
        return;

#ifndef DEBUG_ENGINE

    // Consistency Check
    if (objInst->id < 0 or objInst->id >= TheObjectListLength or
        objInst->TextureSet < 0)
        return;

#endif

    // if BLIT RADAR MODE, got to draw and return
    if (m_RenderState == DX_DBS)
    {
        DrawBlip(objInst, RotMatrix, Pos, sx, sy, sz, scale, CameraSpace);
        return;
    }


    // The object position is always calculated relative to the camera position
    // if coming from out world, if IN CAMERA SPACE, position is already relative to camera,
    // and even visibility is skipped
    if (CameraSpace)
    {
        p.x = Pos->x;
        p.y = Pos->y;
        p.z = Pos->z;
        State = *RotMatrix;
        Visible = true;
    }
    else
    {
        p.x = -CameraPos.x + Pos->x;
        p.y = -CameraPos.y + Pos->y;
        p.z = -CameraPos.z + Pos->z;
    }


    // NEW TEXTURE MANAGEMENT
    // if Textures not referenced, refernce them
#ifndef DEBUG_ENGINE

    if (not objInst->TexSetReferenced)
    {
        objInst->ParentObject->ReferenceTexSet(objInst->TextureSet);
        objInst->TexSetReferenced = true;
    }

#endif
    ///////////////////////////////// CHECK FOR AVAILABLE LOD ///////////////////////////////////////
    // get the object distance
    float LODRange = sqrtf(p.x * p.x + p.y * p.y + p.z * p.z) * m_LODBiasCx;
    // The model pointer
    ObjectLOD *CurrentLOD = NULL;
    // Calculate the LOD based on FOV
    float MaxLODRange;
    int LODused;
    CurrentLOD =
        objInst->ParentObject->ChooseLOD(LODRange, &LODused, &MaxLODRange);

    // if not a lod persent, end here
    if (not CurrentLOD)
        return;

    // ok assign The Model
    Model = (DxDbHeader *)CurrentLOD->root;

    // FRB - Filter out bad/nonexistant models
    if ((Model->Id <= 0) or (Model->Id >= (unsigned int)TheObjectLODsCount))
        return;

    ///////////////////////////////// HERE CHECK FOR VISIBILITY /////////////////////////////////////
    // Camera Spacce objects are always visible
    if (not CameraSpace)
    {
#ifndef DEBUG_ENGINE
        // Compute the object visibility -  Return if Clipped out
        D3DVALUE r = (D3DVALUE)(objInst->Radius() * scale);
        DWORD ClipResult =
            0; // PHASE 4: D3D11 -- without the D3D7 clip test treat as visible (frustum cull later)

        // if Visible assert it, if not visible got to check for Lights
        if (ClipResult bitand D3DSTATUS_DEFAULT)
            goto LightCheck;

        Visible = true;

        // ************ ADD Other Features ***********
        D3DXMatrixIdentity(&Scale);
        Scale.m00 = scale * sx;
        Scale.m11 = scale * sy;
        Scale.m22 = scale * sz;
        D3DXMatrixMultiply(&State, RotMatrix, &Scale);
        // *******************************************
#else
        State = *RotMatrix;
#endif

        // *********** Base transformations **********
        D3DXMatrixTranslation(&Scale, p.x, p.y, p.z);
        D3DXMatrixMultiply(&State, &State, &Scale);
        // *******************************************

        // #16 DIAG REMOVED (#10): per-object/per-frame fopen("objxform_diag.txt") stalled rendering on
        // the ground (tons of file I/O) -- the cockpit could not appear in time. The block was purely
        // diagnostic (projection to a file), did not affect rendering.
    }

    // check if child enlighted
    if (LightOwner not_eq NULL)
        Liter = LightOwner;
    else
    {
        if (not ++LightID)
            LightID++;

        Liter = LightID;
    }

    // Pass the object to the vertex Buffer
#ifdef DEBUG_ENGINE
    VBItemType VB;
    TheVbManager.AddDrawRequest(objInst, objInst->id, &State, true, Liter);
    TheVbManager.GetModelData(VB, objInst->id);

    if (((DxDbHeader *)VB.Root)->dwLightsNr)
    {
        DXLightType *Light =
            (DXLightType *)(VB.Root + ((DxDbHeader *)VB.Root)->pLightsPool);
        DWORD LightsNr = ((DxDbHeader *)VB.Root)->dwLightsNr;

        while (LightsNr--)
        {
            if (objInst->SwitchValues[Light->Switch] bitand Light->SwitchMask)
                TheLightEngine.AddDynamicLight(Liter, Light, RotMatrix, &p,
                                               100);

            Light++;
        }
    }

#else

    ////////////////////////// HERE ONLY IF VISIBLE OR TO TEST FOR LIGHTS \\\\\\\\\\\\\\\\\\\\\\\\\\\\\

    //COUNT_PROFILE("DRAWN OBJECTS");
    // if object is visible and requested to draw ( may be also only add lights ) draw it
    if (Visible)
    {

#ifdef DEBUG_LOD_ID
        strcpy(LodLabel, TheLODNames[Model->Id]);
#endif
        // FOG CALCULATION
        // We r calculating the max range That should be valid for the LINEAR FOR MODE
        // to have m_FogLevel level at LODRange distance...
        float FogLevel = (m_FogLevel < 1.0f) ?
                             LODRange / ((1 - m_FogLevel) * m_LODBiasCx) :
                             m_LinearFogLevel;

        // if just a DOT the draw it as dynamic item
        if (Model->dwNVertices == 1)
        {
            //Calculate Specularness based on sunlight direction
            float Si;
            D3DXVECTOR3 Op;
            D3DXVec3Normalize(&Op, (D3DXVECTOR3 *)&p);
            //
            Op = Op - *(D3DXVECTOR3 *)&LightDir;
            /* Op = Op * Op;*/
            Si = 2.0f - (Op.x + Op.y + Op.z);

            // Ok, this is a HACK I do not like... seems nothing at render level lets u understand what kind of object u r going to render
            // only clue is fron DXM model header, that has a Class Air/Ground/Feature index...
            // hoping it is updated...

            // * Any class not air/ground get a normal draw
            if (Model->VBClass not_eq VB_CLASS_DOMAIN_GROUND and
                Model->VBClass not_eq VB_CLASS_DOMAIN_AIR)
                Si = 0.2f;

            // Ground vehicles, hi Q reflection index
            if (Model->VBClass == VB_CLASS_DOMAIN_GROUND)
            {
                Si *= Si * (1 - PRANDFloatPos() * 0.1f);
                Si *= Si;
                Si *= Si;
                Si /= 256.0f;

                if (Si < 0.2f)
                    Si = 0.2f;
            }

            // Air vehicles, lower Q...
            if (Model->VBClass == VB_CLASS_DOMAIN_AIR)
            {
                Si *= Si * (1 - PRANDFloatPos() * 0.6f);
                Si /= 4.0f;

                if (Si < 0.3f)
                    Si = 0.3f;
            }

            // Calculate the color based on Fog level
            // DWORD Color=(min(255,FloatToInt32(m_FogLevel*255.f)) << 24)+0x102010;
            DWORD Color = F_TO_UARGB(min(255.0f, F_I32(m_FogLevel * 255.f)),
                                     Si * 240.0f, Si * 255.0f, Si * 240.0f);
            Draw3DPoint((D3DVECTOR *)Pos, Color);
#ifdef DEBUG_LOD_ID
            strcpy(LodLabel, ".");
#endif
        }
        else
            TheVbManager.AddDrawRequest(
                objInst, Model->Id, &State,
                (LODRange <= (DYNAMIC_LIGHT_INSIDE_RANGE * 2)) ? true : false,
                Liter, FogLevel);
    }

LightCheck:
#ifdef LIGHT_ENGINE_DEBUG
    START_PROFILE("LIGHTS ON TIME");
#endif

    // if inside Lights visibility range, check for lights --- FRB - Bad dwLightsNr check and SwitchValues
    //if(LODRange<=DYNAMIC_LIGHT_INSIDE_RANGE and Model->dwLightsNr and (Model->dwLightsNr<11) and (objInst->SwitchValues))
    if (LODRange <= DYNAMIC_LIGHT_INSIDE_RANGE and Model->dwLightsNr)
    {
        // Get the Lights area in the model
        DXLightType *Light =
            (DXLightType *)((char *)Model + Model->pLightsPool);
        // The number of lights
        DWORD LightsNr = Model->dwLightsNr;

        // and add all of them to the dynamic lights list
        while (LightsNr--)
        {
            // #16/#26 guard: Switch==-1 means "always on". Otherwise index objInst->SwitchValues
            // ONLY if the array is present and readable up to that index -- a dangling/garbage
            // objInst->SwitchValues or a bogus Light->Switch was crashing here on 3D entry
            // (PreLoadScene -> DrawableBuilding). If unreadable, treat the light as off.
            bool lightOn = (Light->Switch == -1);

            if (not lightOn and Light->Switch >= 0 and objInst->SwitchValues and
                not F4IsBadReadPtr(objInst->SwitchValues,
                                   (unsigned)(Light->Switch + 1) *
                                       sizeof(objInst->SwitchValues[0])))
            {
                lightOn = (objInst->SwitchValues[Light->Switch] bitand
                           Light->SwitchMask) != 0;
            }

            if (lightOn)
                TheLightEngine.AddDynamicLight(Liter, Light, RotMatrix, &p,
                                               LODRange);

            Light++;
        }
    }

#ifdef LIGHT_ENGINE_DEBUG
    STOP_PROFILE("LIGHTS ON TIME");
#endif
#endif
}


void CDXEngine::FlushInit(void)
{
    // if not yet created create the Zero Texture
    if (not ZeroTex)
        CreateZeroTexture();

    D3DXMATRIX unit;
    D3DXMatrixIdentity(&unit);

    // #34 D3D11: identity world into cbObject (dead D3D7 material/render-state setup removed).
    DX_SET_WORLD(unit);
    m_LastZBias = DEFAULT_ZBIAS;

    // Select the appropriate View Mode
    SetViewMode();
    //Reset Features
    ResetFeatures();
}


inline void CDXEngine::DrawNode(ObjectInstance *objInst, DWORD LightOwner,
                                DWORD LodID)
{
    // Selects actions for each node
    switch (m_NODE.HEAD->Type)
    {


    case DX_SWITCH:
    case DX_LIGHT:
    case DX_TEXTURE:
    case DX_MATERIAL:
    case DX_ROOT:
        break;


        // * SURFACE MANAGEMENT *
    case DX_SURFACE: // Setup the Texture setup the Texture to be used
        // Artscout - 2026: cockpit shadow replay -- draw every OPAQUE surface immediately, in model
        // space. No alpha/solid deferral (depth ordering is irrelevant to a depth map) and no GLASS:
        // an Alpha surface is the canopy / HUD glass, which must not cast an opaque shadow into the pit.
        if (m_ShadowWalk)
        {
            if (not m_NODE.SURFACE->dwFlags.b.Alpha)
            {
                m_SurfacePit = true;
                DrawSurface();
            }
            break;
        }
#ifdef EDIT_ENGINE
        if (m_SkipSwitch)
            break;

#endif

        if (m_NODE.SURFACE->dwFlags.b.Texture and
            m_NODE.SURFACE->TexID[0] not_eq -1)
            m_TexID = m_TexUsed[m_NODE.SURFACE->TexID[0]];
        else
            m_TexID = -1;


        // Alpha Surfaces are deferred to another Draw
        if (m_NODE.SURFACE->dwFlags.b.Alpha)
        {
#ifdef STAT_DX_ENGINE
            COUNT_PROFILE("Alpha Surfaces Nr");
#endif
            // PushSurface(&m_AlphaStack, &AppliedState);
            PushSurfaceIntoSort(&m_AlphaStack, &AppliedState);
            break;
        }

        // Solid Surfaces are deferred to another Draw
        if (m_NODE.SURFACE->dwFlags.b.VColor)
        {
#ifdef STAT_DX_ENGINE
            COUNT_PROFILE("Solid Surfaces Nr");
#endif
            PushSurface(&m_SolidStack, &AppliedState);
            break;
        }

        m_SurfacePit =
            m_PitMode; // Artscout - 2026: immediate draw -- cull decision follows the live pit flag
        DrawSurface();
        break;

    case DX_DOF:
        DOFManage();
        break;

    case DX_ENDDOF:
#ifdef EDIT_ENGINE
        if (m_SkipSwitch)
        {
            m_DofLevel--;

            if (not m_DofLevel)
                m_SkipSwitch = false;

            break;
        }

#endif
        PopMatrix(&AppliedState);
        DX_SET_WORLD(AppliedState);
        break;

        // if bad slot exit else get the Slot Children
    case DX_SLOT:
        // Artscout - 2026: the shadow replay runs BEFORE FlushObjects, so the slot children (attached
        // stores) have not been queued yet and there is nothing to descend into. They also sit outside
        // the pit's fitted shadow extent -- the pit's own shell is the only occluder that matters here.
        if (m_ShadowWalk)
            break;
#ifdef EDIT_ENGINE
        if (m_SkipSwitch)
            break;

#endif

        if (m_NODE.SLOT->SlotNr >= objInst->ParentObject->nSlots)
            break;

        {
            ObjectInstance *subObject =
                objInst->SlotChildren[m_NODE.SLOT->SlotNr];

            if (not subObject)
                break;

            D3DXMATRIX p;
            D3DXMatrixMultiply(&p, &m_NODE.SLOT->rotation, &AppliedState);
            Ppoint k;
            k.x = 0;
            k.y = 0;
            k.z = 0;
            // Draw the object IN CAMERA SPACE - Child always depend on parent Lights...
            DrawObject(subObject, &p, &k, 1, 1, 1, 1, true, LightOwner);
        }
        break;

    default:
        char s[128];
        printf(s, "Corrupted Model ID : %d ", LodID);
        MessageBox(NULL, s, "DX Engine", NULL);
    }
}


void CDXEngine::FlushObjects(void)
{

    ObjectInstance *objInst = NULL;
    DWORD LodID;
    bool Lited, WasInPitMode;
    DWORD LightOwner;

    //TheTextureBank.SetDeferredLoad(true);

    // not a previous object instalce
    m_LastObjectInstance = NULL;

    // COBRA - RED - The it stuff... Pits need to be stenciled, so, all its objects are popped as 1st
    // from the VB Manager, and then drawn, its solid suraces too are to be drawn just after
    // finished Pit mode
    WasInPitMode = false;

    ///////////////////////////// HERE STARTS THE DRAWING ENGINE LOOP //////////////////////////////
    // The Loop flushes all objects from the VBuffers

    // Till objects to Draw
    while (TheVbManager.GetDrawItem(&objInst, &LodID, &AppliedState, &Lited,
                                    &LightOwner, &m_FogLevel))
    {
        // ok, just entered Pit Mode
        if (m_PitMode and not WasInPitMode)
        {
            //START_PROFILE("3D PIT");
            // enable stenciling in Write Mode
            SetStencilMode(STENCIL_WRITE);
            // #34 D3D11: pit fog is handled by the shader; dead D3D7 FOGSTART removed
        }

        // ok, just Exited Pit Mode
        if (not m_PitMode and WasInPitMode)
        {
            // Save transformation State
            D3DXMATRIX OldState = AppliedState;
            // Immediatly draw Solid surfaces ( coming from Pit )
            DrawSolidSurfaces();
            AppliedState = OldState;
            // enable stenciling in Check Mode
            SetStencilMode(STENCIL_CHECK);
            // #34 D3D11: pit fog is handled by the shader; dead D3D7 FOGSTART removed
        }

        WasInPitMode = m_PitMode;

        // The Stack For the State Transformations resetted
        StateStackLevel = 0;

        // Consistency Check
        if (not objInst)
            continue;

        // assign for engine use
        m_TheObjectInstance = objInst;

        // gets the pointer to the Model Vertex Buffer
        TheVbManager.GetModelData(m_VB, LodID);

        // Consistency Check
        if (not m_VB.Valid)
            continue;

#ifdef STAT_DX_ENGINE
        COUNT_PROFILE("*** DX Objects");
#endif
        // Execute the Scripts 0 bitand 1 if existant
        DXScriptVariableType *Script = ((DxDbHeader *)m_VB.Root)->Scripts;
        D3DVECTOR pos;
        pos.x = AppliedState.m30;
        pos.y = AppliedState.m31;
        pos.z = AppliedState.m32;

        if (Script[0].Script)
            if (not DXScriptArray[Script[0].Script](&pos, objInst,
                                                    Script[0].Arguments))
                goto DrawSection;

        if (Script[1].Script)
            (not DXScriptArray[Script[1].Script](&pos, objInst,
                                                 Script[1].Arguments));

    DrawSection:

#ifndef DEBUG_ENGINE
#ifdef LIGHT_ENGINE_DEBUG
        START_PROFILE("LIGHTS UPDATE TIME");
#endif
#endif

        gDebugLodID = LodID;

        // Update the lights for the object
        if (Lited)
            TheLightEngine.UpdateDynamicLights(LightOwner, &pos,
                                               objInst->Radius());

#ifndef DEBUG_ENGINE
#ifdef LIGHT_ENGINE_DEBUG
        STOP_PROFILE("LIGHTS UPDATE TIME");
#endif
#endif


        // Ok... transform the object
        DX_SET_WORLD(AppliedState);
        // Stup the Fog level fro this object
        // #34: dead D3D7 FOGEND removed


        // Calculates the Texture Base Index in the Texture Bank
        int nTexsPerBank =
            m_VB.NTex / max(1, objInst->ParentObject->nTextureSets);
        DWORD *texOffset = m_VB.Texs + objInst->TextureSet * nTexsPerBank;

        // Register each texture for the Model ( and load it if not available ) and setup local Textures List
        for (int a = 0; a < nTexsPerBank; a++)
            m_TexUsed[a] = *texOffset++;

        //////////////////////// ********* HERE STARTS THE REAL NODES PARSING ***** ///////////////////////////////////
        //                                                                                                           //
        //                                                                                                           //
        //                                                                                                           //
        //                                                                                                           //
        // // Starting address
        m_NODE.BYTE = (BYTE *)m_VB.Nodes;

        // Till end of Model
        // #54 LOAD-HANG GUARD: this traversal holds cs_VbManager (taken in FlushBuffers:2344);
        // a broken/half-loaded model with dwNodeSize==0 (or a lost DX_MODELEND) -> INFINITE loop
        // -> the lock is never released -> the loader thread hangs forever in SetupModel (LOCK_VB_MANAGER).
        // Bail out on a zero step and on a cap = the model's declared node count (+slack).
        long _ndGuard = 0;
        long _ndMax =
            (long)m_VB.NNodes +
            16; // a model cannot have more nodes than its header declares

        while (m_NODE.HEAD->Type not_eq DX_MODELEND)
        {
            // Draw the Node
            DrawNode(objInst, LightOwner, LodID);
            // Traverse the model
            DWORD _ndStep = m_NODE.HEAD->dwNodeSize;

            // #54: a zero-sized node (or runaway count) would loop forever holding cs_VbManager;
            // bail out of the traversal instead of hanging.
            if (_ndStep == 0 or ++_ndGuard > _ndMax)
                break;

            m_NODE.BYTE += _ndStep;
        }

        //                                                                                                           //
        //                                                                                                           //
        //                                                                                                           //
        //                                                                                                           //
        //                                                                                                           //
        ///////////////////////////////////////////////////////////////////////////////////////////////////////////////
    }

    //TheTextureBank.SetDeferredLoad(false);
}


void CDXEngine::DrawAlphaSurfaces(void)
{
    D3DXMATRIX State;
    ObjectInstance *LastObj = NULL;
    float LastFog = 0;

    if (g_pRenderer) // PHASE 5: translucent surfaces (canopy glass) -- alpha-blend (D3D7 removed #34)
        g_pRenderer->SetObjectAlphaBlend(true);

    while (PopSurface(&m_AlphaStack, &State))
    {
        if (AppliedState not_eq State)
            DX_SET_WORLD(State);

        AppliedState = State;
#ifndef DEBUG_ENGINE
#ifdef LIGHT_ENGINE_DEBUG
        START_PROFILE("LIGHTS ON TIME");
#endif
#endif

        // if Changed object, remap all lights
        if (LastObj not_eq m_TheObjectInstance)
            TheLightEngine.EnableMappedLights();

        LastObj = m_TheObjectInstance;

#ifndef DEBUG_ENGINE
#ifdef LIGHT_ENGINE_DEBUG
        STOP_PROFILE("LIGHTS ON TIME");
#endif
#endif

        DrawSurface();
    }

    if (g_pRenderer) // PHASE 5: restore the opaque state (D3D7 removed #34)
        g_pRenderer->SetObjectAlphaBlend(false);
}


void CDXEngine::DrawSortedAlpha(DWORD Level, bool SetupMode)
{

    D3DXMATRIX State;

    // Initialize data parameters
    if (SetupMode)
        FlushInit();

    // Artscout - 2026: this is a MODEL alpha surface (`POLY_3DOBJECT`), not a 2D sprite, and in
    // D3D7 it was drawn with the object-pass lighting state. The port inherited whatever 2D state
    // ran last -- texture + vertex colour, NO lighting -- so glass surfaces never responded to
    // outside brightness (the F-16's gold canopy stayed gold at night). Restore the object pass
    // base flags; DrawSurface still re-issues the per-surface caches (texture, specular, dwzBias,
    // emissive / afterburner). Called per item, because 2D items can run between 3D ones.
    if (g_pRenderer)
    {
        g_pRenderer->BeginObjectPass();
        // Artscout - 2026: ...but BeginObjectPass rebuilds the pass FLAG WORD from scratch
        // (FF_VERTEXCOLOR | FF_LIGHTING | FF_ALPHATEST), and FF_TEXTURE0 is not in it -- that flag
        // is only ever set as a side effect of SetTexture. DrawSurface re-issues SelectTexture
        // ONLY when the surface's texture differs from LastTexID, so every sorted-alpha item that
        // happens to reuse the previous item's texture drew with the flag cleared: the pixel
        // shader then skips the texture stage entirely, keeping texA = 1, and an alpha-shaped
        // sprite becomes an OPAQUE flat quad of its vertex colour times the light.
        //   That is the wingtip "grey box". The F-16CJ's lamp glows are five camera-facing
        // billboards under switch 8 (nodes 54524/54812/55036/55260/55484) sharing ONE texture --
        // a white sheet whose corona lives entirely in the ALPHA channel (77% of the lamp's cell
        // is alpha 0). With the flag lost, all of that is discarded and the quad paints its full
        // 1.5 ft square over the scene. Measured in `graybox.rdc`: the wing skin draws with
        // gFlags 0x10000D (FF_TEXTURE0 set), the two glow billboards over it with 0x10000C.
        //   Invalidating the cache here costs one redundant SetTexture per alpha item and keeps
        // the flag word and the bound texture in step.
        LastTexID = 0xcccccccc;
    }

    // Setup Alpha features
    if (g_pRenderer) // PHASE 5: sorted transparency (D3D7 removed #34)
        g_pRenderer->SetObjectAlphaBlend(true);

    // Get the surface data and update transformations / features
    GetSurface(Level, &m_AlphaStack, &State);
    DX_SET_WORLD(State);
    AppliedState = State;

    if (m_LastObjectInstance not_eq m_TheObjectInstance)
        TheLightEngine.EnableMappedLights(),
            m_LastObjectInstance = m_TheObjectInstance;

    // Draw the surface
    DrawSurface();
}


void CDXEngine::DrawSolidSurfaces(void)
{
    D3DXMATRIX State;
    ObjectInstance *LastObj = NULL;
    float LastFog = 0;

    // #34 D3D11: cull/zwrite come from BeginObjectPass (dead D3D7 state setup removed).

    while (PopSurface(&m_SolidStack, &State))
    {
        if (State not_eq AppliedState)
            DX_SET_WORLD(State);

        AppliedState = State;
#ifndef DEBUG_ENGINE
#ifdef LIGHT_ENGINE_DEBUG
        START_PROFILE("LIGHTS ON TIME");
#endif
#endif

        // if Changed object, remap all lights
        if (LastObj not_eq m_TheObjectInstance)
            TheLightEngine.EnableMappedLights();

        LastObj = m_TheObjectInstance;
#ifndef DEBUG_ENGINE
#ifdef LIGHT_ENGINE_DEBUG
        STOP_PROFILE("LIGHTS ON TIME");
#endif
#endif

        DrawSurface();
    }
}


#ifdef DEBUG_ENGINE
// Artscout - 2026: #34 removed dead DrawFrameSurfaces (EDIT_ENGINE wireframe draw; no callers, all D3D7 m_pD3DD).

#endif


// Artscout - 2026: cockpit sun shadows -- the depth-only replay of the pit's own geometry.
//   Fit: an ortho along the sun's direction over the pit model's bounding box, in the PIT'S MODEL
// SPACE. That frame is what makes this cheap: the pit is a rigid shell, so its self-shadow depends on
// the sun direction relative to the model, not on where the camera or the aircraft is. The map is
// therefore view-independent and identical for both VR eyes, and it is built from the same node walk
// the normal draw uses -- same DOF/switch handling, no second geometry path to drift.
//   Alpha (canopy/glass) surfaces are skipped: transparent glass must not cast an opaque shadow.
//   Attached stores are NOT replayed: at this point they have not been queued (they are added as slot
// children during the normal walk), and they live outside the pit's fitted extent anyway.
bool CDXEngine::RenderPitShadowMap(void)
{
    extern bool g_bPitShadow;
    extern float g_fPitShadowStrength;
    if (!g_bPitShadow || m_ShadowWalk || !g_pRenderer || !g_pRenderer->PitShadowSupported())
        return false;
    if (!g_bUseGpu)
        return false;

    CDrawItem *item = TheVbManager.GetPitListRoot();
    if (!item || !item->Object)
        return false;
    ObjectInstance *objInst = (ObjectInstance *)item->Object;
    if (!objInst->ParentObject)
        return false;

    // The pit model's own extent, in model units (the same units the vertex data uses; the pit draws
    // at scale 1 under the DX engine -- see COCKPIT-OVERHAUL.md).
    const float minX = objInst->ParentObject->minX, maxX = objInst->ParentObject->maxX;
    const float minY = objInst->ParentObject->minY, maxY = objInst->ParentObject->maxY;
    const float minZ = objInst->ParentObject->minZ, maxZ = objInst->ParentObject->maxZ;
    if (maxX - minX < 1.0e-3f || maxY - minY < 1.0e-3f || maxZ - minZ < 1.0e-3f)
        return false;

    // The sun, in the pit's model frame. LightDir is the RAY direction (away from the sun; see
    // SetSunLight), and the item's RotMatrix is the pit's world rotation. Row-vector convention:
    // multiplying a direction through the matrix's rotation gives its components along the model axes.
    float toSun[3] = {-LightDir.x, -LightDir.y, -LightDir.z};
    {
        const float len = (float)sqrt(toSun[0] * toSun[0] + toSun[1] * toSun[1] + toSun[2] * toSun[2]);
        if (len < 1.0e-6f)
            return false;
        toSun[0] /= len;
        toSun[1] /= len;
        toSun[2] /= len;
    }
    const D3DXMATRIX &R = item->RotMatrix;
    float s[3];
    s[0] = toSun[0] * R.m00 + toSun[1] * R.m10 + toSun[2] * R.m20; // dot(toSun, row0)
    s[1] = toSun[0] * R.m01 + toSun[1] * R.m11 + toSun[2] * R.m21;
    s[2] = toSun[0] * R.m02 + toSun[1] * R.m12 + toSun[2] * R.m22;
    {
        const float len = (float)sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
        if (len < 1.0e-6f)
            return false;
        s[0] /= len;
        s[1] /= len;
        s[2] /= len;
    }

    // Light-space basis. forward = the direction the light "camera" looks (from the sun at the pit).
    const float f[3] = {-s[0], -s[1], -s[2]};
    float up0[3] = {0.0f, 0.0f, 1.0f};
    if (fabsf(f[2]) > 0.9f)
    {
        up0[0] = 0.0f;
        up0[1] = 1.0f;
        up0[2] = 0.0f;
    }
    float r[3], u[3];
    // right = normalize(cross(up0, forward)); up = cross(forward, right)
    r[0] = up0[1] * f[2] - up0[2] * f[1];
    r[1] = up0[2] * f[0] - up0[0] * f[2];
    r[2] = up0[0] * f[1] - up0[1] * f[0];
    {
        const float len = (float)sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        if (len < 1.0e-6f)
            return false;
        r[0] /= len;
        r[1] /= len;
        r[2] /= len;
    }
    u[0] = f[1] * r[2] - f[2] * r[1];
    u[1] = f[2] * r[0] - f[0] * r[2];
    u[2] = f[0] * r[1] - f[1] * r[0];

    // Fit in the LIGHT'S OWN VIEW SPACE, measured from the bbox centre. The eye is then placed on
    // the sun side of the box by the depth span, so every corner is in front of the light camera --
    // and, critically, the ortho's depth bounds are measured from THAT eye, not from the centre.
    // (Mixing the two offsets the depth by the eye pull-back; a map whose depths do not match the
    // matrix that samples it shadows the wrong surfaces.)
    const float ccx = (minX + maxX) * 0.5f;
    const float ccy = (minY + maxY) * 0.5f;
    const float ccz = (minZ + maxZ) * 0.5f;
    float vx0 = 0.0f, vx1 = 0.0f, vy0 = 0.0f, vy1 = 0.0f, vz0 = 0.0f, vz1 = 0.0f;
    bool first = true;
    for (int c = 0; c < 8; ++c)
    {
        const float dx = ((c & 1) ? maxX : minX) - ccx;
        const float dy = ((c & 2) ? maxY : minY) - ccy;
        const float dz = ((c & 4) ? maxZ : minZ) - ccz;
        const float vx = dx * r[0] + dy * r[1] + dz * r[2];
        const float vy = dx * u[0] + dy * u[1] + dz * u[2];
        const float vz = dx * f[0] + dy * f[1] + dz * f[2];
        if (first)
        {
            vx0 = vx1 = vx;
            vy0 = vy1 = vy;
            vz0 = vz1 = vz;
            first = false;
        }
        else
        {
            if (vx < vx0) vx0 = vx;
            if (vx > vx1) vx1 = vx;
            if (vy < vy0) vy0 = vy;
            if (vy > vy1) vy1 = vy;
            if (vz < vz0) vz0 = vz;
            if (vz > vz1) vz1 = vz;
        }
    }
    // A margin: the PCF kernel samples half a texel beyond the edge, and the light pass must not
    // clip geometry the fit says is inside.
    const float span = (vx1 - vx0) > (vy1 - vy0) ? (vx1 - vx0) : (vy1 - vy0);
    const float margin = span * 0.02f + 1.0e-3f;
    vx0 -= margin; vx1 += margin; vy0 -= margin; vy1 += margin;
    const float marginZ = 1.0f; // model units of depth slack on both ends
    const float eyeZ = vz1 + marginZ; // pull the eye back past the far corner, toward the sun
    const float nearZ = vz0 + eyeZ;
    const float farZ = vz1 + eyeZ;

    // Reversed-Z ortho (near -> 1, far -> 0), like every other projection in this renderer: the
    // shadow PSO uses GREATER_EQUAL and a map cleared to 0 (far = "nothing lit it").
    //   The CPU matrix is consumed by the shader as out_j = dot(p, ROW_j): HLSL packs cbuffer matrices
    //   column-major by default, so cb0[j] IS the CPU's row j, and mul(p, M) dots p with each register
    //   (verified in the DXBC: "dp4 o0.x, v0, cb0[0]"). That is why the light view and the ortho are
    //   composed DIRECTLY into the rows here -- row j = (axis_j * scale_j, translation_j) -- instead of
    //   building two D3DX matrices and multiplying them under a convention this file cannot see.
    const float sx = 2.0f / (vx1 - vx0);
    const float sy = 2.0f / (vy1 - vy0);
    const float sz = -1.0f / (farZ - nearZ); // reversed: near -> 1, far -> 0
    const float tx = (vx1 + vx0) / (vx0 - vx1);
    const float ty = (vy1 + vy0) / (vy0 - vy1);
    const float tz = farZ / (farZ - nearZ);
    // eye = centre - f * eyeZ, so dot(eye, axis) = dot(centre, axis) for r/u and dot(centre, f) - eyeZ.
    const float exr = ccx * r[0] + ccy * r[1] + ccz * r[2];
    const float exu = ccx * u[0] + ccy * u[1] + ccz * u[2];
    const float exf = ccx * f[0] + ccy * f[1] + ccz * f[2] - eyeZ;

    D3DXMATRIX vp;
    vp.m00 = r[0] * sx; vp.m01 = r[1] * sx; vp.m02 = r[2] * sx; vp.m03 = -exr * sx + tx;
    vp.m10 = u[0] * sy; vp.m11 = u[1] * sy; vp.m12 = u[2] * sy; vp.m13 = -exu * sy + ty;
    vp.m20 = f[0] * sz; vp.m21 = f[1] * sz; vp.m22 = f[2] * sz; vp.m23 = -exf * sz + tz;
    vp.m30 = 0.0f; vp.m31 = 0.0f; vp.m32 = 0.0f; vp.m33 = 1.0f;

    // Bias: a constant depth slack in clip units. The normal offset in the shader does the heavy
    // lifting (see CockpitSunShadow); this only has to cover the ortho depth quantisation, so it is
    // small -- a fat bias detaches contact shadows and reads as floating geometry.
    g_pRenderer->SetPitShadowVP((const float *)&vp, 1.5e-3f, g_fPitShadowStrength);
    if (not g_pRenderer->BeginPitShadowPass())
        return false; // no depth target -> do NOT replay (it would write into the scene)

    // Replay the pit list's geometry, depth-only, in model space. The list is intact here: FlushObjects
    // dispenses it later. m_NODE/TheMaterial/etc. are scratch that FlushObjects re-initialises per item.
    D3DXMATRIX identity;
    D3DXMatrixIdentity(&identity);
    m_ShadowWalk = true;
    for (CDrawItem *d = TheVbManager.GetPitListRoot(); d; d = d->Next)
    {
        if (!d->Object)
            continue;
        ObjectInstance *o = (ObjectInstance *)d->Object;
        TheVbManager.GetModelData(m_VB, d->ID);
        if (!m_VB.Valid)
            continue;
        m_TheObjectInstance = o;
        AppliedState = identity; // model space: the shadow VP carries the whole light transform
        DX_SET_WORLD(AppliedState);
        m_NODE.BYTE = (BYTE *)m_VB.Nodes;
        long guard = 0;
        const long maxNodes = (long)m_VB.NNodes + 16;
        while (m_NODE.HEAD->Type not_eq DX_MODELEND)
        {
            DrawNode(o, d->LightID, d->ID);
            const DWORD step = m_NODE.HEAD->dwNodeSize;
            if (step == 0 or ++guard > maxNodes)
                break;
            m_NODE.BYTE += step;
        }
    }
    m_ShadowWalk = false;

    g_pRenderer->EndPitShadowPass();
    return true;
}

// Artscout - 2026: the visible source for every active dynamic light.
//   The models carry no emissive geometry at most lamp positions (the F-16's wingtip nav lights and
//   its intake strips are plain grey / very dim surfaces, and the pit model has no emissive surfaces
//   at all), so a lamp whose light node works still had NO source: a grey box outside, a black dot in
//   the cockpit, and only the light's spill on the skin around it. This draws one camera-facing
//   additive billboard per active dynamic light -- position from the light, colour from its diffuse,
//   size from its range -- through the particle path (one DrawIndexedInstanced for all of them).
//   Knobs: LightSprites, LightSpriteSize, LightSpriteGain.
void CDXEngine::DrawLightSprites(void)
{
    // Artscout - 2026: OFF by default now. This was written on the premise that the models carry no
    // lamp geometry -- they do: the lens is a plain (not SwEmissive) surface whose colour lives in
    // the vertex COLOR2, and the port was dropping COLOR2 on exactly those surfaces (see the
    // emissive gate in DrawSurface). With that fixed the models light themselves and a sprite on
    // top double-lights them. What is left for this path is the handful of lamps whose model really
    // has no lens -- the F-16CJ's wingtip nav lights are a 1-pixel POINTLIST and nothing else --
    // so it stays as an opt-in knob (`set g_bLightSprites 1`), not a default.
    extern bool g_bLightSprites;
    extern float g_fLightSpriteSize;
    extern float g_fLightSpriteGain;
    if (!g_bLightSprites || !g_pRenderer || !g_bUseGpu)
        return;

    const int n = CDXLight::ActiveLightCount();
    if (n <= 0)
        return;

    // The glow dot, baked once: white centre -> transparent rim. RGB carries the falloff for the
    // additive blend (A the same, for any alpha use). LoadTextureRGBA is the engine's bake-a-bitmap
    // path (the moon uses it); the returned handle is renderer-owned and opaque here.
    static void *s_dot = 0;
    if (!s_dot)
    {
        enum
        {
            D = 32
        };
        static unsigned char px[D * D * 4];
        for (int y = 0; y < D; ++y)
        {
            for (int x = 0; x < D; ++x)
            {
                const float dx = ((x + 0.5f) / D) * 2.0f - 1.0f;
                const float dy = ((y + 0.5f) / D) * 2.0f - 1.0f;
                const float r = sqrtf(dx * dx + dy * dy);
                // BROAD bright core, soft rim. The first version used (1-r)^2, which is a pin-point
                // with a faint halo -- a pixel history showed the sprite drawing and passing at the
                // lamp, yet the lamp's own (unlit, black) housing still read black around it. A
                // plateau to r=0.35 then a smoothstep fade fills the housing with glow.
                float a = 0.0f;
                if (r < 0.35f)
                    a = 1.0f;
                else if (r < 1.0f)
                {
                    const float t = (r - 0.35f) / 0.65f;
                    a = 1.0f - t * t * (3.0f - 2.0f * t); // smoothstep down to 0
                }
                const unsigned char v = (unsigned char)(a * 255.0f);
                unsigned char *p = &px[(y * D + x) * 4];
                p[0] = p[1] = p[2] = v;
                p[3] = v;
            }
        }
        s_dot = (void *)g_pRenderer->LoadTextureRGBA(px, D, D);
    }
    if (!s_dot)
        return;

    // Layout MUST match IRenderer::DrawParticlesInstanced's documented record (== D3D12ParticleInstance):
    // 3f centre, 2f size, 1f rot, 1 packed colour, 4f uv rect = 44 bytes.
    struct LightSprite
    {
        float center[3];
        float size[2];
        float rot;
        unsigned color; // 0xAARRGGBB, read as BGRA bytes like every engine colour
        float uvRect[4];
    };
    static LightSprite recs[MAX_DYNAMIC_LIGHTS];
    int m = 0;
    for (int i = 0; i < n && m < MAX_DYNAMIC_LIGHTS; ++i)
    {
        const CDXLightElement *el = CDXLight::ActiveLight(i);
        if (!el)
            continue;
        const D3DLIGHT7 &L = el->Light;

        float px = L.dvPosition.x, py = L.dvPosition.y, pz = L.dvPosition.z;
        const float d2 = px * px + py * py + pz * pz;
        // A light at the eye is not a lamp on the model (the pit's flood/instrument fill lives there)
        // and its sprite would sit in your face. 2 ft.
        if (d2 < 4.0f)
            continue;
        const float d = sqrtf(d2);

        float size = L.dvRange * 0.35f * g_fLightSpriteSize;
        if (size < 0.15f)
            size = 0.15f;
        else if (size > 4.0f)
            size = 4.0f;

        // Nudge toward the eye so the sprite is not buried by the surface it sits on -- the light node
        // usually sits a little INSIDE its housing, and the sprite is depth-tested, so a small nudge
        // left the (unlit, black) fixture in front and the glow invisible: "the black sprite is still
        // here". Scale the nudge with the glow so a big housing gets pushed clear of it.
        const float nudge = 0.4f + size * 0.5f;
        const float k = nudge / d;
        px -= px * k;
        py -= py * k;
        pz -= pz * k;

        const float gain = (g_fLightSpriteGain > 0.0f) ? g_fLightSpriteGain : 0.0f;
        float r = L.dcvDiffuse.r * gain, g = L.dcvDiffuse.g * gain,
              b = L.dcvDiffuse.b * gain;
        if (r > 1.0f)
            r = 1.0f;
        if (g > 1.0f)
            g = 1.0f;
        if (b > 1.0f)
            b = 1.0f;

        LightSprite &s = recs[m++];
        s.center[0] = px;
        s.center[1] = py;
        s.center[2] = pz;
        s.size[0] = size;
        s.size[1] = size;
        s.rot = 0.0f;
        s.color = 0xFF000000u | ((unsigned)(r * 255.0f) << 16) |
                  ((unsigned)(g * 255.0f) << 8) | (unsigned)(b * 255.0f);
        s.uvRect[0] = 0.0f;
        s.uvRect[1] = 0.0f;
        s.uvRect[2] = 1.0f;
        s.uvRect[3] = 1.0f;
    }
    if (m > 0)
        g_pRenderer->DrawParticlesInstanced(recs, m, s_dot, 0); // 0 = additive
}

extern DWORD LODsLoaded;
// *************** This function is the REAL SCENE DRAW FUNCTION *********************
// it flushes all requested Drawsand draws all poly types
void CDXEngine::FlushBuffers(void)
{
    float FogStart = 0.0;
    D3DErroCount = 3;

#ifndef DEBUG_ENGINE
    //REPORT_VALUE("LODs : ", LODsLoaded);
#endif

    // First of all save present renderer State
    DWORD StateHandle = 0;

    if (g_bUseGpu)
    {
        // PHASE 4/#DX12 п.4: GPU object pass (D3D11 or D3D12) -- shaders/state/transforms/lighting.
        if (g_pRenderer and g_pRenderer->IsValid())
        {
            g_pRenderer->BeginObjectPass();
            g_pRenderer->SetProj((const float *)&Projection);
            g_pRenderer->SetView((const float *)&CameraView);
            g_pRenderer->SetCameraPos(CameraPos.x, CameraPos.y,
                                      CameraPos.z); // #29 specular

            // Sun (directional) + ambient. CROSS-CHECK WITH FF7: object light model =
            // vertexColor * (TheSun.dcvAmbient + TheSun.dcvDiffuse.N.L), where dcvAmbient/dcvDiffuse
            // are time-of-day modulated in SetSunLight() (statestack.cpp). Previously we took
            // TheSunColour (unmodulated) + a fixed ambient 0.45 -> objects stayed bright at night.
            // Now we give the shader TOD values -> on par with the reference (dark night).
            // Ambient light source by mode (like the reference SetLight(0,&The*)):
            // NVG -> green boost TheNVG, TV -> TheTV, else the sun.
            D3DLIGHT7 &envL = (m_RenderState == DX_NVG) ? TheNVG :
                              (m_RenderState == DX_TV)  ? TheTV :
                                                          TheSun;
            GpuLightCPU sun;
            ZeroMemory(&sun, sizeof(sun));
            // Artscout - 2026: ray direction, NOT negated -- see SetSunLight.
            sun.Direction[0] = LightDir.x;
            sun.Direction[1] = LightDir.y;
            sun.Direction[2] = LightDir.z;
            sun.Color[0] = envL.dcvDiffuse.r;
            sun.Color[1] = envL.dcvDiffuse.g;
            sun.Color[2] = envL.dcvDiffuse.b;
            sun.Params[1] = 0.0f; // directional
            const float amb[4] = {envL.dcvAmbient.r, envL.dcvAmbient.g,
                                  envL.dcvAmbient.b, 1.0f};
            g_pRenderer->SetLights(amb, 1, &sun, sizeof(sun));
            // #28: save for per-object dynamic lighting (UpdateDynamicLights).
            g_d3d11Sun = sun;
            g_d3d11Amb[0] = amb[0];
            g_d3d11Amb[1] = amb[1];
            g_d3d11Amb[2] = amb[2];
            g_d3d11Amb[3] = amb[3];

            // PHASE 5: the stencil buffer is cleared to 0 each frame -> reset the CPU counter too
            // for ref, else on an 8-bit stencil ref&0xFF==0 once every 256 frames (black frame).
            m_StencilRef = 0;
        }

        FlushInit();
        m_LinearFogLevel = MAX_FOG_RANGE;
        m_LastSpecular = 0;
    }
    // #34 D3D11: dead D3D7 device/state setup (else-branch) removed.


    LOCK_VB_MANAGER;

    // Start resetting Draw Pointers
    TheVbManager.ResetDrawList();

    // Artscout - 2026: cockpit sun shadows -- replay the pit's geometry depth-only BEFORE FlushObjects
    // dispenses the list, so this frame's pit draws can sample the map the replay just built. Once per
    // frame (re-armed by SetSunLight): the map lives in the pit's model space, so it is view-independent
    // and the second eye / the quad group reuse it.
    if (not m_PitShadowDone)
        m_PitShadowDone = RenderPitShadowMap();

    // Flush all cached VB objects
    if (m_RenderState == DX_DBS)
        FlushBlips();
    else
        FlushObjects();

    // Draw the Solid Surfaces
    DrawSolidSurfaces();

    // Flush Dynamic Buffers bitand sorted objects
    FlushDynamicObjects();

    // Artscout - 2026: the lamps' visible sources -- after the object pass, BEFORE the light list is
    // reset (the sprites are built from that list). See DrawLightSprites.
    DrawLightSprites();

    //Reset Features
    ResetFeatures();

    // Setup VB draw list for a new round
    TheVbManager.ClearDrawList();
    // Clear any light
    TheLightEngine.ResetLightsList();

    UNLOCK_VB_MANAGER;

    // #34 D3D11: no D3D7 state block to restore.

    gDebugLodID = -1;
    m_AlphaStack.StackLevel = 0;
}


#ifdef EDIT_ENGINE


void CDXEngine::ModelInit(ObjectInstance *objInst, DxDbHeader *Header,
                          DWORD *Textures, D3DXMATRIX *State, DWORD LightOwner,
                          DWORD nTexsPerBank)
{
    D3DXMATRIX Position;

    ResetState();

    m_TheObjectInstance = objInst;
    AppliedState = *State;

    // Setup the state for the DX engine
    CheckHR(m_pD3DD->ApplyStateBlock(DxEngineStateHandle));

    // *** Default engine initializations ***
    m_pD3DD->SetRenderState(D3DRENDERSTATE_DIFFUSEMATERIALSOURCE,
                            D3DMCS_COLOR1);
    m_pD3DD->SetRenderState(D3DRENDERSTATE_AMBIENTMATERIALSOURCE,
                            D3DMCS_COLOR1);
    m_pD3DD->SetRenderState(D3DRENDERSTATE_SPECULARMATERIALSOURCE,
                            D3DMCS_MATERIAL);
    m_pD3DD->SetRenderState(D3DRENDERSTATE_EMISSIVEMATERIALSOURCE,
                            D3DMCS_COLOR2);
    m_pD3DD->SetRenderState(D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD);
    m_pD3DD->SetRenderState(D3DRENDERSTATE_CLIPPING, FALSE);
    m_pD3DD->SetRenderState(D3DRENDERSTATE_FOGENABLE, FALSE);

    // Set Up the View port
    m_pD3DD->SetViewport(&ViewPort);

    // Set Up the Field of View Projection
    m_pD3DD->SetTransform(D3DTRANSFORMSTATE_PROJECTION,
                          (LPD3DMATRIX)&Projection);

    // Set Up the camera View for the drawing
    m_pD3DD->SetTransform(D3DTRANSFORMSTATE_VIEW, (LPD3DMATRIX)&CameraView);

    // Initialize data parameters
    FlushInit();

    //Reset Features
    ResetFeatures();

    // The Stack For the State Transformations resetted
    StateStackLevel = 0;
    // Transform the model
    AppliedState = *State;

    // Execute the Scripts 0 bitand 1 if existant
    D3DVECTOR pos;
    pos.x = AppliedState.m30;
    pos.y = AppliedState.m31;
    pos.z = AppliedState.m32;

    if (not m_ScriptsOn)
        goto DrawSection;

    DXScriptVariableType *Script = (Header)->Scripts;

    if (Script[0].Script)
        if (not DXScriptArray[Script[0].Script](&pos, objInst,
                                                Script[0].Arguments))
            goto DrawSection;

    if (Script[1].Script)
        (not DXScriptArray[Script[1].Script](&pos, objInst,
                                             Script[1].Arguments));

DrawSection:

    TheLightEngine.UpdateDynamicLights(LightOwner, &pos,
                                       2000.0f /*objInst->Radius()*/);

    // Ok... transform the object
    DX_SET_WORLD(AppliedState);

    // Calculates the Texture Base Index in the Texture Bank
    DWORD *texOffset = (DWORD *)(Textures + objInst->TextureSet * nTexsPerBank);

    // Register each texture for the Model ( and load it if not available ) and setup local Textures List
    for (DWORD a = 0; a < nTexsPerBank; a++)
        m_TexUsed[a] = *texOffset++;
}


void CDXEngine::DrawNodeEx(NodeScannerType *NODE, ObjectInstance *objInst,
                           DWORD LightOwner, DWORD LodID)
{
    m_TheObjectInstance = objInst;
    m_NODE = *NODE;
    DrawNode(objInst, LightOwner, LodID);
}


void CDXEngine::DofManageEx(NodeScannerType *NODE, ObjectInstance *objInst,
                            D3DXMATRIX *NewState)
{

    m_TheObjectInstance = objInst;
    m_NODE = *NODE;
    // Reset the Applied State Matrix
    AppliedState = *NewState;
    // Manage the DOF
    DOF();
    // copy result to destination matrix
    *NewState = AppliedState;
}


bool CDXEngine::SwitchManageEx(NodeScannerType *NODE, ObjectInstance *objInst,
                               D3DXMATRIX *NewState)
{
    bool value;

    m_TheObjectInstance = objInst;
    m_NODE = *NODE;

    // Reset the Applied State Matrix
    AppliedState = *NewState;

    // Manage the switch
    SWITCHManage();

    value = m_SkipSwitch;
    m_SkipSwitch = false;
    return value;
}


void CDXEngine::PushMatrixEx(D3DXMATRIX *NewState)
{
    PushMatrix(NewState);
}

void CDXEngine::PopMatrixEx(D3DXMATRIX *NewState)
{
    PopMatrix(NewState);
}

#endif
