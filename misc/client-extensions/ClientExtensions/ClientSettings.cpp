#include "ClientSettings.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace {
    constexpr char kDefaults[] =
        "; ClientExtensions settings. Changes apply in game within about a second.\n"
        "; Delete this file to go back to the defaults.\n"
        "\n"
        "[SpellQueue]\n"
        "; Press your next spell up to `window` ms before the global cooldown (or its cooldown) ends and it\n"
        "; fires the moment it can (retail's Spell Queue Window). The server holds it (SpellQueue.Enabled).\n"
        "enabled = 1\n"
        "window = 150\n"
        "\n"
        "[SmoothTurn]\n"
        "; Creatures turn at a set speed instead of snapping to a new facing.\n"
        "; turn_rate    degrees per second out of combat (360 = a full turn in one second)\n"
        "; combat_rate  degrees per second in combat; faster so mobs stay responsive\n"
        "; steering     1 = out of combat, creatures keep their momentum and arc into sharp turns (retail)\n"
        "; steer_time   seconds a steering arc takes to rejoin the path; bigger = wider arcs\n"
        "enabled = 1\n"
        "turn_rate = 360\n"
        "combat_rate = 720\n"
        "steering = 1\n"
        "steer_time = 1.0\n";

    char const* Path() {
        static char path[MAX_PATH] = {};
        if (!path[0]) {
            HMODULE self = nullptr;
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCSTR>(&Path), &self)
                || !GetModuleFileNameA(self, path, MAX_PATH)) {
                path[0] = 0;
                return path;
            }
            char* slash = strrchr(path, '\\');
            if (!slash) { path[0] = 0; return path; }
            strcpy(slash + 1, "ClientExtensions.ini");
        }
        return path;
    }
}

bool ClientSettings::Changed(FILETIME& seen, DWORD& lastLook) {
    DWORD now = GetTickCount();
    bool first = !lastLook;
    if (!first && now - lastLook < 1000) return false;
    lastLook = now ? now : 1;
    if (!Path()[0]) return first;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExA(Path(), GetFileExInfoStandard, &fad)) {
        if (FILE* f = fopen(Path(), "w")) { fputs(kDefaults, f); fclose(f); }
        if (!GetFileAttributesExA(Path(), GetFileExInfoStandard, &fad)) return first;
    }
    if (!first && CompareFileTime(&fad.ftLastWriteTime, &seen) == 0) return false;
    seen = fad.ftLastWriteTime;
    return true;
}

int ClientSettings::Int(char const* section, char const* key, int def) {
    return Path()[0] ? static_cast<int>(GetPrivateProfileIntA(section, key, def, Path())) : def;
}

float ClientSettings::Float(char const* section, char const* key, float def) {
    char buf[64];
    if (!Path()[0] || !GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), Path()) || !buf[0]) return def;
    return static_cast<float>(atof(buf));
}
