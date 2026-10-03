// Smooth turning + steering for creatures (retail's Turning / Steering splines, on 3.3.5a). Pairs with the
// core's Movement.SmoothGroundPath, which rounds path corners; this covers what a path cannot: the first
// turn when a move starts, the final facing when it stops, and turning on the spot (an NPC turning to face
// you is a zero-length move with a final facing). Ported from the wxl-smoothturn WarcraftXL extension: it is
// how the server's movement is meant to look, so it lives here, not in an optional plugin.
//
// Hooks, all on the unit's movement object (unit+0xD8, owner at +0x144):
//  - the per-frame spline evaluator (0x98CA00) writes facing = path direction every frame; we let it, then
//    limit how far facing moved from last frame's value;
//  - the spline finish (0x6EB0B0) applies the final facing (angle / target / point) once; we put the old
//    facing back and turn toward the new one over the next frames from the frame tick (0x77ECB0).
// Steering (retail's Steering flag, on for every out-of-combat NPC): the shown position follows the path
// through a spring that keeps the creature's momentum, so a sharp reversal becomes a small U-turn arc and
// the facing follows that arc; see GameplayMath.h. In combat creatures stay exactly on their path.
// Whatever you control (your character, a vehicle) is never touched: it is the client's active mover.
// Settings: [SmoothTurn] in ClientExtensions.ini (ClientSettings.h).
#include "ClientDetours.h"
#include "ClientSettings.h"
#include "Logger.h"
#include "Gameplay/GameplayMath.h"

#include <windows.h>
#include <cstring>
#include <unordered_map>

using namespace Gameplay;

namespace {
    // ---- stock 3.3.5a (12340) -------------------------------------------------------------------------
    // spline evaluator: __thiscall(movement, uint32 timeMs, float* outPos), ret 8 -- writes facing at 0x98cc04
    typedef int (__fastcall* SplineEvalFn)(void* mov, void* edx, uint32_t timeMs, float* outPos);
    // spline finish / final facing: __thiscall(movement, 6 args), ret 0x18 -- final facing at 0x6eb0f6..0x6eb197
    typedef void (__fastcall* SplineFinishFn)(void* mov, void* edx, uint32_t a1, uint32_t a2, uint32_t a3,
                                              uint32_t a4, uint32_t a5, uint32_t a6);
    // frame tick: __cdecl(float deltaSeconds, uint32 frameTimeMs) -- wxl-core's OnUpdate source
    typedef void (__cdecl* FramePumpFn)(float dt, uint32_t frameMs);
    SplineEvalFn   SplineEval   = reinterpret_cast<SplineEvalFn>(0x0098CA00);
    SplineFinishFn SplineFinish = reinterpret_cast<SplineFinishFn>(0x006EB0B0);
    FramePumpFn    FramePump    = reinterpret_cast<FramePumpFn>(0x0077ECB0);
    void* const kEvalSite   = reinterpret_cast<void*>(0x0098CA00);
    void* const kFinishSite = reinterpret_cast<void*>(0x006EB0B0);
    // whole instructions MinHook relocates: push ebp / mov ebp,esp / next instruction
    constexpr unsigned char kStockEval[6]   = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x0C };
    constexpr unsigned char kStockFinish[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18 };
    constexpr unsigned char kStockPump[7]   = { 0x55, 0x8B, 0xEC, 0x51, 0xD9, 0x45, 0x08 };

    CLIENT_FUNCTION(ObjectPtr, 0x004D4DB0, __cdecl, void*, (uint32_t lo, uint32_t hi, uint32_t typeMask))
    constexpr uintptr_t kActiveMover = 0x00CA1238;  // uint64 guid of what the player controls
    constexpr size_t kMovPos      = 0x10;   // movement: position x, y, z
    constexpr size_t kMovFacing   = 0x20;   // movement: facing (radians, [0, 2pi))
    constexpr size_t kMovSpline   = 0xBC;   // movement: active spline or null
    constexpr size_t kMovOwner    = 0x144;  // movement: owning unit
    constexpr size_t kSplineFlags = 0x20;   // spline: flags; 0x4200 = orientation fixed / falling
    constexpr size_t kUnitMov     = 0xD8;   // unit: movement object
    constexpr size_t kObjDesc     = 0x08;   // object: descriptors (guid at +0)
    constexpr size_t kUnitFlags   = 0x3B * 4;  // descriptor: UNIT_FIELD_FLAGS
    constexpr uint32_t kUnitInCombat = 0x80000;

    // ---- settings ([SmoothTurn] in ClientExtensions.ini) ---------------------------------------------
    float g_rate       = 360.0f;  // degrees per second, out of combat
    float g_combatRate = 720.0f;  // degrees per second, in combat
    bool  g_enabled    = true;
    bool  g_steering   = true;
    float g_steerTime  = 1.0f;    // seconds a steering arc takes to rejoin the path
    FILETIME g_seen{};
    DWORD g_lastLook = 0;
    bool  g_yielded    = false;   // something hooked over our spline detours (stale wxl-smoothturn): stand aside

    template <class T> T& Field(void* base, size_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off); }

    // per unit guid while it moves (and while a steering arc settles after it stops)
    struct Track
    {
        Steer    shown;              // drawn position / velocity
        float    tx, ty;             // true path position at the last evaluation
        uint32_t lastMs;             // evaluator time of the last evaluation
        bool     evaluated;          // evaluated since the last frame tick (still moving)
    };
    struct Pending { float target; float written; };    // per unit guid: a final facing still being turned to
    std::unordered_map<uint64_t, Track>   g_track;
    std::unordered_map<uint64_t, Pending> g_pending;

    bool InCombat(void* unit)
    {
        const uint8_t* d = Field<uint8_t*>(unit, kObjDesc);
        return d && (*reinterpret_cast<const uint32_t*>(d + kUnitFlags) & kUnitInCombat);
    }

    uint64_t GuidOf(void* obj)
    {
        const uint8_t* d = obj ? Field<uint8_t*>(obj, kObjDesc) : nullptr;
        return d ? *reinterpret_cast<const uint64_t*>(d) : 0;
    }

    /// The unit this movement object belongs to, or null if it is ours (the active mover) or unknown.
    void* NpcOwner(void* mov, uint64_t& guid)
    {
        void* unit = Field<void*>(mov, kMovOwner);
        guid = GuidOf(unit);
        if (!guid || guid == *reinterpret_cast<const uint64_t*>(kActiveMover)) return nullptr;
        return unit;
    }

    float RateFor(void* unit)
    {
        return (InCombat(unit) ? g_combatRate : g_rate) * (kTwoPi / 360.0f);
    }

    bool Active() { return g_enabled && !g_yielded; }

    /// Whether `site` still starts with MinHook's jmp to `detour`, i.e. nothing has hooked over us since.
    bool StillOurs(void* site, void* detour)
    {
        const uint8_t* p = static_cast<const uint8_t*>(site);
        return p[0] == 0xE9 && reinterpret_cast<uintptr_t>(p + 5) + *reinterpret_cast<const int32_t*>(p + 1)
                                   == reinterpret_cast<uintptr_t>(detour);
    }

    int __fastcall SplineEvalDetour(void* mov, void* edx, uint32_t timeMs, float* outPos)
    {
        const float before = Field<float>(mov, kMovFacing);
        const int r = SplineEval(mov, edx, timeMs, outPos);
        uint64_t guid;
        void* unit = Active() ? NpcOwner(mov, guid) : nullptr;
        void* spline = Field<void*>(mov, kMovSpline);
        if (!unit || !spline || (Field<uint32_t>(spline, kSplineFlags) & 0x4200)) return r;

        g_pending.erase(guid);                           // a new move replaces any unfinished final turn
        const float tx = outPos[0], ty = outPos[1];      // where the path puts it now
        auto [it, fresh] = g_track.try_emplace(guid);
        Track& t = it->second;
        if (fresh) { t.shown.x = tx; t.shown.y = ty; t.tx = tx; t.ty = ty; t.lastMs = timeMs; }
        uint32_t dtMs = timeMs - t.lastMs;
        if (dtMs > 100) dtMs = 100;                      // hitch: cap one step
        const float dt = dtMs * 0.001f;
        const float tvx = dt > 0 ? (tx - t.tx) / dt : 0.0f, tvy = dt > 0 ? (ty - t.ty) / dt : 0.0f;
        t.tx = tx; t.ty = ty; t.lastMs = timeMs; t.evaluated = true;

        float& facing = Field<float>(mov, kMovFacing);
        float want = facing;                             // stock: along the path
        if (g_steering && !InCombat(unit))
        {
            if (dt > 0) SteerStep(t.shown, tx, ty, tvx, tvy, dt, g_steerTime);
            outPos[0] = t.shown.x; outPos[1] = t.shown.y;    // drawn on the arc, not the path
            if (t.shown.vx * t.shown.vx + t.shown.vy * t.shown.vy > 0.01f)
                want = Wrap(std::atan2(t.shown.vy, t.shown.vx)
                            + ((Field<uint32_t>(spline, kSplineFlags) & 0x8000000) ? kTwoPi / 2 : 0.0f));  // backward
        }
        else
            t.shown = Steer{ tx, ty, tvx, tvy };         // in combat: exactly on the path
        facing = Approach(before, want, RateFor(unit) * dt);
        return r;
    }

    void __fastcall SplineFinishDetour(void* mov, void* edx, uint32_t a1, uint32_t a2, uint32_t a3,
                                       uint32_t a4, uint32_t a5, uint32_t a6)
    {
        const float before = Field<float>(mov, kMovFacing);
        SplineFinish(mov, edx, a1, a2, a3, a4, a5, a6);
        float& facing = Field<float>(mov, kMovFacing);
        uint64_t guid;
        const float target = Wrap(facing);
        if (!Active() || !NpcOwner(mov, guid) || Approach(before, target, 0.01f) == target) return;  // no real turn
        g_pending[guid] = Pending{ target, before };
        facing = before;                                 // turned from the frame tick instead of snapping
    }

    void Tick(float dt)
    {
        if (ClientSettings::Changed(g_seen, g_lastLook))
        {
            g_enabled    = ClientSettings::Int("SmoothTurn", "enabled", 1) != 0;
            g_rate       = ClientSettings::Float("SmoothTurn", "turn_rate", 360.0f);
            g_combatRate = ClientSettings::Float("SmoothTurn", "combat_rate", 720.0f);
            g_steering   = ClientSettings::Int("SmoothTurn", "steering", 1) != 0;
            g_steerTime  = ClientSettings::Float("SmoothTurn", "steer_time", 1.0f);
        }
        // WarcraftXL loads after us, so a stale wxl-smoothturn hooks over our detours rather than failing
        // our stock-bytes check: notice that within a second and let it have the job alone
        static uint32_t frame = 0;
        if ((frame++ % 60) == 0 && !g_yielded
            && !(StillOurs(kEvalSite, (void*)&SplineEvalDetour) && StillOurs(kFinishSite, (void*)&SplineFinishDetour)))
        {
            g_yielded = true;
            g_track.clear();
            g_pending.clear();
            LOG_WARN << "SmoothTurn: something hooked the spline code over ours (an old wxl-smoothturn?) -- standing aside";
        }

        // stopped moving: ease out what is left of a steering arc instead of snapping onto the end point
        for (auto it = g_track.begin(); it != g_track.end();)
        {
            Track& t = it->second;
            if (t.evaluated) { t.evaluated = false; ++it; continue; }
            void* unit = ObjectPtr(static_cast<uint32_t>(it->first), static_cast<uint32_t>(it->first >> 32), 8);
            void* mov = unit ? Field<void*>(unit, kUnitMov) : nullptr;
            float* pos = mov ? &Field<float>(mov, kMovPos) : nullptr;
            const float ox = t.shown.x - t.tx, oy = t.shown.y - t.ty;
            const bool ours = pos && ((std::fabs(pos[0] - t.shown.x) < 1e-3f && std::fabs(pos[1] - t.shown.y) < 1e-3f)
                                      || (std::fabs(pos[0] - t.tx) < 1e-3f && std::fabs(pos[1] - t.ty) < 1e-3f));
            if (!ours || !g_steering || ox * ox + oy * oy < 1e-4f) { it = g_track.erase(it); continue; }  // done, gone or moved
            SteerStep(t.shown, t.tx, t.ty, 0.0f, 0.0f, dt, g_steerTime);
            pos[0] = t.shown.x; pos[1] = t.shown.y;
            ++it;
        }
        for (auto it = g_pending.begin(); it != g_pending.end();)
        {
            void* unit = ObjectPtr(static_cast<uint32_t>(it->first), static_cast<uint32_t>(it->first >> 32), 8);
            void* mov  = unit ? Field<void*>(unit, kUnitMov) : nullptr;
            float* facing = mov ? &Field<float>(mov, kMovFacing) : nullptr;
            // gone, or something else set its facing since (teleport, new move): let that stand
            if (!facing || std::fabs(*facing - it->second.written) > 1e-4f) { it = g_pending.erase(it); continue; }
            *facing = it->second.written = Approach(it->second.written, it->second.target, RateFor(unit) * dt);
            if (it->second.written == it->second.target) it = g_pending.erase(it); else ++it;
        }
    }

    void __cdecl FramePumpDetour(float dt, uint32_t frameMs)
    {
        FramePump(dt, frameMs);
        Tick(dt);
    }
}

void InstallSmoothTurn() {
    // all three or none: a finish hook without the frame tick would leave final turns stuck half-way
    if (!ClientDetours::IsStock("SmoothTurn.SplineEval", kEvalSite, kStockEval, sizeof(kStockEval))
        || !ClientDetours::IsStock("SmoothTurn.SplineFinish", kFinishSite, kStockFinish, sizeof(kStockFinish))
        || !ClientDetours::IsStock("SmoothTurn.FramePump", (void*)FramePump, kStockPump, sizeof(kStockPump)))
        return;
    ClientDetours::Add("SmoothTurn.SplineEval", (void*)&SplineEval, (void*)SplineEvalDetour, __FILE__, __LINE__);
    ClientDetours::Add("SmoothTurn.SplineFinish", (void*)&SplineFinish, (void*)SplineFinishDetour, __FILE__, __LINE__);
    ClientDetours::Add("SmoothTurn.FramePump", (void*)&FramePump, (void*)FramePumpDetour, __FILE__, __LINE__);
}
