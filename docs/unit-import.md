# Unit family import (Beta)

`unit_catalog` is an optional client mod with an authoring panel in SDGO Mod Manager.
The generic release contains tools only: neither supplied sample unit nor owner drafts/assets are bundled.
The owner imports a RAR/ZIP, reviews decoded units/weapons and dependencies,
fills missing definitions or uploads supplemental records/resources, selects publication
images, then confirms generated icons and publishes paired definitions and resources. Native game review remains a separate, optional workflow.

Packages prepared from any source can be submitted through the documented import protocols. Acceptance depends on protocol validation and the currently supported runtime capabilities. Source extraction and source-format conversion are outside the public framework.

Malformed or unsupported core records, conflicting identities and missing core
models are rejected during intake. Editable omissions stay in a draft with explicit
required fields. No confirmation or export is allowed while blocking issues remain.
Single-form units with no transform target are detected automatically; linked forms
have an explicit inventory root and supported native behavior profile.
Missing skills remain blocking by default. An explicit **remove missing skill** action is available
for verified skill slots 1/2: it zeros the reference in every affected form on both sides, preserves
the source files, records the omission, and invalidates prior preview approvals. That skill's effect
is lost. Known skills and the unverified third client slot are not removed by this action.

See [the Chinese guide](unit-import.zh-CN.md) for the full workflow and boundaries.

```powershell
python -B tools/mod_manager.py --workspace D:/ModManager
```

With a host INI configured, final confirmation writes only the typed definition
tables in one transaction. Without it, confirmation publishes into the local module
catalog and exports a server plan; it does not claim to have changed a game server.
The tool never restarts services, grants units, sets shop prices or edits gacha slots.

Open the **Unit import / 机体导入** panel and select the existing unit workspace, client and optional
WorldServer INI. The frozen Manager includes the runtime; RAR inspection requires local 7-Zip.
There is no separate unit HTTP server, session or launcher.

Select a permanent native unit-item template. Review the generated shop and capsule icons, save the draft,
and use **确认图标并导入**. Approval records the current content revision and generated image hashes.
A resource review is not reported as a native game rendering or combat pass.

For optional game review, use **准备独立预览副本**, then **打开原生预览客户端** and log in normally.
The private copy uses existing native presentation controls and blocks spending.
Its genuine native-call receipts are recorded separately. Scoped cleanup keeps current previews and authored drafts.
If borrowed artwork depicts a different unit, capture the actual model's original ready/selection poses and upload PNGs.
Uploaded images use the selected-source generator rather than silently retaining the old referenced artwork.

Each inventory root also receives an original encyclopedia-table entry, with distinct 122×122 owned/unowned icons.
Linked alternate forms do not become separate inventory entries. Missing blueprint metadata does not inherit an unrelated recipe.

After online confirmation, refresh the shop's item list, create a product, set its price and apply it.
The Manager also offers **同步已确认机体与扭蛋头像**: explicitly synchronize approved unit candidates
and images into the configured capsule catalog, then choose slots/weights, compile and apply there.
Synchronization preserves existing machine economics and requires the same game database.
Offline drafts cannot become live candidates. Approved imported C/B/A units may use ordinary native custom slots; rare-slot and S-rank restrictions still apply.
The fixed-card generator calls the existing `Assets.card` crop/frame/rank/rare-silhouette implementation;
it no longer pads a whole robot image into an empty rectangle. SR custom candidates retain their native portrait
and let the existing game's custom selector draw the grade frame. Capsule compiler v9 retains master's uploaded portrait/crop overrides and resident activity resources, and invalidates the old cache.
The product generator preserves the distinct native unit sprite consumers instead of assigning one image to all three.
Manager serves the same generator output used in publication; complete SKU pricing stays owned by the shop module.

Generated client packages contain the mod, approved native records and a sealed native
resource archive. The game reads imported models, animations, textures and effects from
that archive; local resource copies also remain available to the authoring tools. The
unit and capsule catalogs share one resource reader when both are enabled. Update the
framework before installing content alongside an older capsule catalog.
With the collection module enabled, root/form metadata is registered in the same SQL transaction for its live catalog pool. Import client packages through Manager with the game closed, or use `Install-UnitCatalog.ps1 -ClientRoot ...`.
The installer preserves other mods, refuses foreign collisions and supports uninstall
from its ownership receipt. Players then use their normal launcher.

The stock framework ships the mod disabled, without owner content. Resource thumbnails
are not screenshots and do not certify actual game rendering. Native login, inventory, encyclopedia and sortie checks use the separate original GameTest workflow.
Static checks and native table registration tests do not certify live combat,
transformation, multiplayer, ammunition or settlement behavior.

For a non-purchasing check of an installed collection machine, enable
`CollectionPreviewOnly=1` in the sealed GameTest component and run with
`--pages-only`, without `--collection-draw`. This reuses the existing native
preview case and checks that no purchase transport was submitted. Activity and
collection machines display one native poster per machine; they do not list
every unit portrait in their pools. Verify root registrations, machine posters,
and fixed/custom candidate portraits separately.
