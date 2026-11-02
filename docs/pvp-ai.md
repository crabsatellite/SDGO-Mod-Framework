# PvP AI players (Beta)

Put `!ai` in a PvP room title to add AI opponents and teammates, using the existing
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

Update every participating client, including the host, to `match_ai 0.1.5` or
later and restart the client. The opening freeze, TAB roster and Teleport fixes take
effect in the next battle; Easy, Medium and Hard use the same release point.

## Use

Set the room title, ready up and start normally. AI join at battle start.

Normal, death match, Grid, leader and Tag modes enable AI on the same native
countdown frame that releases player control. AI cannot move or attack early.
Hold TAB to see AI on both teams in the native battle roster, including their
names, kills, assists and points with normal scoring and sorting.

Teleport moves locally controlled PvP AI to a native spawn point, preserving their
current HP. It no longer takes the original NPC destruction fallback, which could
kill a healthy bot inside the playable map. Normal damage and map hazards still apply.

| Title | Effect |
|---|---|
| `!ai` | Fill both teams to three participants, including humans and existing AI |
| `!ai3` | Add three enemies |
| `!ai e2 a1 lv2` | Two enemies and one ally from the level 2 unit pool |
| `!ai e3 a2 lv4` | Three enemies and two allies from the level 4 pool |
| `!ai e3 lv4 hard` | Three level 4 enemies with Hard [battle AI](battle-ai.md) (`easy`, `medium`, `hard`; default medium) |
| `!ai0` or no `!ai` | Do not add AI |

`eN` means the team opposite the host; `aN` means the host's team. Commands are
case-insensitive, with at most six participants per team and twelve overall.
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
| `FillTeamSize` | 3 | Total participants per team for `!ai`, 1–6 |
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
