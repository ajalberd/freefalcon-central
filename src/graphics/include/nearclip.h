/***************************************************************************\
    NearClip.h
    Artscout - 2026

    The CPU near-clip distance (feet) for the software-transformed 3D paths:
    BSPlib's polylib clipper (NEAR_CLIP_DISTANCE, clipflags.h) and Render3D's
    primitive clipper (NEAR_CLIP, render3d.h) -- squares, triangles and lines
    such as the RTT display quads, cursors, padlock boxes, trails and sky bits.

    Both were a hard 1.0 ft while the GPU near plane (ContextMPR::ZNEAR) is
    0.2 ft. The clippers do not reject a vertex inside the near plane, they
    MOVE it onto the plane (IntersectNear), so anything drawn between 0.2 and
    1.0 ft from the eye -- routine in VR, where the pit is within arm's reach --
    was folded and displaced rather than drawn where it is. The GPU clips at
    its own near plane anyway, so matching it is the principled value.

    Models drawn through the DX engine (the pit, aircraft, stores) never reach
    either clipper: StateStackClass::DrawObject hands them to the GPU.

    A function-local static in an inline function, not a global: one instance
    across every static lib without a C++17 inline variable, and without a link
    dependency on the lib that holds the cfg table (same trick as fflog.h).
    Tunable as `set g_fCpuNearClip <ft>` (f4config.cpp); 1.0 = the old clip.
\***************************************************************************/
#ifndef _NEARCLIP_H_
#define _NEARCLIP_H_

inline float& CpuNearClip()
{
    static float s_nearClip = 0.2f; // == ContextMPR::ZNEAR
    return s_nearClip;
}

#endif // _NEARCLIP_H_
