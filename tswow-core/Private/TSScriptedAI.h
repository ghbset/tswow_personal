// @tswow-begin
#pragma once

#include "TSMain.h"
#include "ScriptedCreature.h"
#include "TSBoss.h"

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
    void JustEngagedWith(Unit* who) override;
    void JustDied(Unit* killer) override;
    void DamageTaken(Unit* attacker, uint32& damage, DamageEffectType damageType, SpellInfo const* spellInfo) override;
    void HealReceived(Unit* healer, uint32& heal) override;
    void DoAction(int32 action) override;
    void MovementInform(uint32 type, uint32 id) override;
    void SetData(uint32 id, uint32 value) override;
    void EnterEvadeMode(EvadeReason why) override;
    void JustSummoned(Creature* summon) override;
    void SummonedCreatureDespawn(Creature* summon) override;

    void SetAutoMelee(bool autoMelee) { _autoMelee = autoMelee; }
    bool GetAutoMelee() const { return _autoMelee; }
    TSBossRuntime& Boss() { return _boss; }
    bool IsFakeDead() const { return _boss.Down(); }
    void ReviveFakeDeath() { _boss.Revive(me); }

    SummonList summons;
private:
    bool _autoMelee = true;
    TSBossRuntime _boss;   // idle unless livescripts called Bosses.Create for this entry
};
// @tswow-end
