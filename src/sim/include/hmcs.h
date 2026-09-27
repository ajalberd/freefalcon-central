#ifndef _HMCS_H
#define _HMCS_H

// Artscout - 2026: JHMCS-style helmet-mounted cueing. The helmet line of sight is the view
// direction of the 3D pit (the HMD in VR, TrackIR/mouse look on a flat screen); while it is
// valid it slaves the caged AIM-9 seeker, lets an uncaged AIM-9 search where you look, and
// points the radar in ACM BORE, so a dogfight AIM-120 locks what you look at. The symbology
// is drawn at optical infinity through each eye's own projection (Hmcs_Draw, from VCock_Exec).
//
// FFViper.cfg:
//   set g_nHmcs 1              0 = off (the 2001 padlock-HMS behaviour), 1 = aircraft whose class
//                              data carries the HMS flag (0x20000000), 2 = every aircraft
//   set g_bHmcsSlaveSeeker 1   helmet slaves the AIM-9 seeker
//   set g_bHmcsSlaveRadar 1    helmet points ACM BORE
//   set g_bHmcsHudBlank 1      blank symbology that falls inside the HUD box below
//   set g_fHmcsHudHalfWidth 12.5, g_fHmcsHudTop 6, g_fHmcsHudBottom -16   (deg from boresight)
//   set g_fHmcsScale 1         symbology size
//   set g_bHmcsLog 0           once-a-second state line in FFDebug.log
//   set g_nHmcsLevel 3         the HMCS knob at launch: 0 off, 1 dim, 2 mid, 3 bright
// The knob is a clickable spot on the blank plate below the CMDS panel (3dbuttons.dat):
// SimHmcsKnobUp / SimHmcsKnobDown (left / right click). SimHmcsToggle is the same thing on a key.

class SimBaseClass;
class RenderOTW;
struct Trotation;

bool Hmcs_Equipped(SimBaseClass* platform);
bool Hmcs_Cueing(SimBaseClass* platform);
// Body-relative helmet line of sight (rad, az right-positive, el up-positive). False while not cueing.
bool Hmcs_GetLos(SimBaseClass* platform, float* az, float* el);
bool Hmcs_InHud(float az, float el);
// The radar calls this each frame it points ACM BORE down the helmet line of sight (draws the cue circle).
void Hmcs_NoteRadarSlaved();
// The HMCS knob: step brightness (+1 up, -1 down; bottom stop = off), and the on/off key.
void Hmcs_StepKnob(int dir);
void Hmcs_Toggle();
// Draws the symbology with whatever camera/projection the caller has set on the renderer.
void Hmcs_Draw(RenderOTW* renderer, const Trotation* headMatrix);

#endif
