// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

// The mod store's script commands (see mod_store.hpp). Included by script.cpp,
// which has the buildin machinery. The optional first argument is a scope:
//
//   modstore_set {"<scope>",} "<path>", <value>;              -> 1 or 0
//   modstore_get({"<scope>",} "<path>" {, <default>})         -> value
//   modstore_inc({"<scope>",} "<path>", <by>)                 -> new value
//   modstore_delete({"<scope>",} "<path>")                    -> entries removed
//   modstore_exists({"<scope>",} "<path>")                    -> 1 or 0
//   modstore_keys({"<scope>",} "<path>", <array$>)            -> count
//   modstore_count({"<scope>",} "<path>")                     -> count
//   modstore_top({"<scope>",} "<path>", <n>, <names$>, <values>) -> count
//   modstore_limit({"<scope>"})                               -> bytes allowed
//   modstore_used({"<scope>"})                                -> bytes in use
//
// <scope> is "global" (the default), "account" or "char"; the last two use
// the attached player.

namespace {

/// The document a command targets, from its optional scope argument at
/// `first`. `next` is the index of the argument after the scope, if any.
bool modstore_begin(script_state* st, int32 first, int32 min_after, mod_store::s_target& t, int32& next) {
	mod_store::e_scope scope = mod_store::SCOPE_GLOBAL;
	next = first;
	if (script_hasdata(st, first) && script_isstring(st, first)) {
		int32 after = 0;
		for (int32 i = first + 1; script_hasdata(st, i); ++i)
			after++;
		if (after >= min_after && mod_store::parse_scope(script_getstr(st, first), scope))
			next = first + 1;
	}
	std::string mod = mod_store::mod_of_npc(st->oid);
	// Not script_rid2sd: without a player that ends the whole script, and a
	// store call that can't run should return 0 and let the script carry on.
	map_session_data* sd = st->rid != 0 ? map_id2sd(st->rid) : nullptr;
	if (scope != mod_store::SCOPE_GLOBAL && sd == nullptr) {
		ShowWarning("Mod store: %s: the %s scope needs an attached player.\n", mod.empty() ? "?" : mod.c_str(), mod_store::scope_name(scope));
		return false;
	}
	std::string error;
	if (!mod_store::target(mod, scope, sd, t, error)) {
		ShowWarning("Mod store: %s: %s.\n", mod.empty() ? "?" : mod.c_str(), error.c_str());
		return false;
	}
	if (mod.empty()) {
		ShowWarning("Mod store: only a mod's scripts (npc/mods/<mod>/) can use the store.\n");
		script_reportsrc(st);
		return false;
	}
	return true;
}

/// Write `values` into the array variable at argument `index`.
template <typename T, typename Set>
bool modstore_fill(script_state* st, int32 index, const std::vector<T>& values, Set&& write) {
	script_data* data = script_getdata(st, index);
	if (!data_isreference(data)) {
		ShowError("script:modstore: argument %d is not a variable\n", index - 1);
		script_reportdata(data);
		return false;
	}
	const char* name = reference_getname(data);
	map_session_data* sd = nullptr;
	if (not_server_variable(*name) && !script_rid2sd(sd)) {
		script_reportdata(data);
		return false;
	}
	for (size_t i = 0; i < values.size() && i < SCRIPT_MAX_ARRAYSIZE; ++i)
		write(st, sd, name, static_cast<int32>(i), values[i], reference_getref(data));
	return true;
}

}  // namespace

BUILDIN_FUNC(modstore_set) {
	mod_store::s_target t;
	int32 at = 0;
	if (!modstore_begin(st, 2, 2, t, at)) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}
	mod_store::s_value value;
	if (script_isstring(st, at + 1)) {
		value.is_string = true;
		value.text = script_getstr(st, at + 1);
	} else {
		value.number = script_getnum64(st, at + 1);
	}
	std::string error;
	script_pushint(st, mod_store::set(t, script_getstr(st, at), value, error) ? 1 : 0);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(modstore_get) {
	mod_store::s_target t;
	int32 at = 0;
	mod_store::s_value value;
	bool found = false;
	std::string error;
	if (modstore_begin(st, 2, 1, t, at) && mod_store::get(t, script_getstr(st, at), value, found, error) && found) {
		if (value.is_string)
			script_pushstrcopy(st, value.text.c_str());
		else
			script_pushint64(st, value.number);
		return SCRIPT_CMD_SUCCESS;
	}
	if (!error.empty())
		ShowWarning("Mod store: %s: %s.\n", t.mod.c_str(), error.c_str());
	// Missing: the default, or 0.
	if (script_hasdata(st, at + 1)) {
		if (script_isstring(st, at + 1))
			script_pushstrcopy(st, script_getstr(st, at + 1));
		else
			script_pushint64(st, script_getnum64(st, at + 1));
	} else {
		script_pushint(st, 0);
	}
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(modstore_inc) {
	mod_store::s_target t;
	int32 at = 0;
	int64 result = 0;
	std::string error;
	if (modstore_begin(st, 2, 2, t, at))
		mod_store::inc(t, script_getstr(st, at), script_getnum64(st, at + 1), result, error);
	script_pushint64(st, result);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(modstore_delete) {
	mod_store::s_target t;
	int32 at = 0;
	int32 removed = 0;
	std::string error;
	if (modstore_begin(st, 2, 1, t, at))
		mod_store::remove(t, script_getstr(st, at), removed, error);
	script_pushint(st, removed);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(modstore_exists) {
	mod_store::s_target t;
	int32 at = 0;
	bool found = false;
	std::string error;
	if (modstore_begin(st, 2, 1, t, at))
		mod_store::exists(t, script_getstr(st, at), found, error);
	script_pushint(st, found ? 1 : 0);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(modstore_keys) {
	mod_store::s_target t;
	int32 at = 0;
	std::vector<std::string> names;
	std::string error;
	if (!modstore_begin(st, 2, 2, t, at) || !mod_store::keys(t, script_getstr(st, at), names, error)
		|| !modstore_fill(st, at + 1, names, [](script_state* s, map_session_data* sd, const char* name, int32 i, const std::string& v, reg_db* ref) {
			setd_sub_str(s, sd, name, i, v.c_str(), ref);
		})) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}
	script_pushint(st, static_cast<int32>(names.size()));
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(modstore_count) {
	mod_store::s_target t;
	int32 at = 0;
	std::vector<std::string> names;
	std::string error;
	if (modstore_begin(st, 2, 1, t, at))
		mod_store::keys(t, script_getstr(st, at), names, error);
	script_pushint(st, static_cast<int32>(names.size()));
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(modstore_top) {
	mod_store::s_target t;
	int32 at = 0;
	std::vector<std::pair<std::string, int64>> rows;
	std::string error;
	if (!modstore_begin(st, 2, 4, t, at)
		|| !mod_store::top(t, script_getstr(st, at), static_cast<size_t>(std::max<int32>(script_getnum(st, at + 1), 0)), rows, error)) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}
	std::vector<std::string> names;
	std::vector<int64> values;
	for (const auto& row : rows) {
		names.push_back(row.first);
		values.push_back(row.second);
	}
	bool ok = modstore_fill(st, at + 2, names, [](script_state* s, map_session_data* sd, const char* name, int32 i, const std::string& v, reg_db* ref) {
			setd_sub_str(s, sd, name, i, v.c_str(), ref);
		})
		&& modstore_fill(st, at + 3, values, [](script_state* s, map_session_data* sd, const char* name, int32 i, const int64& v, reg_db* ref) {
			setd_sub_num(s, sd, name, i, v, ref);
		});
	script_pushint(st, ok ? static_cast<int32>(rows.size()) : 0);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(modstore_limit) {
	mod_store::e_scope scope = mod_store::SCOPE_GLOBAL;
	if (script_hasdata(st, 2))
		mod_store::parse_scope(script_getstr(st, 2), scope);
	script_pushint64(st, static_cast<int64>(mod_store::limit(scope)));
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(modstore_used) {
	mod_store::s_target t;
	int32 at = 0;
	if (!modstore_begin(st, 2, 0, t, at)) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}
	std::string error;
	bool ignored = false;
	// Loads the document if this is its first use, so the answer is real.
	mod_store::exists(t, "_", ignored, error);
	script_pushint64(st, static_cast<int64>(mod_store::used(t)));
	return SCRIPT_CMD_SUCCESS;
}
