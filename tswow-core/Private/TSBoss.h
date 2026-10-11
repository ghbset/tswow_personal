// @tswow-begin
#pragma once

#include "TSMain.h"
#include "TSLua.h"
#include "EventMap.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "TSPosition.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Creature;
class Player;
class Unit;
class TSBoss;
class TSBossLink;
class TSBossPhase;
class TSBossObjectWave;
class GameObject;

// Boss framework (docs/BOSS_API_DESIGN.md §3.2). Livescripts describe a boss once
// with the builders below; TSScriptedAI runs that description for every creature
// of the entry through a TSBossRuntime. Builders are owned by their TSBoss and
// only ever handed to Lua as non-owning pointers.

// Who an ability hits. Lazy: resolved against the creature each time it is used.
// Start from a group (Boss.GetEnemies / GetRaid / GetFriendlyCreatures), narrow it
// with filters and roles, optionally SortBy a key, then pick.
class TSBossSelector
{
public:
    enum Source { SELF, ENEMIES, RAID, FRIENDLY_CREATURES };
    // keep in sync with `BossSort` in global.d.ts
    enum Sort { NONE, DISTANCE, THREAT, HEALTH, HEALTH_PCT, DPS, MAX_HEALTH, POWER, POWER_PCT, LEVEL, ITEM_LEVEL, SIZE, TIME_PLAYED, ARMOR, NEARBY_ALLIES, HEALING };
    enum Role { TANK, HEALER, MELEE_DPS, RANGED_DPS };

    explicit TSBossSelector(Source source) : _source(source) {}

    TSBossSelector* ExcludeTank() { _excludeTank = true; return this; }
    TSBossSelector* Players();
    TSBossSelector* Ranged();
    TSBossSelector* Melee();
    TSBossSelector* With(uint32 aura);
    TSBossSelector* Without(uint32 aura);
    TSBossSelector* OfClass(sol::variadic_args classes);
    TSBossSelector* Between(float minDist, float maxDist);
    TSBossSelector* Where(sol::protected_function fn);
    TSBossSelector* OfRace(sol::variadic_args races);
    TSBossSelector* OfGender(uint32 gender);
    TSBossSelector* OfEntry(sol::variadic_args entries);
    TSBossSelector* ManaUsers();
    TSBossSelector* WithPowerType(uint32 powerType);
    TSBossSelector* InFront(sol::optional<float> arcDegrees);
    TSBossSelector* Behind(sol::optional<float> arcDegrees);
    TSBossSelector* InLineOfSight();
    TSBossSelector* Within(float dist);
    TSBossSelector* Beyond(float dist);
    TSBossSelector* AboveZ(float z);
    TSBossSelector* BelowZ(float z);
    TSBossSelector* Moving();
    TSBossSelector* Standing();
    TSBossSelector* Casting();
    TSBossSelector* InWater();
    TSBossSelector* Mounted();
    TSBossSelector* HealthBelow(float pct);
    TSBossSelector* HealthAbove(float pct);
    TSBossSelector* InSubGroup(uint32 group);
    TSBossSelector* ExcludePets();
    // the FFXIV-style ones
    TSBossSelector* FacingBoss();
    TSBossSelector* FacingAway();
    TSBossSelector* OnLeft();
    TSBossSelector* OnRight();
    TSBossSelector* Supports();
    TSBossSelector* Dps();
    TSBossSelector* Near(TSPosition pos, float radius);
    // goes through everyone who qualifies before picking anyone a second time
    TSBossSelector* NoRepeats() { _noRepeats = true; return this; }
    TSBossSelector* SortByDistanceTo(TSPosition pos);
    TSBossSelector* SortByAuraTime(uint32 aura);
    TSBossSelector* SortByStacks(uint32 aura);
    // largest first instead of smallest first
    TSBossSelector* Reverse() { _reverse = true; return this; }
    TSBossSelector* SecondThreat() { _sort = THREAT; _reverse = true; return SetPick(NTH, 2); }
    TSBossSelector* MostStacked() { _sort = NEARBY_ALLIES; return SetPick(LAST, 1); }
    TSBossSelector* MostIsolated() { _sort = NEARBY_ALLIES; return SetPick(FIRST, 1); }
    // `count` random tanks, healers and damage dealers each
    TSBossSelector* OnePerRole(sol::optional<uint32> count) { return SetPick(EACH_ROLE, count.value_or(1)); }

    // GetRaid only: the dead players instead of the living ones
    TSBossSelector* Dead() { _dead = true; return this; }
    // never whoever this selector picked the time before (if anyone else qualifies)
    TSBossSelector* NotLastTarget() { _notLast = true; return this; }

    // roles, worked out from talents (see RoleOf in TSBoss.cpp)
    TSBossSelector* MainTank();
    TSBossSelector* OffTanks();
    TSBossSelector* Tanks();
    TSBossSelector* Healers();
    TSBossSelector* MeleeDps();
    TSBossSelector* RangedDps();

    // smallest first: closest, lowest threat, lowest health, lowest dps
    TSBossSelector* SortBy(uint32 key) { _sort = Sort(key); return this; }
    TSBossSelector* First(sol::optional<uint32> count) { return SetPick(FIRST, count.value_or(1)); }
    TSBossSelector* Last(sol::optional<uint32> count) { return SetPick(LAST, count.value_or(1)); }
    TSBossSelector* Nth(uint32 index) { return SetPick(NTH, index); }
    TSBossSelector* All() { return SetPick(ALL, 0); }
    TSBossSelector* Random(sol::optional<uint32> count) { return SetPick(RANDOM, count.value_or(1)); }
    TSBossSelector* HighestThreat() { _sort = THREAT; return SetPick(LAST, 1); }
    TSBossSelector* Farthest() { _sort = DISTANCE; return SetPick(LAST, 1); }
    TSBossSelector* Closest() { _sort = DISTANCE; return SetPick(FIRST, 1); }

    std::vector<Unit*> Resolve(Creature* me) const;
private:
    enum Pick { ALL, RANDOM, FIRST, LAST, NTH, EACH_ROLE };
    TSBossSelector* SetPick(Pick pick, uint32 count) { _pick = pick; _count = count; return this; }
    TSBossSelector* AddRole(Role role);
    TSBossSelector* Add(std::function<bool(Creature*, Unit*)> filter) { _filters.push_back(std::move(filter)); return this; }

    Source _source;
    bool _excludeTank = false, _dead = false, _notLast = false, _noRepeats = false, _reverse = false;
    std::function<double(Creature*, Unit*)> _customKey;
    Sort _sort = NONE;
    Pick _pick = ALL;
    uint32 _count = 1;
    std::vector<std::function<bool(Creature*, Unit*)>> _filters;
};

// What a trigger waits for. Boss.Health() etc. hand out an unset condition;
// the verb (Below, Reaches, ...) completes it.
class TSBossCondition
{
public:
    enum Type { NONE, HEALTH_BELOW, HEALTH_EVERY, POWER_REACHES, STACKS_REACH, ADDS_DEAD, ELAPSED };
    explicit TSBossCondition(Type subject, uint32 aura = 0) : aura(aura), _subject(subject) {}

    TSBossCondition* Below(float pct);
    TSBossCondition* Every(float pct);
    TSBossCondition* Reaches(float amount);
    TSBossCondition* Reach(float stacks);
    TSBossCondition* AllDead();

    Type type = NONE;
    uint32 aura;
    float value = 0;
private:
    TSBossCondition* Set(Type expected, Type result, float v);
    Type _subject;
};

class TSBossTrigger
{
public:
    explicit TSBossTrigger(TSBossCondition* cond) : cond(cond) {}
    TSBossTrigger* GoTo(TSBossPhase* phase) { goTo = phase; return this; }
    TSBossTrigger* Do(sol::protected_function f) { fn = f; return this; }

    TSBossCondition* cond;
    TSBossPhase* goTo = nullptr;
    sol::protected_function fn;
    uint32 index = 0;
};

// A list of things to do at once: phase enter / exit.
class TSBossActions
{
public:
    TSBossActions* Cast(uint32 spell, sol::optional<TSBossSelector*> target);
    TSBossActions* Talk(uint8 group);
    TSBossActions* SetPassive();
    TSBossActions* SetAggressive();
    TSBossActions* TakeOff();
    TSBossActions* Land(TSPosition pos);
    TSBossActions* MoveTo(TSPosition pos);
    TSBossActions* Summon(uint32 entry, TSPosition pos);
    TSBossActions* Do(sol::protected_function fn);

    // Runs the steps from `from`. Stops after a Land step and returns the index
    // to resume at once she has touched down; returns DONE otherwise.
    std::size_t Run(Creature* me, std::size_t from = 0) const;
    static constexpr std::size_t DONE = std::size_t(-1);
    static constexpr uint32 LAND_POINT = 0x54534C44;    // movement id of Land()
private:
    std::vector<std::function<void(Creature*)>> _steps;
    std::vector<std::size_t> _waits;    // indexes of Land steps
};

// One timed thing in a phase: a spell, a summon or a bare callback.
class TSBossTimer
{
public:
    enum Kind { ABILITY, CALLBACK, SUMMON };
    explicit TSBossTimer(Kind kind) : kind(kind) {}

    TSBossTimer* Every(uint32 min, sol::optional<uint32> max) { everyMin = min; everyMax = std::max(min, max.value_or(min)); return this; }
    TSBossTimer* After(uint32 ms) { after = ms; hasAfter = true; return this; }
    TSBossTimer* Repeat(uint32 times) { repeat = times; return this; }
    // Each time it comes up it fires this many times in a row, this far apart.
    TSBossTimer* Burst(uint32 times, uint32 interval) { burst = times; burstInterval = interval; return this; }
    TSBossTimer* Target(TSBossSelector* selector) { target = selector; return this; }
    TSBossTimer* Do(sol::protected_function f) { fn = f; return this; }
    TSBossTimer* NoRepeat() { noRepeat = true; return this; }
    // After the cast finishes the boss stands still for this long: no turning,
    // no melee, no other ability. For spells whose animation outlasts the cast.
    TSBossTimer* Recover(uint32 ms) { recover = ms; return this; }
    // starts the wave each time this timer fires
    TSBossTimer* Triggers(TSBossObjectWave* value) { wave = value; return this; }
    TSBossObjectWave const* wave = nullptr;
    TSBossTimer* At(sol::table positions);
    TSBossTimer* Count(uint32 n) { count = n; return this; }
    TSBossTimer* ZoneInCombat() { zoneInCombat = true; return this; }

    Kind kind;
    uint32 id = 0;                  // EventMap id, 1-based index into TSBoss::timers
    std::vector<uint32> spells;     // ABILITY: one spell, or the Rotate() pool
    uint32 entry = 0;               // SUMMON
    uint32 everyMin = 0, everyMax = 0, after = 0, repeat = 0, count = 0, burst = 0, burstInterval = 0, recover = 0;
    bool hasAfter = false, noRepeat = false, zoneInCombat = false;
    TSBossSelector* target = nullptr;
    std::vector<Position> spots;
    sol::protected_function fn;
};

class TSBossPhase
{
public:
    TSBossPhase(TSBoss* boss, uint32 index) : index(index), _boss(boss) {}

    TSBossTimer* Ability(uint32 spell);
    TSBossTimer* Rotate(sol::variadic_args spells);
    TSBossTimer* Timer();
    TSBossTimer* Summon(uint32 entry);
    TSBossTrigger* When(TSBossCondition* condition);
    TSBossCondition* Elapsed(uint32 ms);
    TSBossActions* OnEnter() { return &enter; }
    TSBossActions* OnExit() { return &exit; }
    TSBossPhase* Inherit(TSBossPhase* other) { inherits.push_back(other); return this; }
    TSBossPhase* DelayAll(uint32 ms) { delayAll = ms; return this; }
    TSBossPhase* PauseWhileCasting() { pauseWhileCasting = true; return this; }
    // The boss cannot drop below 1 health while in this phase.
    TSBossPhase* Immortal() { immortal = true; return this; }

    // own timers plus those of every inherited phase
    void CollectTimers(std::vector<TSBossTimer*>& out) const;

    std::vector<TSBossTimer*> timers;
    std::vector<TSBossTrigger*> triggers;
    std::vector<TSBossPhase*> inherits;
    TSBossActions enter, exit;
    uint32 index;
    uint32 delayAll = 0;
    bool pauseWhileCasting = false;
    bool immortal = false;
private:
    TSBossTimer* AddTimer(TSBossTimer::Kind kind);
    TSBoss* _boss;
};

// Lethal damage knocks the boss down instead of killing it; it gets up again
// after a delay or when a script calls creature.ReviveFakeDeath().
class TSBossFakeDeath
{
public:
    TSBossFakeDeath* Times(uint32 n) { times = n; return this; }
    TSBossFakeDeath* ReviveAfter(uint32 ms) { reviveAfter = ms; return this; }
    TSBossFakeDeath* ReviveAt(float healthPct) { reviveAt = healthPct; return this; }
    TSBossFakeDeath* GoTo(TSBossPhase* phase) { goTo = phase; return this; }
    TSBossActions* OnFall() { return &fall; }
    TSBossActions* OnRevive() { return &revive; }

    bool enabled = false;
    uint32 times = 1;
    uint32 reviveAfter = 0;     // 0: only a script revives it
    float reviveAt = 100.f;
    TSBossPhase* goTo = nullptr;
    TSBossActions fall, revive;
};

// Several bosses fought as one encounter. Members must be within 300 yards.
class TSBossLink
{
public:
    TSBossLink* SharedHealth() { sharedHealth = true; return this; }
    TSBossLink* Handoff(uint32 min, sol::optional<uint32> max) { handoffMin = min; handoffMax = std::max(min, max.value_or(min)); return this; }
    TSBossLink* AllDieWithin(uint32 ms) { allDieWithin = ms; return this; }
    TSBossLink* LootFromLastOnly() { lootFromLastOnly = true; return this; }
    TSBossActions* OnHandoff() { return &handoff; }
    TSBossActions* OnMemberDeath() { return &memberDeath; }

    std::vector<uint32> entries;
    bool sharedHealth = false, lootFromLastOnly = false;
    uint32 handoffMin = 0, handoffMax = 0, allDieWithin = 0;
    TSBossActions handoff, memberDeath;
};

// A wave through gameobjects placed in the room (Onyxia's lava fissures, floor
// tiles, braziers). Every matching object within StartWithin() of the boss goes
// off first; then every Step() it moves on to the matching objects within Reach()
// of the ones that just went off, until none are left. Each object plays its
// animation and, if it is a trap, casts its trap spell with the boss as the
// original caster.
class TSBossObjectWave
{
public:
    TSBossObjectWave* Entries(sol::variadic_args ids) { for (auto const& id : ids) entries.push_back(id.as<uint32>()); return this; }
    TSBossObjectWave* Named(std::string value) { name = std::move(value); return this; }
    TSBossObjectWave* TrapSpell(uint32 spell) { trapSpell = spell; return this; }
    TSBossObjectWave* StartWithin(float yards) { startRange = yards; return this; }
    TSBossObjectWave* Reach(float yards) { reach = yards; return this; }
    TSBossObjectWave* Delay(uint32 ms) { delay = ms; return this; }
    TSBossObjectWave* Step(uint32 ms) { step = ms; return this; }

    bool Matches(GameObject const* go) const;

    std::vector<uint32> entries;
    std::string name;
    uint32 trapSpell = 0, delay = 0, step = 1000;
    float startRange = 15.f, reach = 15.f;
};

class TSBoss
{
public:
    explicit TSBoss(uint32 entry) : always(this, 0), entry(entry) {}

    TSBossPhase* Phase(uint32 index);
    TSBossSelector* Self() { return NewSelector(TSBossSelector::SELF); }
    TSBossSelector* GetEnemies() { return NewSelector(TSBossSelector::ENEMIES); }
    TSBossSelector* GetRaid() { return NewSelector(TSBossSelector::RAID); }
    TSBossSelector* GetFriendlyCreatures() { return NewSelector(TSBossSelector::FRIENDLY_CREATURES); }
    TSBossCondition* Health() { return NewCondition(TSBossCondition::HEALTH_BELOW); }
    TSBossCondition* Power() { return NewCondition(TSBossCondition::POWER_REACHES); }
    TSBossCondition* Stacks(uint32 aura) { return NewCondition(TSBossCondition::STACKS_REACH, aura); }
    TSBossCondition* Adds() { return NewCondition(TSBossCondition::ADDS_DEAD); }
    TSBoss* Berserk(uint32 ms, uint32 spell);
    // Auras the fight puts on players (markers, debuffs): stripped from everyone when it ends.
    TSBoss* RemoveOnReset(sol::variadic_args auras);
    std::vector<uint32> resetAuras;
    TSBossObjectWave* ObjectWave() { _waves.push_back(std::make_unique<TSBossObjectWave>()); return _waves.back().get(); }
    std::vector<std::unique_ptr<TSBossObjectWave>> _waves;
    TSBossFakeDeath* FakeDeath() { fakeDeath.enabled = true; return &fakeDeath; }

    TSBossSelector* NewSelector(TSBossSelector::Source source);
    TSBossCondition* NewCondition(TSBossCondition::Type subject, uint32 aura = 0);

    TSBossPhase always;             // runs in every phase
    TSBossFakeDeath fakeDeath;
    TSBossLink* link = nullptr;
    std::map<uint32, std::unique_ptr<TSBossPhase>> phases;
    std::vector<std::unique_ptr<TSBossTimer>> timers;
    std::vector<std::unique_ptr<TSBossTrigger>> triggers;
    uint32 entry;
private:
    std::vector<std::unique_ptr<TSBossSelector>> _selectors;
    std::vector<std::unique_ptr<TSBossCondition>> _conditions;
};

namespace TSBosses
{
    TSBoss* Create(uint32 entry);
    TSBossLink* Link(sol::variadic_args bosses);
    TSBoss const* Find(uint32 entry);
    // Bumped by Clear(); a runtime holding an older number must drop its TSBoss.
    uint32 Generation();
    // Must run before the Lua state is replaced: the builders hold Lua functions.
    void Clear();
    void LoadLua(sol::state& state);
}

// Per-creature state of a running encounter. Owned by TSScriptedAI.
class TSBossRuntime
{
public:
    void Engage(Creature* me);
    void Update(Creature* me, uint32 diff);
    // Ends the encounter state. Pass the creature to also undo a fake death.
    void Reset(Creature* me = nullptr);
    bool Active() const { return _boss != nullptr; }
    void NoteSummon() { _everSummoned = true; }

    void DamageTaken(Creature* me, Unit* attacker, uint32& damage);
    void HealReceived(Creature* me, uint32& heal);
    void Died(Creature* me);
    // Call after the creature itself has evaded.
    static void LinkEvaded(TSBossLink const* link, Creature* me);
    TSBossLink const* Link() const { return _boss ? _boss->link : nullptr; }

    void MovementInform(Creature* me, uint32 type, uint32 id);

    // damage this unit (or its owner's pets) has done to the boss this fight
    uint64 DamageBy(ObjectGuid guid) const;
    uint64 HealingBy(ObjectGuid guid) const;
    // credits effective healing to every framework boss the healer or the target is fighting
    static void HealingDone(Unit* healer, Unit* target, uint32 gain);
    // talent tree (0-2) the player has most points in, remembered for the fight
    uint8 TalentTabOf(Player* player);
    // NotLastTarget(): who each selector picked last
    ObjectGuid& LastPick(TSBossSelector const* selector) { return _lastPicks[selector]; }
    // NoRepeats(): everyone each selector has picked in the current round
    std::unordered_set<ObjectGuid>& Picked(TSBossSelector const* selector) { return _picked[selector]; }

    bool Holding() const { return _holdTotal != 0; }
    bool Down() const { return _down; }
    void UpdateDown(Creature* me, uint32 diff);
    void Revive(Creature* me);
private:
    void Fall(Creature* me);
    void EndHold(Creature* me);
    void RunEnter(Creature* me, std::size_t from);
    void LinkTick(Creature* me);
    bool PhaseTriggerPending() const;

    void SetPhase(Creature* me, TSBossPhase const* phase);
    void Schedule(TSBossTimer const* timer);
    void Fire(Creature* me, TSBossTimer const* timer);
    bool Met(Creature* me, TSBossTrigger const* trigger);
    bool CheckTriggers(Creature* me, TSBossPhase const* phase);

    struct TimerState { uint32 fires = 0; int32 last = -1; uint32 burstLeft = 0; };
    // an object wave in progress: the ring about to go off and everything already done
    struct WaveState { TSBossObjectWave const* wave; std::vector<ObjectGuid> ring; std::unordered_set<ObjectGuid> done; uint32 timer; };
    std::vector<WaveState> _objectWaves;
    void StartWave(Creature* me, TSBossObjectWave const* wave);
    void UpdateWaves(Creature* me, uint32 diff);

    TSBoss const* _boss = nullptr;
    TSBossPhase const* _phase = nullptr;
    uint32 _generation = 0;
    uint32 _phaseTime = 0;
    bool _everSummoned = false;
    bool _down = false;
    uint32 _downTime = 0;
    uint32 _falls = 0;
    uint32 _linkTimer = 0;
    // Recover(): cast time + recovery of the ability in progress; rooted and
    // unable to turn from the end of the cast until the total has passed
    uint32 _holdTotal = 0, _holdCast = 0, _holdElapsed = 0;
    bool _holdLocked = false;
    // phase-enter steps still waiting for a landing; abilities hold until then
    std::size_t _enterStep = TSBossActions::DONE;
    uint32 _enterWait = 0;
    EventMap _events;
    std::vector<TimerState> _timers;
    std::unordered_map<ObjectGuid, uint64> _damage;
    std::unordered_map<ObjectGuid, uint64> _healing;
    std::unordered_map<ObjectGuid, uint8> _talentTabs;
    std::unordered_map<TSBossSelector const*, ObjectGuid> _lastPicks;
    std::unordered_map<TSBossSelector const*, std::unordered_set<ObjectGuid>> _picked;

    std::vector<uint32> _triggerFired;
};
// @tswow-end
