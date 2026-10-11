#ifndef LANTIRN_H
#define LANTIRN_H

#include <iso646.h>
#include "drawable.h"

struct Tpoint;
class AircraftClass;

class LantirnClass : public DrawableClass
{
public:
    enum
    {
        FLIR_ON = 0x1,
        AVAILABLE = 0x2,
        CONH = 0x4,
    };

private:
    VirtualDisplay* display; // The renderer we are to draw upon
    // Artscout - 2026: under D3D11 the FLIR/TGP 3D scene must render into an off-screen RTT (not the
    // backbuffer, which leaks the sensor image onto the screen). m_pRTT is that target; m_pMfdImage
    // is the MFD's 2D surface we read the scene back into (Munitions 3D-viewer pattern).
    class ImageBuffer* m_pRTT;
    class ImageBuffer* m_pMfdImage;

public:
    void GetCameraPos(Tpoint* pos);
    bool IsEnabled()
    {
        return (m_flags bitand AVAILABLE) ? TRUE : FALSE;
    };
    bool IsFLIR()
    {
        return (m_flags bitand FLIR_ON) ? TRUE : FALSE;
    };
    bool IsHonC()
    {
        return (m_flags bitand CONH) == 0 ? TRUE : FALSE;
    };
    void ToggleHonC()
    {
        m_flags xor_eq CONH;
    };
    // Artscout - 2026: FINS FLIR as the BMS manual has it (TO 1F-16CMAM-34-1-1 2.6.2.1). OSB 20 on the FLIR page
    // puts it in OPER, OSB 18 in STBY; it needs the nav pod powered (LEFT HDPT) and cooled down ("NOT TIMED OUT",
    // cfg FlirCooldown seconds, default 600) before it will go to OPER. GAIN / LEVEL are adjusted on the page or
    // with the ICP FLIR arrows (SimFlirLevelUp/Down); the HUD raster brightness is the ICP BRT wheel
    // (SimBrtWheelUp/Down); GRAY switches the manual gain/level on; BSGT slews the HUD image onto the real world.
    float flirCool; // s of cool-down left after the pod was powered; 0 = ready
    float flirGain, flirLevel; // 0..100, as on the page ("50G", "100L")
    bool flirManual; // GRAY: use gain and level; off = fixed auto values
    float hudBright; // HUD raster brightness 0..1 (the BRT wheel); 0 = no image on the HUD
    bool bsgt; // BSGT mode: the page's OSBs slew the HUD image
    float hudTrimPitch, hudTrimYaw; // radians, BSGT result (pitch up positive, yaw right positive)
    bool IsFlirTimedOut(void)
    {
        return flirCool <= 0.0f;
    };
    bool FlirPowered(AircraftClass* self);
    void SetFlirOper(bool on);
    float FlirIntensity(void);
    void AdjustFlirGain(int steps);
    void AdjustFlirLevel(int steps);
    void AdjustHudBright(int steps);
    void BsgtSlew(float dPitchRad, float dYawRad);
    void ToggleFLIR();
    void SetFovScale(float fscale)
    {
        m_fscale = fscale;
    };
    float GetFovScale(void)
    {
        return m_fscale;
    };
    float GetDPitch()
    {
        return m_dpitch;
    };
    void SetDPitch(float dp)
    {
        m_dpitch = dp;
    };
    void SetFOV(float fov);
    float GetFov();
    LantirnClass(); // constructor
    ~LantirnClass(); // desctructor

    VirtualDisplay* GetDisplay(void)
    {
        return privateDisplay;
    };
    void Display(VirtualDisplay*);
    void DisplayInit(ImageBuffer*);
    void SetTFRAlt(int n)
    {
        m_tfr_alt = n;
    };
    int GetTFRAlt(void)
    {
        return m_tfr_alt;
    };
    enum TfrRide
    {
        TFR_SOFT,
        TFR_MED,
        TFR_HARD
    };
    TfrRide GetTFRRide(void)
    {
        return m_tfr_ride;
    };
    void SetTFRRide(TfrRide ride)
    {
        m_tfr_ride = ride;
    };
    void StepTFRRide();

    enum TfrMode
    {
        TFR_ECCM,
        TFR_LP1,
        TFR_WX,
        TFR_STBY,
        TFR_NORM
    };
    TfrMode GetTFRMode(void)
    {
        return m_tfrmode;
    };
    void SetTFRMode(TfrMode mode)
    {
        m_tfrmode = mode;
    };
    void StepTFRMode();
    float GetScanLoc()
    {
        return scanpos;
    };
    // debug stuff
    float gdist; // distance to ground intersection
    int evasize; // are we being evasize
    float holdheight; // what height we want to be at
    float turnradius; // what turn radius we can do
    float pitch; // desired pitch angle
    float GetHoldHeight()
    {
        return holdheight;
    };
    float GetGLimit();
    void Exec(AircraftClass* self); // do whats necessary

    // Artscout - 2026: TFR warm-up and advisories (TO 1F-16CMAM-34-1-1 2.6.3.4-2.6.3.6)
    float coolTime; // s of warm-up left after the NVP was powered ("NOT TIMED OUT"); 0 = ready
    bool powerInit, powerSeen; // NVP (LEFT HDPT) power as seen last frame
    bool limitExceeded; // HUD / MFD "LIMIT": bank or dive outside what the TFR can command
    int turnAdvisory; // HUD "TERRAIN": 1 = terrain to the left (turn right), -1 = right, 2 = both, 0 = none
    int sideFrame;
    bool IsTimedOut(void)
    {
        return coolTime <= 0.0f;
    };
    void UpdateCooldown(AircraftClass* self);
    void UpdateAdvisories(AircraftClass* self);

    // Artscout - 2026: the nav FLIR raster on the 3D-pit HUD (TO 1F-16CMAM-34-1-1 2.6.2.1). The IR scene is drawn
    // into a render target of its own (PrepareHudFlir before the pit's atlas pass; RenderHudFlir inside it at the HUD, once per frame), then
    // composited on the HUD glass under the symbology (DrawHudFlirQuad, per eye, with the HUD's own canvas, collimation
    // and stencil). Drawing it into the HUD's patch of the shared atlas did not show and disturbed the other displays.
    bool HudFlirWanted(void);
    void PrepareHudFlir(class Render3D* mainRenderer);
    void RenderHudFlir(class Render3D* mainRenderer, float halfAngleDeg, float boresightRad);
    void DrawHudFlirDebug(VirtualDisplay* anyDisplay);
    void DrawHudFlirQuad(VirtualDisplay* hudCanvas);
    class RenderIR* m_hudIr;
    class TextureHandle* m_hudTex;
    int m_hudTexSize;
    bool m_hudTexValid;

    //new variables
    float PID_MX, PID_lastErr, PID_error, PID_Output;
    float MaxG, MinG;
    float gAlt;
    float lookingAngle;
    float gDist2;
    float gammaCorr;
    float gamma;
    float min_Radius;
    float min_safe_dist;
    bool SpeedUp;
    float roll;

    float GetGroundDistance(AircraftClass* self, float zOffset, float yaw,
                            float pitch);
    float FeatureCollisionPrediction(AircraftClass* self, float zOffset,
                                     BOOL MeasureHorizontally,
                                     BOOL GreatestAspect, float Clearance,
                                     float GridSizeNM, float boxScale,
                                     float* featureHeight);
    float GetEVAFactor(AircraftClass* self, int eva);

    float featureDistance, featureDistance2, featureDistance3;
    float featureHeight, featureHeight2, featureHeight3;
    float featureAngle, featureAngle2, featureAngle3;

private:
    float m_fscale; // delta fov
    float m_dpitch; // delta pitch
    unsigned int m_flags;
    int m_tfr_alt;
    TfrRide m_tfr_ride;
    TfrMode m_tfrmode;
    void DrawTerrain();
    float scanpos; // -1 to 1
    float scandir;
    float scanrate;
    void MoveBeam();
    float GetGroundIntersection(AircraftClass* self, float yaw, float pitch,
                                float galt, int& type);
};

extern LantirnClass* theLantirn;
#endif
