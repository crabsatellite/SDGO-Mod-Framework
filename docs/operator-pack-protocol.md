# Operator pack and ability extension protocol

[简体中文](operator-pack-protocol.zh-CN.md) · [Operator feature guide](operator-assistant.md)

`sdgo.operator-pack` version 1, normally stored as `.opack`, lets server owners exchange operator client
resources, ability drafts, acquisition rules, and shop draft defaults. This reference describes the
format and validation rules SDGO Mod Manager accepts when importing such a pack, how imported resources
are compiled, the Manager API, and the ability extension ABI, for developers who build packs or
extension Mods. Pack data never executes itself and does not directly apply server or shop changes.

Voice, artwork, and other game assets are always **client content**. These assets and the `.opack` files
that contain them may be delivered only through the server owner's client package or update. They
must not be embedded in the SDGO Mod Manager executable, public framework releases, source repository,
or generic server package. Manager and public framework artifacts contain only the common protocol,
importer, compiler, and ability runtime.

## Assemble a neutral sharing package

The fictional “Training Operator” uses only the author's `training.png`, the same image for all seven roles,
explicitly inherited native dialogue/voices, a 5% general GP bonus and no drops. `60001` is an example template
that must exist in the target client/server catalog; Manager allocates the new operator's actual ID.
No existing owner's character, game artwork or voice recording is used.

Owners can create an operator, upload their own PNG and edit abilities directly in Manager. Package authors
can run this in a framework source environment with their own 1–2048-pixel `training.png`:

```python
from pathlib import Path
from manager.engines.operators.assets import validate_png, sprite_binary
from manager.engines.operators.legacy_pack import package

image = Path("training.png").read_bytes()
width, height = validate_png(image)
files = {
    "txrs/9001.txr": image,
    "sprs/9001.spr": sprite_binary("txrs/9001.txr", width, height),
}
abilities = {
    "schema_version": 1, "mode": "custom",
    "general": {"gp_percent": 5, "unit_exp_percent": 0, "energy_save_percent": 0},
    "victory": {"gp_percent": 0, "unit_exp_percent": 0, "energy_save_percent": 0},
    "defeat": {"gp_percent": 0, "unit_exp_percent": 0, "energy_save_percent": 0},
    "timed": [], "pilot_exp_percent": 0, "mission_parts": None, "extensions": {},
}
raw = package(
    files,
    {"dialogue": "", "sprites": ["sprs/9001.spr"] * 7, "intro_voice": ""},
    pack_id="example.training.operator", name="训练副官", source_template=60001,
    resources_metadata={"voice_policy": "native_fallback"},
    abilities=abilities,
    acquisition={"delivery": "product", "drops": []},
    storefront={"name": "训练副官", "label": "永久副官", "price": 1000,
                "currency": "GB", "validity": 1},
)
with Path("training-operator.opack").open("xb") as output:
    output.write(raw)
```

`package` writes `operator-pack.json`, exact sizes/hashes and the resources into the ZIP. The TXR contains real
PNG encoding; the framework creates the SPR layout and texture reference. Sharing one image across roles is
only a demonstration: production authors should supply suitable artwork/crops without changing the array order.
To replace voices, also provide dialogue GDT, intro ZSD and both voice fields' dependencies, and select
`provided_only`. This example's explicit `native_fallback` depends on target-native dialogue and voices; it is
not a self-contained localized voice pack.

1. Select the target client/server definitions in **副官与能力**, then preview the `.opack` with **导入副官分享包**.
2. Review artwork, templates, base dependencies, the 5% general bonus and `product` delivery; confirm a draft.
3. API callers read the current `revision`, submit `import-preview`, then pass its `pack_sha256` as the import
   request's `sha256`. Preview normalizes the ZIP: neither the original file hash nor `source_archive_sha256`
   identifies that normalized cache entry.
4. Save the draft, apply server definitions and generate/install the operator client package separately. Refresh
   native services as Manager requires; separately check Battle components/restarts for independent abilities.
5. To sell it, read the applied operator category from the same database in Shop, create an unlisted offer, then
   explicitly list/apply it. `storefront` is a prefill; `delivery=product` neither grants nor lists the operator.
6. Distribute the generated client update. An `.opack` is an author exchange package, not the runtime ZIP accepted
   by the ordinary **导入配置包** installer.

## Four layers

| Layer | Version 1 purpose | State after import |
|---|---|---|
| `resources` | Dialogue table, seven sprite roles, sortie voice, and dependencies | Compiled into the new operator's client namespace |
| `abilities` | Native numeric bonuses, mission parts, pilot EXP, and namespaced extension payloads | Saved as a draft; applied separately to server and runtime |
| `acquisition` | Free-for-all/product registration and mutually exclusive drops | Saved in the catalog; applied separately to the server |
| `storefront` | Product name, label, price, currency, and validity defaults | Used only to create a disabled shop draft |

`layers` must exactly equal:

```json
{
  "resources": 1,
  "abilities": 1,
  "acquisition": 1,
  "storefront": 1
}
```

`required_features` is reserved in version 1 and must be empty. It does not register ability
extensions; every nonempty value is rejected.

## ZIP and manifest

The ZIP root contains `operator-pack.json`. Native resources are accepted only at:

- `gdrs/<digits>.gdt`
- `sprs/<digits>.spr`
- `txrs/<digits>.txr`
- `sdrs/<digits>.zsd`

Paths are compared case-insensitively. Absolute paths, drive prefixes, empty segments, `.`, `..`,
control characters, duplicate paths, symlinks, and encrypted entries are rejected. Limits are
128 MiB compressed, 256 MiB expanded, 64 MiB per file, and 4096 entries.

Files outside the resource roots appear in `ignored_files`; they are never executed, installed, or
re-exported. Do not include scripts, DLLs, installers, or other executable content.

`files` must list every native resource in the ZIP with its exact integer byte length and SHA-256.
The Chinese reference contains a complete manifest-shaped example. Its placeholder lengths and hashes
must be replaced with values calculated from the actual resource bytes.

`pack_id` must match `[a-z0-9][a-z0-9_.-]{0,95}`. `name` is limited to 1–48 UTF-16 characters.
`distribution` must exactly be:

```json
{"scope": "client-content", "bundle_with_manager": false}
```

## Resource layer

`resources.profile` contains exactly `dialogue`, seven `sprites`, and `intro_voice`. An empty role
inherits that resource from `source_template`; every nonempty path must exist in `files` with the
correct resource type. `source_template` controls resource fallback. The separately selected
`template_id` controls skills and actions, so they may differ.

SPR version 10000 and 11000, kind 20, and 1–1024 frames are accepted. The importer preserves every
frame's dimensions and UV values, including native wrap and mirror coordinates. Each referenced TXR
should be included as its own file.

The texture reference has a separate 32-wchar field: at most 31 UTF-16 code units plus its terminator.
The following 32-wchar text field and remaining frame bytes are preserved independently.

Dialogue records are 710 bytes. Byte offsets identify a 50-wchar `event` at `+0`,
the caption at `+100`, and two independent 50-wchar voice references at `+502` and
`+602`. The importer traverses every dialogue row and both voice variants; there is no fixed number of
voice files or events. ZSD keeps natively playable MP3, WAVE, or MPEG sync-stream bytes without an
external transcoder.
The current reader does not interpret the overlapping `+50` position as another independent event field;
uninterpreted bytes remain unchanged.

`voice_policy` is either:

- `provided_only`: clear dialogue voice references whose assets were not supplied. This is the default
  when `locale` is present.
- `native_fallback`: keep references to matching resources in the target client.

A separately localized pack should use `provided_only` and include the full dependency closure, so
missing assets cannot silently substitute another language.

Preview exposes `base_dependencies` (missing dependencies the target client must provide),
`voice_events`, `sprite_frames`, and `ignored_files`. Resources with an invalid format are always
rejected.

Provided resources are rewritten into the imported operator's private client namespace. They do not
replace the template's GDT, SPR, TXR, or ZSD files.

The client compiler writes a per-operator `resources.zdx`/`resources.zpk` bundle and a hashed
`resource-overlay.ini`. The shared native resource broker opens these virtual keys through the
original game stream interface. Loose generated files alone are not a supported runtime installation.
The original game archive stays unchanged; the source `.opack` remains separate for sharing and recovery.
Compiled texture references use the bounded private alias `sdgo/optex/<operator_id>/<numeric_index>.txr`
to fit the native SPR field. The broker restricts each alias to the matching operator bundle; other
generated resources retain their operator-specific namespace.

## Ability layer

Omitting `abilities`, or setting it to `null`, means full template inheritance. An explicit object must
contain exactly:

```text
schema_version, mode, general, victory, defeat, timed,
pilot_exp_percent, mission_parts, extensions
```

`mode` is `inherit` or `custom`. An explicit `inherit` object must still include every field, with zero
bonus groups, no timed slots or extensions, `pilot_exp_percent: 0`, and `mission_parts: null`.

Each of `general`, `victory`, and `defeat` contains integer `gp_percent`, `unit_exp_percent`, and
`energy_save_percent` values from 0 through 100. Booleans are rejected. The mappings are:

| Protocol field | Native field or execution boundary |
|---|---|
| `gp_percent` | `PointAdd`, the GP percentage |
| `unit_exp_percent` | `UnitExpAdd` |
| `energy_save_percent` | `EnergySave` |
| `pilot_exp_percent` | Separate BattleServer runtime capability; never `PointAdd` |

`general`, `victory`, and `defeat` compile to `BattleResult`, `TeamWin`, and `TeamLose`. Unconfirmed
native fields such as `ChangeSp` and `ServiceType` continue to inherit from the template; the protocol
does not assign semantics based on their names.

`timed` has at most four entries, sorted by `start`, with strict `HH:MM` values. Both native bounds are
inclusive and midnight-crossing ranges are not supported. Overlap is accepted, but the first matching
native slot from `AddOn1` through `AddOn4` wins.

`mission_parts` is `null` for inheritance, `true`, or `false`.

The compiler first reuses an exact native `SkillType=1` row. Original `SkillNo` values 1–27 are
protected. New `SkillNo` or `TimeID` allocation is permitted only when the target server has a verified
native capability for it, so pack authors must not assume arbitrary new combinations are deployable.

## Acquisition and storefront layers

`acquisition.delivery` is `all_players` or `product`. The first is granted through the native operator
login/page flow after server application. The second registers an operator item without granting or
listing it.

Drop rules use an existing `category_id`, `chance_bps` from 0 through 10000, and `min_amount` /
`max_amount` from 1 through 65535. Their total chance cannot exceed 10000; the remainder means no drop.
At most one rule wins per native operator-drop trigger. The same `category_id` may appear in multiple
mutually exclusive rules to express common, surprise, and jackpot quantity bands. An optional `pool`
must already exist in the target catalog.

`storefront` accepts `name`, `label`, positive integer `price`, `GB` or `MB` `currency`, and positive
integer `validity`. These fields are only draft defaults. Only enabled, managed `product` registrations
appear in the shop import list, and their item category must already have been applied to the same host
database. Shop creation always produces `enabled=false`; it does not call shop apply or publish.

## Namespaced extension SDK

`abilities.extensions` maps namespaces to finite JSON payloads. Names must match
`[a-z][a-z0-9_.-]{0,63}`, be present in `catalog.registered_ability_extensions`, and have an enabled,
version-compatible trusted native handler on the server.

A pack cannot register a handler, load a DLL, or execute a script. The framework checks only that the
payload is finite serializable JSON. There is no universal parameter-schema registry. Extension authors
must define and version their payload contract in their own documentation and namespace.

The handler ABI is in
[`include/sdgo/operator_effects.h`](../include/sdgo/operator_effects.h):

- export: `SdgoOperatorEffectsRegisterExtensionHandlerV1`
- registration call: `cdecl`
- callback: `stdcall`
- maximum decoded `ExtensionsHex` JSON: 16384 bytes
- payload JSON is not NUL-terminated; all context pointers are callback-lifetime only
- `write_mask` must be a subset of `writable_mask`
- writable targets are pilot EXP, unit EXP, and GP

Every top-level extension key is required. If any handler is missing, the whole operator profile returns
`NO_CHANGE`; built-in effects are not partially applied. `current_*` includes built-in effects and
earlier trusted extension results, so a handler must not settle the same reward twice.

The shared settlement-filter ABI is in
[`include/sdgo/operator_reward_filter.h`](../include/sdgo/operator_reward_filter.h). The early mission
phase exposes unit EXP only; the final phase exposes pilot EXP and GP. Extensions return target values
through the writable mask and must not replace native EXP settlement with delayed SQL compensation.

## Manager API

The HTTP prefix is `/api/providers/operators`. Internal routes include:

| Method | Route | Purpose |
|---|---|---|
| `GET` | `/api/operators/protocol` | Read the contract and examples without starting the operator workspace |
| `GET` | `/api/operators` | Read catalog revision, extension registry, and imported packs |
| `POST` | `/api/operators/import-preview` | Normalize/cache a pack without changing the catalog |
| `POST` | `/api/operators/import` | Create an independent operator draft from the preview |
| `GET` | `/api/operators/packs/{sha256}/portrait` | Read a package portrait preview |
| `GET` | `/api/operators/{id}/share` | Export the current package-backed operator as `.opack` |
| `PUT` | `/api/operators/{id}` | Save the operator draft |

`import-preview` accepts `{"zip":"<base64>","filename":"operator.opack"}` or an explicit local
`{"path":"..."}`. Its response key is `pack_sha256`. The following import request uses that value under
the body key `sha256`, with optional `name`, `template_id`, and `source_template`.

Catalog writes require the current `X-Catalog-Revision` and use the Manager session token and same-origin
protection.

## Activation states

These states are independent:

1. package preview and validation;
2. saved operator draft;
3. applied server catalog and native ability rows;
4. generated and installed client resources;
5. installed BattleServer ability runtime files;
6. restarted host with the new startup configuration;
7. live status whose PID, module version, catalog revision, loaded profile count, and hook mode match
   the intended host.

Ability configuration is loaded at BattleServer process startup, so installed files take effect only
after the relevant host restarts. Manager status readback rejects stale PIDs, mismatched versions or
revisions, and status that cannot be tied to the current host process.
