# Per-round random owned loadouts

[简体中文](room-random.zh-CN.md)

Add the standalone token `!rand` or `!random` to a room title. Every human in the room gets a new
loadout from **their own hangar** each round, without leaving or recreating the room. Commands are
case-insensitive.

| mod | host | part |
|---|---|---|
| `room_random` | battle server | draws each human's unit or deck at round start |
| `room_random_client` | client | applies the round's result to the client's start data |

Both are required: `room_random` on BattleServer and `room_random_client` on every participating client.
Clients need `room_random_client` 0.1.13 or later; with AI, Battle servers need `pvp_ai` 0.3.2 or later.

## Room-title commands

Combinations such as `!ai !rand`, `!ai3 lv4 !rand` and `!rand !ai e2 a1 lv4` work. Separate commands with
spaces and keep AI options right after `!ai`, before the next `!command`; see
[combined commands](room-commands.md). AI keep their own pools.

Native titles are limited to 20 bytes. With [extended room titles](room-titles.md) on clients, Agents and
Battle servers and updated `room_random` and AI mods, titles of up to 128 Unicode scalars / 512 UTF-8
bytes are read in full, for example `Friendly practice for newcomers !ai e2 a1 lv4 !rand`.

Without the command, rounds use the native sortie flow.

## Rules

- Single-unit modes (normal, death match, leader, missions): one unit per round, kept through respawns.
- Native deck modes 6/7: distinct owned serials, at most four slots and **exactly ten points**
  (usually C=2, B=3, A=4, S=5), e.g. `5+5`, `5+3+2`, `4+4+2`, `4+3+3`, `4+2+2+2`, `3+3+2+2`.
- Room rank limits and C-only rules apply to every human's candidates; native ready, team, mode and
  battleship checks still apply.
- Disabled, expired, out-of-energy, training or non-sortie units are never drawn. Locked units (sale
  protection) can be drawn.
- When alternatives exist, the next round avoids the previous model or model set.
- Costs come from native unit data, rather than a fixed mapping from grade letters. New variants
  enter only their owner's pool when their actual cost and room restrictions are valid.
- If any player has no usable unit or cannot reach ten points, the whole start is cancelled. Four
  ordinary two-point C units reach only eight; variants with different native costs are evaluated
  by those actual costs rather than rejected solely by their grade.

The result applies to this round only; `GoUserUnitNum`, inventory, saved decks and locks are not changed.
Drawn units keep their level, upgrades, skills, paint, emblem, experience and serial.

## Install

Players install the client part from the server owner's client package or update with the game closed; it has no settings.

The server supports `BattleServer-548A891D-0046C000` with the framework installed. Stop the target
BattleServer, then:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\install_room_random.ps1 `
  -ServerRoot "D:\Server\BattleServer1" -DatabaseIni "D:\Server\WorldServer\WorldServer.ini"
```

Install on every BattleServer that hosts rooms. The installer also updates already installed
`pvp_ai` / `coop_ai` DLLs to the shared title parser, keeping their settings; it does not install
missing AI mods or start/stop services. Replaced files are backed up to `sdgo/backups/room_random/`.

## Settings

Set **原生服务器 INI 路径** (the WorldServer INI) under **每局随机机体** in SDGO Mod Manager. Only the
path is stored, never the credentials; keep it and `config.ini` out of client packages. Driver, query
timeout and per-player row limit are developer settings. The 32-bit `SQL Server` ODBC driver is used.

## Developer notes

The mod runs one read-only, parameterized hangar query for the whole room before the native start
validation, computes every human's plan, then feeds it to the native human battle constructor; any
failure restores everything. Releasing battle pilots restores the room's original sortie data, and
respawns do not redraw. Decks are enumerated by dynamic programming over exact ten-point sets. All four
native human start paths (normal/mission, AI 11, AI 12, 13/14, the last with its own outer entry) are
covered. If a disconnect skipped the release callback, the previous round's cache is retired only after
the native start has cleared the human pilot records, which prevents `ERR_BATTLE_START_FAIL` on re-entry.

The server sends native `0x0861` full-unit records and, for modes 6/7, `0x098B` full-ship records before
the `0x06C3` battle start, to humans and observers only. The client's 3D room queues those notices and
builds units from a separate frozen start snapshot, so updating the visible cache is not enough.
`room_random_client` syncs the cache after the native message dispatcher and, at the human construction
entry, supplies a private copy of the complete record matched to this round's start entry, room and owner;
the frozen snapshot is not modified. The 3D path resolves the room through the current 3D manager, not
the legacy profile room pointer; mission and normal rooms store slots differently. The native human
converter is a static four-argument function (`RET 10h`) whose `ECX` is scratch, not a room object, and a
3D start command can advance the app state before conversion; the mod therefore uses the room model observed
with the actual start packet and revalidates its vtable and room identity. A missing notice or a
changed owner, room or start entry leaves the native input unchanged and is logged.

Logs: server `native loadout notices queued`; client `battle start cache` and
`human construction ... snapshot ... -> ...`.

The client retains each human's complete round-start entry. If a unit or deck notice arrives after
the start notice, it completes only that human's pending construction data. A rejected notice or a
later selection for one slot does not discard another player's valid start data. In the 3D room,
the verified active room model takes precedence over a retained legacy cache pointer.

In random rooms, the client also holds the native start transition until every human's complete
unit record, and the ship record in deck modes, matches this round. It keeps a private packet
copy and resumes the original parser once when the final matching notice arrives. Duplicate
notices cannot start it again; changing rooms or replacing the start discards the old pending
transition. Ordinary rooms retain the native timing.

Tests: `ctest`, plus `room_random_test.exe <BattleServer.exe> <GOnline.exe>`,
`tests/room_random_client_runtime_test.ps1 -GameExe <GOnline.exe>` and
`tests/room_random_install_test.ps1 -NativeExe <BattleServer.exe>` with locally supplied executables.
