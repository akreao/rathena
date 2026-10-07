// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

// @modstore: a mod's client UI reading the mod store (see mod_store.hpp).
// Included by atcommand.cpp. The client sends it as a server request and
// collects the answer from @@reply lines, which never show in chat:
//
//   @modstore <id> <mod> <scope> <path>
//   @@reply <id> <part>/<parts> <text>
//
// <text>, joined, is "1 <json>" or "0 <reason>". Only paths under `client`
// can be read, and the account and char scopes are the player's own: a mod
// keeps what its UI may show under `client`, and the rest stays on the server.

namespace {

constexpr size_t MODSTORE_REPLY_PART = 180;
constexpr size_t MODSTORE_REPLY_PARTS = 200;

void modstore_reply(map_session_data& sd, const char* id, const std::string& text) {
	size_t parts = std::max<size_t>((text.size() + MODSTORE_REPLY_PART - 1) / MODSTORE_REPLY_PART, 1);
	for (size_t i = 0; i < parts; ++i) {
		std::string line = std::string("@@reply ") + id + " " + std::to_string(i + 1) + "/" + std::to_string(parts) + " "
			+ text.substr(i * MODSTORE_REPLY_PART, MODSTORE_REPLY_PART);
		clif_displaymessage(sd.fd, line.c_str());
	}
}

bool modstore_mod_name(const char* name) {
	size_t len = strlen(name);
	if (len == 0 || len > 64)
		return false;
	for (size_t i = 0; i < len; ++i) {
		char c = name[i];
		if (!ISALNUM(c) && c != '_' && c != '-')
			return false;
	}
	return true;
}

}  // namespace

ACMD_FUNC(modstore) {
	nullpo_retr(-1, sd);
	char id[16], mod[65], scope_name[16], path[256];
	if (message == nullptr || sscanf(message, "%15s %64s %15s %255s", id, mod, scope_name, path) != 4) {
		clif_displaymessage(fd, "Usage: @modstore <id> <mod> <global|account|char> <client path>");
		return -1;
	}
	for (const char* c = id; *c; ++c) {
		if (!ISDIGIT(*c)) {
			clif_displaymessage(fd, "Usage: @modstore <id> <mod> <global|account|char> <client path>");
			return -1;
		}
	}
	mod_store::e_scope scope;
	if (!modstore_mod_name(mod) || !mod_store::parse_scope(scope_name, scope)) {
		modstore_reply(*sd, id, "0 no such mod or scope");
		return 0;
	}
	const std::string wanted = path;
	if (wanted != "client" && wanted.compare(0, 7, "client.") != 0) {
		modstore_reply(*sd, id, "0 a client can only read paths under 'client'");
		return 0;
	}
	mod_store::s_target t;
	std::vector<std::pair<std::string, mod_store::s_value>> entries;
	std::string error;
	if (!mod_store::target(mod, scope, sd, t, error) || !mod_store::read(t, wanted, entries, error)) {
		modstore_reply(*sd, id, "0 " + error);
		return 0;
	}
	std::string json = entries.empty() ? "null" : mod_store::to_json(entries);
	if (json.size() > MODSTORE_REPLY_PART * MODSTORE_REPLY_PARTS - 2) {
		modstore_reply(*sd, id, "0 " + std::to_string(json.size()) + " bytes is too much to send; read a smaller path");
		return 0;
	}
	modstore_reply(*sd, id, "1 " + json);
	return 0;
}
