// Spell queue window, client half (retail 4.1+; the server half is the core's SpellQueue.*). Ported from the
// wxl-spellqueue WarcraftXL extension: the server depends on it, so it lives here, not in an optional plugin.
//
// The stock 3.3.5a client already sends a cast pressed while another is finishing, but refuses one pressed
// during the global cooldown ("not ready"). CGUnit_C::ValidateSpellCast (0x739650) asks the cooldown lookup
// at one call site (0x739773); that call is pointed at QueueAwareCooldown, which answers "ready" when the
// cooldown ends within `window` ms. The server holds the early cast and fires it the moment the cooldown ends.
// Settings: [SpellQueue] in ClientExtensions.ini (ClientSettings.h).
#include "ClientDetours.h"
#include "ClientSettings.h"
#include "Logger.h"
#include "Gameplay/GameplayMath.h"

#include <windows.h>
#include <cstring>

namespace {
    // bool __cdecl GetSpellCooldown(spellId, who /*0 = player, 1 = pet*/, int* duration, int* start, uint32* notOnHold)
    CLIENT_FUNCTION(GetSpellCooldown, 0x00809000, __cdecl, int, (uint32_t, uint32_t, int32_t*, int32_t*, uint32_t*))
    CLIENT_FUNCTION(NowMs, 0x0086AE20, __cdecl, uint32_t, ())     // the clock cooldown starts are stamped with
    constexpr uintptr_t kCallSite = 0x00739773;                    // ValidateSpellCast: call GetSpellCooldown
    constexpr unsigned char kStockCall[5] = { 0xE8, 0x88, 0xF8, 0x0C, 0x00 };

    bool g_enabled = true;
    int32_t g_window = 150;
    FILETIME g_seen{};
    DWORD g_lastLook = 0;

    int __cdecl QueueAwareCooldown(uint32_t spellId, uint32_t who, int32_t* duration, int32_t* start, uint32_t* notOnHold) {
        if (ClientSettings::Changed(g_seen, g_lastLook)) {
            g_enabled = ClientSettings::Int("SpellQueue", "enabled", 1) != 0;
            g_window = ClientSettings::Int("SpellQueue", "window", 150);
        }
        int32_t dur = 0, begin = 0;
        uint32_t running = 1;
        const int onCooldown = GetSpellCooldown(spellId, who, &dur, &begin, &running);
        if (duration) *duration = dur;
        if (start) *start = begin;
        if (notOnHold) *notOnHold = running;
        return Gameplay::QueueCooldownResult(onCooldown, g_enabled, who, running, begin, dur,
                                             NowMs(), g_window);
    }
}

void InstallSpellQueue() {
    void* site = reinterpret_cast<void*>(kCallSite);
    if (!ClientDetours::IsStock("SpellQueue.ValidateSpellCast", site, kStockCall, 5)) return;
    unsigned char call[5] = { 0xE8 };
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&QueueAwareCooldown) - (kCallSite + 5));
    memcpy(call + 1, &rel, 4);
    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(site, call, 5);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    LOG_INFO << "SpellQueue: call@" << site << " -> QueueAwareCooldown";
}
