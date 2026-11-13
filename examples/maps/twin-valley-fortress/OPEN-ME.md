# OPEN: Twin Valley Fortress and Operation Odessa

This folder is a complete, public `sdgo.map-package` example. The Mod package includes this source tree inside the standalone Manager. On first view, the Manager copies it into a versioned, hash-checked source cache. Bundling, caching, or viewing it does **not** register the map, compile a client package, change a database, enable a mission, or restart a game service.

## Files to open first

- `map.json` describes the terrain, objects, collision, spawn positions, room presentation, and the SHA-256 of every package file.
- `missions/odessa.json` is the authored `sdgo.map-mission` source. It shows four difficulty levels, per-difficulty NPC HP, phase objectives, an eight-player party transition, allied retreat, an invisible fortress boundary with native contact effects and warning/damage, native resource references, and cutscene timelines.
- `scripts/add_mission.py` copies another neutral mission JSON file into a map package and updates the `files` digest entry. It does not connect to a client, server, or database.
- `CREDITS.md` records the public art sources and license boundary.

## Import another mission

1. Copy this folder and give the package a new `map_key` in `map.json`.
2. Author a new mission JSON with `schema: "sdgo.map-mission"` and a unique `mission_key`.
3. From the copied package folder, run:

   ```text
   python scripts/add_mission.py . path/to/new-mission.json
   ```

4. In **地图与任务导入**, register the copied folder. Select the mission, then review the generated client package and server plan.
5. Compilation, client installation, server preflight, and server application remain separate explicit actions.

The example refers to compatible resources already present in the target game by their neutral runtime names. It does not include original game programs, databases, credentials, player data, or source-specific extraction/conversion tools.
