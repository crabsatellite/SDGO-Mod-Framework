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
This ordering fix requires `room_random` 0.1.8 or later on BattleServer together with
`room_random_client` 0.1.14 or later on every participating client. With AI, Battle servers also need
`pvp_ai` 0.3.2 or later.

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
Post-settlement restore notices use the reserved round-zero marker. The client removes that marker before
the original `0x0861`/`0x098B` handlers, keeps their native E/F redraw work, and suppresses only the
nested local view-1 request. Ordinary notices and unrelated room-view requests still follow the native
path. This prevents a completed round's restore work from invalidating the next pooled-room generation.

After every human has been built, the server prepares all complete loadout notices while the native room
lock is still held. It submits the genuine `0x06C3` battle start exactly once, then sends native `0x0861`
full-unit records and, for modes 6/7, `0x098B` full-ship records through the same native broadcast chain,
to humans and observers only. The client's 3D room queues those notices and builds units from a separate
frozen start snapshot, so updating the visible cache is not enough.
`room_random_client` syncs the cache after the native message dispatcher and, at the human construction
entry, supplies a private copy of the complete record matched to this round's start entry, room and owner;
the frozen snapshot is not modified. The 3D path resolves the room through the current 3D manager, not
the legacy profile room pointer; mission and normal rooms store slots differently. The native human
converter is a static four-argument function (`RET 10h`) whose `ECX` is scratch, not a room object. The
mod revalidates the manager/model generation observed with the actual start packet, including its
vtable, backpointer and room identity. A missing notice or a
changed owner, room or start entry leaves the native input unchanged and is logged.

Logs: server `native start submitted; loadout notices queued` (or `incomplete` when a native route attempt
reports failure); client `battle start cache` and
`human construction ... snapshot ... -> ...`.

The client retains each human's complete round-start entry. If a unit or deck notice arrives after
the start notice, it completes only that human's pending construction data. A rejected notice or a
later selection for one slot does not discard another player's valid start data. In the 3D room,
the verified active room model takes precedence over a retained legacy cache pointer.

In random rooms, the client holds the native start transition until every human's complete
unit record, and the ship record in deck modes, carries the same round marker as the start notice.
Native callbacks may deliver records before, after or interleaved with that notice. A previous round's
record cannot satisfy the wait, even when the model is identical. Repeated start notices for the
same round keep the matching records already received. It also waits for each human's
native unit/ship update tasks to finish. A tagged random-loadout notice proves that the server has submitted this round's start, so its local
room-view request is obsolete even if the start callback has not run yet. The Mod binds the exact live local state object,
transfers its queued payload to the native cleanup owner, and returns control to the original scheduler,
which retires and frees that one request normally without sending another room-view RPC. It keeps a
private packet copy and resumes the original parser once after the waiting-room manager has processed
the remaining queues.
Late notices update the pending records; the packet dispatcher does not reenter the start parser.
The server advances a per-room marker from 1 through 15, wrapping to 1. The native start packet's
byte at `+0x1F` remains a 0..12 participant count in its low nibble; its high nibble and the unused
high bits of the loadout route field carry the marker only on the wire. The client saves that identity
privately and clears the high bits before every original consumer. The server owns only the exact
common 06C3 broadcast call site, so other Mods may still hook the native broadcast function.
Missing or incompatible markers use the native fallback and are
reported as a component-version mismatch. This uses native completion conditions rather than a fixed loading delay. Duplicate
notices cannot start it again; changing rooms or replacing the start discards the old pending
transition. Ordinary rooms retain the native timing.

Tests: `ctest`, plus `room_random_test.exe <BattleServer.exe> <GOnline.exe>`,
`tests/room_random_client_runtime_test.ps1 -GameExe <GOnline.exe>` and
`tests/room_random_install_test.ps1 -NativeExe <BattleServer.exe>` with locally supplied executables.

Developer consumers can observe human construction through
`sdgo/battle_human_unit_observer.h`. The loaded server Mod exports V1 registration
and status functions for one process-lifetime read-only sink. A new start resets
the room generation, then each final rewritten input has paired BEFORE/AFTER
events around exactly one native constructor call. START_ACCEPTED follows submission of
the genuine native start. A failure before that boundary rolls back and invalidates the
room; after submission, temporary random records remain until native release invalidates them.
These events do not prove packet delivery or gameplay acceptance.

Callbacks may interleave across native threads and must not block, mutate native
data or reenter the provider. Copy bounded preimages before returning; AFTER data
is usable only on native success. The provider preserves FP/SSE state and
`LastError`, and disables a faulting sink. Consumers must check registered status
before using cached generations; the sequence watermark does not prove callback
completion. Register only as the consumer's final load action and retain the sink
for the process lifetime. A loaded incompatible provider must fail closed rather
than be bypassed by another hook at the same native address.
