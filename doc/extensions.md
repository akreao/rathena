# Server extensions

An **extension** is a named, opt-in switch for behaviour that differs from stock rAthena. The code
ships merged and **off**. An operator, or a Ragnarok Offline mod, turns it on, and can set typed
values for it, without editing source. Use one whenever a change to stock behaviour should be
optional. It keeps the fork mergeable with upstream, and it lets a mod depend on the behaviour.

Introduced in #7 (BlaXun); typed values in #10.

## The pieces

| Piece | Where | What it does |
|---|---|---|
| Registry | `db/extension_db.yml` | One entry per extension: `Id`, `Name`, `Description`, `Enabled` (always `false` here), optional `Values` |
| Overrides | `db/import/extension_db.yml` | Turns entries on and sets values. Imported by the base file's `Footer` |
| C++ | `src/map/extensions.hpp` | `extension_enabled(id)`, `extension_int(id, key, stock)`, `extension_string(id, key, stock)` |
| Scripts | `src/map/script.cpp` | `getextension("<id>")` returns 1 or 0; `getextensionvalue("<id>", "<key>"{, <stock>})` |
| In game | `src/map/atcommand.cpp` | `@extensions` lists every extension and its state; `@extensioninfo <id>` shows one in full |

An unknown `Id` reads as **off**: `extension_enabled` returns false and value calls return the
stock value. A typo gets stock behaviour, not a crash, so check the spelling with `@extensions`.

## Adding an extension (in this fork)

1. **Register it** in `db/extension_db.yml`, with `Enabled: false`:
   ```yaml
     - Id: steal_card_roll          # snake_case, short, stable: code and mods use it
       Name: Steal can take cards    # one line, shown by @extensions
       Description: |               # what it changes, and what "off" means; shown by @extensioninfo
         When on, Steal rolls each monster's card slot separately at Values.rate
         (out of 10000). Off is stock rAthena: Steal never takes a card.
       Values:                      # optional; only what the code actually reads
         - Key: rate
           Type: Int                # Int or String
           Default: 100             # used while on and nothing sets Value
           Min: 0
           Max: 10000               # Int only; out-of-range values are capped, with a warning
           Description: Chance out of 10000.
   ```
2. **Gate the code path:**
   ```cpp
   #include "extensions.hpp"
   if (extension_enabled("steal_card_roll")) {
       int64 rate = extension_int("steal_card_roll", "rate", 0);   // 0 = stock
       // ... the new behaviour ...
   }
   ```
   With the extension off, the code must behave exactly as stock rAthena. Read values with the
   *stock* value as the fallback, so an off extension changes nothing.
3. **For NPC scripts,** gate with `if (getextension("steal_card_roll")) { ... }` and read values with
   `getextensionvalue("steal_card_roll", "rate", 0)`. This works like `getbattleflag()`.
4. **Check:** with no override, `@extensions` shows it off and nothing changes. With
   `db/import/extension_db.yml` turning it on, the new path runs, and `@extensioninfo <id>` shows
   the values in use.
5. **Document it in the PR** (base: `ragnarokoffline`). Say what it changes, what off means, its
   values, and that the default is off.

Don't add an extension for an unconditional bug fix; fix the bug. Don't flip `Enabled: true` in the
base file. Don't rename an `Id` once it has shipped, because mods depend on it.

## Turning one on

**On a server:** create `db/import/extension_db.yml`, with the same `Header` as the base file. An
override only needs `Id` and `Enabled`, plus any `Values` with a `Value`:

```yaml
Header:
  Type: EXTENSION_DB
  Version: 1

Body:
  - Id: steal_card_roll
    Enabled: true
    Values:
      - Key: rate
        Value: 250
```

**From a Ragnarok Offline mod:** ship the same file as `db/extension_db.yml` in the mod. The app
merges every enabled mod's `db/` tables into the server's `db/import/`. Entries merge by `Id`, and
values one `Key` at a time, so two mods can set different keys of one extension; for the same key,
the later mod wins. A mod that relies on an extension should say so in its README, and its
`mod.json` should require an app version whose pinned fork has it. Changes take effect when the map
server restarts (the app's **Apply**).

## Existing extensions

`@extensions` in game is the authoritative list. At the time of writing:

| Id | What it does |
|---|---|
| `pc_drop_item_event` | Fires an `OnPCDropItemEvent` script event for each item a monster drops for a player |
| `map_mob_count_rate` | Enables `setmapmobcountrate`, which scales the monster count of every spawn line on one map |
| `eden_paradise_gear` | Replaces the Eden Group gear quests with kRO's 2021 version: new Instructors Ur and Boya and Administrators BK, Michael, Thorn and Emil; the old Eden Team gear NPCs are hidden |
