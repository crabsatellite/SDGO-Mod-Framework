# Battle AI difficulty

[简体中文](battle-ai.zh-CN.md) · [Back to the feature index](../README.md)

Easy, Medium and Hard AI for the bots of [PvP AI](pvp-ai.md), choosing tactics from each bot's current
unit, form and weapons. It starts with the game and stores no FRP address, account or local path.

| mod | host | part |
|---|---|---|
| `battle_ai` | client | decisions, aiming, movement and tactics |
| `battle_ai_server` | battle server | player resource rules for AI (SP, parts, items) |

PvP AI's `pvp_ai` (server) and `match_ai` (client) are also required. All participants use components
from the same package.

## Choosing difficulty in the room title

Clients default to **Medium**. The host writes the difficulty in the `!ai` arguments, so friends need
no per-client configuration.

| Title | Effect |
|---|---|
| `!ai` / `!ai3` | usual bot counts, Medium |
| `!ai3 easy` | Easy: the original AI |
| `!ai e3 a1 medium` | Medium: level 12, paced ordinary attack starts, accurate aim and lead, legal specials, front-facing movement |
| `!ai3 hard` | Hard: EX13, advanced tactics, shield facing, one legal part and one limited item |

Keep `easy`/`medium`/`hard` after `!ai` and before the next `!command`; `!ai !rand hard` does not select
Hard. `lv1`–`lv5` still select NPC unit pools, not unit level or difficulty. The title overrides the
local `Difficulty` setting. With [extended room titles](room-titles.md) the difficulty is read from the
full title, e.g. `Friends practicing together !ai e2 a1 lv4 hard !rand`.

## Medium and Hard

Medium and Hard select a combat form automatically by default; adjust this with **自动选择战斗形态**
in Manager. AI reads the weapons, ammunition and cooldowns of each native form. It can change a
melee-only form to a usable ranged form for a distant
opponent, choose a stronger melee form at close range, or use another form when its current ranged
ammunition runs out. A ready current weapon that can finish the opponent takes priority.

A loaded melee weapon in the other form is a reason to leave an empty form only when it can reach
the target. Otherwise the bot keeps approaching or reloading. An available in-range weapon also
prevents empty guns in the current form from immediately reversing a useful form choice.

Transformation uses the original request and server acknowledgement, including native health,
ammunition and action restrictions. A refused request leaves ordinary combat running. One-way armor
removal stays one-way, and forms excluded from ordinary R input retain their native skill or timed
control. This applies to the enhanced Medium/Hard controller; Easy keeps its existing behavior.
Restart the game after installing the server owner's client package or update.

Both levels keep continuous combat and movement decisions, accurate aim, native ammo, cooldowns,
physics and damage. Medium defaults to a **600 ms minimum between fresh ordinary attacks admitted by the native controller**.
Hard keeps continuous requests whenever native rules allow them.

Medium's interval uses the existing native attack-intent pacing interface, only while the attack
controller has no current or queued action. It does not interrupt a charge, beam, melee combo or special,
pause navigation, delay target selection, or add 600 ms to every weapon's own cooldown. A slower weapon
remains governed by its longer native cycle. Switching ordinary weapons does not reset this interval;
respawning starts with no debt from the previous life. This reduces repeated rapid starts, while its
effect on sustained fire depends on the weapon. A native refusal adds no interval before the next
legal attempt. A ready ranged weapon with a target in range retains an already-started boost until
its first physical step, then resumes normal firing and movement decisions. This also covers mobile
scripted guns, preventing repeated strafe requests from interrupting native startup.

Living teammates share fresh visual contacts, including contacts observed by human players. A bot can
pursue an enemy seen by an ally without seeing it itself. Shooting still requires the bot's own weapon
range, clear muzzle line of sight and native attack permission. When shared sight is lost, pursuit uses
the last observed position rather than the hidden enemy's current coordinates.

On PvP maps without native navigation nodes, pursuit searches the loaded terrain and solid map geometry,
using the bot's actual collision sphere. Public spawn and relocation points provide exploration goals
when no teammate has a current contact. Paths account for platforms, safe drops, solid obstacles and
DeathZone volumes. A blocking destructible can be attacked through normal weapons; indestructible and
destroyed-but-still-solid objects remain obstacles.

These routes can select propulsion devices loaded in the actual map. When the entrance is reachable
and the authored trajectory has a suitable supported exit, a bot approaches the entrance and uses
the original request and notification flow. Native movement owns the trip; the bot replans from its
actual arrival position and continues combat afterward. Paths retain map-boundary and DeathZone
checks. Mission scripts hiding or restoring geometry invalidate old routes and device candidates.
Devices are selected for a useful route, so a bot need not use one in every battle.

Enemy life is read from the native unit lifecycle, separately from the HP sample. A living,
attackable opponent with a zero HP reading remains a target; actual death, knockdown and get-up
protection still apply.

| | Medium: skilled player | Hard: expert player |
|---|---|---|
| Output | continuous decisions and low-HP focus; fresh ordinary requests at least 600 ms apart by default | continuous native-permitted requests, plus legal advanced chains |
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

Native damage notifications can refer to map objects as well as units. NPC-only AI and SP state is
read or updated only after the original object-type check identifies an NPC; destructible scenery
continues to use its native behavior.

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

In SDGO Mod Manager, **战斗 AI 难度** (client) has **启用战斗 AI** (enable), **默认战斗难度** (default
difficulty), and **中等：普通攻击最短间隔（毫秒）** (Medium minimum ordinary attack interval).
The latter maps to `MediumAttackIntervalMs`, defaults to `600`, and accepts `0`–`2000` ms; `0` restores
continuous starts and Hard ignores it. Configurations without this key use `600`.
**自动选择战斗形态** maps to `AutoTransform`, defaults to `1` and applies to Medium and Hard.
Changes apply within two seconds. Other tuning parameters and the decision log appear after
**显示开发信息**. Values live in `sdgo/mods/battle_ai/config.ini`, with defaults in `config.example.ini`;
see the Chinese guide for the full list. The server part **战斗 AI 资源规则** only has an enable switch.
Turning the AI off returns control to the native AI within two seconds.

With decision logging enabled, `Medium ordinary attack pacing observed` records the first held-back
ordinary request per bot generation.

The developer setting `TerrainRayAudit` defaults to `0`. Set it to `1` only to compare indexed
terrain rays with the native traversal; the extra queries increase host work. `LogDecisions` does
not enable this audit.

## Developer notes

Navigation distinguishes the destination floor from the bot's current floor. Being directly above
an enemy is not arrival: the route must reach the projected destination height. Pursuit and exploration
can use native boost while fuel remains above the configured reserve, and descending segments use
ordinary movement. Native collision probing supplies obstacle heights for automatic jumping; the Mod
enables that NPC input permission and preserves the unit's original gravity, speed and collision rules.
Committed attacks, capture and map-controlled movement keep their motor ownership.
Selecting a special does not reserve an action: an unadmitted request that remains natively neutral
for 200 ms returns to ordinary combat, with one second before another attempt. Current or queued
native actions and accepted specials retain their completion ownership.
After a native action finishes, the enhanced controller resumes on a free update even when the
original script omits its callback. Relative movement checks native admission before submitting a
command, and an accepted short step keeps its startup interval instead of being restarted each frame.
An unreachable last-seen position is discarded so public-map exploration can resume.
Boost Down keeps its native fuel depletion while the bot can continue ordinary walking and legal
attacks. A melee weapon remains available when no usable ranged response exists. Live breakable
gates remain solid to route planning when ordinary weapons cannot currently breach them.
Side steps follow measured ground contours. A blocked preferred direction does not suppress checks
of other directions, and rejected positioning requests retain their demand for route recovery.
Static collision scenes are checked as geometry even when they have no gameplay-object pointer.
Walking support and slope admission use the game's native bilinear heightmap and root normal;
rendered triangle height and a model's box corners do not define its walking surface.

Terrain sight queries visit the ray's intersected grid cells and use the original native
triangle predicate. The layout is validated before indexing; only layout information is retained,
so current heights, teammate contacts and firing visibility are still read live. Inactive native
steering storage is represented as no movement destination instead of invalidating the whole observation.

During automatic form selection, AI preserves native control during the animation, then reads the
committed form's actual weapon bank and clears its previous pursuit and weapon-switch state. Requests
are retried at most every 500 ms; a committed form is retained for at least one second. Locally
controlled AI also consumes the native completion acknowledgement when its model has already changed
but the same transformation animation still owns control. Each committed form supplies its own
outgoing native transformation link. Respawn recovery and native action admission finish before AI
can request another transformation.

For enemy life checks, observation and attack resolve the same current registered object, so an older
object with the same ID cannot retain a stale protection state for its replacement.

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
