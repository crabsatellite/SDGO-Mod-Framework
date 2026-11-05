# Battle AI difficulty

[简体中文](battle-ai.zh-CN.md)

Easy, Medium and Hard AI for the bots of [PvP AI](pvp-ai.md), choosing tactics from each bot's current
unit, form and weapons. It starts with the game and stores no FRP address, account or local path.

| mod | host | part |
|---|---|---|
| `battle_ai` | client | decisions, aiming, movement and tactics |
| `battle_ai_server` | battle server | player resource rules for AI (SP, parts, items) |

PvP AI's `pvp_ai` (server) and `match_ai` (client) are also required. All participants use components
from the same package.

Native damage notifications can refer to map objects as well as units. NPC-only AI and SP state is
read or updated only after the original object-type check identifies an NPC; destructible scenery
continues to use its native behavior.

## Choosing difficulty in the room title

Clients default to **Medium**. The host writes the difficulty in the `!ai` arguments, so friends need
no per-client configuration.

| Title | Effect |
|---|---|
| `!ai` / `!ai3` | usual bot counts, Medium |
| `!ai3 easy` | Easy: the original AI |
| `!ai e3 a1 medium` | Medium: level 12, continuous decisions, accurate aim and lead, legal specials, front-facing movement |
| `!ai3 hard` | Hard: EX13, advanced tactics, shield facing, one legal part and one limited item |

Keep `easy`/`medium`/`hard` after `!ai` and before the next `!command`; `!ai !rand hard` does not select
Hard. `lv1`–`lv5` still select NPC unit pools, not unit level or difficulty. The title overrides the
local `Difficulty` setting. With [extended room titles](room-titles.md) the difficulty is read from the
full title, e.g. `Friends practicing together !ai e2 a1 lv4 hard !rand`.

## Medium and Hard

Neither level is weakened with artificial idle, delayed reactions or deliberate misses. Ammo, cooldowns,
physics and damage remain native.

Living teammates share fresh visual contacts, including contacts observed by human players. A bot can
pursue an enemy seen by an ally without seeing it itself. Shooting still requires the bot's own weapon
range, clear muzzle line of sight and native attack permission. When shared sight is lost, pursuit uses
the last observed position rather than the hidden enemy's current coordinates.

On PvP maps without native navigation nodes, pursuit searches the loaded terrain and solid map geometry,
using the bot's actual collision sphere. Public spawn and relocation points provide exploration goals
when no teammate has a current contact. Paths account for platforms, safe drops, solid obstacles and
DeathZone volumes. A blocking destructible can be attacked through normal weapons; indestructible and
destroyed-but-still-solid objects remain obstacles. Forced-movement pads retain native motor control.
These checks do not guarantee that every unit can traverse every complex map without stalling.

| | Medium: skilled player | Hard: expert player |
|---|---|---|
| Output | continuous decisions, fires as soon as native rules allow, focuses low HP | same, plus legal advanced chains |
| Movement | ground strafing with move-and-fire weapons | air strafing, peeking, flanking, orbwalking |
| Specials | to escape grabs or clear pressure | also on confirmed openings (target stunned or in melee) |
| Defensive repositioning | within radius 480 per pressure event | radius 1200; both keep facing the actual threat |
| Expert techniques and resources | none | swing cancels, melee/ranged weaving, shield posture, a normal player part and a limited item |

During defensive movement, a bot with a usable gun releases an uncommitted knife chase through the
native Neutral transition, waits for the gun's actual binding and keeps firing when the weapon permits
moving shots. A queued slash, active combo or special retains its native ownership. Visible enemies
outside ordinary weapon range remain pursuit candidates; firing still obeys the weapon's real range.

With a knife or special selected, perception uses the body's native collision origin. A bound gun uses
its actual native muzzle origin for ranged visibility. Missing melee muzzle data therefore does not
prevent noticing an opponent, and body visibility does not authorize a shot from an unbound gun.
On a free controller, weapon changes use the native non-forced selection transaction; active attacks
retain their native ownership. A rejected pursuit route remains an unmet movement request, so bounded
stuck recovery can continue through the existing collision and DeathZone checks.

If enhanced observation or a policy fails, the Mod logs the bot identity and failure, releases held
input and retries the enhanced policy. It does not silently switch to stock decisions. Easy and the
enable switch still intentionally select native control. Use the framework log and the developer
decision log when diagnosing a persistent failure.

Acknowledged zero-damage melee specials complete through the original native finalizer, so a surviving
victim can leave capture normally. Teleport notifications move locally controlled PvP bots through the
same native relocation path as players, instead of the stock NPC destruction fallback. Normal damage,
death, ammo and cooldown rules remain native.

Generated movement checks the unit's swept collision bounds against native DeathZone blocks before
direct steps and route following. Unsafe directions try bounded alternatives; this does not disable
map hazards or prevent legitimate deaths.

Hard picks one ordinary player part (New-type awakening or martial arts, with their native SP/HP
activation rules) and, on its first qualifying trigger, either Teleport or Mirage for the whole battle,
with native use counts and cooldowns (50/45 s). SP starts at zero and is earned through native hits;
the server applies the native player costs. Easy keeps native level, decisions, aim and resources. The
new rules apply only to PvP bots in the reserved NPC range; native missions, AI modes and bosses are
unchanged. Bots on either team target opposing humans and AI and never teammates.

General behaviour: real weapon-slot data drives weapon choice and range; aim removes the native NPC
offset and leads projectiles whose speed is known or learned from stable samples (curved or variable
trajectories are not linearly predicted); navigation uses authored native paths where available and
collision-aware PvP routes otherwise, with progress checks and re-planning. Line of sight is checked
per target with the client's collision primitives.

With many units present, sensing reads only what it needs to keep overhead low; target range, scan
frequency, reaction speed and combat decisions are unchanged, and combat state and visibility are still read
and checked on every observation.

The native object-lookup entry is bound once when the Mod loads. Each shot, notice and decision
still looks up the current object by ID, so respawns, deaths and target changes remain live.
Memory-readability caching lasts for one observation only; it does not retain targets or line-of-sight
results across decisions.

## Install

Requires the framework; client `GOnline-54BE1568-00C97000`, server `BattleServer-548A891D-0046C000`.
Players receive the client part with the server owner's client package or update. Manual install, with the copy stopped:

```powershell
./tools/install_battle_ai.ps1 -GameDir D:/Games/SDGO -Source ./build/stage/Release/client
./tools/install_battle_ai.ps1 -GameDir D:/Servers/BattleServer1 -Side Server -Source ./build/stage/Release/server
```

The client package contains `battle_ai`, `match_ai`, `mission_ai` and the `ai_catalog` runtime; the server
package contains `battle_ai_server` and `pvp_ai`, together with the matching core and loader. Owner
catalogs, enable states, user `config.ini` and other mods are preserved.
For a Manager framework template that contains no native game executable, use
the same installer with `-FrameworkOnly`. This option rejects a normal game
directory; normal client and server installs still verify the executable hash.
Use `-PvpCombatOnly` for a client update limited to `battle_ai` and `match_ai`;
installed mission AI and catalog runtimes are preserved.

## Settings

In SDGO Mod Manager, **战斗 AI 难度** (client) has **启用战斗 AI** (enable) and **默认战斗难度** (default
difficulty); changes apply within two seconds. Tuning parameters and the decision log appear after
**显示开发信息**. Values live in `sdgo/mods/battle_ai/config.ini`, with defaults in `config.example.ini`;
see the Chinese guide for the full list. The server part **战斗 AI 资源规则** only has an enable switch.
Turning the AI off returns control to the native AI within two seconds.

## Developer notes

Only locally simulated bots in the reserved AI NPC range are controlled; remote copies, humans and mission
bosses are not. The Easy-path target search fix in `match_ai` (native enumeration of `1..playerCount`
versus mod AI object IDs `1001+`, and the native team-1 NPC-only branch) is documented in the
[target selection notes (Chinese)](battle-ai-target-selection.zh-CN.md).

Tests need the official Lua 5.0.3 sources (`-DSDGO_LUA50_SOURCE=...`), which are not distributed:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32 -DSDGO_LUA50_SOURCE=D:/deps/lua-5.0.3
cmake --build build --config Release --parallel 2
./tests/battle_ai_test.ps1 -GameExe D:/Games/SDGO/GOnline.exe -ServerExe D:/Servers/BattleServer1/BattleServer.exe -AgentExe D:/Servers/AgentServer1/AgentServer.exe
./tests/battle_ai_install_test.ps1 -GameExe D:/Games/SDGO/GOnline.exe -ServerExe D:/Servers/BattleServer1/BattleServer.exe
```

`tools/prepare_battle_ai_sandbox.ps1 -ClientDir D:/Games/SDGO` prepares a logged-out test copy under
`build/battle_ai_sandbox/client` without starting the game or connecting to a backend.
