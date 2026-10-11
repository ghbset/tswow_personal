// @tswow-begin
#include "TSBoss.h"
#include "TSPosition.h"

void TSBosses::LoadLua(sol::state& state)
{
    auto selector = state.new_usertype<TSBossSelector>("TSBossSelector", sol::no_constructor);
    LUA_FIELD(selector, TSBossSelector, ExcludeTank);
    LUA_FIELD(selector, TSBossSelector, Players);
    LUA_FIELD(selector, TSBossSelector, Ranged);
    LUA_FIELD(selector, TSBossSelector, Melee);
    LUA_FIELD(selector, TSBossSelector, With);
    LUA_FIELD(selector, TSBossSelector, Without);
    LUA_FIELD(selector, TSBossSelector, OfClass);
    LUA_FIELD(selector, TSBossSelector, Between);
    LUA_FIELD(selector, TSBossSelector, Where);
    LUA_FIELD(selector, TSBossSelector, OfRace);
    LUA_FIELD(selector, TSBossSelector, OfGender);
    LUA_FIELD(selector, TSBossSelector, OfEntry);
    LUA_FIELD(selector, TSBossSelector, ManaUsers);
    LUA_FIELD(selector, TSBossSelector, WithPowerType);
    LUA_FIELD(selector, TSBossSelector, InFront);
    LUA_FIELD(selector, TSBossSelector, Behind);
    LUA_FIELD(selector, TSBossSelector, InLineOfSight);
    LUA_FIELD(selector, TSBossSelector, Within);
    LUA_FIELD(selector, TSBossSelector, Beyond);
    LUA_FIELD(selector, TSBossSelector, AboveZ);
    LUA_FIELD(selector, TSBossSelector, BelowZ);
    LUA_FIELD(selector, TSBossSelector, Moving);
    LUA_FIELD(selector, TSBossSelector, Standing);
    LUA_FIELD(selector, TSBossSelector, Casting);
    LUA_FIELD(selector, TSBossSelector, InWater);
    LUA_FIELD(selector, TSBossSelector, Mounted);
    LUA_FIELD(selector, TSBossSelector, HealthBelow);
    LUA_FIELD(selector, TSBossSelector, HealthAbove);
    LUA_FIELD(selector, TSBossSelector, InSubGroup);
    LUA_FIELD(selector, TSBossSelector, ExcludePets);
    LUA_FIELD(selector, TSBossSelector, Dead);
    LUA_FIELD(selector, TSBossSelector, NotLastTarget);
    LUA_FIELD(selector, TSBossSelector, FacingBoss);
    LUA_FIELD(selector, TSBossSelector, FacingAway);
    LUA_FIELD(selector, TSBossSelector, OnLeft);
    LUA_FIELD(selector, TSBossSelector, OnRight);
    LUA_FIELD(selector, TSBossSelector, Supports);
    LUA_FIELD(selector, TSBossSelector, Dps);
    LUA_FIELD(selector, TSBossSelector, Near);
    LUA_FIELD(selector, TSBossSelector, NoRepeats);
    LUA_FIELD(selector, TSBossSelector, SortByDistanceTo);
    LUA_FIELD(selector, TSBossSelector, SortByAuraTime);
    LUA_FIELD(selector, TSBossSelector, SortByStacks);
    LUA_FIELD(selector, TSBossSelector, Reverse);
    LUA_FIELD(selector, TSBossSelector, SecondThreat);
    LUA_FIELD(selector, TSBossSelector, MostStacked);
    LUA_FIELD(selector, TSBossSelector, MostIsolated);
    LUA_FIELD(selector, TSBossSelector, OnePerRole);
    LUA_FIELD(selector, TSBossSelector, MainTank);
    LUA_FIELD(selector, TSBossSelector, OffTanks);
    LUA_FIELD(selector, TSBossSelector, Tanks);
    LUA_FIELD(selector, TSBossSelector, Healers);
    LUA_FIELD(selector, TSBossSelector, MeleeDps);
    LUA_FIELD(selector, TSBossSelector, RangedDps);
    LUA_FIELD(selector, TSBossSelector, SortBy);
    LUA_FIELD(selector, TSBossSelector, First);
    LUA_FIELD(selector, TSBossSelector, Last);
    LUA_FIELD(selector, TSBossSelector, Nth);
    LUA_FIELD(selector, TSBossSelector, All);
    LUA_FIELD(selector, TSBossSelector, Random);
    LUA_FIELD(selector, TSBossSelector, HighestThreat);
    LUA_FIELD(selector, TSBossSelector, Farthest);
    LUA_FIELD(selector, TSBossSelector, Closest);

    auto condition = state.new_usertype<TSBossCondition>("TSBossCondition", sol::no_constructor);
    LUA_FIELD(condition, TSBossCondition, Below);
    LUA_FIELD(condition, TSBossCondition, Every);
    LUA_FIELD(condition, TSBossCondition, Reaches);
    LUA_FIELD(condition, TSBossCondition, Reach);
    LUA_FIELD(condition, TSBossCondition, AllDead);

    auto trigger = state.new_usertype<TSBossTrigger>("TSBossTrigger", sol::no_constructor);
    LUA_FIELD(trigger, TSBossTrigger, GoTo);
    LUA_FIELD(trigger, TSBossTrigger, Do);

    auto actions = state.new_usertype<TSBossActions>("TSBossActions", sol::no_constructor);
    LUA_FIELD(actions, TSBossActions, Cast);
    LUA_FIELD(actions, TSBossActions, Talk);
    LUA_FIELD(actions, TSBossActions, SetPassive);
    LUA_FIELD(actions, TSBossActions, SetAggressive);
    LUA_FIELD(actions, TSBossActions, TakeOff);
    LUA_FIELD(actions, TSBossActions, Land);
    LUA_FIELD(actions, TSBossActions, MoveTo);
    LUA_FIELD(actions, TSBossActions, Summon);
    LUA_FIELD(actions, TSBossActions, Do);

    auto timer = state.new_usertype<TSBossTimer>("TSBossTimer", sol::no_constructor);
    LUA_FIELD(timer, TSBossTimer, Every);
    LUA_FIELD(timer, TSBossTimer, After);
    LUA_FIELD(timer, TSBossTimer, Repeat);
    LUA_FIELD(timer, TSBossTimer, Burst);
    LUA_FIELD(timer, TSBossTimer, Target);
    LUA_FIELD(timer, TSBossTimer, Do);
    LUA_FIELD(timer, TSBossTimer, NoRepeat);
    LUA_FIELD(timer, TSBossTimer, Recover);
    LUA_FIELD(timer, TSBossTimer, At);
    LUA_FIELD(timer, TSBossTimer, Count);
    LUA_FIELD(timer, TSBossTimer, ZoneInCombat);

    auto phase = state.new_usertype<TSBossPhase>("TSBossPhase", sol::no_constructor);
    LUA_FIELD(phase, TSBossPhase, Ability);
    LUA_FIELD(phase, TSBossPhase, Rotate);
    LUA_FIELD(phase, TSBossPhase, Timer);
    LUA_FIELD(phase, TSBossPhase, Summon);
    LUA_FIELD(phase, TSBossPhase, When);
    LUA_FIELD(phase, TSBossPhase, Elapsed);
    LUA_FIELD(phase, TSBossPhase, OnEnter);
    LUA_FIELD(phase, TSBossPhase, OnExit);
    LUA_FIELD(phase, TSBossPhase, Inherit);
    LUA_FIELD(phase, TSBossPhase, DelayAll);
    LUA_FIELD(phase, TSBossPhase, PauseWhileCasting);
    LUA_FIELD(phase, TSBossPhase, Immortal);

    auto fake = state.new_usertype<TSBossFakeDeath>("TSBossFakeDeath", sol::no_constructor);
    LUA_FIELD(fake, TSBossFakeDeath, Times);
    LUA_FIELD(fake, TSBossFakeDeath, ReviveAfter);
    LUA_FIELD(fake, TSBossFakeDeath, ReviveAt);
    LUA_FIELD(fake, TSBossFakeDeath, GoTo);
    LUA_FIELD(fake, TSBossFakeDeath, OnFall);
    LUA_FIELD(fake, TSBossFakeDeath, OnRevive);

    auto link = state.new_usertype<TSBossLink>("TSBossLink", sol::no_constructor);
    LUA_FIELD(link, TSBossLink, SharedHealth);
    LUA_FIELD(link, TSBossLink, Handoff);
    LUA_FIELD(link, TSBossLink, AllDieWithin);
    LUA_FIELD(link, TSBossLink, LootFromLastOnly);
    LUA_FIELD(link, TSBossLink, OnHandoff);
    LUA_FIELD(link, TSBossLink, OnMemberDeath);

    auto boss = state.new_usertype<TSBoss>("TSBoss", sol::no_constructor);
    LUA_FIELD(boss, TSBoss, Phase);
    LUA_FIELD(boss, TSBoss, Self);
    LUA_FIELD(boss, TSBoss, GetEnemies);
    LUA_FIELD(boss, TSBoss, GetRaid);
    LUA_FIELD(boss, TSBoss, GetFriendlyCreatures);
    LUA_FIELD(boss, TSBoss, Health);
    LUA_FIELD(boss, TSBoss, Power);
    LUA_FIELD(boss, TSBoss, Stacks);
    LUA_FIELD(boss, TSBoss, Adds);
    LUA_FIELD(boss, TSBoss, Berserk);
    LUA_FIELD(boss, TSBoss, RemoveOnReset);
    LUA_FIELD(boss, TSBoss, FakeDeath);
    boss.set_function("Immortal", [](TSBoss& b) { b.always.Immortal(); return &b; });
    // everything below runs in every phase
    boss.set_function("Ability", [](TSBoss& b, uint32 spell) { return b.always.Ability(spell); });
    boss.set_function("Rotate", [](TSBoss& b, sol::variadic_args spells) { return b.always.Rotate(spells); });
    boss.set_function("Timer", [](TSBoss& b) { return b.always.Timer(); });
    boss.set_function("Summon", [](TSBoss& b, uint32 entry) { return b.always.Summon(entry); });
    boss.set_function("When", [](TSBoss& b, TSBossCondition* cond) { return b.always.When(cond); });
    boss.set_function("DelayAll", [](TSBoss& b, uint32 ms) { b.always.DelayAll(ms); return &b; });
    boss.set_function("PauseWhileCasting", [](TSBoss& b) { b.always.PauseWhileCasting(); return &b; });

    auto wave = state.new_usertype<TSBossObjectWave>("TSBossObjectWave", sol::no_constructor);
    LUA_FIELD(wave, TSBossObjectWave, Entries);
    LUA_FIELD(wave, TSBossObjectWave, Named);
    LUA_FIELD(wave, TSBossObjectWave, TrapSpell);
    LUA_FIELD(wave, TSBossObjectWave, StartWithin);
    LUA_FIELD(wave, TSBossObjectWave, Reach);
    LUA_FIELD(wave, TSBossObjectWave, Delay);
    LUA_FIELD(wave, TSBossObjectWave, Step);
    LUA_FIELD(boss, TSBoss, ObjectWave);
    LUA_FIELD(timer, TSBossTimer, Triggers);

    state["Bosses"] = state.create_table_with("Create", &TSBosses::Create, "Link", &TSBosses::Link);
}
// @tswow-end
