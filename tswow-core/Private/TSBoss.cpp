#include <set>
// @tswow-begin
#include "TSBoss.h"
#include "TSCreature.h"
#include "TSUnit.h"
#include "TSPosition.h"
#include "TSScriptedAI.h"

#include "Containers.h"
#include "Creature.h"
#include "GridNotifiersImpl.h"
#include "GridNotifiers.h"
#include "GameObject.h"
#include "CellImpl.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Map.h"
#include "Player.h"
#include "CreatureAI.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "ObjectAccessor.h"
#include "Random.h"
#include "SpellAuras.h"
#include <cstdio>
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TemporarySummon.h"
#include "ThreatManager.h"

#include <algorithm>
#include <list>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

static Position ToPosition(TSPosition const& pos)
{
    return Position(pos.x, pos.y, pos.z, pos.o);
}

template <typename... Args>
static void CallLua(sol::protected_function const& fn, Args&&... args)
{
    if (!fn.valid())
        return;
    TSWOW_LUA_GUARD
    TSLua::handle_error(fn(std::forward<Args>(args)...));
}

// ---------------------------------------------------------------- selectors

TSBossSelector* TSBossSelector::Players()
{
    _filters.push_back([](Creature*, Unit* u) { return u->GetTypeId() == TYPEID_PLAYER; });
    return this;
}

TSBossSelector* TSBossSelector::Ranged()
{
    _filters.push_back([](Creature* me, Unit* u) { return !me->IsWithinMeleeRange(u); });
    return this;
}

TSBossSelector* TSBossSelector::Melee()
{
    _filters.push_back([](Creature* me, Unit* u) { return me->IsWithinMeleeRange(u); });
    return this;
}

TSBossSelector* TSBossSelector::With(uint32 aura)
{
    _filters.push_back([aura](Creature*, Unit* u) { return u->HasAura(aura); });
    return this;
}

TSBossSelector* TSBossSelector::Without(uint32 aura)
{
    _filters.push_back([aura](Creature*, Unit* u) { return !u->HasAura(aura); });
    return this;
}

TSBossSelector* TSBossSelector::OfClass(sol::variadic_args classes)
{
    std::unordered_set<uint32> set;
    for (auto const& cls : classes)
        set.insert(cls.as<uint32>());
    _filters.push_back([set](Creature*, Unit* u) { return set.count(u->GetClass()) != 0; });
    return this;
}

TSBossSelector* TSBossSelector::Between(float minDist, float maxDist)
{
    _filters.push_back([minDist, maxDist](Creature* me, Unit* u)
    {
        float dist = me->GetDistance(u);
        return dist >= minDist && dist <= maxDist;
    });
    return this;
}

TSBossSelector* TSBossSelector::Where(sol::protected_function fn)
{
    _filters.push_back([fn](Creature* me, Unit* u)
    {
        TSWOW_LUA_GUARD
        sol::protected_function_result res = fn(TSCreature(me), TSUnit(u));
        TSLua::handle_error(res);
        return res.valid() && res.get_type() == sol::type::boolean && res.get<bool>();
    });
    return this;
}

static std::unordered_set<uint32> ToSet(sol::variadic_args const& args)
{
    std::unordered_set<uint32> set;
    for (auto const& arg : args)
        set.insert(arg.as<uint32>());
    return set;
}

static float ToArc(sol::optional<float> const& degrees)
{
    return degrees.value_or(180.f) * float(M_PI) / 180.f;
}

TSBossSelector* TSBossSelector::OfRace(sol::variadic_args races)
{
    return Add([set = ToSet(races)](Creature*, Unit* u) { return set.count(u->GetRace()) != 0; });
}
TSBossSelector* TSBossSelector::OfEntry(sol::variadic_args entries)
{
    return Add([set = ToSet(entries)](Creature*, Unit* u) { return set.count(u->GetEntry()) != 0; });
}
TSBossSelector* TSBossSelector::OfGender(uint32 gender) { return Add([gender](Creature*, Unit* u) { return uint32(u->GetNativeGender()) == gender; }); }
TSBossSelector* TSBossSelector::ManaUsers() { return WithPowerType(POWER_MANA); }
TSBossSelector* TSBossSelector::WithPowerType(uint32 type) { return Add([type](Creature*, Unit* u) { return uint32(u->GetPowerType()) == type; }); }
TSBossSelector* TSBossSelector::InFront(sol::optional<float> arc) { return Add([a = ToArc(arc)](Creature* me, Unit* u) { return me->isInFront(u, a); }); }
TSBossSelector* TSBossSelector::Behind(sol::optional<float> arc) { return Add([a = ToArc(arc)](Creature* me, Unit* u) { return me->isInBack(u, a); }); }
TSBossSelector* TSBossSelector::InLineOfSight() { return Add([](Creature* me, Unit* u) { return me->IsWithinLOSInMap(u); }); }
TSBossSelector* TSBossSelector::Within(float dist) { return Add([dist](Creature* me, Unit* u) { return me->GetDistance(u) <= dist; }); }
TSBossSelector* TSBossSelector::Beyond(float dist) { return Add([dist](Creature* me, Unit* u) { return me->GetDistance(u) > dist; }); }
TSBossSelector* TSBossSelector::AboveZ(float z) { return Add([z](Creature*, Unit* u) { return u->GetPositionZ() > z; }); }
TSBossSelector* TSBossSelector::BelowZ(float z) { return Add([z](Creature*, Unit* u) { return u->GetPositionZ() < z; }); }
TSBossSelector* TSBossSelector::Moving() { return Add([](Creature*, Unit* u) { return u->isMoving(); }); }
TSBossSelector* TSBossSelector::Standing() { return Add([](Creature*, Unit* u) { return !u->isMoving(); }); }
TSBossSelector* TSBossSelector::Casting() { return Add([](Creature*, Unit* u) { return u->IsNonMeleeSpellCast(false, false, true); }); }
TSBossSelector* TSBossSelector::InWater() { return Add([](Creature*, Unit* u) { return u->IsInWater(); }); }
TSBossSelector* TSBossSelector::Mounted() { return Add([](Creature*, Unit* u) { return u->IsMounted(); }); }
TSBossSelector* TSBossSelector::HealthBelow(float pct) { return Add([pct](Creature*, Unit* u) { return u->GetHealthPct() < pct; }); }
TSBossSelector* TSBossSelector::HealthAbove(float pct) { return Add([pct](Creature*, Unit* u) { return u->GetHealthPct() > pct; }); }
TSBossSelector* TSBossSelector::ExcludePets() { return Add([](Creature*, Unit* u) { return u->GetTypeId() == TYPEID_PLAYER || !u->IsControlledByPlayer(); }); }
TSBossSelector* TSBossSelector::InSubGroup(uint32 group)
{
    // raid groups as the players number them: 1 to 8
    return Add([group](Creature*, Unit* u) { return u->GetTypeId() == TYPEID_PLAYER && u->ToPlayer()->GetGroup() && uint32(u->ToPlayer()->GetSubGroup()) + 1 == group; });
}

TSBossSelector* TSBossSelector::FacingBoss() { return Add([](Creature* me, Unit* u) { return u->HasInArc(float(M_PI), me); }); }
TSBossSelector* TSBossSelector::FacingAway() { return Add([](Creature* me, Unit* u) { return !u->HasInArc(float(M_PI), me); }); }
// the boss's own left and right: angles turn counter-clockwise, so 0 to pi is its left
TSBossSelector* TSBossSelector::OnLeft() { return Add([](Creature* me, Unit* u) { return Position::NormalizeOrientation(me->GetRelativeAngle(u)) < float(M_PI); }); }
TSBossSelector* TSBossSelector::OnRight() { return Add([](Creature* me, Unit* u) { return Position::NormalizeOrientation(me->GetRelativeAngle(u)) >= float(M_PI); }); }
TSBossSelector* TSBossSelector::Near(TSPosition pos, float radius)
{
    return Add([p = ToPosition(pos), radius](Creature*, Unit* u) { return u->GetExactDist(p) <= radius; });
}
TSBossSelector* TSBossSelector::SortByDistanceTo(TSPosition pos)
{
    _customKey = [p = ToPosition(pos)](Creature*, Unit* u) { return double(u->GetExactDist(p)); };
    return this;
}
TSBossSelector* TSBossSelector::SortByAuraTime(uint32 aura)
{
    // without the aura sorts last
    _customKey = [aura](Creature*, Unit* u) { Aura* a = u->GetAura(aura); return a ? double(a->GetDuration()) : 1e12; };
    return this;
}
TSBossSelector* TSBossSelector::SortByStacks(uint32 aura)
{
    _customKey = [aura](Creature*, Unit* u) { return double(u->GetAuraCount(aura)); };
    return this;
}

// ponytail: roles are guessed from class and the talent tree with the most
// points, plus bear form / frost presence for the hybrid tanks. Good enough for
// stock-shaped trees; give this a per-server override if specs stop mapping.
static TSBossSelector::Role RoleOf(Creature* me, Player* player)
{
    using R = TSBossSelector::Role;
    TSScriptedAI* ai = dynamic_cast<TSScriptedAI*>(me->AI());
    uint8 tab = ai ? ai->Boss().TalentTabOf(player) : 0;
    switch (player->GetClass())
    {
        case CLASS_WARRIOR: return tab == 2 ? R::TANK : R::MELEE_DPS;
        case CLASS_PALADIN: return tab == 0 ? R::HEALER : tab == 1 ? R::TANK : R::MELEE_DPS;
        case CLASS_PRIEST:  return tab == 2 ? R::RANGED_DPS : R::HEALER;
        case CLASS_SHAMAN:  return tab == 0 ? R::RANGED_DPS : tab == 1 ? R::MELEE_DPS : R::HEALER;
        case CLASS_DEATH_KNIGHT: return player->HasAura(48263) ? R::TANK : R::MELEE_DPS;  // Frost Presence
        case CLASS_DRUID:
            if (tab == 0) return R::RANGED_DPS;
            if (tab == 2) return R::HEALER;
            return player->GetShapeshiftForm() == FORM_BEAR || player->GetShapeshiftForm() == FORM_DIREBEAR ? R::TANK : R::MELEE_DPS;
        case CLASS_ROGUE: return R::MELEE_DPS;
        default: return R::RANGED_DPS;  // hunter, mage, warlock
    }
}

TSBossSelector* TSBossSelector::AddRole(Role role)
{
    _filters.push_back([role](Creature* me, Unit* u) { return u->GetTypeId() == TYPEID_PLAYER && RoleOf(me, u->ToPlayer()) == role; });
    return this;
}

TSBossSelector* TSBossSelector::Supports()
{
    return Add([](Creature* me, Unit* u) { return u->GetTypeId() == TYPEID_PLAYER && RoleOf(me, u->ToPlayer()) <= HEALER; });
}
TSBossSelector* TSBossSelector::Dps()
{
    return Add([](Creature* me, Unit* u) { return u->GetTypeId() == TYPEID_PLAYER && RoleOf(me, u->ToPlayer()) >= MELEE_DPS; });
}
TSBossSelector* TSBossSelector::Healers() { return AddRole(HEALER); }
TSBossSelector* TSBossSelector::MeleeDps() { return AddRole(MELEE_DPS); }
TSBossSelector* TSBossSelector::RangedDps() { return AddRole(RANGED_DPS); }

TSBossSelector* TSBossSelector::MainTank()
{
    _filters.push_back([](Creature* me, Unit* u) { return u == me->GetVictim(); });
    return this;
}

TSBossSelector* TSBossSelector::OffTanks()
{
    _filters.push_back([](Creature* me, Unit* u) { return u != me->GetVictim() && u->GetTypeId() == TYPEID_PLAYER && RoleOf(me, u->ToPlayer()) == TANK; });
    return this;
}

TSBossSelector* TSBossSelector::Tanks()
{
    _filters.push_back([](Creature* me, Unit* u) { return u == me->GetVictim() || (u->GetTypeId() == TYPEID_PLAYER && RoleOf(me, u->ToPlayer()) == TANK); });
    return this;
}

std::vector<Unit*> TSBossSelector::Resolve(Creature* me) const
{
    if (_source == SELF)
        return { me };

    std::vector<Unit*> all;
    bool dungeon = me->GetMap()->IsDungeon();
    TSScriptedAI* ai = dynamic_cast<TSScriptedAI*>(me->AI());
    if (_source == FRIENDLY_CREATURES)
    {
        std::vector<Creature*> creatures;
        me->GetCreatureListWithEntryInGrid(creatures, 0, 100.f);
        for (Creature* c : creatures)
            if (c != me && c->IsAlive() && !c->IsTrigger() && !c->IsCritter() && !c->IsControlledByPlayer()
                && !me->IsHostileTo(c) && !c->IsHostileTo(me))
                all.push_back(c);
    }
    else if (_source == RAID && (dungeon || _dead))
    {
        // everyone alive in the instance, whether or not they have threat yet
        // (or, with Dead(), the corpses; outside instances those within 300 yards)
        for (MapReference const& ref : me->GetMap()->GetPlayers())
            if (Player* player = ref.GetSource())
                if (player->IsAlive() != _dead && !player->IsGameMaster() && (dungeon || me->IsWithinDistInMap(player, 300.f)))
                    all.push_back(player);
    }
    else
    {
        for (ThreatReference const* ref : me->GetThreatManager().GetUnsortedThreatList())
        {
            Unit* unit = ref->GetVictim();
            if (!ref->IsAvailable())
                continue;
            if (_source == RAID && unit->GetTypeId() != TYPEID_PLAYER)
                continue;
            // Never the boss's own adds, or anything else that merely ended up on
            // the threat list without being an enemy.
            if (TempSummon* summon = unit->ToTempSummon())
                if (summon->GetSummonerGUID() == me->GetGUID())
                    continue;
            if (unit->GetTypeId() != TYPEID_PLAYER && !me->IsHostileTo(unit) && !unit->IsHostileTo(me))
                continue;
            all.push_back(unit);
        }
    }
    if (all.empty())
        return all;

    Unit* tank = me->GetVictim();
    auto filter = [&](bool excludeTank)
    {
        std::vector<Unit*> out;
        for (Unit* u : all)
        {
            if (excludeTank && u == tank)
                continue;
            if (std::all_of(_filters.begin(), _filters.end(), [&](auto const& f) { return f(me, u); }))
                out.push_back(u);
        }
        return out;
    };

    std::vector<Unit*> units = filter(_excludeTank);
    // Nobody but the tank left (solo, or the raid is dead): hit the tank rather than nothing.
    if (units.empty() && _excludeTank)
        units = filter(false);
    if (units.empty())
        return units;

    if (_noRepeats && ai)
    {
        auto& picked = ai->Boss().Picked(this);
        std::vector<Unit*> fresh;
        for (Unit* u : units)
            if (!picked.count(u->GetGUID()))
                fresh.push_back(u);
        if (fresh.empty())
            picked.clear();     // everyone has had a turn: start the next round
        else
            units = fresh;
    }
    if (_notLast && ai && units.size() > 1)
    {
        ObjectGuid last = ai->Boss().LastPick(this);
        units.erase(std::remove_if(units.begin(), units.end(), [&](Unit* u) { return u->GetGUID() == last; }), units.end());
    }
    auto remember = [&](std::vector<Unit*> const& picked)
    {
        if (!ai || picked.empty())
            return picked;
        if (_notLast)
            ai->Boss().LastPick(this) = picked.front()->GetGUID();
        if (_noRepeats)
            for (Unit* u : picked)
                ai->Boss().Picked(this).insert(u->GetGUID());
        return picked;
    };

    if (_pick == ALL)
        return remember(units);
    if (_pick == EACH_ROLE)
    {
        // tank, healer, damage: `_count` random players from each
        std::vector<Unit*> byRole[3], out;
        for (Unit* u : units)
            if (u->GetTypeId() == TYPEID_PLAYER)
                byRole[std::min<int>(RoleOf(me, u->ToPlayer()), MELEE_DPS)].push_back(u);
        for (auto& group : byRole)
        {
            Trinity::Containers::RandomResize(group, _count);
            out.insert(out.end(), group.begin(), group.end());
        }
        return remember(out);
    }
    if (_pick == RANDOM)
    {
        Trinity::Containers::RandomResize(units, _count);
        return remember(units);
    }

    auto key = [&](Unit* u) -> double
    {
        if (_customKey)
            return _customKey(me, u);
        switch (_sort)
        {
            case NEARBY_ALLIES:
                return double(std::count_if(all.begin(), all.end(), [&](Unit* other) { return other != u && u->GetDistance(other) <= 8.f; }));
            case DISTANCE:   return me->GetDistance(u);
            case THREAT:     return me->GetThreatManager().GetThreat(u);
            case HEALTH:     return u->GetHealth();
            case HEALTH_PCT: return u->GetHealthPct();
            case DPS:        return ai ? double(ai->Boss().DamageBy(u->GetGUID())) : 0.0;
            case HEALING:    return ai ? double(ai->Boss().HealingBy(u->GetGUID())) : 0.0;
            case MAX_HEALTH: return u->GetMaxHealth();
            case POWER:      return u->GetPower(u->GetPowerType());
            case POWER_PCT:  return u->GetMaxPower(u->GetPowerType()) ? 100.0 * u->GetPower(u->GetPowerType()) / u->GetMaxPower(u->GetPowerType()) : 0.0;
            case LEVEL:      return u->GetLevel();
            case ITEM_LEVEL: return u->GetTypeId() == TYPEID_PLAYER ? u->ToPlayer()->GetAverageItemLevel() : 0.0;
            case SIZE:       return u->GetObjectScale();
            case TIME_PLAYED: return u->GetTypeId() == TYPEID_PLAYER ? u->ToPlayer()->GetTotalPlayedTime() : 0.0;
            case ARMOR:      return u->GetArmor();
            default:         return 0.0;
        }
    };
    if (_sort != NONE || _customKey)
        std::stable_sort(units.begin(), units.end(), [&](Unit* a, Unit* b) { return key(a) < key(b); });
    if (_reverse)
        std::reverse(units.begin(), units.end());

    uint32 count = std::min<uint32>(std::max<uint32>(_count, 1), units.size());
    switch (_pick)
    {
        case FIRST: units.resize(count); break;
        case LAST:  units.erase(units.begin(), units.end() - count); break;
        case NTH:   units = { units[count - 1] }; break;    // past the end: the last one
        default: break;
    }
    return remember(units);
}

// --------------------------------------------------------------- conditions

TSBossCondition* TSBossCondition::Set(Type expected, Type result, float v)
{
    if (_subject != expected)
        TS_LOG_ERROR("tswow.boss", "Boss condition verb used on the wrong subject (subject {}, verb for {})", uint32(_subject), uint32(expected));
    else
    {
        type = result;
        value = v;
    }
    return this;
}

TSBossCondition* TSBossCondition::Below(float pct) { return Set(HEALTH_BELOW, HEALTH_BELOW, pct); }
TSBossCondition* TSBossCondition::Every(float pct) { return Set(HEALTH_BELOW, HEALTH_EVERY, pct); }
TSBossCondition* TSBossCondition::Reaches(float amount) { return Set(POWER_REACHES, POWER_REACHES, amount); }
TSBossCondition* TSBossCondition::Reach(float stacks) { return Set(STACKS_REACH, STACKS_REACH, stacks); }
TSBossCondition* TSBossCondition::AllDead() { return Set(ADDS_DEAD, ADDS_DEAD, 0); }

// ------------------------------------------------------------------ actions

TSBossActions* TSBossActions::Cast(uint32 spell, sol::optional<TSBossSelector*> target)
{
    TSBossSelector* selector = target.value_or(nullptr);
    _steps.push_back([spell, selector](Creature* me)
    {
        if (!selector)
            me->CastSpell(me, spell, false);
        else
            for (Unit* unit : selector->Resolve(me))
                me->CastSpell(unit, spell, false);
    });
    return this;
}

TSBossActions* TSBossActions::Talk(uint8 group)
{
    _steps.push_back([group](Creature* me) { me->AI()->Talk(group); });
    return this;
}

TSBossActions* TSBossActions::SetPassive()
{
    _steps.push_back([](Creature* me)
    {
        me->SetReactState(REACT_PASSIVE);
        me->AttackStop();
        me->GetMotionMaster()->Clear();
    });
    return this;
}

TSBossActions* TSBossActions::SetAggressive()
{
    _steps.push_back([](Creature* me) { me->SetReactState(REACT_AGGRESSIVE); });
    return this;
}

TSBossActions* TSBossActions::TakeOff()
{
    _steps.push_back([](Creature* me)
    {
        me->SetCanFly(true);
        me->SetDisableGravity(true);
    });
    return this;
}

TSBossActions* TSBossActions::Land(TSPosition pos)
{
    Position dest = ToPosition(pos);
    _waits.push_back(_steps.size());
    // Flight is switched off on arrival (TSBossRuntime::MovementInform), not here,
    // or she would drop out of the air instead of flying down.
    _steps.push_back([dest](Creature* me) { me->GetMotionMaster()->MoveLand(LAND_POINT, dest); });
    return this;
}

TSBossActions* TSBossActions::MoveTo(TSPosition pos)
{
    Position dest = ToPosition(pos);
    // ends the move facing the position's orientation
    _steps.push_back([dest](Creature* me) { me->GetMotionMaster()->MovePoint(0, dest, true, dest.GetOrientation()); });
    return this;
}

TSBossActions* TSBossActions::Summon(uint32 entry, TSPosition pos)
{
    Position dest = ToPosition(pos);
    _steps.push_back([entry, dest](Creature* me) { me->SummonCreature(entry, dest, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 30s); });
    return this;
}

TSBossActions* TSBossActions::Do(sol::protected_function fn)
{
    _steps.push_back([fn](Creature* me) { CallLua(fn, TSCreature(me)); });
    return this;
}

std::size_t TSBossActions::Run(Creature* me, std::size_t from) const
{
    for (std::size_t i = from; i < _steps.size(); ++i)
    {
        _steps[i](me);
        if (std::find(_waits.begin(), _waits.end(), i) != _waits.end())
            return i + 1;
    }
    return DONE;
}

// ----------------------------------------------------------- timers, phases

TSBossTimer* TSBossTimer::At(sol::table positions)
{
    for (auto const& kv : positions)
        spots.push_back(ToPosition(kv.second.as<TSPosition>()));
    return this;
}

TSBossTimer* TSBossPhase::AddTimer(TSBossTimer::Kind kind)
{
    _boss->timers.push_back(std::make_unique<TSBossTimer>(kind));
    TSBossTimer* timer = _boss->timers.back().get();
    timer->id = _boss->timers.size();
    timers.push_back(timer);
    return timer;
}

TSBossTimer* TSBossPhase::Ability(uint32 spell)
{
    TSBossTimer* timer = AddTimer(TSBossTimer::ABILITY);
    timer->spells.push_back(spell);
    return timer;
}

TSBossTimer* TSBossPhase::Rotate(sol::variadic_args spells)
{
    TSBossTimer* timer = AddTimer(TSBossTimer::ABILITY);
    for (auto const& spell : spells)
        timer->spells.push_back(spell.as<uint32>());
    return timer;
}

TSBossTimer* TSBossPhase::Timer() { return AddTimer(TSBossTimer::CALLBACK); }

TSBossTimer* TSBossPhase::Summon(uint32 entry)
{
    TSBossTimer* timer = AddTimer(TSBossTimer::SUMMON);
    timer->entry = entry;
    return timer;
}

TSBossTrigger* TSBossPhase::When(TSBossCondition* condition)
{
    _boss->triggers.push_back(std::make_unique<TSBossTrigger>(condition));
    TSBossTrigger* trigger = _boss->triggers.back().get();
    trigger->index = _boss->triggers.size() - 1;
    triggers.push_back(trigger);
    return trigger;
}

TSBossCondition* TSBossPhase::Elapsed(uint32 ms)
{
    TSBossCondition* cond = _boss->NewCondition(TSBossCondition::ELAPSED);
    cond->type = TSBossCondition::ELAPSED;
    cond->value = float(ms);
    return cond;
}

void TSBossPhase::CollectTimers(std::vector<TSBossTimer*>& out) const
{
    for (TSBossPhase* other : inherits)
        if (other != this)
            other->CollectTimers(out);
    out.insert(out.end(), timers.begin(), timers.end());
}

// --------------------------------------------------------------------- boss

TSBossPhase* TSBoss::Phase(uint32 index)
{
    if (index == 0)
        return &always;
    auto& phase = phases[index];
    if (!phase)
        phase = std::make_unique<TSBossPhase>(this, index);
    return phase.get();
}

TSBoss* TSBoss::Berserk(uint32 ms, uint32 spell)
{
    always.Ability(spell)->After(ms)->Target(Self());
    return this;
}

TSBoss* TSBoss::RemoveOnReset(sol::variadic_args auras)
{
    for (auto const& aura : auras)
        resetAuras.push_back(aura.as<uint32>());
    return this;
}

TSBossSelector* TSBoss::NewSelector(TSBossSelector::Source source)
{
    _selectors.push_back(std::make_unique<TSBossSelector>(source));
    return _selectors.back().get();
}

TSBossCondition* TSBoss::NewCondition(TSBossCondition::Type subject, uint32 aura)
{
    _conditions.push_back(std::make_unique<TSBossCondition>(subject, aura));
    return _conditions.back().get();
}

// ----------------------------------------------------------------- registry

namespace
{
    // Leaked on purpose: the builders hold Lua references, which must not be
    // released by a static destructor after the Lua state is gone.
    auto& Registry()
    {
        static auto* registry = new std::unordered_map<uint32, std::unique_ptr<TSBoss>>();
        return *registry;
    }
    uint32 generation = 1;

    auto& Links()
    {
        static auto* links = new std::vector<std::unique_ptr<TSBossLink>>();
        return *links;
    }

    // What a linked group shares, per map instance. Only ever touched by the
    // thread that updates that map; the mutex just guards the container.
    struct LinkState
    {
        bool engaged = false;
        bool busy = false;          // inside a shared-health kill
        ObjectGuid active;          // Handoff: the only member that takes damage
        uint32 nextHandoff = 0;
        uint32 firstDeath = 0;
    };

    LinkState& StateOf(TSBossLink const* link, Creature* me)
    {
        static auto* states = new std::map<std::tuple<TSBossLink const*, uint32, uint32>, LinkState>();
        static std::mutex mutex;
        std::lock_guard<std::mutex> lock(mutex);
        return (*states)[std::make_tuple(link, me->GetMapId(), me->GetInstanceId())];
    }

    std::vector<Creature*> Members(TSBossLink const* link, Creature* me)
    {
        std::vector<Creature*> members;
        for (uint32 entry : link->entries)
            me->GetCreatureListWithEntryInGrid(members, entry, 300.f);
        return members;
    }

    void RestoreFromFakeDeath(Creature* me)
    {
        me->RemoveUnitFlag(UNIT_FLAG_UNINTERACTIBLE);
        me->SetStandState(UNIT_STAND_STATE_STAND);
        me->SetReactState(REACT_AGGRESSIVE);
    }
}

TSBossLink* TSBosses::Link(sol::variadic_args bosses)
{
    Links().push_back(std::make_unique<TSBossLink>());
    TSBossLink* link = Links().back().get();
    for (auto const& arg : bosses)
    {
        TSBoss* boss = arg.as<TSBoss*>();
        boss->link = link;
        link->entries.push_back(boss->entry);
    }
    return link;
}

TSBoss* TSBosses::Create(uint32 entry)
{
    auto& boss = Registry()[entry];
    if (boss)
        TS_LOG_ERROR("tswow.boss", "Bosses.Create called twice for creature {}; the later definition wins", entry);
    boss = std::make_unique<TSBoss>(entry);
    return boss.get();
}

TSBoss const* TSBosses::Find(uint32 entry)
{
    auto itr = Registry().find(entry);
    return itr != Registry().end() ? itr->second.get() : nullptr;
}

uint32 TSBosses::Generation() { return generation; }

// ponytail: no lock. Livescripts are only ever (re)loaded with the realm stopped
// or before maps tick; add a shared_mutex if live reload is ever supported.
void TSBosses::Clear()
{
    ++generation;
    Registry().clear();
    Links().clear();
}

// ------------------------------------------------------------------ object waves

bool TSBossObjectWave::Matches(GameObject const* go) const
{
    GameObjectTemplate const* info = go->GetGOInfo();
    if (!entries.empty() && std::find(entries.begin(), entries.end(), go->GetEntry()) == entries.end())
        return false;
    if (!name.empty() && info->name != name)
        return false;
    if (trapSpell && (info->type != GAMEOBJECT_TYPE_TRAP || info->trap.spellId != trapSpell))
        return false;
    return !entries.empty() || !name.empty() || trapSpell;    // no filter set: match nothing
}

static std::vector<GameObject*> WaveObjectsNear(WorldObject* center, float range, TSBossObjectWave const* wave)
{
    std::list<GameObject*> found;
    Trinity::GameObjectInRangeCheck check(center->GetPositionX(), center->GetPositionY(), center->GetPositionZ(), range);
    Trinity::GameObjectListSearcher<Trinity::GameObjectInRangeCheck> searcher(center, found, check);
    Cell::VisitGridObjects(center, searcher, range);
    std::vector<GameObject*> out;
    for (GameObject* go : found)
        if (wave->Matches(go))
            out.push_back(go);
    return out;
}

void TSBossRuntime::StartWave(Creature* me, TSBossObjectWave const* wave)
{
    // Everything around the boss goes first, so the wave spreads out from where it
    // stands instead of crawling away from one object.
    std::vector<ObjectGuid> first;
    for (GameObject* go : WaveObjectsNear(me, wave->startRange, wave))
        first.push_back(go->GetGUID());
    if (!first.empty())
        _objectWaves.push_back({ wave, std::move(first), {}, wave->delay });
}

void TSBossRuntime::UpdateWaves(Creature* me, uint32 diff)
{
    for (std::size_t i = 0; i < _objectWaves.size();)
    {
        WaveState& state = _objectWaves[i];
        if (state.timer > diff)
        {
            state.timer -= diff;
            ++i;
            continue;
        }

        std::vector<GameObject*> fired;
        for (ObjectGuid guid : state.ring)
            if (GameObject* go = me->GetMap()->GetGameObject(guid))
            {
                go->SendCustomAnim(go->GetGoAnimProgress());
                if (go->GetGOInfo()->type == GAMEOBJECT_TYPE_TRAP && go->GetGOInfo()->trap.spellId)
                {
                    CastSpellExtraArgs args;
                    args.OriginalCaster = me->GetGUID();
                    go->CastSpell(go, go->GetGOInfo()->trap.spellId, args);
                }
                fired.push_back(go);
            }
        state.done.insert(state.ring.begin(), state.ring.end());

        std::vector<ObjectGuid> next;
        for (GameObject* go : fired)
            for (GameObject* near : WaveObjectsNear(go, state.wave->reach, state.wave))
                if (!state.done.count(near->GetGUID()) && std::find(next.begin(), next.end(), near->GetGUID()) == next.end())
                    next.push_back(near->GetGUID());

        if (next.empty())
            _objectWaves.erase(_objectWaves.begin() + i);
        else
        {
            state.ring = std::move(next);
            state.timer = state.wave->step;
            ++i;
        }
    }
}

// ------------------------------------------------------------------ runtime

void TSBossRuntime::EndHold(Creature* me)
{
    if ((_holdLocked || _holdTotal) && me)
    {
        me->RemoveUnitFlag2(UNIT_FLAG2_CANNOT_TURN);
        me->ClearUnitState(UNIT_STATE_ROOT);
        // A "face this unit" move, not "face this angle": after a locked hold the
        // client stopped following the victim again by itself, so re-arm it.
        if (Unit* victim = me->GetVictim())
        {
            Movement::MoveSplineInit init(me);
            init.MoveTo(me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), false);
            init.SetFacing(victim);
            init.Launch();
        }
    }
    _holdLocked = false;
    _holdTotal = 0;
}

void TSBossRuntime::Reset(Creature* me)
{
    EndHold(me);
    if (_boss && me)
        for (uint32 aura : _boss->resetAuras)
            for (MapReference const& ref : me->GetMap()->GetPlayers())
                if (Player* player = ref.GetSource())
                    player->RemoveAurasDueToSpell(aura);
    if (_down && me && me->IsAlive())
    {
        RestoreFromFakeDeath(me);
        me->SetFullHealth();
    }
    _down = false;
    _falls = 0;
    _boss = nullptr;
    _phase = nullptr;
    _phaseTime = 0;
    _everSummoned = false;
    _damage.clear();
    _objectWaves.clear();
    _healing.clear();
    _talentTabs.clear();
    _lastPicks.clear();
    _picked.clear();
    _enterStep = TSBossActions::DONE;
    _events.Reset();
}

void TSBossRuntime::Engage(Creature* me)
{
    Reset();
    _boss = TSBosses::Find(me->GetEntry());
    if (!_boss)
        return;
    _generation = TSBosses::Generation();
    // A boss that spawns asleep or sitting (Onyxia) only stood up when certain hits
    // landed. Left asleep, the client plays no flying idle and no cast animations.
    me->SetStandState(UNIT_STAND_STATE_STAND);
    _timers.assign(_boss->timers.size(), TimerState());
    _triggerFired.assign(_boss->triggers.size(), 0);

    for (TSBossTimer* timer : _boss->always.timers)
        Schedule(timer);

    if (TSBossLink const* link = _boss->link)
    {
        LinkState& state = StateOf(link, me);
        if (!state.engaged)
        {
            state = LinkState();
            state.engaged = true;
            if (link->handoffMin)
            {
                state.active = me->GetGUID();
                state.nextHandoff = GameTime::GetGameTimeMS() + urand(link->handoffMin, link->handoffMax);
                link->handoff.Run(me);
            }
        }
        _linkTimer = 1000;  // pull the other members on the first tick
    }
    if (!_boss->phases.empty())
        SetPhase(me, _boss->phases.begin()->second.get());
}

void TSBossRuntime::Schedule(TSBossTimer const* timer)
{
    Milliseconds delay(timer->hasAfter ? timer->after : urand(timer->everyMin, timer->everyMax));
    _events.ScheduleEvent(timer->id, delay);
}

void TSBossRuntime::SetPhase(Creature* me, TSBossPhase const* phase)
{
    EndHold(me);    // a hold carried into the new phase would block its takeoff or move
    std::vector<TSBossTimer*> timers;
    if (_phase)
    {
        _phase->exit.Run(me);
        if (!_boss)
            return;
        _phase->CollectTimers(timers);
        for (TSBossTimer* timer : timers)
            _events.CancelEvent(timer->id);
    }

    _phase = phase;
    _phaseTime = 0;
    _enterStep = TSBossActions::DONE;
    for (TSBossTrigger* trigger : phase->triggers)
        if (trigger->cond->type != TSBossCondition::HEALTH_EVERY)
            _triggerFired[trigger->index] = 0;

    timers.clear();
    phase->CollectTimers(timers);
    for (TSBossTimer* timer : timers)
    {
        _timers[timer->id - 1] = TimerState();
        Schedule(timer);
    }
    RunEnter(me, 0);
}

void TSBossRuntime::RunEnter(Creature* me, std::size_t from)
{
    TSBossPhase const* phase = _phase;
    std::size_t next = phase->enter.Run(me, from);
    if (_phase == phase)    // a step may have reset or moved the fight on
    {
        _enterStep = next;
        _enterWait = 0;
    }
}

// She has touched down: back to walking, then the rest of the enter steps.
void TSBossRuntime::MovementInform(Creature* me, uint32 type, uint32 id)
{
    if (type != EFFECT_MOTION_TYPE || id != TSBossActions::LAND_POINT)
        return;
    me->SetCanFly(false);
    me->SetDisableGravity(false);
    if (_boss && _phase && _enterStep != TSBossActions::DONE)
        RunEnter(me, _enterStep);
}

bool TSBossRuntime::Met(Creature* me, TSBossTrigger const* trigger)
{
    TSBossCondition const* cond = trigger->cond;
    uint32& fired = _triggerFired[trigger->index];
    if (fired && cond->type != TSBossCondition::HEALTH_EVERY)
        return false;

    switch (cond->type)
    {
        case TSBossCondition::HEALTH_BELOW:
            return me->HealthBelowPct(int32(cond->value));
        case TSBossCondition::HEALTH_EVERY:
        {
            float next = 100.f - cond->value * float(fired + 1);
            return cond->value > 0 && next > 0 && me->GetHealthPct() <= next;
        }
        case TSBossCondition::POWER_REACHES:
            return float(me->GetPower(me->GetPowerType())) >= cond->value;
        case TSBossCondition::STACKS_REACH:
            return float(me->GetAuraCount(cond->aura)) >= cond->value;
        case TSBossCondition::ELAPSED:
            return float(_phaseTime) >= cond->value;
        case TSBossCondition::ADDS_DEAD:
        {
            if (!_everSummoned)
                return false;
            TSScriptedAI* ai = dynamic_cast<TSScriptedAI*>(me->AI());
            if (!ai)
                return false;
            for (ObjectGuid const& guid : ai->summons)
                if (Creature* summon = ObjectAccessor::GetCreature(*me, guid))
                    if (summon->IsAlive())
                        return false;
            return true;
        }
        default:
            return false;
    }
}

// true when the encounter state changed under us (phase switch or reset)
bool TSBossRuntime::CheckTriggers(Creature* me, TSBossPhase const* phase)
{
    for (TSBossTrigger* trigger : phase->triggers)
    {
        if (!Met(me, trigger))
            continue;
        ++_triggerFired[trigger->index];
        TSBossPhase const* before = _phase;
        CallLua(trigger->fn, TSCreature(me));
        if (!_boss || _phase != before)
            return true;
        if (trigger->goTo && trigger->goTo != _phase)
        {
            SetPhase(me, trigger->goTo);
            return true;
        }
    }
    return false;
}

void TSBossRuntime::Fire(Creature* me, TSBossTimer const* timer)
{
    TimerState& state = _timers[timer->id - 1];
    if (state.burstLeft)
        --state.burstLeft;
    else
    {
        ++state.fires;
        state.burstLeft = timer->burst ? timer->burst - 1 : 0;
    }

    std::vector<Unit*> targets;
    if (timer->target)
        targets = timer->target->Resolve(me);
    else if (Unit* victim = me->GetVictim())
        targets.push_back(victim);

    // A passive boss has no victim to face, so turn to whoever it is about to hit.
    // AzerothCore's Onyxia does the same before every Fireball in the air.
    if (timer->kind == TSBossTimer::ABILITY && !me->GetVictim() && !targets.empty() && targets[0] != me)
    {
        if (!me->IsFlying())
            me->SetFacingToObject(targets[0], false);
        // In the air a turn-on-the-spot move froze her animation every time it was
        // tried (idle hover and cast wind-up both stopped), so she is re-placed where
        // she already is with the new facing: a snap turn, but she keeps animating.
        else if (me->movespline->Finalized())
            me->NearTeleportTo(Position(me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), me->GetAbsoluteAngle(targets[0])));
    }

    switch (timer->kind)
    {
        case TSBossTimer::ABILITY:
        {
            uint32 pick = urand(0, timer->spells.size() - 1);
            if (timer->noRepeat && timer->spells.size() > 1 && int32(pick) == state.last)
                pick = (pick + 1) % timer->spells.size();
            state.last = pick;
            // ponytail: a boss can only really cast one spell at a time, so with
            // several targets the extra casts are triggered (instant, no cast bar).
            SpellInfo const* info = sSpellMgr->GetSpellInfo(timer->spells[pick]);
            uint32 castTime = info ? info->CalcCastTime() : 0;
            // A cone with a cast time is a telegraph: she aims once, when the cast
            // starts, and stays put until it lands so stepping out of it works.
            // Stock behaviour is to snap round to the target as the cast finishes,
            // silently, so the cone the client showed is not the one that hits.
            bool aimed = false;
            if (info && castTime && !me->IsFlying() && !targets.empty() && targets[0] != me)
                for (SpellEffectInfo const& effect : info->GetEffects())
                    if (effect.TargetA.GetSelectionCategory() == TARGET_SELECT_CATEGORY_CONE || effect.TargetB.GetSelectionCategory() == TARGET_SELECT_CATEGORY_CONE)
                        aimed = true;
            if (aimed)
            {
                me->SetFacingToObject(targets[0], true);
                me->SetInFront(targets[0]);
                me->SetUnitFlag2(UNIT_FLAG2_CANNOT_TURN);
            }
            for (std::size_t i = 0; i < targets.size(); ++i)
                me->CastSpell(targets[i], timer->spells[pick], i > 0);
            if ((aimed || timer->recover) && !targets.empty())
            {
                // aimed: the hold (root, no melee) starts on the next update instead of at cast end
                _holdCast = aimed ? 0 : castTime;
                _holdTotal = castTime + timer->recover;
                _holdElapsed = 0;
            }
            break;
        }
        case TSBossTimer::SUMMON:
        {
            std::vector<Position> spots = timer->spots;
            if (spots.empty())
                spots.assign(std::max<uint32>(timer->count, 1), me->GetPosition());
            else if (timer->count)
            {
                std::vector<Position> picked;
                for (uint32 i = 0; i < timer->count; ++i)
                    picked.push_back(Trinity::Containers::SelectRandomContainerElement(spots));
                spots = picked;
            }
            for (Position const& spot : spots)
                if (TempSummon* summon = me->SummonCreature(timer->entry, spot, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 30s))
                    if (timer->zoneInCombat)
                        me->AI()->DoZoneInCombat(summon);
            break;
        }
        case TSBossTimer::CALLBACK:
            break;
    }

    if (timer->wave)
        StartWave(me, timer->wave);
    if (timer->fn.valid())
    {
        if (timer->target)
            for (Unit* target : targets)
                CallLua(timer->fn, TSCreature(me), TSUnit(target));
        else
            CallLua(timer->fn, TSCreature(me));
    }

    // a callback may have evaded or killed the boss
    if (!_boss || (!_phase && !_boss->phases.empty()))
        return;

    uint32 delayAll = std::max(_boss->always.delayAll, _phase ? _phase->delayAll : 0);
    if (delayAll)
        _events.DelayEvents(Milliseconds(delayAll));
    if (state.burstLeft)
        _events.ScheduleEvent(timer->id, Milliseconds(timer->burstInterval));
    else if (timer->everyMin && (!timer->repeat || state.fires < timer->repeat))
        _events.ScheduleEvent(timer->id, Milliseconds(timer->everyMin), Milliseconds(timer->everyMax));
}

void TSBossRuntime::Update(Creature* me, uint32 diff)
{
    if (!_boss)
        return;
    if (_generation != TSBosses::Generation())
    {
        Reset();    // livescripts were reloaded mid-fight; the definition is gone
        return;
    }

    UpdateWaves(me, diff);

    if (_boss->link)
    {
        _linkTimer += diff;
        if (_linkTimer >= 1000)
        {
            _linkTimer = 0;
            LinkTick(me);
            if (!_boss)
                return;
        }
    }

    if (_holdTotal)
    {
        _holdElapsed += diff;
        if (!_holdLocked && _holdElapsed >= _holdCast)
        {
            _holdLocked = true;
            me->SetUnitFlag2(UNIT_FLAG2_CANNOT_TURN);
            // ponytail: server-side root only (chase stops, no root packet to the
            // client), so a real root aura ending mid-hold is not accounted for.
            // Bosses are immune; use SetControlled if that ever matters.
            me->StopMoving();
            me->AddUnitState(UNIT_STATE_ROOT);
        }
        if (_holdElapsed < _holdTotal)
            return;
        EndHold(me);
    }

    // Landing: nothing else happens until she is down. 15 seconds is a safety
    // net in case the landing movement was interrupted and never reports in.
    if (_enterStep != TSBossActions::DONE)
    {
        _enterWait += diff;
        if (_enterWait < 15000)
            return;
        MovementInform(me, EFFECT_MOTION_TYPE, TSBossActions::LAND_POINT);
        if (!_boss)
            return;
    }

    _phaseTime += diff;
    if (CheckTriggers(me, &_boss->always) || (_phase && CheckTriggers(me, _phase)))
        return;

    _events.Update(diff);
    bool pause = _boss->always.pauseWhileCasting || (_phase && _phase->pauseWhileCasting);
    while (!(pause && me->HasUnitState(UNIT_STATE_CASTING)))
    {
        uint32 id = _events.ExecuteEvent();
        if (!id)
            break;
        Fire(me, _boss->timers[id - 1].get());
        if (!_boss)
            return;
    }
}

uint64 TSBossRuntime::DamageBy(ObjectGuid guid) const
{
    auto itr = _damage.find(guid);
    return itr != _damage.end() ? itr->second : 0;
}

uint64 TSBossRuntime::HealingBy(ObjectGuid guid) const
{
    auto itr = _healing.find(guid);
    return itr != _healing.end() ? itr->second : 0;
}

void TSBossRuntime::HealingDone(Unit* healer, Unit* target, uint32 gain)
{
    if (!healer || !target || !gain)
        return;
    Player* owner = healer->GetCharmerOrOwnerPlayerOrPlayerItself();
    Unit* credited = owner ? owner : healer;
    std::set<TSScriptedAI*> bosses;
    for (Unit* unit : { credited, target })
        for (auto const& pair : unit->GetThreatManager().GetThreatenedByMeList())
            if (Creature* creature = pair.second->GetOwner())
                if (TSScriptedAI* ai = dynamic_cast<TSScriptedAI*>(creature->AI()))
                    bosses.insert(ai);
    for (TSScriptedAI* ai : bosses)
        ai->Boss()._healing[credited->GetGUID()] += gain;
}

uint8 TSBossRuntime::TalentTabOf(Player* player)
{
    auto itr = _talentTabs.find(player->GetGUID());
    if (itr != _talentTabs.end())
        return itr->second;

    uint32 points[MAX_TALENT_TABS] = {};
    uint32 const* pages = GetTalentTabPages(player->GetClass());
    for (TalentEntry const* talent : sTalentStore)
        for (uint8 tab = 0; tab < MAX_TALENT_TABS; ++tab)
            if (talent->TabID == pages[tab])
                for (uint8 rank = 0; rank < MAX_TALENT_RANK; ++rank)
                    if (talent->SpellRank[rank] && player->HasTalent(talent->SpellRank[rank], player->GetActiveSpec()))
                        points[tab] += rank + 1;
    uint8 best = uint8(std::max_element(points, points + MAX_TALENT_TABS) - points);
    _talentTabs[player->GetGUID()] = best;
    return best;
}

// ------------------------------------------------------ death control, links

bool TSBossRuntime::PhaseTriggerPending() const
{
    auto pending = [this](TSBossPhase const* phase)
    {
        for (TSBossTrigger* trigger : phase->triggers)
            if (trigger->goTo && trigger->cond->type == TSBossCondition::HEALTH_BELOW && !_triggerFired[trigger->index])
                return true;
        return false;
    };
    return pending(&_boss->always) || (_phase && pending(_phase));
}

// Runs before the damage is applied, so it can still be changed or cancelled.
void TSBossRuntime::DamageTaken(Creature* me, Unit* attacker, uint32& damage)
{
    if (!_boss)
        return;
    if (_down)
    {
        damage = 0;
        return;
    }

    TSBossLink const* link = _boss->link;
    if (link && link->handoffMin && StateOf(link, me).active != me->GetGUID())
    {
        damage = 0;
        return;
    }

    bool lethal = damage >= me->GetHealth();
    // A big hit must not skip a phase: stop at 1 health and let the trigger run.
    if (lethal && (_boss->always.immortal || (_phase && _phase->immortal) || PhaseTriggerPending()))
    {
        damage = me->GetHealth() - 1;
        lethal = false;
    }

    if (lethal && _boss->fakeDeath.enabled && _falls < _boss->fakeDeath.times)
    {
        damage = 0;
        Fall(me);
        return;
    }

    if (attacker && damage)
    {
        Player* owner = attacker->GetCharmerOrOwnerPlayerOrPlayerItself();
        _damage[owner ? owner->GetGUID() : attacker->GetGUID()] += std::min(damage, me->GetHealth());
    }

    if (link && link->sharedHealth)
    {
        LinkState& state = StateOf(link, me);
        float pct = lethal ? 0.f : float(me->GetHealth() - damage) / float(me->GetMaxHealth());
        for (Creature* other : Members(link, me))
        {
            if (other == me || !other->IsAlive())
                continue;
            if (!lethal)
                other->SetHealth(std::max<uint32>(1, uint32(float(other->GetMaxHealth()) * pct)));
            else
            {
                state.busy = true;
                Unit::Kill(attacker, other);
                state.busy = false;
            }
        }
    }
}

void TSBossRuntime::HealReceived(Creature* me, uint32& heal)
{
    TSBossLink const* link = Link();
    if (!link || !link->sharedHealth || _down)
        return;
    float pct = float(std::min<uint64>(uint64(me->GetHealth()) + heal, me->GetMaxHealth())) / float(me->GetMaxHealth());
    for (Creature* other : Members(link, me))
        if (other != me && other->IsAlive())
            other->SetHealth(std::max<uint32>(1, uint32(float(other->GetMaxHealth()) * pct)));
}

void TSBossRuntime::Fall(Creature* me)
{
    _down = true;
    _downTime = 0;
    ++_falls;
    me->InterruptNonMeleeSpells(false);
    me->SetHealth(0);
    me->StopMoving();
    me->RemoveAllAurasOnDeath();
    me->SetUnitFlag(UNIT_FLAG_UNINTERACTIBLE);
    me->SetReactState(REACT_PASSIVE);
    me->AttackStop();
    me->SetTarget(ObjectGuid::Empty);
    me->GetMotionMaster()->Clear();
    me->GetMotionMaster()->MoveIdle();
    me->SetStandState(UNIT_STAND_STATE_DEAD);
    _boss->fakeDeath.fall.Run(me);
}

void TSBossRuntime::UpdateDown(Creature* me, uint32 diff)
{
    if (!_boss || _generation != TSBosses::Generation())
    {
        Reset(me);
        return;
    }

    // A downed boss has no victim, so it has to notice a wipe by itself.
    bool anyoneLeft = false;
    for (MapReference const& ref : me->GetMap()->GetPlayers())
        if (Player* player = ref.GetSource())
            if (player->IsAlive() && !player->IsGameMaster() && me->IsWithinDistInMap(player, 250.f))
                anyoneLeft = true;
    if (!anyoneLeft)
    {
        me->AI()->EnterEvadeMode(CreatureAI::EVADE_REASON_NO_HOSTILES);
        return;
    }

    _downTime += diff;
    if (_boss->fakeDeath.reviveAfter && _downTime >= _boss->fakeDeath.reviveAfter)
        Revive(me);
}

void TSBossRuntime::Revive(Creature* me)
{
    if (!_down || !_boss)
        return;
    _down = false;
    TSBossFakeDeath const& fake = _boss->fakeDeath;
    RestoreFromFakeDeath(me);
    me->SetHealth(std::max<uint32>(1, me->CountPctFromMaxHealth(int32(fake.reviveAt))));
    me->AI()->DoZoneInCombat();
    fake.revive.Run(me);
    if (_boss && fake.goTo && fake.goTo != _phase)
        SetPhase(me, fake.goTo);
}

void TSBossRuntime::Died(Creature* me)
{
    TSBossLink const* link = Link();
    if (!link)
        return;
    LinkState& state = StateOf(link, me);

    std::vector<Creature*> survivors;
    for (Creature* other : Members(link, me))
        if (other != me && other->IsAlive())
            survivors.push_back(other);

    if (link->lootFromLastOnly && !survivors.empty())
    {
        me->loot.clear();
        me->RemoveDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
    }
    if (state.busy)
        return;
    if (survivors.empty())
    {
        state = LinkState();
        return;
    }

    for (Creature* survivor : survivors)
        link->memberDeath.Run(survivor);
    if (link->allDieWithin && !state.firstDeath)
        state.firstDeath = GameTime::GetGameTimeMS();
    if (link->handoffMin && state.active == me->GetGUID())
    {
        Creature* next = Trinity::Containers::SelectRandomContainerElement(survivors);
        state.active = next->GetGUID();
        state.nextHandoff = GameTime::GetGameTimeMS() + urand(link->handoffMin, link->handoffMax);
        link->handoff.Run(next);
    }
}

// Once a second, from every engaged member.
void TSBossRuntime::LinkTick(Creature* me)
{
    TSBossLink const* link = _boss->link;
    LinkState& state = StateOf(link, me);
    uint32 now = GameTime::GetGameTimeMS();
    std::vector<Creature*> members = Members(link, me);

    // everyone fights together
    if (Unit* victim = me->GetVictim())
        for (Creature* other : members)
            if (other != me && other->IsAlive() && !other->IsInCombat() && !other->IsInEvadeMode())
                other->EngageWithTarget(victim);

    if (link->handoffMin && state.active == me->GetGUID() && now >= state.nextHandoff)
    {
        std::vector<Creature*> others;
        for (Creature* other : members)
            if (other != me && other->IsAlive())
                others.push_back(other);
        state.nextHandoff = now + urand(link->handoffMin, link->handoffMax);
        if (!others.empty())
        {
            Creature* next = Trinity::Containers::SelectRandomContainerElement(others);
            state.active = next->GetGUID();
            link->handoff.Run(next);
        }
    }

    // ponytail: brings the dead back through their spawn, so AllDieWithin only
    // works for members placed in the world, not for summoned ones. They stand
    // up at their spawn point and are pulled back in by the next tick.
    if (state.firstDeath && now - state.firstDeath >= link->allDieWithin)
    {
        state.firstDeath = 0;
        for (Creature* other : members)
            if (!other->IsAlive())
                other->Respawn(true);
    }
}

void TSBossRuntime::LinkEvaded(TSBossLink const* link, Creature* me)
{
    StateOf(link, me) = LinkState();
    for (Creature* other : Members(link, me))
    {
        if (other == me)
            continue;
        if (!other->IsAlive())
            other->Respawn(true);   // a wipe resets the whole group
        else if (other->IsAIEnabled() && !other->IsInEvadeMode())
        {
            TSScriptedAI* ai = dynamic_cast<TSScriptedAI*>(other->AI());
            if (other->IsInCombat() || (ai && ai->IsFakeDead()))
                other->AI()->EnterEvadeMode(CreatureAI::EVADE_REASON_OTHER);
        }
    }
}
// @tswow-end
