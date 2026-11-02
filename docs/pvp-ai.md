# PvP AI players (Beta)

Put `!ai` in a two-team PvP room title to add AI opponents and teammates, using the existing
combat, scoring, respawn and spectator features. 中文：[pvp-ai.zh-CN.md](pvp-ai.zh-CN.md)

This feature is in **Beta**.

![AI teammates and opponents in battle](pvp-ai-demo.webp)

Also supports death match, Grid, leader and Tag modes.

## Install

Both parts are in the full framework package; friend-server players receive `match_ai` through
`update.bat`. Every participant needs matching versions:

| Mod | Install on |
|---|---|
| `pvp_ai` | Battle servers |
| `match_ai` | Every participating client |

Follow the [installation guide](setup.md#install), then use your usual launcher.

These population rules require `pvp_ai 0.3.0-beta.2` on Battle servers and
`match_ai 0.1.5` or later on every participating client, including the host.
Restart the affected programs. Easy, Medium and Hard use the same release point.

## Use

Set the room title, ready up and start normally. AI join at battle start.

Normal, death match, Grid, leader and Tag modes enable AI on the same native
countdown frame that releases player control. AI cannot move or attack early.
Hold TAB to see AI on both teams in the native battle roster, including their
names, kills, assists and points with normal scoring and sorting.

| Title | Effect |
|---|---|
| `!ai coop` | Group humans with the host at start; add no allied AI and fill all remaining room places with opponents |
| `!ai coop fill` | Group humans, then fill both sides; three humans in an eight-player room get one AI ally versus four AI |
| `!ai coop e4 a0` | Group humans and add exactly four opponents; three humans get 3v4 with one place left empty |
| `!ai versus fill` | Preserve manual/random human teams and fill both sides; same as `!ai fill` |
| `!ai` or `!ai fill` | Fill both teams within the room capacity, normally 4v4 in an eight-player room |
| `!ai3` | Add three enemies |
| `!ai e* a0` or `!ai enemy` | Add no allied AI; fill remaining room places with enemy AI |
| `!ai e* a*` | Same as `!ai fill` |
| `!ai e5 a0` | Add exactly five enemy AI and no allied AI; three allied humans make 3v5 |
| `!ai e2 a1 lv2` | Two enemies and one ally from the level 2 unit pool |
| `!ai e3 a2 lv4` | Three enemies and two allies from the level 4 pool |
| `!ai e3 lv4 hard` | Three level 4 enemies with Hard [battle AI](battle-ai.md) (`easy`, `medium`, `hard`; default medium) |
| `!ai0` or no `!ai` | Do not add AI |

`eN` and `aN` count additional AI, excluding humans. `*` fills vacancies. Enemy
and ally mean opposite or alongside the host's final team at each start, rather
than a fixed red/blue color. Commands are case-insensitive.

Human team selection and AI population are independent:

- **Native teams** (default, or explicit `versus`): preserve manual choices, or resolve native random human teams first. AI placement then follows the host's final side. Humans on the other side remain opponents.
- **Humans together** (`coop`): at start, group every human on the host's selected side, then add AI. This overrides native random teams; other humans' temporary side selections cannot separate friends. The host can choose either side.

If a random-team room has not assigned the host a side yet, `coop` uses team 1.
To choose a color, use manual teams and let the host select that side.

Bare `coop` defaults to opponents only. Combine it with `fill`, `enemy` or exact
`eN aN`, such as `!ai coop e4 a1`. Bare `versus` defaults to both-side fill.
Choose one human policy; omitting it preserves native teams. `!rand` chooses
owned units and does not select teams or decide which side receives AI.
The title identifies the rules; grouping and AI insertion happen at battle
start, with actual teams visible on the native TAB roster.

For friends versus computers, use `!ai coop`: one human produces 1v7, three
produce 3v5, four produce 4v4. Use `!ai coop e4 a0` for three humans versus four
computers, or `!ai coop fill` to add one AI ally for 4v4. For opposing humans,
keep native teams: with one human on each side, `!ai e4 a2` produces 3v5.

For an eight-player room, counts below list the host's side first:

| Existing humans | `!ai fill` | `!ai enemy` |
|---|---|---|
| 1 / 0 | Add 3 / 4 AI for 4v4 | Add 0 / 7 AI for 1v7 |
| 3 / 0 | Add 1 / 4 AI for 4v4 | Add 0 / 5 AI for 3v5 |
| 2 / 1 | Add 2 / 3 AI for 4v4 | Add 0 / 5 AI for 2v6 |
| 4 / 0 | Add 0 / 4 AI for 4v4 | Add 0 / 4 AI for 4v4 |

Each start resolves the current humans, host and room capacity again. Automatic
fill follows side changes, joins, departures and host changes. Leaving during
battle does not insert replacement AI. A room full of humans cannot start in
`coop` because no opponent place remains; free a place or use native teams. Exact counts stay
exact instead of becoming balanced counts. Native random teams can separate
friends; use `coop` to group them automatically. `!ai coop e5 a0` gives three
humans 3v5, but a fourth human makes that exact request exceed capacity and
cancels start; `!ai coop` instead adapts to 4v4. Two-side fill
aims for half the room on each side without moving humans already over that
target. Bots use participant slots and never replace humans or use observer seats.

AI rooms currently support eight participants total. Bots use any free native
slot; a storage-array half no longer limits their team. Oversubscribed requests,
missing host identity, conflicting counts or unavailable unit pools cancel the
start instead of silently adding fewer bots. More than eight participants are
outside this scope. Misspelled, repeated or conflicting options also cancel the
start; choose one of `fill`, `enemy` or a count rule, then correct errors before
retrying. Teams larger than the map's spawn-point count cycle through that side's
existing points, so some overlap is possible. Starting with this version, bare
`!ai` follows room capacity and ignores the old `FillTeamSize` setting; use a
six-player room with `!ai` for fixed 3v3.
`lv1`–`lv5` select unit pools. With [extended room titles](room-titles.md) enabled on clients,
Agents and Battle servers, an updated `pvp_ai` reads commands anywhere in the confirmed full title
(up to 128 Unicode scalars / 512 UTF-8 bytes). For example:
`Friendly practice for newcomers !ai e2 a1 lv4 !rand`.
With `room_random` also installed and enabled, humans randomize each round while AI retain their own pools.
Keep `e2 a1 lv4` after `!ai`, before `!rand`; all participants still need `match_ai`.
Without the title extension, the native 20-byte limit remains.
See [combined commands](room-commands.md).

Every battle draws new units for each AI from its pool, without duplicates and within the
mode's point and unit-count limits.

The host runs the AI and other clients display synchronized copies. Grid/Tag
modes can use AI decks. The existing dead view can follow AI players, and AI appear on the
battle Tab scoreboard. Respawn, life consumption and victory conditions follow the selected mode.
AI on either team target opposing humans and AI and never their teammates, at every difficulty.

## Settings

Edit them in SDGO Mod Manager under **对战 AI · 服务端**, then restart the affected battle
servers. Log switches appear after **显示开发信息**. Values are stored in `[settings]` of
`sdgo/mods/pvp_ai/mod.ini`.

| Setting | Default | Meaning |
|---|---|---|
| `DefaultLevel` | 2 | Pool when no `lvN` is specified, 1–5 |
| `LifeCostGrade1`…`4` | 250/334/455/556 | Death match life costs for C/B/A/S grades; 0 preserves the original |
| `Level1`…`Level5` | See file | NPC data ids in each unit pool |
| `LogRoom` | 1 | Developer: log room information at battle start |
| `LogBattle` | 0 | Developer: score, death and respawn logs |

Custom pools need matching NPC/unit data on both sides; see
[adding more bot units](coop-wingmen.md#adding-more-bot-units). Values cannot have
inline comments. The client part (**对战 AI · 客户端**) only has the developer log switches
`LogErrors` and `LogDiagnostics`, both off by default.

Logs are in `sdgo/logs/` beside the client or battle server. If AI do not appear,
check that both mods loaded, the room command, available unit data and free team slots.
