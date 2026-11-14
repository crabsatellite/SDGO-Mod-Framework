# Custom skills: records, icons and paired import

[简体中文](unit-skill-import.zh-CN.md) · [Unit import](unit-import.md) · [Full protocol reference (Chinese)](unit-import-protocol.zh-CN.md)

A complete unit publication can include skills absent from the target client. Each skill has a conflict-checked
ID, a native client record and resources, and a corresponding server definition. Existing native consumers
interpret that data. A new ID alone does not require another skill Mod, replacing it with a stock ID, or removing it.

This entirely fictional example gives a “Training Test Unit” a “Training Guard” skill using the native stat path.
Its defense field is `5`; the other fields are explicitly authored below. ID `74001`, owner `example-training`
and the resource names are illustrative, not reservations or downloadable/default content. Authors supply their own records and artwork.

## Choose the input route

| Route | Purpose |
|---|---|
| Single-family RAR/ZIP editor and confirmation | Edits units and weapons; direct confirmation still checks the server skill catalog. Its restrictions do not describe every complete-publication import capability. |
| Project `native_content` / 配套原生资料 | Stores, edits and exchanges records, resources and server definitions. Saving an attachment does not compile or install a runtime publication. |
| Complete publication / 批量阵容服务端应用 | Applies the compiled client catalog/resources and shared server skill definitions. A publication may contain only one family. |

Use the last route for a complete package. Do not clear skills to satisfy the first route's checks.
The editor's remove-skill option is appropriate only when the author explicitly chooses to lose that skill and its effect.
The compatibility field `pending_skill_ids` lists dependency-ledger entries; it does not mean batch apply must reject
those skills. Complete publications check actual definitions, records and ownership rather than treating this field as a global support table.

## 1. Supply the complete native record

Choose an **unused ID in the correct native category**. The supported client routes `1–75000` to ordinary skill
slots; new ordinary skills normally use a free ID in `70001–75000`. Check this example's `74001` against both
target catalogs before using it. Native part skills also have ordinary-slot ranges `3220001–3225000`,
`3473001–3478000`, `3558001–3559000` and `3613002–3615999`; classification alone does not establish their dependencies or behavior.

Other IDs route to the client's separate extra-skill slot. For example, a kind-13 record at `79950` can register
and display an icon, while ordinary skill-state notifications do not activate it through `SkillNumber_1/2`.
Both single-family and batch validation check this category. Keep the full authoring record, then relocate it
with its references/hashes to an appropriate free ID, or supply the required category adapter before publication.
This native consumer rule is **not a whitelist of skill effects**.

Provide a target-compatible kind `13`, 1132-byte record whose ID is `74001`. Names, descriptions, activation,
stats, icon frame and native dispatch belong to that record; an ID or a text description alone is insufficient.

When composing a native skill, update its displayed text as well as its numeric effects. On the supported client,
the unit-information page reads the name at byte `5` (50 UTF-16LE code units including the terminator), the
description at byte `531` / `0x213` (100 units including the terminator), and the icon frame as a little-endian
`u16` at byte `426`. A renamed 45% skill whose copied description still says 85% is incomplete authoring.
The bundled demo recipes provide their own descriptions and write them into the native records.
An authoring tool supplies the complete valid record. The importer preserves and validates it; a zero-filled
buffer is not a substitute for an actual skill.

For this example, the paired client values match the server row below: `Activate=0`, `ActivateValue=0`,
`Defense=5`, the other declared shared numeric fields `0`, and `Feature=0`, `UseTarget=0`. Select icon frame `0`.
These are this fictional author's choices, not defaults for unknown imported skills.

Native data-driven conditions, stats and effects can be combined when the target program has their consumers.
Supply linked skills, effects, textures and other referenced records, or verify the identical target versions.
A missing ID means its dependency needs importing; it does not mean the skill should be deleted.

A nonzero little-endian `u32` at byte offset `284` of a complete kind-13 skill record refers to a kind `79`,
89-byte skill-effect parameter record. A publication may carry the complete record with profile
`native-type79-89-v1`, or the selected paired client may supply a byte-valid compatible original with the same identity.
The importer checks only the proven layout constraints and preserves the remaining fields as opaque native data. It does not
invent their semantics, substitute a nearby ID or clear the reference. Do not fabricate a missing original with an arbitrary zero-filled placeholder; structural validation alone does not prove provenance or effects. Missing dependencies
are rejected before database mutation and again by client catalog validation. Dependency closure does not prove Actor activation or gameplay.
Preflight also rejects a published row that differs from the paired original at the same ID. Allocate an independent ID and rebind its typed references when both versions must remain available.
The static baseline index covers the original table codecs it can read; a negative lookup is not proof of vacancy across unrecognized codecs. Check new identities against the actual native global tree before activation. The client also rejects a global-key collision before allocating the publication's records.

This target client selects the first or last three entries of its six-slot weapon parameters according to the current transformation state.
A three-slot record from another version is not proven compatible by padding, repeating its first three entries, or finding an identical first-three-slot payload.
Verify the source and target consumers and each form's skill and weapon bindings before supplying the complete target record.
Observe the actual effects on the affected weapons in both forms during native acceptance.

For a generic example, an author verifies from both consumers that forms 0 and 1 reference separate three-weapon groups.
If one proven property is `[0,200,0]` in form 0 and `[300,300,0]` in form 1, its target six-slot group is
`[0,200,0,300,300,0]`. Keeping only the first group or repeating it changes the authored effect.
A form with no parameter reference may use a default only after its native default behavior has been established.
For additional forms or independent parameter groups, define each target node's bindings explicitly; one six-slot target record does not hold arbitrarily many groups.
This semantic check belongs to target-record preparation. The public importer consumes the completed compatible records.

The authoring attachment can contain:

```text
training-skill/
  manifest.json
  records/training-guard.bin
  sprs/example_training_guard.spr
  txrs/example_training_guard.txr
  skill-atlases.json
  server-skills.json
```

This is authoring material, not an installable player package.

## 2. Bind the icon to the skill

`txrs/example_training_guard.txr` contains the author's artwork in an actual supported texture encoding;
renaming an arbitrary image does not encode it. The native `sprs/example_training_guard.spr` contains at least
frame `0`, whose texture reference points to that TXR. Framework SPR encoding and texture validation APIs can be reused.

Declare the skill, SPR and all textures referenced by every frame using
[`sdgo.unit-skill-atlases/1`](unit-skill-atlases.schema.json). The 16-bit icon frame at skill record `+426` must be
below the sprite's frame count. Multiple skills can share an atlas and choose different existing frames.
Use an independent sprite path instead of replacing the global stock skill atlas.

In these JSON examples, replace every `<...>` placeholder with a lowercase SHA-256 of the actual bytes before validation:

```json
{
  "schema": "sdgo.unit-skill-atlases/1",
  "owner": "example-training",
  "revision": "<atlas-revision-sha256>",
  "sprites": [{
    "path": "sprs/example_training_guard.spr",
    "sha256": "<spr-sha256>",
    "textures": [{
      "path": "txrs/example_training_guard.txr",
      "sha256": "<txr-sha256>"
    }]
  }],
  "skills": [{
    "id": 74001,
    "record_sha256": "<complete-1132-byte-record-sha256>",
    "sprite": "sprs/example_training_guard.spr"
  }]
}
```

At runtime, `unit_catalog` selects the atlas for the actual registered record. Detail controls and battle skill
initialization use their respective native paths. `skill-atlases.bin` also binds the final catalog, resource index
and whole archive hashes. Recompile it after changing records or artwork; do not reuse an old sidecar with a new catalog.

## 3. Supply the complete server definition

[`sdgo.unit-server-dependencies/1`](unit-server-dependencies.schema.json) accepts a complete hash-pinned stock
template or an explicit full `tbl_SkillRev` row. The client record does not determine every server field.
Server `BattleMode` and the client's final dispatch byte are separate inputs.

This example's `server-skills.json` supplies all fields without depending on a particular server's private template:

```json
{
  "schema": "sdgo.unit-server-dependencies/1",
  "owner": "example-training",
  "revision": "<server-skills-revision-sha256>",
  "skills": [{
    "id": 74001,
    "client_sha256": "<complete-1132-byte-record-sha256>",
    "definition": {
      "kind": "explicit",
      "origin": "authored",
      "values": {
        "SkillID": 74001,
        "SkillName": "Training Guard",
        "Activate": 0,
        "ActivateValue": 0,
        "MoveLv": 0,
        "DashLv": 0,
        "QuickLv": 0,
        "MotiveLv": 0,
        "BoostIncreaseRate": 0.0,
        "BoostPower": 0,
        "Attack": 0,
        "Defense": 5,
        "RadarRadius": 0,
        "SpIncreaseRate": 0,
        "Scale": 1.0,
        "AutoBulletChargeRate": 0.0,
        "BulletChargeRate": 0.0,
        "AtkSpdRate": 0.0,
        "ActiveResult": 0,
        "Feature": 0,
        "UseTarget": 0,
        "BattleMode": 0,
        "PartSpLevel": 0
      }
    }
  }]
}
```

Choose a deterministic content revision rule that excludes the `revision` field itself and update the revision
when content changes. File-reference SHA-256 values always cover the complete final file. `client_sha256` and
the atlas's `record_sha256` must identify the same final skill record. Relocation must update its leading DWORD,
unit and linked-skill references, and server `SkillID`, then regenerate affected hashes.

## 4. Connect the authoring declarations

The attachment's root `manifest.json` uses [`sdgo.unit-native-content/1`](unit-native-content.schema.json):

```json
{
  "schema": "sdgo.unit-native-content/1",
  "owner": "example-training",
  "revision": "<native-content-revision-sha256>",
  "records": [{
    "kind": 13,
    "id": 74001,
    "profile": "native-type13-1132-v1",
    "member": "records/training-guard.bin",
    "sha256": "<complete-1132-byte-record-sha256>"
  }, {
    "kind": 79,
    "id": 8000789,
    "profile": "native-type79-89-v1",
    "member": "records/training-guard-parameters.bin",
    "sha256": "<complete-89-byte-record-sha256>"
  }],
  "resources": [{
    "path": "sprs/example_training_guard.spr",
    "member": "sprs/example_training_guard.spr",
    "sha256": "<spr-sha256>"
  }, {
    "path": "txrs/example_training_guard.txr",
    "member": "txrs/example_training_guard.txr",
    "sha256": "<txr-sha256>"
  }],
  "server_skills": {
    "manifest_path": "server-skills.json",
    "manifest_sha256": "<complete-server-skills-json-sha256>"
  },
  "skill_atlases": {
    "manifest_path": "skill-atlases.json",
    "manifest_sha256": "<complete-skill-atlases-json-sha256>"
  }
}
```

`member` names a file inside the attachment; `path` names a game resource. The unit project's `native_content`
references this manifest's member name and complete file hash. Include every referenced member.
Both the client unit record and server unit definition refer to `74001` in `SkillNumber_1` or `SkillNumber_2`.
This fictional example also writes `8000789` as the little-endian `u32` at offset `284` of
`records/training-guard.bin`; the first DWORD of the kind-79 file has the same identity. Each SHA-256 binds the complete final bytes of its own record.
Project save/reimport retains the declarations. Single-family confirmation has separate publication checks;
accepting an attachment does not install a runtime package.

## 5. Compile a complete publication

For package builders with prepared target-native data, the public import-side APIs are:

| API | Purpose |
|---|---|
| `owner.units.native_content.validate_project_bindings` | Validates attachment members, records, hashes and supporting declarations. |
| `owner.units.server_dependencies.prepare` | Resolves full server definitions, paired fields, conflicts and ownership without SQL writes. |
| `owner.units.compiler.native_catalog` | Combines supported final unit, weapon, skill and parameter records into `catalog.bin`, including kind `13` and kind `79`. |
| `owner.units.skill_parameters.validate_publication` | Resolves each skill `+284` reference from the publication or paired client and rejects missing complete kind-79 records before SQL. |
| `owner.native_archive.build_archive` / `build_archive_file` | Compiles declared resources into the native overlay index and archive. |
| `owner.units.skill_atlases.compile_bundle` | Binds atlas declarations to the final catalog/index/archive and emits `skill-atlases.bin`. |
| `owner.units.batch_server.load_publication` | Reads and validates the publication's server plan, client catalog and form requests; client installation separately checks its manifest and resources. |

Finalize records and resources first, compile the catalog and archive, then compile sidecars bound to those final hashes:

```python
from hashlib import sha256
from owner.units.skill_atlases import compile_bundle

# atlas_manifest is the JSON above; skill_record is the author's complete record.
# final_catalog / overlay_index / overlay_archive are this publication's final bytes.
skill_atlases = compile_bundle(
    [atlas_manifest],
    sha256(final_catalog).hexdigest(),
    sha256(overlay_index).hexdigest(),
    sha256(overlay_archive).hexdigest(),
    {74001: skill_record},
    {
        "sprs/example_training_guard.spr": sprite_bytes,
        "txrs/example_training_guard.txr": texture_bytes,
    },
)
```

For split archives, use the ordered whole-stream `archive_sha256` returned by the archive compiler.
Preserve other publishers' installed records, resources, atlases and scripts when rebuilding an aggregate package;
this single-skill fragment is not a replacement for an existing installation.

The runtime publication is separate from the authoring attachment:

```text
publication/
  server-plan.json
  form-graph-requests.json                 # when form graph requests are needed
  package/
    manifest.json
    client/sdgo/mods/unit_catalog/
      catalog.bin
      definitions.json
      resource-overlay.ini
      resources.zdx
      resources.zpk
      skill-atlases.bin
      ...                                 # matching framework files and other declared members
```

`server-plan.json` uses `sdgo.unit-roster-server-plan/1` with `owner`, `revision`, `families`, `skills`, `funnels`
and `affiliations`. Its `skills` block is the complete server declaration above. An empty `funnels` block still
has `sdgo.unit-server-funnels/1`, owner, revision and an empty `funnels` array; no affiliations means `[]`.
Include the complete family plan that actually references the skill, even for a one-family publication.
The client manifest binds all installation files and the resource overlay; the authoring ZIP cannot replace it.

## 6. Apply both sides in Manager

Configure the target group's WorldServer INI under Unit Import. Choose `publication/` in **批量阵容服务端应用**,
then run **只读预检**. Review skill rows, families, resources and conflicts. Stop the group as required by the UI,
use **停服后应用…**, and run **读回核对**. Shared dependency transactions install skill definitions before family rows.
Manager does not start or stop native services. Players install the matching client package or the server owner's
client update; client installation does not execute server SQL. Restart the group and check the real name,
description, icon, activation and removal. This does not create shop products, capsule entries or unit grants.

## Special weapons and attacks after transformation

A player's “special skill” may be a weapon callback rather than a kind-13 passive. Start with the live form's
`WeaponNumber_1/2/3/FatalAttack` and its complete 1,871-byte weapon record:

| Use | Required content |
| --- | --- |
| Ordinary ranged callback | Function at `+1569`, a `ranged` binding, the actual native function and its resources |
| Special ranged callback | Function at `+1669`, a `special_ranged` binding and its dependencies |
| Missile or other initializer | Function at `+1771`, an `initializer` binding and its dependencies |
| Weapon `SpecialFeature` | Complete kind-12 / 1,559-byte record (`native-type12-1559-v1`) and effect dependencies |
| Funnel / fang | Kind 11, paired server fields, models/animations, launch, hit/return and destruction |
| Native fixed resources or IDs | The consumer's specific contract; see [resource bindings](resource-bindings.md) and [skill identities](skill-identity.md) |

For a fictional example, the transformed form of “Example Equipment Unit” uses `example_Burst`, whose body reads
`fxrs/example_trace.zfx`. A registered function does not prove that this effect was installed. The publication also
needs the ZFX, its textures and mesh models, and those models' resources, or verified original bytes from the owner's
configured compatible target. Checking visible paths in the weapon BIN alone misses resources inside script bodies.

Include resources in the same publication's archive and exact hash manifest. Use independent names and update the
actual script constants. Files left only in an author's local folder are unavailable to players. Removing the effect,
clearing a skill or substituting an arbitrary stock effect does not establish the same behavior.

`owner.units.native_programs.literal_resource_references(program_bytes)` reads complete serialized path constants
from target Lua 5.0 bodies and excludes debug source names. `validate_resource_dependencies(bundle, weapons,
read_resource, catalog_sha256=...)` verifies programme/weapon/catalog bindings and returns hashes of bytes read from
the actual target. Manager batch preflight and apply check these references before SQL writes. Original reuse needs
the configured compatible client. Computed paths, native fixed resources, action events and model nodes still need
their own dependency contracts and runtime checks; path validation is not combat acceptance.

Native acceptance follows the actual operation: commit R transformation, select the target form's slot, verify the
bound weapon ID, complete its attack and check ammunition or melee events, effects, hits and movement. Wait for later
segments and cleanup; then test the reverse transform, death/respawn and normal battle return. Transform/weapon-1
checks require GameTest client 0.1.39 or newer. Earlier SDKs do not provide equivalent weapon-binding/ammunition proof.
After changing authoring inputs, use an SDK-supported content installation input or prepare a new candidate and read
back its sealed hashes. `reuse_prepared` retains client content and does not synchronize later author-template edits.

When activation is absent, distinguish missing condition coverage from an import or consumer failure.
`ActivateValue=0` does not always mean unconditional activation: some native types depend on the match mode,
team state and a server notification. For a fictional "activate while our team is disadvantaged" skill,
record the state and notification actually used by that mode before checking ordinary-slot activation and effects.
Losing local HP alone does not establish that condition. Preserve the original condition and create the scenario
through normal combat; editing the record or directly setting the active state is not an acceptance test.

## When an additional adapter is needed

### Ordinary skill slots and compatibility aliases

A kind-13 record does not choose its actor slot. The supported client's native classifier treats IDs in
`1–75000`, `3220001–3225000`, `3473001–3478000`, `3558001–3559000`, and `3613002–3615999` as ordinary
skills. Other positive IDs enter the separate extra-skill path. Use `owner.units.skill_slots.ordinary(id)`;
batch import refuses an extra-slot ID in `SkillNumber_1` or `SkillNumber_2`.

For example, an author moving a fictional extra-slot skill into slot 1 allocates an unused ordinary ID, clones
the complete record, changes its first DWORD and any explicitly mapped linked-skill field at byte 354, and
rebinds the intended unit fields. Keep activation conditions, effect parameters, dispatch, text, icon frame and
resource references. The skill `+284` parameter edge and the linked-skill field at byte 354 are independent:
relocating the skill to an ordinary ID normally preserves `+284` unchanged. Only an intentional parameter relocation updates
the kind-79 first DWORD, skill `+284`, manifest identity and every affected digest together. Include the complete 89-byte
record unless the paired client proves an identical compatible original; never clear, guess or substitute a nearby parameter.
Check the native global tree, every installed content scope, shipped presets and server SQL
for collisions; a free entry in `0253` alone is insufficient.

For a complete, already validated 1132-byte record, the client-side clone is limited to these typed fields:

```python
import struct
from owner.units.skill_slots import ordinary

assert len(original_record) == 1132 and ordinary(allocated_id)
clone = bytearray(original_record)
struct.pack_into("<I", clone, 0, allocated_id)
linked = struct.unpack_from("<I", clone, 354)[0]
if linked in linked_id_map:
    struct.pack_into("<I", clone, 354, linked_id_map[linked])
ordinary_record = bytes(clone)
```

`allocated_id` and `linked_id_map` come from the checked allocation plan. This snippet changes no SQL or files and
does not reserve IDs; complete the paired transaction and sidecar steps below before installing the clone.

If the old ID may be referenced elsewhere, retain its record, server row, ledger and atlas as a compatibility
alias. Clone the ordinary linked graph without rewriting the old graph. Regenerate the catalog, programme and
atlas bindings, blueprint availability, and any installed directed-form candidate and independent host authority.
Form-graph validation pins full unit records, SQL rows, family revisions and inventory-entry ownership; changing
skill fields requires rebuilding those witnesses too. Keep unchanged graph nodes and root item revisions coherent,
without changing their values or adopting unrelated families.
In one stopped-server transaction, insert the paired owned skill
definitions, update the intended owned unit references and ownership hashes, and read back all changes. Reject
unowned IDs or changed preimages, roll back on failure, and install the matching client content before restarting.
Do not rewrite player loadouts or numerically replace arbitrary Lua constants. Any fixed-ID effect additionally
needs its specific adapter and host authority. Native tests must observe activation in the intended ordinary slot,
linked effects, transformation and death/respawn; record registration alone does not prove these behaviors.

- Native data-driven combinations use the record/resource/server-definition route above. An independent icon needs
  a valid SPR/TXR and atlas binding.
- Only effects with extra fixed-ID dispatch need a specialized adapter such as the
  [native skill identity selector](skill-identity.md). Its supported source set is not the import protocol's global
  skill whitelist. Do not attach an identity mapping to an ordinary stat skill merely because its ID is new.
- For a missing target consumer, unknown layout/dispatch or missing dependency, report the specific unsupported
  item. A declaration, image or SQL row cannot create an effect absent from the target program.

Public framework material contains generic protocols, import code and fictional explanations. Authors keep actual
records, artwork, models, complete content packages, private provenance and server settings in their own content
workspaces; these do not become framework examples or defaults.
