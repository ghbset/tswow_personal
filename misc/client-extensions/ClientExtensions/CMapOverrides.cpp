#include "ClientDetours.h"
#include "Logger.h"
#include "SharedDefines.h"

#include <Windows.h>

CLIENT_DETOUR(CMap__SafeOpen, 0x007BD480, __cdecl, int, (char *Src, HANDLE* a2)) {
    for (int i = 0; i < 10; i++) {
        if (SFileOpenFile(Src, a2)) {
            return 1;
        }
    }
    LOG_DEBUG << "BAD FILE READ, NO FIND " << Src;
    return SFileOpenFile("Spells\\ErrorCube.mdx", a2);
}
