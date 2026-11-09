# Map and mission import

[简体中文](map-import.zh-CN.md)

Register maps and missions that were made outside the game and prepared in the **map package protocol**: new terrain, objects, spawn points, sky, minimap and music, plus PvE missions with phase objectives, enemy waves, character dialogue, opening/mid-mission/ending cutscenes and banner effects. Imported maps appear in the battle room map list and imported missions in the mission room list, next to the original ones.

The feature only imports. It reads protocol data, generates the records and resources the game needs and installs them on the client and the server. It does not unpack, export or change the game's own maps and missions.

## Parts

| Part | Role |
| --- | --- |
| Client Mod `map_catalog` | Registers imported map rows and mission records at startup, serves terrain/model/texture resources, loads mission scripts (cutscenes, phase events) and enables the character dialogue panel for imported missions |
| Manager page "地图与任务导入" (map and mission import) | Register map packages, edit names/modes/servers, compile, install the client package, check and apply the server rows |
| Map package protocol | `map.json` describes a map; `missions/*.json` describe missions. See below |

## Using it in Manager

1. In the module settings, set the **map workspace** (leave empty to use the Manager data folder), the **matching client folder** (compile baseline and install target) and the **WorldServer INI**.
2. **Register a map package**: enter the folder that contains `map.json`. Every file digest is checked and the package is copied into the workspace. Registering the same `map_key` again replaces its files and keeps your edits.
3. On each map card adjust the Chinese/English name, the PvP modes, the server types, which missions to publish, whether the map is published and its position. Click Save.
4. Click **Compile all enabled maps**. All enabled maps and missions become one client package and one server plan.
5. Close the client, then click **Install client package**. You can also download the client package and hand it to other players as the server owner's client package or update.
6. Click **Check server** to see the rows that will be added or updated and any conflicts; with no conflicts click **Apply to server**, then **Read back**.
7. Restart the World/Battle services and the client.

Numbers are assigned at compile time: map numbers continue after the client's own maps, and every kind of mission record continues after the original mission records, without gaps (the game reads these lists as contiguous numbers). Removing or moving a map therefore renumbers the maps and missions after it; compile, install and apply again and the server updates and removes the rows this workspace wrote earlier.

The server write runs in one transaction. A row with the same key that was not written by the map import refuses the whole apply, so original data is never overwritten.

## Map package protocol (`map.json`)

A map package is a folder: one `map.json` plus the glTF 2.0 models (`.glb`/`.gltf`), PNG images and optional MP3 it references. Every file must be listed with its SHA-256 in `files`.

**Coordinates** match the game world: X east, Y up, Z north, origin at the terrain centre, game units. Height and mask images cover the whole terrain with row 0 at the north edge and column 0 at the west edge. glTF assets keep their own convention (right-handed, -Z forward) and are converted at compile time. `rotation_y` is clockwise seen from above: 0 faces north, 90 faces east.

| Field | Meaning |
| --- | --- |
| `schema`, `schema_version` | Always `sdgo.map-package`, `1` |
| `map_key` | Unique map identifier: 3..64 lowercase letters, digits, `.`, `_`, `-` |
| `name`, `description` | `{"zh": …, "en": …}`; names up to 48 characters |
| `modes` | PvP modes: `normal`, `deathmatch`, `grid`, `tag`, `captain`, `ai`, `ai_battleship` (may be empty for a mission-only map) |
| `servers` | Server types: `training`, `beginner`, `expert`, `clan` |
| `terrain.half_extent` | Terrain half size: 4096, 8192 or 16384 |
| `terrain.height` | `image` (257×257 greyscale PNG), `min`, `max` |
| `terrain.layers` | 1..8 surface layers: `texture` (PNG), `mask` (optional on the first layer), `tiling` |
| `terrain.lighting` | `sun_direction` (y must be negative), `sun_color`, `ambient`, `bake_shadows` (baked light and shadows) |
| `terrain.fog`, `terrain.sky` | Fog colour and start/end distance; sky `zenith`/`horizon`/`ground` colours |
| `terrain.play_area` | Playable `half_x`, `half_z` and height `ceiling` |
| `objects` | Up to 1024 objects: `model` (a glTF in the package) or `native_model` (a model the target client already has), `position`, `rotation_y`, `scale`, `collision` (`auto`, `none` or a list of boxes), optional `id`, `only` (`pvp`/`mission`), `initially_hidden` |
| `spawns` | `team_a` and `team_b` with 6 points each, `shared` with 1..12, plus `grid_respawn_a`/`grid_respawn_b` for Grid mode; each point has `position` and `facing` |
| `sectors` | Named mission sector spawn points (`{name: {spawns: [...]}}`) |
| `item_patterns` | Item spawn sets; `red` and `blue` need the same number of sets |
| `presentation` | `preview` (PNG), `minimap` (PNG), `bgm` (an MP3 in the package or `{"native": "BGM\\NNNN.mp3"}`) |
| `files` | SHA-256 of every file in the package |

## Mission protocol (`missions/*.json`)

Mission files live under `missions/` in the map package and are listed in `files`. One mission is one entry of the mission list and can have 1..4 difficulties.

| Field | Meaning |
| --- | --- |
| `schema`, `schema_version` | Always `sdgo.map-mission`, `1` |
| `mission_key`, `name`, `description` | Mission identifier, name and description |
| `briefing.zh` | Briefing panel: up to 5 lines of 49 characters |
| `difficulties` | Per difficulty: `level`, `required_rank`, `time_limit` (seconds), `lives` |
| `bgm` | Mission music (same form as the map's `presentation.bgm`) |
| `npcs` | Units: `template` (an NPC the target server already has), or `unit` + `base` (any unit using an NPC template); `hp`, `boss`, `name` and `ai` (an AI script name such as the battle AI's `VSAI_Infighter_01`) can be changed |
| `lines` | Character dialogue: `portrait` (a client portrait `sprs\\NNNN.spr`), `speaker`, `text` |
| `banners` | Banners: `image` (512×128 PNG), shown as an effect in cutscenes |
| `sectors` | Sector change: `warp` (1..8 points), `facing`, optional `rect` (playable area) |
| `cutscenes` | Cutscenes, see below |
| `phases` | 1..8 phases played in order, see below |
| `opening`, `ending` | Cutscene played at the start and on success |

**Phases** (`phases[]`):

| Field | Meaning |
| --- | --- |
| `key`, `objective` | Phase key; the objective text is shown in the objective bar |
| `goal` | `kill`: destroy a number of given units (several entries must all be met); or `survive`: hold out for some seconds |
| `fail` | `time_limit`: time limit; `lose`: the mission fails when a given allied unit is destroyed |
| `spawns` | Waves: `npc`, `positions`, `at` (seconds after the phase starts), `every` (interval), `side` (`enemy`/`ally`), `min_free` |
| `slots` | On-screen unit limits: `ally` + `enemy` ≤ 10 |
| `enter` | On entering the phase: `cutscene`, `lines` (dialogue), `sector` (sector change); use `opening` for the first phase |
| `reward` | Phase reward `points`, `unit_exp` |
| `next` | Next phase (default: the following one) |

**Cutscenes** (`cutscenes{}`): `length` (1..60 seconds), `actors` (`npc`, `at`, `facing`), `camera` (keyframes `pos`/`look` starting at `t=0`, compiled into a smooth curve) and `events`:

| Event | Meaning |
| --- | --- |
| `anim` + `actor` | Play a unit animation (the compiler checks that the unit's model has it); `loop` optional |
| `move_to` + `actor` | Move the actor to a point |
| `banner` | Show a banner in front of the camera; `distance` or `at` optional |
| `line` | Show a dialogue line |
| `effect` | Play an effect the client already has: `{fx, at, scale}` |
| `sound` | Play a sound effect the client already has |
| `shake` | Camera shake `[amplitude, seconds]` |

Players cannot move during a cutscene; control returns when it ends.

The compiler checks placement and camera work: spawn points, mission spawns, cutscene actors and warp points must not be inside an object; a cutscene camera must not be below the ground or inside an object, and its view toward the target must not be blocked by terrain or objects (models the client already has are tested with their own collision, so open structures such as gate frames and arches stay open). Errors name the cutscene, the time and the camera position.

## Activation and compatibility

- Enable the client Mod `map_catalog` and install the same publication on both the client and the server; updating only one side leaves maps or missions missing.
- Servers read these tables at startup: restart the World/Battle services after applying, and restart the client after installing the client package.
- Imported missions run in mission rooms as native missions; character dialogue uses the game's own character dialogue panel (portrait and name).
- Referenced client models, portraits, effects, sounds and BGM must exist in the target client; the compiler checks them.

## Troubleshooting

- In the client log `sdgo/logs/sdgo.log`, `map_catalog: native map rows registered` and `native mission records registered` confirm registration; `imported resource loaded: trns\…` shows that a battle used the imported terrain; `imported mission script call: …` and `imported cutscene finished: …` trace phase events and cutscenes.
- If a mission does not start or units are missing, turn on "write the game's native debug log" in the Mod settings; the client `app_log` then names the missing record numbers.
- Compile errors name the exact field (for example a spawn point below the ground, an undefined dialogue line, or an animation the actor's model does not have).
- If "Check server" reports conflicts, the target database already has non-import rows with the same numbers and the apply is refused; make sure the client and the server come from the same original data.
