#pragma once
// Pure maths behind SpellQueue.cpp and SmoothTurn.cpp, header-only so test_gameplay.cpp checks it natively.
#include <cmath>
#include <cstdint>

namespace Gameplay {
    // ---- spell queue ------------------------------------------------------------------------------------
    /// What CGUnit_C::ValidateSpellCast should hear from GetSpellCooldown: the stock answer, except "ready"
    /// (0) when the player's own cooldown ends within `window` ms. Pet cooldowns and on-hold cooldowns
    /// (e.g. until you leave combat) always get the stock answer.
    inline int QueueCooldownResult(int onCooldown, bool enabled, uint32_t who, uint32_t notOnHold,
                                   int32_t start, int32_t duration, uint32_t nowMs, int32_t window)
    {
        if (!onCooldown || !enabled || who != 0 || !notOnHold) return onCooldown;
        const int32_t remaining = start + duration - static_cast<int32_t>(nowMs);
        return remaining > window ? onCooldown : 0;
    }

    // ---- smooth turning ---------------------------------------------------------------------------------
    constexpr float kTwoPi = 6.28318531f;

    /// @return @p a wrapped into [0, 2pi), the range the client keeps facing in.
    inline float Wrap(float a)
    {
        a = std::fmod(a, kTwoPi);
        return a < 0.0f ? a + kTwoPi : a;
    }

    /// Turns @p from toward @p to the short way round by at most @p maxStep radians; result in [0, 2pi).
    inline float Approach(float from, float to, float maxStep)
    {
        float diff = std::fmod(to - from, kTwoPi);
        if (diff > kTwoPi / 2)  diff -= kTwoPi;
        if (diff < -kTwoPi / 2) diff += kTwoPi;
        if (std::fabs(diff) <= maxStep) return Wrap(to);
        return Wrap(from + (diff > 0.0f ? maxStep : -maxStep));
    }

    // Retail-style steering: the shown position follows the true spline position through a critically
    // damped spring with velocity feed-forward. On a steady path it sits exactly on the path; when the path
    // turns sharply (a wander reversal) it keeps its momentum and arcs round, rejoining within ~`settle` s.
    struct Steer
    {
        float x = 0, y = 0;     // shown position
        float vx = 0, vy = 0;   // shown velocity
    };

    /// Advances @p s by @p dt toward the true position (tx,ty) = now, moving at (tvx,tvy).
    inline void SteerStep(Steer& s, float tx, float ty, float tvx, float tvy, float dt, float settle)
    {
        // critically damped: ~98% settled after 4/w. Floor 0.1 s: below it w*h passes ~1 at the 20 ms sub-step
        // and the spring diverges to NaN (wxl-smoothturn's 0.05 floor did, for steer_time < ~0.09).
        const float w = 4.0f / (settle > 0.1f ? settle : 0.1f);
        const float steps = std::ceil(dt / 0.02f);                  // sub-step so a hitch cannot go unstable
        const float h = dt / (steps > 0 ? steps : 1);
        tx -= tvx * dt; ty -= tvy * dt;                             // (tx,ty) is where it is now: start from a frame ago
        for (int i = 0; i < static_cast<int>(steps); ++i)
        {
            const float ax = w * w * (tx - s.x) + 2.0f * w * (tvx - s.vx);
            const float ay = w * w * (ty - s.y) + 2.0f * w * (tvy - s.vy);
            s.vx += ax * h; s.vy += ay * h;
            s.x  += s.vx * h; s.y  += s.vy * h;
            tx += tvx * h; ty += tvy * h;                           // the true point moves on meanwhile
        }
    }
}
