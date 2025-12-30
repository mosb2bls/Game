#pragma once
#include <string>
#include <algorithm>
#include "Window.h"
#include "Animation.h"

// FPS viewmodel state machine with ADS (zoom) + reload interrupt rules.
// Hold RMB (mouseButtons[2]) to zoom (ADS), and use zoom-specific clips if they exist.
// Fire supports hold-to-shoot with a fixed shots-per-second rate, restarting the fire clip per bullet.
// Reload has top priority: if R is pressed while zooming, it forces unzoom, plays reload, then re-zooms if RMB is still held.
class modelState
{
public:
    // Key and mouse bindings matching your Window input arrays
    int fireMouseButton = 0;
    int zoomMouseButton = 2;
    int reloadKey = 'R';

    // Animation clip names (must exactly match names inside the .gem animation set)
    std::string idleClip = "04 idle";
    std::string walkClip = "07 walk";
    std::string fireClip = "08 fire";
    std::string reloadClip = "17 reload";

    std::string zoomIdleClip = "zoom";
    std::string zoomWalkClip = "zoom walk";
    std::string zoomFireClip = "zoom fire";

    // Fire behaviour tuning: sustained fire rate + animation playback speed while firing
    bool  allowHoldFire = true;
    float shotsPerSecond = 12.0f;
    float fireAnimRate = 3.0f;

    // Locomotion animation playback speeds (hip vs ADS variants)
    float idleAnimRate = 1.0f;
    float walkAnimRate = 1.0f;
    float zoomIdleRate = 1.0f;
    float zoomWalkRate = 1.0f;

    // Reload animation playback speed
    float reloadAnimRate = 1.0f;

    const float PI = 3.141592654f;

    // Viewmodel offsets in view space when hip-firing
    float gunX = 0.08f;
    float gunY = 0.00f;
    float gunZ = 0.00f;
    float modelRotY = +PI * 1.01f;

    // Viewmodel offsets in view space when ADS (zooming)
    float zoomGunX = -0.04f;
    float zoomGunY = 0.02f;
    float zoomGunZ = 0.0f;
    float zoommodelRotY = +PI;

    // Camera zoom offset (how far forward the camera moves when ADS)
    float zoomCameraOffset = 0.5f;

public:
    // Update state machine each frame. Returns true if a bullet/shot should be spawned this frame.
    bool update(Window& w, AnimationInstance& inst, float dt)
    {
        // If the animation instance has no animation bound, nothing can be driven
        if (!inst.animation) return false;

        // Clamp dt to avoid huge simulation jumps (e.g., window drag / breakpoint resume)
        dt = std::min(dt, 0.05f);

        // Read ADS intent as a "hold" action
        bool wantZoom = (w.mouseButtons[zoomMouseButton] != 0);

        // Detect reload as a one-shot press: must release 'R' to re-arm the trigger
        // This avoids repeated reload spam and also tolerates missing key-up events.
        if (!w.keys[reloadKey]) reloadArmed = true;
        bool reloadPressed = reloadArmed && (w.keys[reloadKey] != 0);

        // Basic locomotion detection (used for idle/walk clip selection)
        bool moving = (w.keys['W'] || w.keys['A'] || w.keys['S'] || w.keys['D']);

        // First-time setup: decide initial clip based on current inputs, then start from time 0
        if (currentClip.empty())
        {
            zoomActive = wantZoom;
            currentClip = pickLocomotionClip(inst, moving, zoomActive);
            inst.resetAnimationTime();
        }

        // Reload is the highest priority action; if triggered, it overrides any zoom/fire state.
        // Requirement: if zooming and R pressed -> force unzoom -> reload -> if RMB still held, zoom again.
        if (reloadPressed && has(reloadClip, inst))
        {
            reloadArmed = false;

            pendingZoomAfterReload = wantZoom;
            zoomActive = false;

            action = Action::Reload;
            currentClip = reloadClip;
            inst.resetAnimationTime();

            fireAccumulator = 0.0f;
        }

        // While reloading, we only advance the reload animation and wait until it finishes.
        if (action == Action::Reload)
        {
            inst.update(currentClip, dt * reloadAnimRate);

            if (inst.animationFinished())
            {
                action = Action::None;

                bool stillWantZoom = (w.mouseButtons[zoomMouseButton] != 0);
                zoomActive = pendingZoomAfterReload && stillWantZoom;
                pendingZoomAfterReload = false;

                currentClip = pickLocomotionClip(inst, moving, zoomActive);
                inst.resetAnimationTime();
            }
            return false;
        }

        // ADS state is a direct reflection of the current RMB hold (only when not reloading)
        zoomActive = wantZoom;

        // Fire logic: if hold-fire is enabled and LMB is down, accumulate time and emit shots at shotsPerSecond.
        // Each emitted shot restarts the fire animation clip (so the muzzle kick syncs per bullet).
        bool fireDown = (w.mouseButtons[fireMouseButton] != 0);
        bool shotThisFrame = false;

        if (allowHoldFire && fireDown)
        {
            std::string fireUse = pickFireClip(inst, zoomActive);

            if (!fireUse.empty())
            {
                action = Action::Fire;
                fireAccumulator += dt;

                const float interval = (shotsPerSecond > 0.0f) ? (1.0f / shotsPerSecond) : 1e9f;
                while (fireAccumulator >= interval)
                {
                    fireAccumulator -= interval;
                    shotThisFrame = true;

                    currentClip = fireUse;
                    inst.resetAnimationTime();
                }

                inst.update(currentClip, dt * fireAnimRate);
                return shotThisFrame;
            }
        }

        // If we reach here, we are not firing: clear fire state and select locomotion clip.
        action = Action::None;
        fireAccumulator = 0.0f;

        // Locomotion selection: prefer zoom idle/walk if zoom is active and those clips exist, otherwise fallback to hip clips.
        std::string desired = pickLocomotionClip(inst, moving, zoomActive);
        if (!desired.empty() && desired != currentClip)
        {
            currentClip = desired;
            inst.resetAnimationTime();
        }

        float rate = pickLocomotionRate(moving, zoomActive);
        inst.update(currentClip, dt * rate);

        // Keep locomotion clips looping by restarting when finished (idle/walk/zoom idle/zoom walk)
        if (inst.animationFinished())
            inst.resetAnimationTime();

        return false;
    }

    // Retrieve the viewmodel offsets for this frame (ADS changes position and rotation).
    void getGunOffset(float& outX, float& outY, float& outZ, float& RotY) const
    {
        if (zoomActive)
        {
            outX = zoomGunX; outY = zoomGunY; outZ = zoomGunZ; RotY = zoommodelRotY;
        }
        else
        {
            outX = gunX; outY = gunY; outZ = gunZ; RotY = modelRotY;
        }
    }

    // Get the camera forward offset for zoom effect (0 when not zooming, zoomCameraOffset when zooming)
    float getCameraZoomOffset() const
    {
        return zoomActive ? zoomCameraOffset : 0.0f;
    }

    bool isZooming() const { return zoomActive; }

private:
    enum class Action { None, Fire, Reload };

    Action action = Action::None;
    std::string currentClip;

    // Fire cadence accumulator used to emit shots at a fixed rate while holding LMB
    float fireAccumulator = 0.0f;

    // Reload press gating and ADS restore intent after reload completes
    bool reloadArmed = true;
    bool pendingZoomAfterReload = false;

    // Current ADS state (hold RMB)
    bool zoomActive = false;

private:
    // Check whether a named animation clip exists in the currently bound Animation object
    bool has(const std::string& name, AnimationInstance& inst)
    {
        return inst.animation && inst.animation->hasAnimation(name);
    }

    // Pick the correct fire clip depending on ADS state, with fallback to hip-fire if zoom fire is missing
    std::string pickFireClip(AnimationInstance& inst, bool zoom)
    {
        if (zoom && has(zoomFireClip, inst)) return zoomFireClip;
        if (has(fireClip, inst)) return fireClip;
        return "";
    }

    // Pick idle/walk clips based on movement and ADS state, with graceful fallback if zoom clips are not present
    std::string pickLocomotionClip(AnimationInstance& inst, bool moving, bool zoom)
    {
        if (zoom)
        {
            if (moving && has(zoomWalkClip, inst)) return zoomWalkClip;
            if (!moving && has(zoomIdleClip, inst)) return zoomIdleClip;
        }

        if (moving && has(walkClip, inst)) return walkClip;
        if (!moving && has(idleClip, inst)) return idleClip;

        // Final fallback: if clip names are wrong, at least play the first available animation
        if (inst.animation && !inst.animation->animations.empty())
            return inst.animation->animations.begin()->first;

        return "";
    }

    // Select playback speed for locomotion clips (hip vs ADS variants)
    float pickLocomotionRate(bool moving, bool zoom) const
    {
        if (zoom) return moving ? zoomWalkRate : zoomIdleRate;
        return moving ? walkAnimRate : idleAnimRate;
    }
};
