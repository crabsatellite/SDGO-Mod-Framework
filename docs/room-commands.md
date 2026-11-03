# Combined room-title commands

[简体中文](room-commands.zh-CN.md)

AI and random-loadout Mods share the same command grammar. Separate commands with
spaces; names are case-insensitive. Each Mod reads its own command without changing the title.

| Title | Effect |
|---|---|
| `!ai3 lv4 !rand` | Request three level-4 enemies in PvP or wingmen in missions; randomize humans each round |
| `!rand !ai e2 a1 lv4` | Two enemy AI and one ally from the level-4 pool in PvP; randomize humans |
| `!ai e2 a1 lv4 !rand` | Same as the previous example |
| `!ai enemy !rand` | Keep human teams; fill vacancies with enemy AI only; randomize human units |
| `!ai fill !rand` | Fill both teams around existing humans and randomize human units |
| `!ai coop !rand` | Group humans with the host at start; fill opponents only and randomize human units |
| `!ai coop fill` | Group humans and fill both sides; three humans in an eight-player room get 4v4 |
| `!ai coop e4 a0` | Group humans and add four opponents only; three humans get 3v4 |
| `!ai e5 a0 !rand` | Add exactly five enemy AI with no allied AI; randomize human units |

Existing team/free-slot limits still apply. Keep `e2`, `a1` and `lv4` after `!ai`,
before the next `!command`. In `!ai !rand lv4`, `lv4` belongs to `!rand` and does
not change the AI level. Unknown `!commands` also end the preceding argument span.
The first valid occurrence wins. `!aim`, `room!ai3`, `!ai3x` and `!rand!ai3` are not `!ai`.

In PvP, enemy/ally mean opposite/alongside the host after human team selection.
Human policy and AI population are independent: default or explicit `versus`
preserves native manual/random teams; `coop` groups all humans on the host's
selected side at start and overrides native random teams. `eN` / `aN`
count additional AI; `e*` / `a*` fill vacancies. The current room limit is eight
participants; unsatisfiable exact counts cancel the start. For friends versus
computers, use `!ai coop` for opponents-only fill, such as 1v7, 3v5 or 4v4.
`coop fill` fills both sides; `coop e4 a0` fixes four opponents. `!rand` chooses
owned units, not teams. Choose either `coop` or `versus`. With no population
argument, they default to opponents-only or both-side fill respectively.
`!ai`, `!ai fill` and `!ai e* a*` share the two-side fill policy; `!ai enemy`
is equivalent to `!ai e* a0`. A numeric rule adds only the specified counts,
with omitted counts defaulting to zero. Named policies cannot mix with numeric options.

With [Battle AI](battle-ai.md), PvP also accepts `easy`, `medium` or `hard` in the
`!ai` argument span, for example `!ai3 hard`. The default is Medium. `lv1`–`lv5`
select the existing NPC pool; Medium's actual unit level is 12 and Hard's is EX13.
In `!ai !rand hard`, `hard` does not select AI difficulty.

## Longer descriptive names

With [extended room titles](room-titles.md), these complete names also work:

| Mode | Full title |
|---|---|
| Mission | `Weekend mission with friends !ai3 lv4 !rand` |
| PvP | `Friendly practice for newcomers !ai e2 a1 lv4 !rand` |
| PvP with Hard Battle AI | `Friends practicing together !ai e2 a1 lv4 hard !rand` |

Enable `room_titles` on every client, `room_titles_agent` on the serving Agents
and `room_titles_server` on the Battle servers. These three Mods default to off.
Update the consuming `coop_ai`, `pvp_ai` and `room_random` DLLs with the same framework build.
When using Battle AI, update `battle_ai`, `match_ai` and `battle_ai_server` too.
The limit becomes 128 Unicode scalars / 512 UTF-8 bytes; commands after byte 20 are read too.
Without the extension, native titles retain the 20-byte limit.

The AI and random-loadout features must still be installed, enabled and configured
independently. A package filtered to only `room-titles` contains the three title
Mods, so select the gameplay features too or use a full framework package.
Players use their normal launcher. The host edits the title normally and waits
for server confirmation before starting. Failed text remains editable.

Randomization applies to each human's eligible inventory; AI use their existing
unit pools. Longer names do not add new commands, unlock inventory or increase team capacity.

Developers can use the optional `FindRoomCommand` core API or its identical
older-core fallback in `include/sdgo/room_commands.h`. The feature provider in
`include/sdgo/room_titles.h` supplies full titles; the parser alone does not extend native storage.
