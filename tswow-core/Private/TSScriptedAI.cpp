// @tswow-begin
#include "TSScriptedAI.h"

void TSScriptedAI::UpdateAI(uint32 /*diff*/)
{
    if (!UpdateVictim())
        return;

    if (_autoMelee)
        DoMeleeAttackIfReady();
}

void TSScriptedAI::EnterEvadeMode(EvadeReason why)
{
    if (me->IsAlive() && !me->IsInEvadeMode())
        summons.DespawnAll();
    ScriptedAI::EnterEvadeMode(why);
}

void TSScriptedAI::JustSummoned(Creature* summon)
{
    summons.Summon(summon);
}

void TSScriptedAI::SummonedCreatureDespawn(Creature* summon)
{
    summons.Despawn(summon);
}
// @tswow-end
