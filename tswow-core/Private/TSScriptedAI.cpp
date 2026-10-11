// @tswow-begin
#include "TSScriptedAI.h"
#include "TSEvents.h"
#include "TSCreature.h"

void TSScriptedAI::UpdateAI(uint32 diff)
{
    if (_boss.Down())
    {
        _boss.UpdateDown(me, diff);
        return;
    }

    if (!UpdateVictim())
        return;

    _boss.Update(me, diff);

    if (_autoMelee && !_boss.Holding())
        DoMeleeAttackIfReady();
}

void TSScriptedAI::EnterEvadeMode(EvadeReason why)
{
    if (me->IsAlive() && !me->IsInEvadeMode())
        summons.DespawnAll();
    TSBossLink const* link = _boss.Link();
    _boss.Reset(me);
    ScriptedAI::EnterEvadeMode(why);
    if (link)
        TSBossRuntime::LinkEvaded(link, me);
}

void TSScriptedAI::JustEngagedWith(Unit* /*who*/)
{
    _boss.Engage(me);
}

void TSScriptedAI::JustDied(Unit* /*killer*/)
{
    _boss.Died(me);
    if (_boss.Active())
        summons.DespawnAll();
    _boss.Reset(me);
}

void TSScriptedAI::JustSummoned(Creature* summon)
{
    summons.Summon(summon);
    _boss.NoteSummon();
}

void TSScriptedAI::SummonedCreatureDespawn(Creature* summon)
{
    summons.Despawn(summon);
}
void TSScriptedAI::DamageTaken(Unit* attacker, uint32& damage, DamageEffectType /*damageType*/, SpellInfo const* /*spellInfo*/)
{
    _boss.DamageTaken(me, attacker, damage);
}

void TSScriptedAI::HealReceived(Unit* /*healer*/, uint32& heal)
{
    _boss.HealReceived(me, heal);
}

void TSScriptedAI::MovementInform(uint32 type, uint32 id)
{
    _boss.MovementInform(me, type, id);
}

void TSScriptedAI::DoAction(int32 action)
{
    FIRE_ID(me->GetCreatureTemplate()->events.id, Creature, OnDoAction, TSCreature(me), action);
}

void TSScriptedAI::SetData(uint32 id, uint32 value)
{
    FIRE_ID(me->GetCreatureTemplate()->events.id, Creature, OnSetData, TSCreature(me), id, value);
}
// @tswow-end
