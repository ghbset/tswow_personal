#pragma once
// ClientExtensions.ini: player-tunable settings, in the game folder beside ClientExtensions.dll.
// Written with every key at its default the first time it is missing; edits apply in game within about a
// second, no restart. A key left out, or the whole file deleted, means the compiled default.
// The default text (and so the list of keys) is in ClientSettings.cpp.

#include <windows.h>

namespace ClientSettings {
    /// True when the file changed since `seen` (always on the first call). Looks at most once a second.
    bool Changed(FILETIME& seen, DWORD& lastLook);
    int   Int(char const* section, char const* key, int def);
    float Float(char const* section, char const* key, float def);
}
