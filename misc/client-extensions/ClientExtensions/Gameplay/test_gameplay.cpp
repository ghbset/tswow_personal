// Offline check of GameplayMath.h (not part of the DLL):
//   g++ -std=c++17 test_gameplay.cpp -o /tmp/test_gameplay && /tmp/test_gameplay
#include "GameplayMath.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
using namespace Gameplay;
static bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }
int main()
{
    // spell queue: cooldown started at 1000 for 1500 ms -> ends at 2500
    assert(QueueCooldownResult(1, true, 0, 1, 1000, 1500, 2350, 150) == 0);    // 150 left: inside the window
    assert(QueueCooldownResult(1, true, 0, 1, 1000, 1500, 2349, 150) == 1);    // 151 left: still "not ready"
    assert(QueueCooldownResult(1, true, 0, 1, 1000, 1500, 2600, 150) == 0);    // already over
    assert(QueueCooldownResult(0, true, 0, 1, 0, 0, 5000, 150) == 0);          // not on cooldown: stock
    assert(QueueCooldownResult(1, false, 0, 1, 1000, 1500, 2400, 150) == 1);   // disabled
    assert(QueueCooldownResult(1, true, 1, 1, 1000, 1500, 2400, 150) == 1);    // pet: stock
    assert(QueueCooldownResult(1, true, 0, 0, 1000, 1500, 2400, 150) == 1);    // on hold: never let through
    assert(QueueCooldownResult(1, true, 0, 1, 1000, 1500, 2400, 0) == 1);      // window 0 = stock
    assert(QueueCooldownResult(1, true, 0, 1, -100, 200, 0xFFFFFFF0u, 150) == 0); // clock near wrap
    std::puts("spellqueue ok");

    assert(Near(Approach(0.0f, 1.0f, 0.25f), 0.25f));            // clamps the step
    assert(Near(Approach(0.0f, 0.1f, 0.25f), 0.1f));             // reaches a close target exactly
    assert(Near(Approach(0.1f, kTwoPi - 0.1f, 0.05f), 0.05f));   // short way round, across 0
    assert(Near(Approach(kTwoPi - 0.1f, 0.1f, 0.05f), kTwoPi - 0.05f));
    assert(Near(Approach(0.0f, -1.0f, 5.0f), kTwoPi - 1.0f));    // result wrapped into [0, 2pi)
    std::puts("angle ok");

    // steering: steady walk stays on the path; a reversal overshoots a little, then rejoins
    Steer st; st.vx = 2.5f;                                       // walking +x at 2.5 yd/s, on the path
    float tx = 0;
    for (int f = 0; f < 60; ++f) { tx += 2.5f / 30; SteerStep(st, tx, 0, 2.5f, 0, 1.0f / 30, 1.0f); }
    assert(std::fabs(st.x - tx) < 1e-3f);                         // no lag on a steady path
    float maxOver = 0;
    for (int f = 0; f < 60; ++f)                                  // path reverses: walk -x for 2 s
    {
        tx -= 2.5f / 30; SteerStep(st, tx, 0, -2.5f, 0, 1.0f / 30, 1.0f);
        maxOver = std::fmax(maxOver, st.x - tx);
    }
    assert(maxOver > 0.2f && maxOver < 1.5f);                     // carried on, then turned back (the arc)
    assert(std::fabs(st.x - tx) < 0.05f);                         // back on the path after ~1 s
    for (float settle : { 0.0f, 0.05f, 0.1f, 1.0f, 5.0f })        // any steer_time stays stable, hitches too
    {
        Steer s;
        for (int f = 0; f < 120; ++f) SteerStep(s, 5, 0, 0, 0, f % 10 ? 1.0f / 30 : 0.1f, settle);
        assert(std::isfinite(s.x) && std::fabs(s.x) < 7);
    }
    std::printf("steer ok (overshoot %.2f yd)\n", maxOver);
}
