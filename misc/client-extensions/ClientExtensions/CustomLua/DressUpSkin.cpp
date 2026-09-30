// SetDressUpSkin(model, skinColor) - make a DressUpModel draw its unit with another skin colour.
//
// The wardrobe's item cards use it for retail's "transmog skin": a hidden, NPC-only shadow skin colour
// (CharSections, datascripts in modules/wardrobe) so the look stands out on a dark mannequin. The card is
// still your own character, so TryOn keeps working. SetDressUpSkin(model, nil) goes back to your own skin.
//
// How (Wow.exe 3.3.5a 12340): DressUpModelFrame::UpdateModel (0x5989E0) builds the unit's appearance record
// on the stack - race, sex, class, hair colour, skin colour, face, facial hair, hair style - and hands it
// to CCharacterComponent::Init (0x4F24D0). While a frame with an override is being built, Init sees the
// override's skin colour, and face 0 (the shadow skin only has face variation 0) - if the client's CharSections
// has that colour for the race; otherwise the card keeps the unit's own skin.
#include "ClientLua.h"
#include "ClientDetours.h"

#include <unordered_map>

namespace {
    CLIENT_FUNCTION(LuaType, 0x0084DEB0, __cdecl, int, (lua_State* L, int index))
    CLIENT_FUNCTION(LuaRawGetI, 0x0084E670, __cdecl, void, (lua_State* L, int index, int n))
    CLIENT_FUNCTION(LuaToUserData, 0x0084E1C0, __cdecl, void*, (lua_State* L, int index))
    CLIENT_FUNCTION(LuaSetTop, 0x0084DBF0, __cdecl, void, (lua_State* L, int index))

    // the client's own CharSections lookup (race, sex, section, variation, colour), as Init uses it
    CLIENT_FUNCTION(CharSectionExists, 0x004F3B50, __cdecl, char, (int index, int race, int sex, unsigned section, int variation, int color))
    CLIENT_ADDRESS(int, CharSectionsIndex, 0x00B6B864)

    constexpr int RECORD_RACE = 0;
    constexpr int RECORD_SEX = 1;
    constexpr int RECORD_SKIN = 4;
    constexpr int RECORD_FACE = 5;

    std::unordered_map<void*, int> skinOverrides;   // DressUpModel frame -> skin colour
    void* building = nullptr;                        // the frame DressUpModelFrame::UpdateModel is building
}

LUA_FUNCTION(SetDressUpSkin, (lua_State* L)) {
    if (LuaType(L, 1) != LUA_TTABLE) return 0;
    LuaRawGetI(L, 1, 0);                             // a frame table keeps its C++ object at [0]
    void* frame = LuaToUserData(L, -1);
    LuaSetTop(L, -2);
    if (!frame) return 0;
    if (ClientLua::IsNumber(L, 2)) {
        skinOverrides[frame] = static_cast<int>(ClientLua::ToNumber(L, 2));
    } else {
        skinOverrides.erase(frame);
    }
    return 0;
}

// __thiscall targets, detoured as __fastcall: `this` arrives in ECX, EDX is unused. Written out by hand:
// CLIENT_DETOUR only puts the calling convention on the original's pointer, not on the detour itself.
namespace {
    typedef void (__fastcall* UpdateModelFn)(void* frame, void* edx, int* model);
    typedef int (__fastcall* ComponentInitFn)(void* component, void* edx, int* record, int flags);
    UpdateModelFn DressUpUpdateModel = reinterpret_cast<UpdateModelFn>(0x005989E0);
    ComponentInitFn CharacterComponentInit = reinterpret_cast<ComponentInitFn>(0x004F24D0);

    void __fastcall DressUpUpdateModelDetour(void* frame, void* edx, int* model) {
        building = frame;
        DressUpUpdateModel(frame, edx, model);
        building = nullptr;
    }

    int __fastcall CharacterComponentInitDetour(void* component, void* edx, int* record, int flags) {
        if (building) {
            auto it = skinOverrides.find(building);
            // only if this client's CharSections has the colour for this race (an HD patch may replace the
            // table): otherwise keep the unit's own skin rather than fail to build the body
            if (it != skinOverrides.end()
                && CharSectionExists(*CharSectionsIndex, record[RECORD_RACE], record[RECORD_SEX], 0, 0, it->second)) {
                record[RECORD_SKIN] = it->second;
                record[RECORD_FACE] = 0;
            }
        }
        return CharacterComponentInit(component, edx, record, flags);
    }

    int updateModelHook = ClientDetours::Add("DressUpUpdateModel", (void*)&DressUpUpdateModel,
        (void*)DressUpUpdateModelDetour, __FILE__, __LINE__);
    int componentInitHook = ClientDetours::Add("CharacterComponentInit", (void*)&CharacterComponentInit,
        (void*)CharacterComponentInitDetour, __FILE__, __LINE__);
}
