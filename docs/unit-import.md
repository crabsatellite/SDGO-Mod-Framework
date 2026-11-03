# Unit family import (Beta)

[简体中文](unit-import.zh-CN.md) · [Back to the feature index](../README.md)

Packages prepared from any source can be submitted through the documented import protocols. Acceptance depends on protocol validation and the currently supported runtime capabilities. Source extraction and source-format conversion are outside the public framework.

Import a unit archive (RAR/ZIP) in SDGO Mod Manager, fill in missing definitions, generate the shop, capsule and
encyclopedia images, then write the unit definitions to the server and build a client package for players.

This feature is in Beta: the basic workflow is still under development. Single-form units and complete two-form
units are supported, and skills must reuse skills the server already has. Before going live, try an imported unit in
real battles on a test server to check its weapons, skills and transformation.

| part | runs on | does |
|---|---|---|
| Manager **机体导入** (Unit import) panel | owner's PC | parse, complete, generate images, confirm and publish |
| `unit_catalog` | client | loads the imported units' native records and resources |

The generic release contains only the tools, never units. Owner drafts, images, publications and install receipts stay
in the owner's workspace.

## Projects, blueprints and activity pools

The blueprint authoring candidate adds editable recipes with explicit recovered,
authored or unknown provenance. A recipe requires its blueprint item, output,
three to five ordered materials and minimum levels; seven raw slots are retained
as source structure while unsupported UI counts remain blocked. The native first material
is the retained key unit. Unknown fields stay required. Published definitions
can be retired without deleting data needed by already-owned blueprint items.
Read `capabilities.blueprints.native_registration` before expecting a playable
recipe: it remains false until the paired native compiler, guarded installation
and original game synthesis path are verified.

The acquisition preview uses the configured host's real sources. Every imported
S-rank-or-higher root requires its own activity machine; A/B/C roots use a complete
obtainable blueprint route or ordinary native capsule patterns. Previewing a route
does not apply machine pricing, probabilities or rewards.

The Manager also supports metadata-first `sdgo.unit-project` authoring. Keep
descriptions, aliases, search examples, sources and original activity posters in
the project; a portable authoring ZIP carries those files with exact hashes.
Capsule editors can read the same project library and copy selected images and
metadata into their own portable catalog before a unit is ready for runtime
publication. This never registers a prize or applies a machine. User content
remains private owner data; the framework release contains only the tools.

The capsule panel also edits dedicated activity pools. Choose **普通 G 币**
(ordinary GP), **少量代币** (small token cost), or **99 代币高阶** (premium).
The **本服模板参数** controls store the owner's target frequency and price in
the private authoring catalog. Unset values use the configured host's native
reference frequency and most common matching price. The premium preset is
explicit and uses the native same-tier mean at 99 tokens. A missing ordinary
reference never silently selects the premium preset.
Owner machine data, unit content and preset overrides are not framework defaults.

Presets fill an editable form with native reference rewards. Adjust currency,
price, units, blueprints, consumables, amounts and frequencies before saving.
Multiple amounts of the same item are supported. Frequencies total 10000 slots;
all amounts of any category together must stay at or below 50% for the native
repeat-avoidance path. These are configured sequence frequencies, not a promise
of independent random draws. Native starts, limits, billing and delivery remain
in effect. Save validates the host catalog; compile and apply pair the client
poster with the server pool in the existing deployment transaction. Updates
preserve native runtime state and player progress rather than resetting them.

## Setup

Open `SDGO Mod Manager.exe`, choose **机体导入** in the sidebar and set:

- **Client**: the client directory that matches the server.
- **Workspace**: select an existing directory with `unit-families.json` to keep its drafts, IDs, publications and
  history; leave it empty to use `data/units` under the Manager workspace.
- **WorldServer INI**: leave it empty for offline authoring that does not write the game server. Database credentials
  are read only from the local INI and never enter player packages.

Manager bundles Python, Pillow and pyodbc; RAR parsing needs a local 7-Zip. Opening the panel changes no game
resources and stops no services.

## Import workflow

1. Select or drop a RAR/ZIP. IDs and fields are read from inside the BIN records, not from file names. Unsupported
   layouts, corrupt records, duplicate identities, incomplete transformation links or missing core models are rejected.
2. Single-form units (transform target zero) are detected automatically. Two-form units show each form's stats,
   target, weapons and resources; choose the form that enters the hangar.
3. Decoded stats are shown as-is. Missing server fields stay empty rather than zero: type them, upload a JSON
   definition, or choose a stock unit as a template. Templates only fill empty fields and never overwrite decoded data.
   Every field shows its source and stays editable.
4. Choose a native permanent unit-item template. The tool assigns a category ID in its managed range and registers only
   the unit category.
5. Review the dependency list. BIN, model, animation, effect, texture and SPR files can be uploaded separately; a
   missing weapon needs both a client record and a server definition.
6. Choose or upload the shop and capsule images and review the generated output, see [Images](#images).
7. Save the draft, check the generated icons, tick each item and click **确认图标并导入** (confirm icons and import).

Confirmation records the current content revision and generated image hashes; later edits need a new confirmation.

### Preview in the game (optional)

Click **准备独立预览副本** (prepare a preview copy) and **打开原生预览客户端** (open the preview client), then log in
normally to see the new unit in the game's own shop unit view and capsule candidates.

- The preview copy is a separate client managed by this module. It keeps the original client's fixes and connection
  settings and does not stop the original game or servers.
- The new unit exists only in that copy's memory. There is no real shop product yet, so only the unit view is shown,
  never the template product's name or price.
- Purchase, gift and capsule requests are blocked in the preview copy. Preparing it writes nothing to the server and
  grants nothing.
- Preview copies live in the workspace's `native-previews` folder. Do not distribute them; give players the player
  client package. The panel can clean up expired copies while keeping the current preview and drafts.

### Images

- **Shop**: the shop list shows the 122×122 owned encyclopedia icon; the import page's product preview uses the same.
- **Fixed capsules**: cards use the existing card generator (portrait crop, native frame, rank bar and rare
  silhouette), shared with the capsule compiler.
- **Custom capsules** (such as SR): use the unit's own candidate portrait; the game's control draws the grade frame.
- **Detail image, alternate list image and candidate portrait** are three separate channels. They default to the
  natively referenced images; **由所选源图生成** (generate from the selected source) replaces a channel with your image.
- **Encyclopedia**: one entry per hangar root form, with 122×122 owned/unowned icons; alternate forms are not separate
  hangar units. Without a matching blueprint, no template recipe is borrowed.

If the archive has no icon of its own, borrowed stock artwork may show a different unit. Capture the real model's
ready/selection poses (see [developer notes](#gametest-checks-and-captures)), upload the PNGs and switch to
**由所选源图生成**. Panel thumbnails are generator output; the in-game preview shows the actual result.

### Limits

- Archives up to 32 MiB, 128 MiB / 512 files unpacked; PNGs up to 8 MiB and 1–2048 px per side. Escaping paths,
  links and duplicate paths are rejected, and installers or database scripts inside the archive never run.
- A new unit ID that collides with a stock or other imported unit blocks confirmation; there is no blind binary
  renumbering.
- Only single-form units or complete, explicit two-form links are supported.
- A missing skill blocks confirmation by default. For skill slots 1 and 2 the panel offers **移除缺失技能 … 后导入**
  (remove the missing skill and import): every form referencing it gets `0` on both client and server, the source
  RAR/BIN is kept, earlier preview approvals are invalidated and the skill's effect is gone. Other skills, weapons and
  links are unchanged. Slot 3 cannot be removed yet.

### Server internal names and Chinese display names

Some native server definition columns are `varchar` and cannot store Chinese under a non-Chinese collation, so
internal names must be ASCII: use the archive's English name, type one or pick it from a template. Chinese display
names are stored separately in the client's wide-character records and `tbl_UnitNames.NameCN`. Every field is read
back after writing; truncation, lossy encoding or external edits roll back the whole transaction.

## What confirmation writes

- **With a WorldServer INI**: one SQL transaction under an application lock updates unit, weapon, name and item-category
  definitions. The tool does not restart World/Store/Battle/Agent; refresh those caches as prompted.
- **Offline**: only this tool's unit catalog is updated, and a player client package plus a server plan are generated.
  The panel shows that the game server was not written.

Both modes build and check the publication before marking it confirmed; a SQL failure never marks the draft imported.
If the server committed but the local catalog was not saved, the next start recovers from the publication receipt.
Publications include every registered unit in the workspace, so a second unit never replaces the first.

Import writes only non-player definition tables, this module's ownership ledger and, with the collection capsule
enabled, family metadata (root form and links) for its live pool. Balances, hangars, grants, native SQL procedures, shop
prices and capsule odds are untouched.

## Making units obtainable

**Confirmation does not grant units, create products, set prices or add capsule slots.** The owner sets that up:

- **Shop**: in the **商城商品** panel for the same database, click **刷新物品目录** (refresh item catalog), pick the new
  unit item, add a product, set its price and apply. A saved draft is not a live product.
- **Capsules**: in **机体导入**, click **同步已确认机体与扭蛋头像** (sync confirmed units and portraits), then choose the
  machine, slot and weight in **扭蛋机**, compile and apply. Syncing changes no existing machine, price or weight.
  Imported C/B/A units can use native custom ordinary slots; rare slots and S-rank candidates keep their rules.

Syncing accepts only publications applied to the same game database, identified by the `[DB Game]` server and database
name (WorldServer and StoreServer INIs pointing at the same database match; credentials are not compared). Offline
publications, unconfirmed drafts, other databases and stale catalog revisions are rejected. After editing a published
unit, shop and capsules keep using the last confirmed content until it is confirmed again. After a card generator
upgrade, recompile cached capsule cards.

## Distribution

After confirmation, download the **玩家客户端包** (player client package) and the separate server plan. The client package
contains the matching DLL, native records, resource archive, public definitions and installer, and no server
passwords, player data, logs or full game files. It is for deployment only and cannot be imported as a source archive.

With the game closed, players choose their client under **游戏功能** in Manager and click **导入客户端配置包**. Manager
uses its built-in installer and never runs scripts from the ZIP. Offline installation:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Install-UnitCatalog.ps1 -ClientRoot D:/Game
powershell -NoProfile -ExecutionPolicy Bypass -File Install-UnitCatalog.ps1 -ClientRoot D:/Game -Uninstall
```

The installer updates only resources it owns, keeps other Mods and global switches, and refuses foreign changes or
collisions. Players need the unit package together with the updated capsule resources; update the framework first if
an older capsule catalog is installed.

## Disabling and backups

Disabling keeps definitions, IDs and resources and removes no units players already own; purging server definitions
is a separate reference migration. A published unit's hangar root form stays fixed so edits cannot change the identity
of owned units. Back up the owner workspace (drafts, family identities, IDs and publication receipts), and never
reassign published content from an empty workspace.

## Logs

| log line | meaning |
|---|---|
| `native unit catalog registered: <n> added, <m> already present, <t> total` | imported units loaded |
| `unit <id> weapon <id> is unavailable` and other `... is unavailable` lines | the client package does not match the game data |
| `native allocation failed ...` / `native insertion failed ...` | native registration failed; the client exits rather than load half a unit |

## Developer notes

### Build and tests

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32 -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure -R "unit_(catalog|preview|import|presentation)|mod_manager"
python -B tools/mod_manager.py --workspace D:/ModManager
```

The developer view can point to a native snapshot, a separately built `unit_catalog.dll` or a 7z path. Drafts stay
editable without them; confirmation lists the missing component as blocking.

### Client loading

`unit_catalog` supports `GOnline-54BE1568-00C97000` and checks the executable hash and signature. After the last
native table loads, it registers complete unit/weapon/item records with the native allocator and checks cross-kind ID
collisions in the data tree. Everything is prechecked and allocated first; if native registration still fails after
some records were inserted, the native tree cannot be rolled back, so the Mod logs diagnostics and ends that client.

The game does not read new models from the working directory. Client packages carry `resource-overlay.ini`,
`resources.zdx` and `resources.zpk`, and the game reads models, animations, textures and effects from that archive.
With the capsule catalog also enabled, both share one resource reader.

`registrations.json` and Manager's `/api/providers/units/api/units/registrations` expose stable `unit:<root ID>`
identities, categories, names, forms and the matching `shop_sprite` / `capsule_sprite`. Capsule machine cards are
separate resources, not the unit's three UI sprite fields.

The source-only Type-13 skill profile validates paired record layouts and dependency evidence, but new skill
registration is not enabled. Record formats, image fields, resource archives, transactions and extension points are
described in the [native unit archive interface](unit-import-protocol.zh-CN.md) (Chinese).

### GameTest checks and captures

These checks use the separate [GameTest](game-test.md) client and database and never touch real player data. Set the
GameTest component options before `prepare` seals the inputs, and bind a unit UID the test account actually owns.

**Encyclopedia and details**: enable `UnitPresentations=1` in the candidate stage's `game_test_client/mod.ini`, plus
`CaptureNativeFrames=1` for screenshots:

```powershell
python tools/game_test.py prepare --client-template <local-test-client> --stage <candidate-stage> --run-dir build/game-test-runs/unit-check --target-host <test-loopback-ip> --login-port <test-login-port> --account-profile <private-existing-account.ini> --play-unit-uid <owned-unit-uid> --include-mod unit_catalog
python tools/game_test.py run --run-dir build/game-test-runs/unit-check --timeout 600
```

The scenario reads the actual UID through native login and hangar selection, opens the encyclopedia, searches and
selects the entry, and waits for the detail model and transformation state.

**Portrait captures**: also enable `UnitPortraits=1`, then export:

```powershell
python -m tools.game_test_portraits --run-dir build/game-test-runs/unit-check --unit <root-unit-id> --output build/native-unit-portraits/new-unit
```

It plays `wait_01` / `choice_01` on the initialized native detail entity, reads the real backbuffer before Present and
crops to the model viewport, recording the source frame and input hashes. Check pose, crop and weapon attachment before
uploading the PNGs to Manager.

**Collection capsule without purchasing**: enable `CollectionPreviewOnly=1` and run with `--pages-only`, without
`--collection-draw`. It adds only `pages.collection.preview`, opening the real activity machine, price and native
confirmation and checking that no purchase was submitted. Activity and collection pages show one poster per machine,
not every unit in the pool.

**A specific fixed/custom candidate**: enable `UnitPresentations=1`, set `UnitAcquisitionKind=1` (fixed) or `3`
(custom), `UnitAcquisitionMachine` and `UnitAcquisitionUnit`, and run with `--pages-only`. `pages.unit_acquisition`
opens the unit view through the native machine/candidate controls and waits for the `0x01F8` consumer event, without
purchasing or changing custom selections; add `CaptureNativeFrames=1` to inspect the actual card and model.
