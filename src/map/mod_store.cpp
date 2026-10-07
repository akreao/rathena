// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "mod_store.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

#include <common/showmsg.hpp>
#include <common/sql.hpp>
#include <common/timer.hpp>

#include "battle.hpp"
#include "map.hpp"
#include "npc.hpp"
#include "pc.hpp"

namespace mod_store {

namespace {

constexpr const char* TABLE = "mod_store";
constexpr t_tick SAVE_INTERVAL = 60 * 1000;
constexpr size_t MAX_PATH_LENGTH = 255;
constexpr size_t MAX_MOD = 64;

struct s_doc_key {
	std::string mod;
	e_scope scope;
	uint32 owner;

	bool operator<(const s_doc_key& other) const {
		return std::tie(mod, scope, owner) < std::tie(other.mod, other.scope, other.owner);
	}
};

struct s_doc {
	std::map<std::string, s_value> values;
	size_t bytes = 0;
	std::set<std::string> dirty;    ///< changed since the last save
	std::set<std::string> removed;  ///< deleted since the last save
};

std::map<s_doc_key, s_doc> docs;
bool ready = false;

size_t entry_bytes(const std::string& path, const s_value& value) {
	return path.size() + (value.is_string ? value.text.size() : sizeof(int64));
}

s_doc_key key_of(const s_target& t) {
	return { t.mod, t.scope, t.owner };
}

void warn(const s_target& t, const std::string& path, const std::string& error) {
	ShowWarning("Mod store: %s (%s%s): '%s': %s\n", t.mod.c_str(), scope_name(t.scope),
		t.scope == SCOPE_GLOBAL ? "" : (" " + std::to_string(t.owner)).c_str(), path.c_str(), error.c_str());
}

/// Letters, digits and '_' in each segment, joined by '.'; "" is the root
/// (allowed where `allow_root`).
bool valid_path(const std::string& path, bool allow_root, std::string& error) {
	if (path.empty()) {
		if (allow_root)
			return true;
		error = "a path is required";
		return false;
	}
	if (path.size() > MAX_PATH_LENGTH) {
		error = "the path is longer than " + std::to_string(MAX_PATH_LENGTH) + " characters";
		return false;
	}
	int32 depth = 1;
	bool segment_empty = true;
	for (char c : path) {
		if (c == '.') {
			if (segment_empty)
				break;
			segment_empty = true;
			depth++;
			continue;
		}
		if (!ISALNUM(c) && c != '_') {
			error = "paths are letters, digits and '_', joined by '.'";
			return false;
		}
		segment_empty = false;
	}
	if (segment_empty) {
		error = "a path segment is empty";
		return false;
	}
	if (depth > depth_limit()) {
		error = "the path is deeper than " + std::to_string(depth_limit()) + " segments";
		return false;
	}
	if (path == "global" || path == "account" || path == "char") {
		error = "a scope name is not a path";
		return false;
	}
	return true;
}

bool load(const s_doc_key& key, s_doc& doc) {
	SqlStmt stmt{ *mmysql_handle };
	char path[MAX_PATH_LENGTH + 1];
	char kind[2];
	int64 number = 0;
	std::vector<char> text(std::max<size_t>(value_limit(), 1) + 1);
	uint32 path_len = 0, text_len = 0;
	int8 text_null = 0;

	if (SQL_SUCCESS != stmt.Prepare("SELECT `path`, `kind`, `num`, `str` FROM `%s` WHERE `mod_name` = ? AND `scope` = %d AND `owner` = %u",
			TABLE, static_cast<int32>(key.scope), key.owner)
		|| SQL_SUCCESS != stmt.BindParam(0, SQLDT_STRING, const_cast<char*>(key.mod.c_str()), key.mod.size())
		|| SQL_SUCCESS != stmt.Execute()
		|| SQL_SUCCESS != stmt.BindColumn(0, SQLDT_STRING, path, sizeof(path), &path_len)
		|| SQL_SUCCESS != stmt.BindColumn(1, SQLDT_STRING, kind, sizeof(kind))
		|| SQL_SUCCESS != stmt.BindColumn(2, SQLDT_INT64, &number)
		|| SQL_SUCCESS != stmt.BindColumn(3, SQLDT_BLOB, text.data(), text.size(), &text_len, &text_null)) {
		SqlStmt_ShowDebug(stmt);
		return false;
	}
	while (SQL_SUCCESS == stmt.NextRow()) {
		s_value value;
		value.is_string = kind[0] == 's';
		value.number = number;
		if (value.is_string && !text_null)
			value.text.assign(text.data(), std::min<size_t>(text_len, text.size()));
		std::string p(path, path_len);
		doc.bytes += entry_bytes(p, value);
		doc.values.emplace(std::move(p), std::move(value));
	}
	return true;
}

/// The document, loaded on first use. nullptr if it could not be loaded.
s_doc* doc_for(const s_target& t, std::string& error) {
	if (!ready) {
		error = "the store is not ready";
		return nullptr;
	}
	if (t.mod.empty()) {
		error = "only a mod's scripts and hooks can use the store";
		return nullptr;
	}
	s_doc_key key = key_of(t);
	auto it = docs.find(key);
	if (it != docs.end())
		return &it->second;
	s_doc doc;
	if (!load(key, doc)) {
		error = "the store could not be read";
		return nullptr;
	}
	return &docs.emplace(std::move(key), std::move(doc)).first->second;
}

void save(const s_doc_key& key, s_doc& doc) {
	for (const std::string& path : doc.removed) {
		if (doc.values.count(path))
			continue;  // written again since; the REPLACE below covers it
		SqlStmt stmt{ *mmysql_handle };
		if (SQL_SUCCESS != stmt.Prepare("DELETE FROM `%s` WHERE `mod_name` = ? AND `scope` = %d AND `owner` = %u AND `path` = ?",
				TABLE, static_cast<int32>(key.scope), key.owner)
			|| SQL_SUCCESS != stmt.BindParam(0, SQLDT_STRING, const_cast<char*>(key.mod.c_str()), key.mod.size())
			|| SQL_SUCCESS != stmt.BindParam(1, SQLDT_STRING, const_cast<char*>(path.c_str()), path.size())
			|| SQL_SUCCESS != stmt.Execute())
			SqlStmt_ShowDebug(stmt);
	}
	for (const std::string& path : doc.dirty) {
		auto it = doc.values.find(path);
		if (it == doc.values.end())
			continue;
		const s_value& value = it->second;
		char kind[2] = { value.is_string ? 's' : 'i', '\0' };
		int64 number = value.is_string ? 0 : value.number;
		SqlStmt stmt{ *mmysql_handle };
		if (SQL_SUCCESS != stmt.Prepare("REPLACE INTO `%s` (`mod_name`, `scope`, `owner`, `path`, `kind`, `num`, `str`) VALUES (?, %d, %u, ?, ?, ?, ?)",
				TABLE, static_cast<int32>(key.scope), key.owner)
			|| SQL_SUCCESS != stmt.BindParam(0, SQLDT_STRING, const_cast<char*>(key.mod.c_str()), key.mod.size())
			|| SQL_SUCCESS != stmt.BindParam(1, SQLDT_STRING, const_cast<char*>(path.c_str()), path.size())
			|| SQL_SUCCESS != stmt.BindParam(2, SQLDT_STRING, kind, 1)
			|| SQL_SUCCESS != stmt.BindParam(3, SQLDT_INT64, &number, sizeof(number))
			|| SQL_SUCCESS != stmt.BindParam(4, value.is_string ? SQLDT_BLOB : SQLDT_NULL, const_cast<char*>(value.text.data()), value.text.size())
			|| SQL_SUCCESS != stmt.Execute())
			SqlStmt_ShowDebug(stmt);
	}
	doc.removed.clear();
	doc.dirty.clear();
}

void save_all() {
	for (auto& [key, doc] : docs)
		if (!doc.dirty.empty() || !doc.removed.empty())
			save(key, doc);
}

TIMER_FUNC(save_timer) {
	save_all();
	return 0;
}

/// Children of `path` ("" = root): the next segment of every key under it.
/// Keys sort with '.' (0x2E) before every character a segment may hold, so a
/// child's keys are contiguous and one pass finds each child once.
template <typename F>
void each_child(const s_doc& doc, const std::string& path, F&& visit) {
	const std::string prefix = path.empty() ? "" : path + ".";
	std::string last;
	for (auto it = doc.values.lower_bound(prefix); it != doc.values.end(); ++it) {
		const std::string& k = it->first;
		if (k.compare(0, prefix.size(), prefix) != 0)
			break;
		if (k.size() == prefix.size())
			continue;
		size_t end = k.find('.', prefix.size());
		std::string child = k.substr(prefix.size(), end == std::string::npos ? std::string::npos : end - prefix.size());
		if (child != last) {
			visit(child);
			last = child;
		}
	}
}

}  // namespace

bool parse_scope(const char* name, e_scope& out) {
	if (name == nullptr)
		return false;
	if (!strcmp(name, "global")) {
		out = SCOPE_GLOBAL;
		return true;
	}
	if (!strcmp(name, "account")) {
		out = SCOPE_ACCOUNT;
		return true;
	}
	if (!strcmp(name, "char")) {
		out = SCOPE_CHAR;
		return true;
	}
	return false;
}

const char* scope_name(e_scope scope) {
	switch (scope) {
		case SCOPE_ACCOUNT: return "account";
		case SCOPE_CHAR: return "char";
		default: return "global";
	}
}

std::string mod_of_npc(int32 npc_id) {
	npc_data* nd = map_id2nd(npc_id);
	// A duplicate made for an instance is parsed from "INSTANCING", not from
	// the mod's file, so it belongs to whatever its original belongs to.
	for (int32 hops = 0; nd != nullptr && nd->src_id != 0 && hops < 4; ++hops)
		nd = map_id2nd(nd->src_id);
	if (nd == nullptr || nd->path == nullptr)
		return "";
	// Mods' scripts load from npc/mods/<mod>/... (the app's mod build). A file
	// directly in npc/mods/ belongs to the app, not to a mod.
	const std::string path = nd->path;
	const std::string marker = "npc/mods/";
	size_t at = path.find(marker);
	if (at == std::string::npos)
		return "";
	size_t start = at + marker.size();
	size_t end = path.find('/', start);
	if (end == std::string::npos || end == start || end - start > MAX_MOD)
		return "";
	return path.substr(start, end - start);
}

bool target(const std::string& mod, e_scope scope, map_session_data* sd, s_target& out, std::string& error) {
	out.mod = mod;
	out.scope = scope;
	out.owner = 0;
	if (scope == SCOPE_GLOBAL)
		return true;
	if (sd == nullptr) {
		error = std::string("the ") + scope_name(scope) + " scope needs a player";
		return false;
	}
	out.owner = scope == SCOPE_CHAR ? sd->status.char_id : sd->status.account_id;
	return true;
}

size_t limit(e_scope scope) {
	switch (scope) {
		case SCOPE_ACCOUNT: return static_cast<size_t>(battle_config.mod_store_account_bytes);
		case SCOPE_CHAR: return static_cast<size_t>(battle_config.mod_store_char_bytes);
		default: return static_cast<size_t>(battle_config.mod_store_global_bytes);
	}
}

size_t value_limit() {
	return static_cast<size_t>(battle_config.mod_store_value_bytes);
}

int32 depth_limit() {
	return battle_config.mod_store_depth;
}

size_t used(const s_target& t) {
	auto it = docs.find(key_of(t));
	return it == docs.end() ? 0 : it->second.bytes;
}

bool get(const s_target& t, const std::string& path, s_value& out, bool& found, std::string& error) {
	found = false;
	if (!valid_path(path, false, error))
		return false;
	s_doc* doc = doc_for(t, error);
	if (doc == nullptr)
		return false;
	auto it = doc->values.find(path);
	if (it != doc->values.end()) {
		out = it->second;
		found = true;
	}
	return true;
}

bool set(const s_target& t, const std::string& path, const s_value& value, std::string& error) {
	if (!valid_path(path, false, error)) {
		warn(t, path, error);
		return false;
	}
	if (value.is_string && value.text.size() > value_limit()) {
		error = "the value is longer than the limit of " + std::to_string(value_limit()) + " bytes";
		warn(t, path, error);
		return false;
	}
	s_doc* doc = doc_for(t, error);
	if (doc == nullptr) {
		warn(t, path, error);
		return false;
	}
	// A tree, so it reads back as the table or object it was written as: a
	// path holds a value or entries under it, never both.
	for (size_t dot = path.find('.'); dot != std::string::npos; dot = path.find('.', dot + 1)) {
		if (doc->values.count(path.substr(0, dot))) {
			error = "'" + path.substr(0, dot) + "' holds a value, so nothing can go under it";
			warn(t, path, error);
			return false;
		}
	}
	const std::string below = path + ".";
	auto child = doc->values.lower_bound(below);
	if (child != doc->values.end() && child->first.compare(0, below.size(), below) == 0) {
		error = "it has entries under it; delete them first";
		warn(t, path, error);
		return false;
	}
	auto it = doc->values.find(path);
	size_t before = it == doc->values.end() ? 0 : entry_bytes(path, it->second);
	size_t after = doc->bytes - before + entry_bytes(path, value);
	// A write that doesn't grow the document always works, so data from a
	// release with a higher limit stays editable after it is lowered.
	if (after > doc->bytes && after > limit(t.scope)) {
		error = "the " + std::string(scope_name(t.scope)) + " store would use " + std::to_string(after) + " bytes, over its limit of " + std::to_string(limit(t.scope));
		warn(t, path, error);
		return false;
	}
	doc->values[path] = value;
	doc->bytes = after;
	doc->dirty.insert(path);
	return true;
}

bool inc(const s_target& t, const std::string& path, int64 by, int64& result, std::string& error) {
	s_value current;
	bool found = false;
	if (!get(t, path, current, found, error)) {
		warn(t, path, error);
		return false;
	}
	if (found && current.is_string) {
		error = "it holds a string, not a number";
		warn(t, path, error);
		return false;
	}
	s_value next;
	next.number = (found ? current.number : 0) + by;
	if (!set(t, path, next, error))
		return false;
	result = next.number;
	return true;
}

bool read(const s_target& t, const std::string& path, std::vector<std::pair<std::string, s_value>>& out, std::string& error) {
	out.clear();
	if (!valid_path(path, true, error))
		return false;
	const bool was_loaded = docs.count(key_of(t)) != 0;
	s_doc* doc = doc_for(t, error);
	if (doc == nullptr)
		return false;
	auto exact = path.empty() ? doc->values.end() : doc->values.find(path);
	if (exact != doc->values.end()) {
		out.emplace_back("", exact->second);
	} else {
		const std::string prefix = path.empty() ? "" : path + ".";
		for (auto it = doc->values.lower_bound(prefix); it != doc->values.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it)
			out.emplace_back(it->first.substr(prefix.size()), it->second);
	}
	// A client can name any mod; don't keep a document that was loaded only
	// to find it empty.
	if (!was_loaded && doc->values.empty() && doc->dirty.empty() && doc->removed.empty())
		docs.erase(key_of(t));
	return true;
}

namespace {

struct s_json_node {
	const s_value* value = nullptr;
	std::map<std::string, s_json_node> children;
};

void json_string(std::string& out, const std::string& text) {
	static const char* hex = "0123456789abcdef";
	out += '"';
	for (unsigned char c : text) {
		if (c == '"' || c == '\\') {
			out += '\\';
			out += static_cast<char>(c);
		} else if (c < 0x20 || c >= 0x7F) {
			out += "\\u00";
			out += hex[c >> 4];
			out += hex[c & 15];
		} else {
			out += static_cast<char>(c);
		}
	}
	out += '"';
}

void json_node(std::string& out, const s_json_node& node) {
	if (node.value != nullptr) {
		if (node.value->is_string)
			json_string(out, node.value->text);
		else
			out += std::to_string(node.value->number);
		return;
	}
	out += '{';
	bool first = true;
	for (const auto& child : node.children) {
		if (!first)
			out += ',';
		first = false;
		json_string(out, child.first);
		out += ':';
		json_node(out, child.second);
	}
	out += '}';
}

}  // namespace

std::string to_json(const std::vector<std::pair<std::string, s_value>>& entries) {
	s_json_node root;
	for (const auto& entry : entries) {
		if (entry.first.empty()) {
			root.value = &entry.second;
			continue;
		}
		s_json_node* node = &root;
		size_t start = 0;
		for (;;) {
			size_t dot = entry.first.find('.', start);
			node = &node->children[entry.first.substr(start, dot == std::string::npos ? std::string::npos : dot - start)];
			if (dot == std::string::npos)
				break;
			start = dot + 1;
		}
		node->value = &entry.second;
	}
	std::string out;
	json_node(out, root);
	return out;
}

bool remove(const s_target& t, const std::string& path, int32& removed, std::string& error) {
	removed = 0;
	if (!valid_path(path, false, error)) {
		warn(t, path, error);
		return false;
	}
	s_doc* doc = doc_for(t, error);
	if (doc == nullptr) {
		warn(t, path, error);
		return false;
	}
	const std::string prefix = path + ".";
	for (auto it = doc->values.lower_bound(path); it != doc->values.end();) {
		const std::string& k = it->first;
		if (k != path && k.compare(0, prefix.size(), prefix) != 0)
			break;
		doc->bytes -= entry_bytes(k, it->second);
		doc->removed.insert(k);
		doc->dirty.erase(k);
		it = doc->values.erase(it);
		removed++;
	}
	return true;
}

bool exists(const s_target& t, const std::string& path, bool& out, std::string& error) {
	out = false;
	if (!valid_path(path, false, error))
		return false;
	s_doc* doc = doc_for(t, error);
	if (doc == nullptr)
		return false;
	auto it = doc->values.lower_bound(path);
	if (it == doc->values.end())
		return true;
	out = it->first == path || it->first.compare(0, path.size() + 1, path + ".") == 0;
	return true;
}

bool keys(const s_target& t, const std::string& path, std::vector<std::string>& out, std::string& error) {
	out.clear();
	if (!valid_path(path, true, error))
		return false;
	s_doc* doc = doc_for(t, error);
	if (doc == nullptr)
		return false;
	each_child(*doc, path, [&](const std::string& child) { out.push_back(child); });
	return true;
}

bool top(const s_target& t, const std::string& path, size_t count, std::vector<std::pair<std::string, int64>>& out, std::string& error) {
	out.clear();
	if (!valid_path(path, true, error))
		return false;
	s_doc* doc = doc_for(t, error);
	if (doc == nullptr)
		return false;
	const std::string prefix = path.empty() ? "" : path + ".";
	each_child(*doc, path, [&](const std::string& child) {
		auto it = doc->values.find(prefix + child);
		if (it != doc->values.end() && !it->second.is_string)
			out.emplace_back(child, it->second.number);
	});
	std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
	if (out.size() > count)
		out.resize(count);
	return true;
}

void logout(map_session_data& sd) {
	for (auto it = docs.begin(); it != docs.end();) {
		const s_doc_key& key = it->first;
		bool mine = (key.scope == SCOPE_CHAR && key.owner == sd.status.char_id)
			|| (key.scope == SCOPE_ACCOUNT && key.owner == sd.status.account_id);
		if (!mine) {
			++it;
			continue;
		}
		save(key, it->second);
		it = docs.erase(it);
	}
}

void init() {
	// The table is in sql-files/main.sql (and upgrade_20261005.sql); the map
	// server's login only needs to read and write it. The app's mod reader has
	// no rights on it, so no mod can read another mod's store through query_sql.
	if (SQL_ERROR == Sql_Query(mmysql_handle, "SELECT 1 FROM `%s` LIMIT 0", TABLE)) {
		Sql_ShowDebug(mmysql_handle);
		ShowError("Mod store: the `%s` table is missing (sql-files/upgrades/upgrade_20261005.sql); mods cannot keep data.\n", TABLE);
		return;
	}
	add_timer_func_list(save_timer, "mod_store_save_timer");
	add_timer_interval(gettick() + SAVE_INTERVAL, save_timer, 0, 0, SAVE_INTERVAL);
	ready = true;
	ShowStatus("Mod store ready: %u bytes per mod (global), %u per account, %u per character.\n",
		battle_config.mod_store_global_bytes, battle_config.mod_store_account_bytes, battle_config.mod_store_char_bytes);
}

void final() {
	if (ready)
		save_all();
	docs.clear();
	ready = false;
}

}  // namespace mod_store
