// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#ifndef MOD_STORE_HPP
#define MOD_STORE_HPP

#include <string>
#include <utility>
#include <vector>

#include <common/cbasetypes.hpp>

class map_session_data;

/// The mod store: data a mod keeps for itself, written through the store and
/// never through SQL (scripts' query_sql can be given a read-only login).
///
/// Each (mod, scope, owner) is one document: a sorted map from dotted paths
/// ("market.items.501.price") to an integer or a string. Documents are loaded
/// from the `mod_store` table on first use and kept in memory; changes are
/// saved on a timer, when the owning player logs out, and at shutdown, so a
/// read or write never waits for the database after the first load.
///
/// Scopes: global (one per mod), account and char (one per player, deleted
/// with the character). A mod's namespace comes from where its code was loaded
/// from -- npc/mods/<mod>/ or the Lua hook's mod -- never from a name it passes.
///
/// Limits are battle settings (mod_store_*), written by the app at every
/// start, so a release can raise them without a code change.
namespace mod_store {

enum e_scope : uint8 {
	SCOPE_GLOBAL = 0,
	SCOPE_ACCOUNT = 1,
	SCOPE_CHAR = 2,
};

struct s_value {
	bool is_string = false;
	int64 number = 0;
	std::string text;
};

/// Which document an operation works on.
struct s_target {
	std::string mod;
	e_scope scope = SCOPE_GLOBAL;
	uint32 owner = 0;  ///< account_id or char_id; 0 for global
};

void init();
void final();

/// Save and forget a player's account and char documents. Called when the
/// player leaves the map server.
void logout(map_session_data& sd);

/// "global", "account" or "char".
bool parse_scope(const char* name, e_scope& out);
const char* scope_name(e_scope scope);

/// The mod an NPC script belongs to (the folder under npc/mods/), or "" for
/// any script that isn't a mod's.
std::string mod_of_npc(int32 npc_id);

/// The document `scope` names for `sd` (nullptr for global). An empty error
/// means success.
bool target(const std::string& mod, e_scope scope, map_session_data* sd, s_target& out, std::string& error);

// Operations. Each returns false and sets `error` on failure, and writes a
// warning to the log naming the mod, the scope and the path.
bool get(const s_target& t, const std::string& path, s_value& out, bool& found, std::string& error);
bool set(const s_target& t, const std::string& path, const s_value& value, std::string& error);
bool inc(const s_target& t, const std::string& path, int64 by, int64& result, std::string& error);
/// Removes the path and everything under it; `removed` counts entries.
bool remove(const s_target& t, const std::string& path, int32& removed, std::string& error);
bool exists(const s_target& t, const std::string& path, bool& out, std::string& error);
/// The names of `path`'s children, in order ("" is the document's root).
bool keys(const s_target& t, const std::string& path, std::vector<std::string>& out, std::string& error);
/// `path`'s children that hold an integer, highest first, at most `limit`.
bool top(const s_target& t, const std::string& path, size_t limit, std::vector<std::pair<std::string, int64>>& out, std::string& error);

/// `path` and everything under it ("" = the whole document), named relative
/// to `path` ("" for a value at `path` itself), in order. A path holds either
/// a value or entries under it, never both.
bool read(const s_target& t, const std::string& path, std::vector<std::pair<std::string, s_value>>& out, std::string& error);
/// What read() returned, as JSON: a value, or an object of objects. Bytes
/// outside printable ASCII are written as \u00XX, one per byte.
std::string to_json(const std::vector<std::pair<std::string, s_value>>& entries);
/// Bytes the document uses, and the most it may (from the battle settings).
size_t used(const s_target& t);
size_t limit(e_scope scope);
size_t value_limit();
int32 depth_limit();

}  // namespace mod_store

#endif /* MOD_STORE_HPP */
