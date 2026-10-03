#include "TSBossAI.h"
#include "InstanceScript.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "AchievementMgr.h"

static bool ResolveBossScript(Creature* creature, uint32& boss, InstanceScript *& script)
{
#if TRINITY
    if (!creature || !creature->GetCreatureData()) return false;
    uint32 _boss = sObjectMgr->GetCreatureBoss(creature->GetSpawnId());
    if (_boss == UINT32_MAX) return false;
    InstanceScript * _script = creature->GetInstanceScript();
    if (!_script) return false;
    boss = _boss;
    script = _script;
    return true;
#endif
}

static void IterBosses(uint32 boss, InstanceScript const* script, std::function<bool(Creature*)> callback)
{
#if TRINITY
    std::vector<uint32> const& guids = script->BossSpawnGUIDs(boss);
    for (uint32 guid : guids)
    {
        Creature* creature = script->instance->GetCreatureBySpawnId(guid);
        if (!creature) continue;
        if (!callback(creature)) break;
    }
#endif
}

void TSBossAI::OnJustEngage(Creature* creature, Unit* target)
{
#if TRINITY
    uint32 boss;
    InstanceScript * script;
    if (!ResolveBossScript(creature, boss, script)) return;
    script->SetBossState(boss, EncounterState::IN_PROGRESS);
    script->SendEncounterUnit(ENCOUNTER_FRAME_ENGAGE, creature);
    IterBosses(boss,script, [&](Creature* boss) {
        if (boss != creature && !boss->IsInCombat())
        {
            boss->EngageWithTarget(target);
        }
        return true;
    });
#endif
}

void TSBossAI::OnJustDied(Creature* creature, Unit* attacker)
{
    bool isAnyAlive = false;
    uint32 boss;
    InstanceScript * script;
    if (!ResolveBossScript(creature, boss, script)) return;
    script->SendEncounterUnit(ENCOUNTER_FRAME_DISENGAGE, creature);
    IterBosses(boss,script, [&](Creature* boss) {
        if (boss->IsAlive()) {
            isAnyAlive = true;
            return false;
        }
        else {
            return true;
        }
    });

    if (!isAnyAlive)
    {
        script->SetBossState(boss, EncounterState::DONE);
    }
}

// A wipe on any spawn of a boss fails the encounter and evades the rest of its spawns.
void TSBossAI::OnEvade(Creature* creature)
{
#if TRINITY
    uint32 boss;
    InstanceScript * script;
    if (!ResolveBossScript(creature, boss, script)) return;
    script->SendEncounterUnit(ENCOUNTER_FRAME_DISENGAGE, creature);
    if (script->GetBossState(boss) != EncounterState::IN_PROGRESS) return;
    script->SetBossState(boss, EncounterState::FAIL);
    IterBosses(boss, script, [&](Creature* other) {
        if (other != creature && other->IsAlive() && other->IsInCombat() && other->IsAIEnabled())
        {
            other->AI()->EnterEvadeMode(CreatureAI::EVADE_REASON_OTHER);
        }
        return true;
    });
#endif
}
