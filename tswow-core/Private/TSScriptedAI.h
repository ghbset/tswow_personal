// @tswow-begin
#pragma once

#include "TSMain.h"
#include "ScriptedCreature.h"

// AIName "TSScriptedAI": an empty shell driven entirely by livescript events.
// Chases and (optionally) melees its victim, tracks its summons, and despawns
// them on evade. Takes priority over creature_template.ScriptName, so it can
// replace a core C++ boss script.
class TC_GAME_API TSScriptedAI : public ScriptedAI
{
public:
    explicit TSScriptedAI(Creature* creature) : ScriptedAI(creature), summons(creature) {}
    static int32 Permissible(Creature const* /*creature*/) { return PERMIT_BASE_NO; }

    void UpdateAI(uint32 diff) override;
    void EnterEvadeMode(EvadeReason why) override;
    void JustSummoned(Creature* summon) override;
    void SummonedCreatureDespawn(Creature* summon) override;

    void SetAutoMelee(bool autoMelee) { _autoMelee = autoMelee; }
    bool GetAutoMelee() const { return _autoMelee; }

    SummonList summons;
private:
    bool _autoMelee = true;
};
// @tswow-end
