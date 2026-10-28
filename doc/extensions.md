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

## The other gate: Lua hooks

Where a mod should bring the numbers itself, not just switch a behaviour on (a formula, a curve, a
per-skill rule), the fork offers a Lua hook instead. A hook is off until a mod's Lua file registers
it (`db/import/lua/`); with none registered the server is stock. The full reference, with examples,
is the comment at the top of `src/map/skill_lua.hpp`. Every hook:

| Registered with | Hook | What it decides |
|---|---|---|
| `skill(name, {...})` | `ratio`, `hit`, `element` | a skill's damage ratio, accuracy and element |
| | `on_hit` | actions after each hit: drain, heal, status, polymorph, cast |
| | `on_steal` | which drop `TF_STEAL` takes |
| | `level`, `job_level` | the base or job level a skill's formula reads for a player |
| | `heal` | what Heal, Highness Heal or Coluceo Heal restores |
| | `custom`, `replace`, `stock_checks` | a skill of a mod's own, a stock one reworked on its id, the server's own checks for it off |
| | `before_cast`, `on_cast`, `can_cast`, `cost`, `range`, `hits`, `critical` | when a skill may be used, what it costs, how far it reaches, its hits and criticals |
| | `bonus`, `stats`, `on_attack`, `on_hit_taken`, `damage_dealt`, `damage_taken`, `on_kill`, `on_death`, `on_skill`, `tick`, `craft`, `on_craft`, `on_use` | a passive's effect, for every player who learned it |
| `item(aegis, {...})` | `on_attack`, `on_hit_taken` | the wearer attacked, or was attacked |
| | `on_use` | a consumable is used |
| `status(name, {...})` | the passive hooks, `on_start`, `on_end` | a status of a mod's own: stacks, decay, an icon, `persist` |
| `status("SC_...", {...})`, `status("*", {...})` | `on_start`, `on_end`, `chance` | a server status starts (its duration, or refused), ends, or rolls its chance |
| `player({...})` | `base_stat` | what a player's base stat counts for (a soft cap, for instance) |
| | `cast_dex` | the DEX the cast-time and skill-delay formulas read for a player |

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
| `attendance_repeat` | Attendance rewards start over at day 1 after the last day of the period |
| `attendance_script_rewards` | Enables `setattendancereward`, which changes one day's reward of the running attendance period |
| `stylist_script_costs` | Enables `setstylistcost`, which sets the stylist window's cost of one look in Zeny or a coupon, and `setstylistpayment`, which picks how a player pays; an order is bought whole or not at all |
| `channel_log` | Writes each message players send in one chat channel (`#discord` by default) to the map server's log, for a program that relays it, such as a Discord bridge |
