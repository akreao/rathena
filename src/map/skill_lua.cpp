// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "skill_lua.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

#include <common/core.hpp>      // db_path
#include <common/random.hpp>    // rnd
#include <common/showmsg.hpp>

#include "../../3rdparty/lua/src/lua.h"
#include "../../3rdparty/lua/src/lauxlib.h"
#include "../../3rdparty/lua/src/lualib.h"

#include "battle.hpp"
#include "itemdb.hpp"
#include "map.hpp"
#include "mob.hpp"
#include "mod_store.hpp"
#include "pc.hpp"
#include "script.hpp"
#include "skill.hpp"
#include "status.hpp"
#include "skills/skill_impl.hpp"

namespace {

// ---------------------------------------------------------------------------
// The state and its limits
// ---------------------------------------------------------------------------

lua_State* L = nullptr;

// A script can allocate this much in total, and run this many thousand
// instructions per call. Both are far beyond any formula; they exist so a
// mistake (an endless loop, a table that grows forever) costs one hook, not
// the map server.
constexpr size_t MEMORY_LIMIT = 64 * 1024 * 1024;
constexpr int32 CALL_BUDGET = 1000;       // x1000 instructions, per hook call
constexpr int32 LOAD_BUDGET = 20000;      // x1000 instructions, per file
size_t memory_used = 0;
int32 budget = 0;

// The mod whose code is running, for the log.
std::string current_mod;

void* limited_alloc(void*, void* ptr, size_t osize, size_t nsize) {
	size_t old = ptr != nullptr ? osize : 0;

	if (nsize == 0) {
		std::free(ptr);
		memory_used -= old;
		return nullptr;
	}
	if (memory_used - old + nsize > MEMORY_LIMIT)
		return nullptr;  // Lua turns this into a "not enough memory" error

	void* grown = std::realloc(ptr, nsize);

	if (grown != nullptr)
		memory_used = memory_used - old + nsize;
	return grown;
}

void count_hook(lua_State* state, lua_Debug*) {
	if (--budget <= 0)
		luaL_error(state, "stopped after %d thousand instructions", CALL_BUDGET);
}

// ---------------------------------------------------------------------------
// Hooks, by skill name and then by id
// ---------------------------------------------------------------------------

enum e_hook : uint8 { HOOK_RATIO = 0, HOOK_HIT, HOOK_ELEMENT, HOOK_ON_HIT, HOOK_ON_STEAL, HOOK_MAX };
const char* const hook_names[HOOK_MAX] = { "ratio", "hit", "element", "on_hit", "on_steal" };

enum e_item_hook : uint8 { ITEM_HOOK_ON_ATTACK = 0, ITEM_HOOK_ON_HIT_TAKEN, ITEM_HOOK_MAX };
const char* const item_hook_names[ITEM_HOOK_MAX] = { "on_attack", "on_hit_taken" };

constexpr int32 PRIORITY_MIN = 0;
constexpr int32 PRIORITY_MAX = 10;
constexpr int32 PRIORITY_DEFAULT = 5;

/// One mod's registration for one hook on one skill. Several of these may
/// live on the same (skill, hook) when more than one mod wants in: they run
/// in ascending `priority` order, ties broken by `load_order` (the order
/// their files were run, which is the mod's alphabetical place).
struct s_hook {
	int32 ref = LUA_NOREF;
	std::string mod;
	int32 priority = PRIORITY_DEFAULT;
	uint32 load_order = 0;
};

struct s_skill_hooks {
	std::string skill;
	std::vector<s_hook> hooks[HOOK_MAX];
};

/// Same shape as s_skill_hooks, keyed by item AegisName; mod registrations
/// are kept here until startup resolves each name to its nameid.
struct s_item_hooks {
	std::string aegis;
	std::vector<s_hook> hooks[ITEM_HOOK_MAX];
};

/// Monotonic counter so a late-registering hook keeps its place behind
/// earlier ones at the same priority.
uint32 next_load_order = 0;

std::unordered_map<std::string, s_skill_hooks> hooks_by_name;
std::unordered_map<uint16, s_skill_hooks*> hooks_by_id;

s_skill_hooks* hooks_for(uint16 skill_id) {
	if (L == nullptr || hooks_by_id.empty())
		return nullptr;

	auto it = hooks_by_id.find(skill_id);

	return it == hooks_by_id.end() ? nullptr : it->second;
}

std::unordered_map<std::string, s_item_hooks> item_hooks_by_name;
std::unordered_map<t_itemid, s_item_hooks*> item_hooks_by_id;

// While a skill cast from Lua (c:cast) runs, no Lua hook runs: an item whose
// on_attack casts a skill that hits would otherwise trigger itself forever.
bool in_lua_cast = false;

s_item_hooks* item_hooks_for(t_itemid nameid) {
	if (L == nullptr || item_hooks_by_id.empty() || nameid == 0)
		return nullptr;

	auto it = item_hooks_by_id.find(nameid);

	return it == item_hooks_by_id.end() ? nullptr : it->second;
}

int32 message_handler(lua_State* state) {
	luaL_traceback(state, state, lua_tostring(state, 1), 1);
	return 1;
}

/// Call the function under `nargs` arguments with `nresults` results. On an
/// error the hook is switched off -- a broken hook fails once, loudly, rather
/// than on every hit -- and false is returned with the stack cleaned up.
bool protected_call(s_hook& hook, const char* skill, int32 nargs, int32 nresults) {
	int32 base = lua_gettop(L) - nargs;

	lua_pushcfunction(L, message_handler);
	lua_insert(L, base);
	current_mod = hook.mod;
	budget = CALL_BUDGET;

	int32 status = lua_pcall(L, nargs, nresults, base);

	lua_remove(L, base);

	if (status == LUA_OK)
		return true;

	ShowError("Lua: %s's hook for %s failed and is now off until the server restarts:\n%s\n", hook.mod.c_str(), skill, lua_tostring(L, -1));
	lua_pop(L, 1);
	luaL_unref(L, LUA_REGISTRYINDEX, hook.ref);
	hook.ref = LUA_NOREF;
	return false;
}

// ---------------------------------------------------------------------------
// What a hook sees: c.caster, c.target, c.skill_lv, ...
// ---------------------------------------------------------------------------

const char* unit_kind(const block_list& bl) {
	switch (bl.type) {
		case BL_PC: return "pc";
		case BL_MOB: return "mob";
		case BL_HOM: return "homun";
		case BL_MER: return "merc";
		case BL_ELEM: return "elemental";
		case BL_PET: return "pet";
		case BL_NPC: return "npc";
		default: return "other";
	}
}

void set_int(const char* name, lua_Integer value) {
	lua_pushinteger(L, value);
	lua_setfield(L, -2, name);
}

sc_type status_named(lua_State* state, int32 arg) {
	const char* name = luaL_checkstring(state, arg);
	int64 value;

	if (!script_get_constant(name, &value) || value <= SC_NONE || value >= SC_MAX)
		luaL_error(state, "%s is not a status (use a name like SC_STUN)", name);
	return static_cast<sc_type>(value);
}

// unit:has_status("SC_BLESSING")
int32 lua_unit_has_status(lua_State* state) {
	luaL_checktype(state, 1, LUA_TTABLE);
	sc_type type = status_named(state, 2);
	lua_getfield(state, 1, "id");
	block_list* bl = map_id2bl(static_cast<int32>(lua_tointeger(state, -1)));
	const status_change* sc = bl != nullptr ? status_get_sc(bl) : nullptr;

	lua_pushboolean(state, sc != nullptr && sc->hasSCE(type));
	return 1;
}

void push_unit(const block_list* bl) {
	if (bl == nullptr) {
		lua_pushnil(L);
		return;
	}

	const status_data* st = status_get_status_data(*bl);

	lua_createtable(L, 0, 24);
	set_int("id", bl->id);
	lua_pushstring(L, unit_kind(*bl));
	lua_setfield(L, -2, "kind");
	lua_pushstring(L, status_get_name(*bl));
	lua_setfield(L, -2, "name");
	set_int("level", status_get_lv(bl));
	set_int("str", st->str);
	set_int("agi", st->agi);
	set_int("vit", st->vit);
	set_int("int", st->int_);
	set_int("dex", st->dex);
	set_int("luk", st->luk);
	set_int("hp", st->hp);
	set_int("maxhp", st->max_hp);
	set_int("sp", st->sp);
	set_int("maxsp", st->max_sp);
	set_int("race", st->race);
	set_int("element", st->def_ele);
	set_int("size", st->size);
	lua_pushboolean(L, st->class_ == CLASS_BOSS);
	lua_setfield(L, -2, "boss");
	lua_pushboolean(L, status_isdead(*bl));
	lua_setfield(L, -2, "dead");

	if (const map_session_data* sd = BL_CAST(BL_PC, bl); sd != nullptr) {
		set_int("job", sd->status.class_);
		set_int("job_level", sd->status.job_level);
		// Item bonuses a hook might want to honour on a skill that stock
		// rAthena does not apply them to.
		set_int("classchange", sd->bonus.classchange);
		// Equipped items by slot, 0 when nothing is in that slot. Scripts
		// can gate on gear ("c.caster.weapon_id == const('VORPAL_BLADE')")
		// without a separate item() registration.
		auto slot_id = [&](equip_index e) -> t_itemid {
			int16 idx = sd->equip_index[e];
			return idx >= 0 ? sd->inventory.u.items_inventory[idx].nameid : 0;
		};
		set_int("weapon_id", slot_id(EQI_HAND_R));
		set_int("shield_id", slot_id(EQI_HAND_L));
		set_int("armor_id", slot_id(EQI_ARMOR));
		set_int("shoes_id", slot_id(EQI_SHOES));
		set_int("robe_id", slot_id(EQI_GARMENT));
		set_int("helm_top_id", slot_id(EQI_HEAD_TOP));
		set_int("helm_mid_id", slot_id(EQI_HEAD_MID));
		set_int("helm_bottom_id", slot_id(EQI_HEAD_LOW));
		set_int("accessory_1_id", slot_id(EQI_ACC_L));
		set_int("accessory_2_id", slot_id(EQI_ACC_R));
	} else if (const mob_data* md = BL_CAST(BL_MOB, bl); md != nullptr) {
		set_int("mob_id", md->mob_id);
	}

	lua_pushcfunction(L, lua_unit_has_status);
	lua_setfield(L, -2, "has_status");
}

// The hit a damage hook is running for. Actions record into it; outside
// a damage hook (on_hit, on_attack, on_hit_taken) it is null and they refuse.
s_skill_lua_hit* current_hit = nullptr;

s_skill_lua_hit& hit_or_error(lua_State* state, const char* what) {
	if (current_hit == nullptr)
		luaL_error(state, "c:%s() only works inside on_hit, on_attack or on_hit_taken", what);
	return *current_hit;
}

int32 unit_arg(lua_State* state, int32 arg, const s_skill_lua_hit& hit) {
	const char* who = luaL_optstring(state, arg, "target");

	if (strcmp(who, "target") == 0)
		return hit.target_id;
	if (strcmp(who, "caster") == 0)
		return hit.src_id;
	return luaL_error(state, "who must be \"target\" or \"caster\", not \"%s\"", who);
}

// c:drain() -- the attacker's HP/SP drain item bonuses, on this hit's
// damage. The direction is fixed (attacker drains defender) because the
// underlying server call is: calling it in on_hit_taken is harmless but
// redundant, since the stock weapon-attack path already drains for every
// weapon hit that lands.
int32 lua_hit_drain(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "drain");
	hit.actions.push_back({ SKILL_LUA_DRAIN, hit.src_id });
	return 0;
}

// c:heal(hp, sp, {who}) -- restore a unit. `who` is "caster" (default,
// the attacker) or "target" (the defender). The default matches the
// skill() on_hit case where a skill heals its own caster off the hit it
// just dealt. An item() on_hit_taken hook that wants to restore the
// wearer picks "target" explicitly, since the wearer is the defender.
int32 lua_hit_heal(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "heal");
	s_skill_lua_action action = { SKILL_LUA_HEAL };

	action.hp = std::max<lua_Integer>(0, luaL_checkinteger(state, 2));
	action.sp = std::max<lua_Integer>(0, luaL_optinteger(state, 3, 0));
	action.unit_id = unit_arg(state, 4, hit);
	hit.actions.push_back(action);
	return 0;
}

// c:status("SC_STUN", rate, duration_ms, {val1}, {who}) -- rate out of 10000.
int32 lua_hit_status(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "status");
	s_skill_lua_action action = { SKILL_LUA_STATUS };

	action.type = status_named(state, 2);
	action.rate = static_cast<int32>(std::clamp<lua_Integer>(luaL_checkinteger(state, 3), 0, 10000));
	action.duration = std::clamp<lua_Integer>(luaL_checkinteger(state, 4), 0, 3600000);
	action.val1 = static_cast<int32>(luaL_optinteger(state, 5, 1));
	action.unit_id = unit_arg(state, 6, hit);
	hit.actions.push_back(action);
	return 0;
}

// c:polymorph() -- what Hylozoist Card does: turn a monster target into a
// random one from the Dead Branch list. Bosses and status-immune monsters
// are never changed.
int32 lua_hit_polymorph(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "polymorph");
	hit.actions.push_back({ SKILL_LUA_POLYMORPH, hit.target_id });
	return 0;
}

// c:chance(n) -- true n times in 10000, from the server's own random numbers.
// c:cast("MG_FIREBOLT", 3, {who}) -- cast a skill the way bAutoSpell does:
// at "target" (default) or "caster", with the skill's own checks and delays.
int32 lua_hit_cast(lua_State* state) {
	s_skill_lua_hit& hit = hit_or_error(state, "cast");
	uint16 skill_id = lua_type(state, 2) == LUA_TNUMBER ? (uint16)lua_tointeger(state, 2) : skill_name2id(luaL_checkstring(state, 2));

	if (skill_id == 0 || skill_get_index(skill_id) == 0)
		return luaL_error(state, "c:cast: there is no skill %s (use the AegisName, like MG_FIREBOLT)", luaL_tolstring(state, 2, nullptr));

	s_skill_lua_action action = { SKILL_LUA_CAST };

	action.type = skill_id;
	action.val1 = static_cast<int32>(std::clamp<lua_Integer>(luaL_optinteger(state, 3, 1), 1, MAX_SKILL_LEVEL));
	action.unit_id = unit_arg(state, 4, hit);
	hit.actions.push_back(action);
	return 0;
}

int32 lua_hit_chance(lua_State* state) {
	lua_Integer n = luaL_checkinteger(state, 2);
	lua_pushboolean(state, rnd() % 10000 < n);
	return 1;
}

/// Extra fields a damage hook (on_hit/on_attack/on_hit_taken) wants on `c`
/// but a chain hook (ratio/hit/element) does not need. All optional: null
/// means a chain hook, non-null means a damage hook.
struct s_damage_ctx {
	int64 damage;
	int32 dmg_lv;          ///< ATK_* (ATK_DEF means it connected)
	int32 element;         ///< ELE_*
	int32 attack_type;     ///< BF_WEAPON / BF_MAGIC / BF_MISC mask
	bool critical;
};

const char* attack_type_name(int32 attack_type) {
	if (attack_type & BF_MAGIC) return "magic";
	if (attack_type & BF_MISC) return "misc";
	return "weapon";
}

void push_context(uint16 skill_id, uint16 skill_lv, const block_list* src, const block_list* target, const s_damage_ctx* dmg) {
	lua_createtable(L, 0, dmg != nullptr ? 18 : 12);
	lua_pushstring(L, skill_id != 0 ? skill_get_name(skill_id) : "");
	lua_setfield(L, -2, "skill");
	set_int("skill_id", skill_id);
	set_int("skill_lv", skill_lv);
	push_unit(src);
	lua_setfield(L, -2, "caster");
	push_unit(target);
	lua_setfield(L, -2, "target");
	lua_pushcfunction(L, lua_hit_chance);
	lua_setfield(L, -2, "chance");

	if (dmg != nullptr) {
		set_int("damage", dmg->damage);
		set_int("element", dmg->element);
		lua_pushboolean(L, dmg->dmg_lv >= ATK_DEF && dmg->damage > 0);
		lua_setfield(L, -2, "connected");
		lua_pushboolean(L, dmg->critical);
		lua_setfield(L, -2, "critical");
		lua_pushstring(L, attack_type_name(dmg->attack_type));
		lua_setfield(L, -2, "weapon_type");
		lua_pushcfunction(L, lua_hit_drain);
		lua_setfield(L, -2, "drain");
		lua_pushcfunction(L, lua_hit_heal);
		lua_setfield(L, -2, "heal");
		lua_pushcfunction(L, lua_hit_status);
		lua_setfield(L, -2, "status");
		lua_pushcfunction(L, lua_hit_polymorph);
		lua_setfield(L, -2, "polymorph");
		lua_pushcfunction(L, lua_hit_cast);
		lua_setfield(L, -2, "cast");
	}
}

/// ratio, hit and element: `value` is the stock result on the way in. Every
/// registered hook runs in priority order; each sees what the previous one
/// returned as `stock`, and returning nil keeps the running value. A hook
/// that fails is switched off and the chain carries on with the rest.
void call_number_hook(e_hook which, uint16 skill_id, uint16 skill_lv, const block_list* src, const block_list* target, int64& value) {
	s_skill_hooks* hooks = hooks_for(skill_id);

	if (hooks == nullptr || hooks->hooks[which].empty())
		return;

	for (s_hook& hook : hooks->hooks[which]) {
		if (hook.ref == LUA_NOREF)
			continue;  // this one failed earlier and is off until restart

		lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
		push_context(skill_id, skill_lv, src, target, nullptr);
		lua_pushinteger(L, value);
		if (!protected_call(hook, hooks->skill.c_str(), 2, 1))
			continue;

		if (lua_isinteger(L, -1)) {
			value = lua_tointeger(L, -1);
		} else if (lua_type(L, -1) == LUA_TNUMBER) {
			value = static_cast<int64>(lua_tonumber(L, -1));  // toward zero, as the C++ would
		} else if (!lua_isnil(L, -1)) {
			ShowError("Lua: %s's %s hook for %s returned a %s, not a number; keeping the running value.\n", hook.mod.c_str(), hook_names[which], hooks->skill.c_str(), luaL_typename(L, -1));
		}
		lua_pop(L, 1);
	}
}

// ---------------------------------------------------------------------------
// The wrapper around a skill's own class
// ---------------------------------------------------------------------------

class LuaSkillImpl : public SkillImpl {
	std::unique_ptr<const SkillImpl> stock_;

public:
	LuaSkillImpl(e_skill skill_id, std::unique_ptr<const SkillImpl> stock) : SkillImpl(skill_id), stock_(std::move(stock)) {}

	void castendNoDamageId(block_list* src, block_list* target, uint16 skill_lv, t_tick tick, int32& flag) const override {
		stock_->castendNoDamageId(src, target, skill_lv, tick, flag);
	}
	void castendDamageId(block_list* src, block_list* target, uint16 skill_lv, t_tick tick, int32& flag) const override {
		stock_->castendDamageId(src, target, skill_lv, tick, flag);
	}
	void castendPos2(block_list* src, int32 x, int32 y, uint16 skill_lv, t_tick tick, int32& flag) const override {
		stock_->castendPos2(src, x, y, skill_lv, tick, flag);
	}
	void applyAdditionalEffects(block_list* src, block_list* target, uint16 skill_lv, t_tick tick, int32 attack_type, enum damage_lv dmg_lv) const override {
		stock_->applyAdditionalEffects(src, target, skill_lv, tick, attack_type, dmg_lv);
	}
	void applyCounterAdditionalEffects(block_list* src, block_list* target, uint16 skill_lv, t_tick tick, int32& attack_type) const override {
		stock_->applyCounterAdditionalEffects(src, target, skill_lv, tick, attack_type);
	}
	void modifyDamageData(Damage& dmg, const block_list& src, const block_list& target, uint16 skill_lv) const override {
		stock_->modifyDamageData(dmg, src, target, skill_lv);
	}

	void calculateSkillRatio(const Damage* wd, const block_list* src, const block_list* target, uint16 skill_lv, int32& base_skillratio, int32 mflag) const override {
		stock_->calculateSkillRatio(wd, src, target, skill_lv, base_skillratio, mflag);

		int64 value = base_skillratio;

		call_number_hook(HOOK_RATIO, skill_id_, skill_lv, src, target, value);
		base_skillratio = static_cast<int32>(std::clamp<int64>(value, 0, INT32_MAX));
	}

	void modifyHitRate(int16& hit_rate, const block_list* src, const block_list* target, uint16 skill_lv) const override {
		stock_->modifyHitRate(hit_rate, src, target, skill_lv);

		int64 value = hit_rate;

		call_number_hook(HOOK_HIT, skill_id_, skill_lv, src, target, value);
		hit_rate = static_cast<int16>(std::clamp<int64>(value, INT16_MIN, INT16_MAX));
	}

	void modifyElement(const Damage& dmg, const block_list& src, const block_list& target, uint16 skill_lv, int32& element, int32 flag) const override {
		stock_->modifyElement(dmg, src, target, skill_lv, element, flag);

		int64 value = element;

		call_number_hook(HOOK_ELEMENT, skill_id_, skill_lv, &src, &target, value);
		if (value >= ELE_NEUTRAL && value < ELE_ALL)
			element = static_cast<int32>(value);
	}
};

// ---------------------------------------------------------------------------
// The functions a mod's file can call while it loads
// ---------------------------------------------------------------------------

// skill("NJ_KAENSIN", { priority = 3, ratio = ..., on_hit = ... })
// Every mod that registers for the same (skill, hook) is kept: at call time
// they run in ascending priority order, ties broken by load order. Priority
// is optional (0..10, default 5) and applies to every hook in the call. A
// mod that registers twice for the same (skill, hook) replaces its own
// previous entry rather than stacking against itself.
int32 lua_register_skill(lua_State* state) {
	const char* name = luaL_checkstring(state, 1);
	luaL_checktype(state, 2, LUA_TTABLE);

	s_skill_hooks& entry = hooks_by_name[name];
	entry.skill = name;

	// Optional priority: pulled first so it is in hand before the hooks are
	// read, and so it only has to be validated once.
	int32 priority = PRIORITY_DEFAULT;
	lua_getfield(state, 2, "priority");
	if (!lua_isnil(state, -1)) {
		if (!lua_isinteger(state, -1))
			return luaL_error(state, "skill %s: priority must be a whole number between %d and %d", name, PRIORITY_MIN, PRIORITY_MAX);
		lua_Integer p = lua_tointeger(state, -1);
		if (p < PRIORITY_MIN || p > PRIORITY_MAX)
			return luaL_error(state, "skill %s: priority %d is outside the %d..%d range", name, static_cast<int32>(p), PRIORITY_MIN, PRIORITY_MAX);
		priority = static_cast<int32>(p);
	}
	lua_pop(state, 1);

	lua_pushnil(state);
	while (lua_next(state, 2) != 0) {
		const char* key = lua_type(state, -2) == LUA_TSTRING ? lua_tostring(state, -2) : "";

		// `priority` is a key we read above, not a hook. Skip it rather than
		// complaining about an unknown hook name.
		if (strcmp(key, "priority") == 0) {
			lua_pop(state, 1);
			continue;
		}

		int32 which = -1;

		for (int32 i = 0; i < HOOK_MAX; i++)
			if (strcmp(key, hook_names[i]) == 0)
				which = i;
		if (which < 0)
			return luaL_error(state, "skill %s: unknown hook \"%s\" (ratio, hit, element, on_hit, on_steal or priority)", name, key);
		if (!lua_isfunction(state, -1))
			return luaL_error(state, "skill %s: %s must be a function", name, key);

		std::vector<s_hook>& vec = entry.hooks[which];
		s_hook hook;
		hook.mod = current_mod;
		hook.priority = priority;
		hook.load_order = next_load_order++;
		hook.ref = luaL_ref(state, LUA_REGISTRYINDEX);  // pops the value

		// Replace this mod's previous registration for this (skill, hook), so
		// a mod that re-declares a hook (e.g. after a reload) does not stack
		// against itself.
		auto existing = std::find_if(vec.begin(), vec.end(),
			[&](const s_hook& h) { return h.mod == current_mod; });
		if (existing != vec.end()) {
			luaL_unref(state, LUA_REGISTRYINDEX, existing->ref);
			*existing = hook;
		} else {
			vec.push_back(hook);
		}

		// Keep the chain in priority order. Stable so load_order breaks ties
		// deterministically.
		std::stable_sort(vec.begin(), vec.end(),
			[](const s_hook& a, const s_hook& b) {
				if (a.priority != b.priority)
					return a.priority < b.priority;
				return a.load_order < b.load_order;
			});
	}
	return 0;
}

// item("VORPAL_BLADE", { priority = 3, on_attack = ..., on_hit_taken = ... })
// Same chaining and priority rules as skill(), keyed by the item's
// AegisName. The nameid is resolved at skill_lua_attach time, so a typo
// shows up in the log as "no item called X" rather than a silent no-op.
int32 lua_register_item(lua_State* state) {
	const char* name = luaL_checkstring(state, 1);
	luaL_checktype(state, 2, LUA_TTABLE);

	s_item_hooks& entry = item_hooks_by_name[name];
	entry.aegis = name;

	int32 priority = PRIORITY_DEFAULT;
	lua_getfield(state, 2, "priority");
	if (!lua_isnil(state, -1)) {
		if (!lua_isinteger(state, -1))
			return luaL_error(state, "item %s: priority must be a whole number between %d and %d", name, PRIORITY_MIN, PRIORITY_MAX);
		lua_Integer p = lua_tointeger(state, -1);
		if (p < PRIORITY_MIN || p > PRIORITY_MAX)
			return luaL_error(state, "item %s: priority %d is outside the %d..%d range", name, static_cast<int32>(p), PRIORITY_MIN, PRIORITY_MAX);
		priority = static_cast<int32>(p);
	}
	lua_pop(state, 1);

	lua_pushnil(state);
	while (lua_next(state, 2) != 0) {
		const char* key = lua_type(state, -2) == LUA_TSTRING ? lua_tostring(state, -2) : "";

		if (strcmp(key, "priority") == 0) {
			lua_pop(state, 1);
			continue;
		}

		int32 which = -1;

		for (int32 i = 0; i < ITEM_HOOK_MAX; i++)
			if (strcmp(key, item_hook_names[i]) == 0)
				which = i;
		if (which < 0)
			return luaL_error(state, "item %s: unknown hook \"%s\" (on_attack, on_hit_taken or priority)", name, key);
		if (!lua_isfunction(state, -1))
			return luaL_error(state, "item %s: %s must be a function", name, key);

		std::vector<s_hook>& vec = entry.hooks[which];
		s_hook hook;
		hook.mod = current_mod;
		hook.priority = priority;
		hook.load_order = next_load_order++;
		hook.ref = luaL_ref(state, LUA_REGISTRYINDEX);

		auto existing = std::find_if(vec.begin(), vec.end(),
			[&](const s_hook& h) { return h.mod == current_mod; });
		if (existing != vec.end()) {
			luaL_unref(state, LUA_REGISTRYINDEX, existing->ref);
			*existing = hook;
		} else {
			vec.push_back(hook);
		}

		std::stable_sort(vec.begin(), vec.end(),
			[](const s_hook& a, const s_hook& b) {
				if (a.priority != b.priority)
					return a.priority < b.priority;
				return a.load_order < b.load_order;
			});
	}
	return 0;
}

// const("SC_STUN") -> the number the server uses for it. Any script constant.
int32 lua_constant(lua_State* state) {
	const char* name = luaL_checkstring(state, 1);
	int64 value;

	if (!script_get_constant(name, &value))
		return luaL_error(state, "%s is not a constant the server knows", name);
	lua_pushinteger(state, value);
	return 1;
}

// log(...) and print(...): to the map server's log, tagged with the mod.
int32 lua_log(lua_State* state) {
	std::string line;

	for (int32 i = 1, n = lua_gettop(state); i <= n; i++) {
		if (i > 1)
			line += ' ';
		line += luaL_tolstring(state, i, nullptr);
		lua_pop(state, 1);
	}
	ShowInfo("Lua: %s: %s\n", current_mod.c_str(), line.c_str());
	return 0;
}

// Pure Lua, run once before any mod: setting() reads the MOD_SETTINGS table
// that the app writes, and the base library loses what could read files.
const char* PRELUDE = R"lua(
dofile, loadfile, load, collectgarbage = nil, nil, nil, nil
function setting(mod, key, default)
  local m = MOD_SETTINGS and MOD_SETTINGS[mod]
  local v = m and m[key]
  if v == nil then return default end
  return v
end
)lua";

bool run_file(const std::string& path, const std::string& mod) {
	current_mod = mod;
	budget = LOAD_BUDGET;
	lua_pushcfunction(L, message_handler);

	// "t": text only. Precompiled chunks can crash the VM on purpose.
	int32 status = luaL_loadfilex(L, path.c_str(), "t");

	if (status == LUA_OK)
		status = lua_pcall(L, 0, 0, -2);
	if (status != LUA_OK) {
		ShowError("Lua: %s: could not load %s:\n%s\n", mod.c_str(), path.c_str(), lua_tostring(L, -1));
		lua_pop(L, 1);
	}
	lua_pop(L, 1);  // the message handler
	return status == LUA_OK;
}

/// The files to run, in order, as (mod, path). load.txt is "<mod>\t<path>"
/// per line, the path relative to db/import; the app writes it in mod order.
std::vector<std::pair<std::string, std::string>> file_list() {
	std::string dir = std::string(db_path) + "/import/lua";
	std::vector<std::pair<std::string, std::string>> files;
	std::ifstream list(dir + "/load.txt");

	if (list) {
		std::string line;

		while (std::getline(list, line)) {
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			size_t tab = line.find('\t');
			if (line.empty() || line[0] == '#' || tab == std::string::npos)
				continue;

			std::string rel = line.substr(tab + 1);

			if (rel.find("..") != std::string::npos) {
				ShowWarning("Lua: skipping %s in load.txt: it leaves db/import.\n", rel.c_str());
				continue;
			}
			files.emplace_back(line.substr(0, tab), std::string(db_path) + "/import/" + rel);
		}
		return files;
	}

	std::error_code ec;

	for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
		if (entry.is_regular_file() && entry.path().extension() == ".lua")
			files.emplace_back("lua", entry.path().string());
	std::sort(files.begin(), files.end());
	return files;
}

} // namespace

// ---------------------------------------------------------------------------
// Entry points
namespace {

// ---------------------------------------------------------------------------
// store: the mod's own data (mod_store.hpp)
//
//   store.get(path, default)   store.set(path, value)   store.inc(path, by)
//   store.delete(path)         store.exists(path)       store.keys(path)
//   store.count(path)          store.top(path, n)       store.used()
//   store.limits()             store.char(unit)         store.account(unit)
//
// store.char(c.caster) and store.account(c.caster) return the same methods
// for that player's character or account. Writes that fail return nil and a
// reason; nothing here raises an error a hook didn't ask for.
// ---------------------------------------------------------------------------

mod_store::s_target store_target(lua_State* state) {
	mod_store::s_target t;
	t.mod = current_mod == "server" ? "" : current_mod;
	t.scope = static_cast<mod_store::e_scope>(lua_tointeger(state, lua_upvalueindex(1)));
	t.owner = static_cast<uint32>(lua_tointeger(state, lua_upvalueindex(2)));
	return t;
}

std::string store_path(lua_State* state, int32 arg) {
	size_t len = 0;
	const char* text = luaL_optlstring(state, arg, "", &len);
	return std::string(text, len);
}

int32 store_fail(lua_State* state, const std::string& error) {
	lua_pushnil(state);
	lua_pushstring(state, error.c_str());
	return 2;
}

/// Write the Lua value at `index` under `path`: a table becomes a subtree.
bool store_write(lua_State* state, const mod_store::s_target& t, const std::string& path, int32 index, int32 depth, std::string& error) {
	index = lua_absindex(state, index);
	mod_store::s_value value;
	switch (lua_type(state, index)) {
		case LUA_TBOOLEAN:
			value.number = lua_toboolean(state, index) ? 1 : 0;
			break;
		case LUA_TNUMBER: {
			int32 is_integer = 0;
			value.number = lua_tointegerx(state, index, &is_integer);
			if (!is_integer) {
				error = "numbers in the store are integers";
				return false;
			}
			break;
		}
		case LUA_TSTRING: {
			size_t len = 0;
			const char* text = lua_tolstring(state, index, &len);
			value.is_string = true;
			value.text.assign(text, len);
			break;
		}
		case LUA_TTABLE: {
			if (depth > mod_store::depth_limit()) {
				error = "the table is nested too deeply";
				return false;
			}
			lua_pushnil(state);
			while (lua_next(state, index) != 0) {
				std::string child;
				if (lua_type(state, -2) == LUA_TNUMBER && lua_isinteger(state, -2))
					child = std::to_string(lua_tointeger(state, -2));
				else if (lua_type(state, -2) == LUA_TSTRING)
					child = lua_tostring(state, -2);
				else {
					lua_pop(state, 2);
					error = "table keys in the store are strings or integers";
					return false;
				}
				if (!store_write(state, t, path + "." + child, -1, depth + 1, error)) {
					lua_pop(state, 2);
					return false;
				}
				lua_pop(state, 1);
			}
			return true;
		}
		default:
			error = std::string("a ") + luaL_typename(state, index) + " can't be stored";
			return false;
	}
	return mod_store::set(t, path, value, error);
}

void push_store_value(lua_State* state, const mod_store::s_value& value) {
	if (value.is_string)
		lua_pushlstring(state, value.text.data(), value.text.size());
	else
		lua_pushinteger(state, value.number);
}

/// A segment as the table key it was written from: "3" was the integer 3
/// (store_write), so an array read back is an array again.
void push_store_key(lua_State* state, const std::string& segment) {
	bool integer = !segment.empty() && segment.size() <= 18
		&& (segment == "0" || segment[0] != '0')
		&& std::all_of(segment.begin(), segment.end(), [](char c) { return c >= '0' && c <= '9'; });
	if (integer)
		lua_pushinteger(state, std::stoll(segment));
	else
		lua_pushlstring(state, segment.data(), segment.size());
}

/// The value at a path, or everything under it as a table -- what store.set
/// wrote, read back.
int32 lua_store_get(lua_State* state) {
	std::vector<std::pair<std::string, mod_store::s_value>> entries;
	std::string error;
	if (!mod_store::read(store_target(state), store_path(state, 1), entries, error) || entries.empty()) {
		lua_settop(state, 2);  // the default, or nil
		return 1;
	}
	if (entries.size() == 1 && entries[0].first.empty()) {
		push_store_value(state, entries[0].second);
		return 1;
	}
	lua_newtable(state);
	const int32 root = lua_gettop(state);
	for (const auto& entry : entries) {
		lua_pushvalue(state, root);
		size_t start = 0;
		for (size_t dot = entry.first.find('.'); dot != std::string::npos; dot = entry.first.find('.', start)) {
			push_store_key(state, entry.first.substr(start, dot - start));
			lua_pushvalue(state, -1);
			if (lua_gettable(state, -3) != LUA_TTABLE) {
				lua_pop(state, 1);
				lua_newtable(state);
				lua_pushvalue(state, -2);  // key
				lua_pushvalue(state, -2);  // the new table
				lua_settable(state, -5);
			}
			lua_remove(state, -2);  // key
			lua_remove(state, -2);  // parent
			start = dot + 1;
		}
		push_store_key(state, entry.first.substr(start));
		push_store_value(state, entry.second);
		lua_settable(state, -3);
		lua_pop(state, 1);
	}
	return 1;
}

int32 lua_store_set(lua_State* state) {
	std::string error;
	if (lua_isnoneornil(state, 2))
		return store_fail(state, "set needs a value (use store.delete to remove one)");
	if (!store_write(state, store_target(state), store_path(state, 1), 2, 1, error))
		return store_fail(state, error);
	lua_pushboolean(state, 1);
	return 1;
}

int32 lua_store_inc(lua_State* state) {
	int64 result = 0;
	std::string error;
	if (!mod_store::inc(store_target(state), store_path(state, 1), luaL_optinteger(state, 2, 1), result, error))
		return store_fail(state, error);
	lua_pushinteger(state, result);
	return 1;
}

int32 lua_store_delete(lua_State* state) {
	int32 removed = 0;
	std::string error;
	if (!mod_store::remove(store_target(state), store_path(state, 1), removed, error))
		return store_fail(state, error);
	lua_pushinteger(state, removed);
	return 1;
}

int32 lua_store_exists(lua_State* state) {
	bool found = false;
	std::string error;
	mod_store::exists(store_target(state), store_path(state, 1), found, error);
	lua_pushboolean(state, found ? 1 : 0);
	return 1;
}

int32 lua_store_keys(lua_State* state) {
	std::vector<std::string> names;
	std::string error;
	if (!mod_store::keys(store_target(state), store_path(state, 1), names, error))
		return store_fail(state, error);
	lua_createtable(state, static_cast<int32>(names.size()), 0);
	for (size_t i = 0; i < names.size(); ++i) {
		lua_pushstring(state, names[i].c_str());
		lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
	}
	return 1;
}

int32 lua_store_count(lua_State* state) {
	std::vector<std::string> names;
	std::string error;
	mod_store::keys(store_target(state), store_path(state, 1), names, error);
	lua_pushinteger(state, static_cast<lua_Integer>(names.size()));
	return 1;
}

/// store.top(path, n): an array of { name = ..., value = ... }, highest first.
int32 lua_store_top(lua_State* state) {
	std::vector<std::pair<std::string, int64>> rows;
	std::string error;
	lua_Integer n = luaL_optinteger(state, 2, 10);
	if (!mod_store::top(store_target(state), store_path(state, 1), static_cast<size_t>(std::max<lua_Integer>(n, 0)), rows, error))
		return store_fail(state, error);
	lua_createtable(state, static_cast<int32>(rows.size()), 0);
	for (size_t i = 0; i < rows.size(); ++i) {
		lua_createtable(state, 0, 2);
		lua_pushstring(state, rows[i].first.c_str());
		lua_setfield(state, -2, "name");
		lua_pushinteger(state, rows[i].second);
		lua_setfield(state, -2, "value");
		lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
	}
	return 1;
}

int32 lua_store_used(lua_State* state) {
	mod_store::s_target t = store_target(state);
	bool ignored = false;
	std::string error;
	mod_store::exists(t, "_", ignored, error);  // loads the document on first use
	lua_pushinteger(state, static_cast<lua_Integer>(mod_store::used(t)));
	return 1;
}

int32 lua_store_limits(lua_State* state) {
	lua_createtable(state, 0, 5);
	set_int("global", static_cast<lua_Integer>(mod_store::limit(mod_store::SCOPE_GLOBAL)));
	set_int("account", static_cast<lua_Integer>(mod_store::limit(mod_store::SCOPE_ACCOUNT)));
	set_int("char", static_cast<lua_Integer>(mod_store::limit(mod_store::SCOPE_CHAR)));
	set_int("value", static_cast<lua_Integer>(mod_store::value_limit()));
	set_int("depth", static_cast<lua_Integer>(mod_store::depth_limit()));
	return 1;
}

/// Push a table of the store's methods, bound to `scope` and `owner`.
void push_store(lua_State* state, mod_store::e_scope scope, uint32 owner) {
	static const luaL_Reg methods[] = {
		{ "get", lua_store_get },       { "set", lua_store_set },
		{ "inc", lua_store_inc },       { "delete", lua_store_delete },
		{ "exists", lua_store_exists }, { "keys", lua_store_keys },
		{ "count", lua_store_count },   { "top", lua_store_top },
		{ "used", lua_store_used },     { nullptr, nullptr },
	};
	lua_createtable(state, 0, 12);
	for (const luaL_Reg* m = methods; m->name != nullptr; ++m) {
		lua_pushinteger(state, scope);
		lua_pushinteger(state, owner);
		lua_pushcclosure(state, m->func, 2);
		lua_setfield(state, -2, m->name);
	}
}

/// store.char(unit) / store.account(unit): `unit` is a hook's c.caster or
/// c.target, and must be a player.
int32 lua_store_scoped(lua_State* state, mod_store::e_scope scope) {
	luaL_checktype(state, 1, LUA_TTABLE);
	lua_getfield(state, 1, "id");
	int32 id = static_cast<int32>(lua_tointeger(state, -1));
	lua_pop(state, 1);
	map_session_data* sd = map_id2sd(id);
	if (sd == nullptr)
		return store_fail(state, std::string("store.") + mod_store::scope_name(scope) + " needs a player");
	push_store(state, scope, scope == mod_store::SCOPE_CHAR ? sd->status.char_id : sd->status.account_id);
	return 1;
}

int32 lua_store_char(lua_State* state) {
	return lua_store_scoped(state, mod_store::SCOPE_CHAR);
}

int32 lua_store_account(lua_State* state) {
	return lua_store_scoped(state, mod_store::SCOPE_ACCOUNT);
}

void register_store() {
	push_store(L, mod_store::SCOPE_GLOBAL, 0);
	lua_pushcfunction(L, lua_store_limits);
	lua_setfield(L, -2, "limits");
	lua_pushcfunction(L, lua_store_char);
	lua_setfield(L, -2, "char");
	lua_pushcfunction(L, lua_store_account);
	lua_setfield(L, -2, "account");
	lua_setglobal(L, "store");
}

}  // namespace

// ---------------------------------------------------------------------------

void do_init_skill_lua() {
	auto files = file_list();

	if (files.empty())
		return;  // no mod uses Lua: no state, and every hook check is one null test

	L = lua_newstate(limited_alloc, nullptr);
	if (L == nullptr) {
		ShowError("Lua: could not create a Lua state.\n");
		return;
	}

	// The sandbox: arithmetic, strings, tables and utf8. No io, os, package
	// (require) or debug, and they are not even compiled in (lua_vm.cpp).
	luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
	luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
	luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
	luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
	luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
	lua_settop(L, 0);

	lua_register(L, "skill", lua_register_skill);
	lua_register(L, "item", lua_register_item);
	lua_register(L, "const", lua_constant);
	lua_register(L, "log", lua_log);
	lua_register(L, "print", lua_log);
	register_store();
	lua_sethook(L, count_hook, LUA_MASKCOUNT, 1000);

	current_mod = "server";
	budget = LOAD_BUDGET;
	next_load_order = 0;
	if (luaL_dostring(L, PRELUDE) != LUA_OK) {
		ShowError("Lua: prelude failed: %s\n", lua_tostring(L, -1));
		do_final_skill_lua();
		return;
	}

	size_t loaded = 0;

	for (const auto& file : files)
		loaded += run_file(file.second, file.first) ? 1 : 0;

	size_t skill_hooks_total = 0;

	for (const auto& it : hooks_by_name)
		for (int32 i = 0; i < HOOK_MAX; i++)
			skill_hooks_total += it.second.hooks[i].size();

	size_t item_hooks_total = 0;

	for (const auto& it : item_hooks_by_name)
		for (int32 i = 0; i < ITEM_HOOK_MAX; i++)
			item_hooks_total += it.second.hooks[i].size();
	ShowStatus("Lua: loaded %zu of %zu file(s), %zu skill hook(s) across %zu skill(s), %zu item hook(s) across %zu item(s).\n",
		loaded, files.size(), skill_hooks_total, hooks_by_name.size(), item_hooks_total, item_hooks_by_name.size());
	skill_lua_attach();
}

void do_final_skill_lua() {
	hooks_by_id.clear();
	hooks_by_name.clear();
	item_hooks_by_id.clear();
	item_hooks_by_name.clear();
	current_hit = nullptr;
	if (L != nullptr) {
		lua_close(L);
		L = nullptr;
	}
	memory_used = 0;
}

void skill_lua_attach() {
	hooks_by_id.clear();
	if (L == nullptr || hooks_by_name.empty())
		return;

	std::unordered_map<std::string, bool> found;

	for (auto& it : skill_db) {
		std::shared_ptr<s_skill_db>& skill = it.second;
		auto hooks = hooks_by_name.find(skill->name);

		if (hooks == hooks_by_name.end())
			continue;
		found[skill->name] = true;
		hooks_by_id[skill->nameid] = &hooks->second;

		const s_skill_hooks& h = hooks->second;
		bool wants_class = !h.hooks[HOOK_RATIO].empty() || !h.hooks[HOOK_HIT].empty() || !h.hooks[HOOK_ELEMENT].empty();

		if (!wants_class || dynamic_cast<const LuaSkillImpl*>(skill->impl.get()) != nullptr)
			continue;
		if (skill->impl == nullptr) {
			// Its formula is still in battle.cpp's switch, which has no hook.
			ShowWarning("Lua: %s has no skill class in this server, so its ratio, hit and element hooks cannot run (on_hit still does).\n", skill->name);
			continue;
		}
		skill->impl = std::make_unique<LuaSkillImpl>(static_cast<e_skill>(skill->nameid), std::move(skill->impl));
	}

	for (const auto& it : hooks_by_name)
		if (!found.count(it.first))
			ShowWarning("Lua: there is no skill called %s (use the AegisName, like MG_FIREBOLT).\n", it.first.c_str());

	// Resolve each registered item AegisName to its nameid. An item that
	// does not exist is named loudly rather than silently ignored.
	item_hooks_by_id.clear();
	for (auto& it : item_hooks_by_name) {
		std::shared_ptr<item_data> id = item_db.search_aegisname(it.first.c_str());

		if (id == nullptr) {
			ShowWarning("Lua: there is no item called %s (use the AegisName, like VORPAL_BLADE).\n", it.first.c_str());
			continue;
		}
		item_hooks_by_id[id->nameid] = &it.second;
	}
}

namespace {

/// Iterate a unit's equipped items (0 if not a player), and the cards slotted
/// in them, looking up each nameid in the item-hook table. Fire `which` on
/// each match, in priority order within each item. One nameid fires at most
/// once per attack, even if the wearer has several copies equipped or
/// slotted (the same accessory twice, four of one card in a weapon).
void fire_item_hooks(e_item_hook which, const block_list* bl, uint16 skill_id, uint16 skill_lv, const block_list* src, const block_list* target, const s_damage_ctx& dmg) {
	const map_session_data* sd = bl != nullptr ? BL_CAST(BL_PC, bl) : nullptr;

	if (sd == nullptr || item_hooks_by_id.empty())
		return;

	constexpr equip_index slots[] = {
		EQI_HAND_R, EQI_HAND_L, EQI_ARMOR, EQI_SHOES, EQI_GARMENT,
		EQI_HEAD_TOP, EQI_HEAD_MID, EQI_HEAD_LOW, EQI_ACC_L, EQI_ACC_R,
	};
	t_itemid fired[sizeof(slots) / sizeof(slots[0]) * (1 + MAX_SLOTS)] = {};
	size_t fired_n = 0;

	auto fire = [&](t_itemid nameid) {
		if (nameid == 0)
			return;
		for (size_t i = 0; i < fired_n; i++)
			if (fired[i] == nameid)
				return;

		s_item_hooks* hooks = item_hooks_for(nameid);
		if (hooks == nullptr || hooks->hooks[which].empty())
			return;

		fired[fired_n++] = nameid;
		for (s_hook& hook : hooks->hooks[which]) {
			if (hook.ref == LUA_NOREF)
				continue;
			lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
			push_context(skill_id, skill_lv, src, target, &dmg);
			protected_call(hook, hooks->aegis.c_str(), 1, 0);
		}
	};

	for (equip_index e : slots) {
		int16 idx = sd->equip_index[e];
		if (idx < 0)
			continue;
		const item& equipped = sd->inventory.u.items_inventory[idx];
		fire(equipped.nameid);
		// card[0] of a forged or named item holds a marker, not a card.
		if (!itemdb_isspecial(equipped.card[0]))
			for (int32 c = 0; c < MAX_SLOTS; c++)
				fire(equipped.card[c]);
	}
}

}  // namespace

std::unique_ptr<s_skill_lua_hit> skill_lua_on_damage(block_list* src, block_list* target, uint16 skill_id, uint16 skill_lv, int64 damage, int32 attack_type, int32 dmg_lv, bool critical, int32 element) {
	// Shared pending hit, reused by any skill()/item() hook that fires.
	// A re-entry (a hook that somehow triggers another attack) is refused
	// to keep the shared actions list unambiguous.
	if (src == nullptr || target == nullptr || current_hit != nullptr || in_lua_cast)
		return nullptr;

	s_skill_hooks* skill_hooks = skill_id != 0 ? hooks_for(skill_id) : nullptr;
	bool has_skill_hooks = skill_hooks != nullptr && !skill_hooks->hooks[HOOK_ON_HIT].empty();
	bool has_item_hooks = !item_hooks_by_id.empty() && (src->type == BL_PC || target->type == BL_PC);

	if (!has_skill_hooks && !has_item_hooks)
		return nullptr;

	auto hit = std::make_unique<s_skill_lua_hit>();
	const status_data* tstatus = status_get_status_data(*target);

	hit->src_id = src->id;
	hit->target_id = target->id;
	hit->skill_id = skill_id;
	hit->damage = damage;
	hit->race = tstatus->race;
	hit->class_ = tstatus->class_;

	s_damage_ctx dmg = { damage, dmg_lv, element, attack_type, critical };

	current_hit = hit.get();

	// Order: skill on_hit first (same point as before this feature), then
	// attacker's item on_attack, then defender's item on_hit_taken. Each
	// stage runs every registered mod in priority order.
	if (has_skill_hooks) {
		for (s_hook& hook : skill_hooks->hooks[HOOK_ON_HIT]) {
			if (hook.ref == LUA_NOREF)
				continue;
			lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);
			push_context(skill_id, skill_lv, src, target, &dmg);
			protected_call(hook, skill_hooks->skill.c_str(), 1, 0);
		}
	}
	fire_item_hooks(ITEM_HOOK_ON_ATTACK, src, skill_id, skill_lv, src, target, dmg);
	fire_item_hooks(ITEM_HOOK_ON_HIT_TAKEN, target, skill_id, skill_lv, src, target, dmg);

	current_hit = nullptr;

	if (hit->actions.empty())
		return nullptr;
	return hit;
}

/// Cast a skill the way bAutoSpell does (skill_additional_effect): the same
/// "can this be cast here" check, ground-skill limit, item requirements and
/// after-cast delay. No Lua hook runs while it does (in_lua_cast).
void cast_like_autospell(block_list* src, block_list* target, uint16 skill_id, uint16 skill_lv) {
	map_session_data* sd = BL_CAST(BL_PC, src);
	t_tick tick = gettick();

	if (sd != nullptr) {
		sd->state.autocast = 1;
		bool refused = skill_isNotOk(skill_id, *sd);
		sd->state.autocast = 0;
		if (refused)
			return;
	}

	e_cast_type type = skill_get_casttype(skill_id);

	if (type == CAST_GROUND && !skill_pos_maxcount_check(src, target->x, target->y, skill_id, skill_lv, BL_PC, false))
		return;
	if (skill_id == PF_SPIDERWEB)
		type = CAST_GROUND;

	in_lua_cast = true;
	if (sd != nullptr) {
		sd->state.autocast = 1;
		skill_consume_requirement(sd, skill_id, skill_lv, 1);
	}
	switch (type) {
		case CAST_GROUND:
			skill_castend_pos2(src, target->x, target->y, skill_id, skill_lv, tick, 0);
			break;
		case CAST_NODAMAGE:
			skill_castend_nodamage_id(src, target, skill_id, skill_lv, tick, 0);
			break;
		case CAST_DAMAGE:
			skill_castend_damage_id(src, target, skill_id, skill_lv, tick, 0);
			break;
	}
	if (sd != nullptr)
		sd->state.autocast = 0;
	in_lua_cast = false;

	if (unit_data* ud = unit_bl2ud(src); ud != nullptr) {
		int32 delay = skill_delayfix(src, skill_id, skill_lv);

		if (DIFF_TICK(ud->canact_tick, tick + delay) < 0)
			ud->canact_tick = i64max(tick + delay, ud->canact_tick);
	}
}

namespace {

const char* drop_type_name(int32 type) {
	switch (type) {
		case IT_CARD: return "card";
		case IT_WEAPON: return "weapon";
		case IT_ARMOR: return "armor";
		default: return "etc";
	}
}

/// Build c.drops: an array of the mob's stealable drops, in the mob db's
/// own slot order. Each entry carries nameid, rate (out of 10000, after the
/// server multiplier) and the item's type as both booleans (is_card,
/// is_equip, is_etc) and a short string ("card", "weapon", "armor", "etc"),
/// so a hook can filter without consulting the item database itself.
void push_drops(const mob_data& md) {
	int32 drop_i = 0;

	lua_createtable(L, (int32)md.db->dropitem.size(), 0);
	for (const std::shared_ptr<s_mob_drop>& entry : md.db->dropitem) {
		if (entry->steal_protected)
			continue;
		if (!item_db.exists(entry->nameid))
			continue;

		std::shared_ptr<item_data> id = item_db.find(entry->nameid);
		int32 type = id != nullptr ? id->type : -1;

		lua_createtable(L, 0, 6);
		set_int("nameid", entry->nameid);
		set_int("rate", entry->rate);
		lua_pushboolean(L, type == IT_CARD);
		lua_setfield(L, -2, "is_card");
		lua_pushboolean(L, type == IT_WEAPON || type == IT_ARMOR);
		lua_setfield(L, -2, "is_equip");
		lua_pushboolean(L, type != IT_CARD && type != IT_WEAPON && type != IT_ARMOR);
		lua_setfield(L, -2, "is_etc");
		lua_pushstring(L, drop_type_name(type));
		lua_setfield(L, -2, "type");

		lua_rawseti(L, -2, ++drop_i);
	}
}

/// Pull a nameid out of what an on_steal hook returned. The hook may return
/// a drop table (one from c.drops, or any table with a `nameid`), the
/// nameid as an integer, or nil to pass. Returns 0 for nil or an
/// unrecognised shape (and logs the shape).
t_itemid read_chosen_drop(const s_hook& hook) {
	if (lua_isinteger(L, -1))
		return static_cast<t_itemid>(lua_tointeger(L, -1));

	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "nameid");
		t_itemid id = lua_isinteger(L, -1) ? static_cast<t_itemid>(lua_tointeger(L, -1)) : 0;
		lua_pop(L, 1);
		return id;
	}

	if (!lua_isnil(L, -1))
		ShowError("Lua: %s's on_steal hook returned a %s, not a drop, nameid or nil; falling through.\n", hook.mod.c_str(), luaL_typename(L, -1));
	return 0;
}

/// True if `nameid` is a stealable drop (`steal_protected == false` and
/// known to item_db) on this mob, so the hook cannot force an item the
/// server would not otherwise give.
bool is_stealable(const mob_data& md, t_itemid nameid) {
	for (const std::shared_ptr<s_mob_drop>& entry : md.db->dropitem) {
		if (entry->nameid != nameid)
			continue;
		if (entry->steal_protected)
			continue;
		if (!item_db.exists(entry->nameid))
			continue;
		return true;
	}
	return false;
}

} // namespace

t_itemid skill_lua_on_steal(block_list* src, block_list* target, uint16 skill_lv) {
	// in_lua_cast refuses re-entry the same way on_damage does: an item
	// whose on_attack cast of TF_STEAL otherwise reruns the on_steal hook
	// against the Lua state the outer call still owns.
	if (L == nullptr || src == nullptr || target == nullptr || target->type != BL_MOB || in_lua_cast)
		return 0;

	s_skill_hooks* skill_hooks = hooks_for(TF_STEAL);

	if (skill_hooks == nullptr || skill_hooks->hooks[HOOK_ON_STEAL].empty())
		return 0;

	const mob_data& md = *reinterpret_cast<mob_data*>(target);

	for (s_hook& hook : skill_hooks->hooks[HOOK_ON_STEAL]) {
		if (hook.ref == LUA_NOREF)
			continue;

		lua_rawgeti(L, LUA_REGISTRYINDEX, hook.ref);

		lua_createtable(L, 0, 7);
		lua_pushstring(L, skill_get_name(TF_STEAL));
		lua_setfield(L, -2, "skill");
		set_int("skill_id", TF_STEAL);
		set_int("skill_lv", skill_lv);
		push_unit(src);
		lua_setfield(L, -2, "caster");
		push_unit(target);
		lua_setfield(L, -2, "target");
		lua_pushcfunction(L, lua_hit_chance);
		lua_setfield(L, -2, "chance");
		push_drops(md);
		lua_setfield(L, -2, "drops");

		if (!protected_call(hook, skill_hooks->skill.c_str(), 1, 1))
			continue;

		t_itemid chosen = read_chosen_drop(hook);

		lua_pop(L, 1);

		if (chosen == 0)
			continue;  // next hook in the chain

		if (!is_stealable(md, chosen)) {
			ShowWarning("Lua: %s's on_steal hook returned nameid %u, which is not a stealable drop on %s; falling through.\n", hook.mod.c_str(), chosen, md.db->name.c_str());
			continue;
		}
		return chosen;
	}

	return 0;
}

void skill_lua_apply(std::unique_ptr<s_skill_lua_hit>& hit) {
	if (hit == nullptr)
		return;

	block_list* src = map_id2bl(hit->src_id);

	for (const s_skill_lua_action& action : hit->actions) {
		block_list* bl = map_id2bl(action.unit_id);

		switch (action.kind) {
			case SKILL_LUA_DRAIN:
				if (src != nullptr && src->type == BL_PC && hit->damage > 0) {
					block_list* target = map_id2bl(hit->target_id);
					if (target != nullptr)
						battle_drain(reinterpret_cast<map_session_data*>(src), target, hit->damage, 0, hit->race, hit->class_);
				}
				break;
			case SKILL_LUA_HEAL:
				if (bl != nullptr && !status_isdead(*bl))
					status_heal(bl, action.hp, action.sp, 0, 0);
				break;
			case SKILL_LUA_STATUS:
				if (bl != nullptr && !status_isdead(*bl))
					sc_start(src != nullptr ? src : bl, bl, static_cast<sc_type>(action.type), action.rate, action.val1, static_cast<t_tick>(action.duration));
				break;
			case SKILL_LUA_CAST:
				if (src != nullptr && bl != nullptr && !status_isdead(*src) && !status_isdead(*bl))
					cast_like_autospell(src, bl, static_cast<uint16>(action.type), static_cast<uint16>(action.val1));
				break;
			case SKILL_LUA_POLYMORPH:
				if (bl != nullptr && bl->type == BL_MOB && !status_isdead(*bl)) {
					mob_data* md = reinterpret_cast<mob_data*>(bl);
					const status_data* st = status_get_status_data(*bl);

					if (st->class_ != CLASS_BOSS && !status_has_mode(st, MD_STATUSIMMUNE)) {
						int32 class_ = mob_get_random_id(MOBG_BRANCH_OF_DEAD_TREE, RMF_DB_RATE, 0);
						if (class_ != 0 && mobdb_checkid(class_))
							mob_class_change(md, class_);
					}
				}
				break;
		}
	}
	hit.reset();
}
