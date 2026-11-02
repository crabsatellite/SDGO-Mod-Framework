# AI mech catalog (Beta, separate addon)

[中文](ai-catalog.zh-CN.md)

`ai_catalog` adds the sortie-ready player units from the owner's database to the LV1–LV5 pools of
`coop_ai` / `pvp_ai`. The owner generates one catalog and installs it on the battle servers and every
participating client; players keep their normal launcher.

It needs the existing AI features: `coop_ai` + `mission_ai` for missions, `pvp_ai` + `match_ai` for
PvP. It only enlarges the random pools: there is no unit selection screen and the native AI modes
11/12 keep their roster. For example `!ai e2 a1 lv2` draws two enemies and one ally from the
enlarged LV2 pool.

It also works with [extended room titles](https://github.com/crabsatellite/SDGO-Mod-Framework/blob/master/docs/room-titles.md):
`Friendly practice for newcomers !ai e2 a1 lv4 !rand` uses the catalog's level-4 pool while
`room_random` randomizes humans when installed and enabled. See
[combined commands](https://github.com/crabsatellite/SDGO-Mod-Framework/blob/master/docs/room-commands.md).

## Owner: generate and install in the Manager

Open **AI 机体清单** (AI catalog) in SDGO Mod Manager:

1. Choose the source: a JSON export of the `GOnlineGame` catalog tables, or the native server INI
   (reads the database directly).
2. Choose the matching client folder (the Manager reads its unit and NPC tables) or an extracted
   `gdrs` folder.
3. Optional: an owner rules JSON, and the previous `catalog.json` when updating.
4. **检查** (check) only writes a report; **生成** (generate) writes an installable catalog to a new folder.
5. With the targets stopped, install to the client and to every participating BattleServer.
6. **导出** (export) creates the player package.

The servers and all participating clients must use the same generated catalog and be switched on or
off together. Clients without the addon should not join expanded-AI rooms.

Keep `catalog.json`: it records ID allocations, including retired units, and must be passed as the
previous catalog on every update. Do not commit an owner's generated catalog to the repository.

## Players: install the exported package

Install the matching framework and AI features first, close the game, extract the package next to
`GOnline.exe` and run once:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Install-AiCatalog.ps1
```

It only adds the symbols this mod needs, refuses conflicts and keeps a backup of the original file.
Then use the normal launcher. Players do not need Python.

## Pools and owner rules

By default only `tbl_Units` rows with `SallyReady=1` and HP > 0 are used, grouped C→LV1, B→LV2,
A→LV3, other S→LV4 and SS/SR→LV5. These are roster groups, not measured AI difficulty.
`backend/ai_catalog.rules.example.json` shows per-unit `exclude`, `levels` and native NPC `templates`.
Templates must exist on both server and client. By default the native AI script is chosen from weapon
type, grade and HP.

## Toggle and remove

The addon is off by default and is switched in the Manager; restart the affected processes after a
change. With it off, `coop_ai` / `pvp_ai` use their own `LevelN` settings again. It is not part of
the Base/Full packages.

## Command line (optional)

Without the Manager, the owner tools need Python 3.10+:

```powershell
python backend/ai_catalog.py --db-export D:/OwnerData/GOnlineGame --client-gdrs D:/OwnerData/gdrs --generate --out my-roster
python backend/install_ai_catalog.py install --role server --target D:/Server/BattleServer1 --source . --catalog my-roster/server/sdgo/mods/ai_catalog/catalog.bin
python backend/install_ai_catalog.py install --role client --target D:/Games/SDGO --source . --catalog my-roster/client/sdgo/mods/ai_catalog/catalog.bin
python backend/export_ai_catalog.py --source . --catalog my-roster/client/sdgo/mods/ai_catalog/catalog.bin --out players.zip
```

Use `--previous-catalog my-roster/catalog.json` and a new output folder for updates. Without
`--generate` the tool only writes an audit. `--connection-env SDGO_SQL_CONNECTION` reads an ODBC
connection string from a local environment variable (needs `pyodbc`). `install_ai_catalog.py` also
has `disable`, `enable`, `uninstall`, `recover` and `--host-exe`.

## Developer notes

Supported builds: `GOnline-54BE1568-00C97000` and `BattleServer-548A891D-0046C000`. No database rows
or encrypted game archives are changed.

- The server supplies the extra NPC records and private unit copies only while building an expanded
  AI pilot (`battle.build_ai_pilot`), restoring thread-local context even on native exceptions.
  Installed `coop_ai` / `pvp_ai` receive a small optional provider bridge; their settings are kept.
- After the last native NPC table loads (`grid.load_npc_table`), the client registers the generated
  NPCs in the process's native map and integrity registry (`grid.make_node`, `grid.insert_node`,
  `grid.register_record`), which also serves creators that bypass `grid.find` with an inline lookup.
  Records and nodes use the game's allocator and are released by its normal table destructor.
- Source hashes and ID collisions are checked before registration; a conflict aborts registration
  without replacing native records, and creators refuse missing or different catalog data.

Build/package: `cmake --build build/catalog --config Release`, then
`python tools/package_ai_catalog.py`. Tests: `test_ai_catalog.py`, CTest `ai_catalog_integration`, and
the caller-supplied-image tests `ai_catalog_native_test` (server) and `ai_catalog_client_map_test` (client).
