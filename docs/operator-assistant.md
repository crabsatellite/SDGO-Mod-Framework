# Operator supplies, resources, and abilities

[简体中文（完整说明）](operator-assistant.zh-CN.md) ·
[Pack and extension protocol](operator-pack-protocol.md)

The **副官与能力** panel in SDGO Mod Manager configures operator names, native/uploaded/package
portraits, abilities, mutually exclusive item drops, free access or product registration, and shop-draft
defaults. Saving a draft, applying server rules, installing client content, activating the BattleServer
runtime, and publishing a shop product remain separate actions.

## Default supply officers

A fresh full import includes five permanent free supply officers for blueprints, experience packs,
cosmetics, upgrade kits, and capsule coins. They preserve the original branch probabilities and
quantity ranges, while each native trigger receives an independent draw. Item pools use typed server
references and matching client registrations; blueprint aliases are deduplicated by recipe and invalid
outputs or materials are excluded.

The five use fixed Chinese names and skip the naming prompt. Existing names are backed up first, and
other operators keep the native naming flow. Existing holdings are upgraded in place to permanent use
while preserving operator IDs, slots, and equipped references. All five can equip mission parts through
paired client/server flags.

## Creating and importing operators

You can create a new entry from an existing operator or import an `.opack` / ZIP:

1. Preview the package. Manager reports its hashes, resource template, missing base dependencies,
   sprite frame counts, voice events, ignored files, abilities, acquisition rules, and shop defaults.
2. Choose the display name, the skill/action `template_id`, and the resource-profile
   `source_template`. The two templates may differ.
3. Confirm import. This creates an independent operator draft; it does not apply the server, install a
   client update, create a shop product, or publish anything.
4. Review the operator, save it, then perform the required client, server, runtime, and shop actions
   separately.

Package-backed operators can be exported again as `.opack`. The package remains client content. Voice,
artwork, and other game assets, including Hong Kong Cantonese assets, are delivered only through the
server owner's client package or update. They are excluded from the Manager executable and public
framework releases.

Packages are execution-free. Files outside the supported native resource roots are reported but never
run or installed. Supplied resources are compiled into the imported operator's own client namespace and
do not overwrite the template's original files. The developer contract documents multiframe SPR,
dialogue voice variants, missing-resource reporting, and package limits.

## Abilities

Leaving abilities unset inherits the complete template behavior. Custom mode supports:

- general, victory, and defeat GP, unit EXP, and energy-save percentages;
- up to four ordered time windows;
- a separate pilot EXP percentage;
- explicit mission-parts permission;
- registered namespaced extension payloads.

`gp_percent` maps to the native `PointAdd` field. Pilot EXP is a separate verified runtime capability.
Unconfirmed native fields such as `ChangeSp` and `ServiceType` remain inherited; Manager does not assign
meanings based on their names.

Timed windows use strict `HH:MM`, include both native bounds, and cannot cross midnight. Overlap is
accepted, but the first matching native slot wins. Existing exact native skill rows are reused; new
skill/time IDs remain gated by verified target-server capabilities.

Custom pilot EXP and extension effects are loaded by the BattleServer runtime at process startup. A file
installation is therefore pending until the relevant host restarts and live status matches the expected
PID, runtime version, catalog revision, loaded profile count, and hook mode.

## Drops, free access, and products

Each native operator-drop trigger can select at most one configured rule. Probabilities use 0.01%
precision, may total at most 100%, and leave the remainder as no drop. Quantities use inclusive bounds.

- `all_players`: permanent access is granted on normal login or when the operator page opens. Existing
  rentals are upgraded. A full 100-slot operator inventory is never cleared to make room.
- `product`: registers the operator item and description. It does not grant, list, price, or publish the
  operator.

For products, the operator panel may prefill a Chinese name, sales label, price, GB/MB currency, and
validity. The shop module uses those values only when the owner creates a product draft. That draft is
always disabled and is not applied or published automatically.

Purchase options and confirmations use the game's existing dialogs and the operator's native icon;
there is no separate product-image upload. Install the matching generated client resources before
listing the product for players.

Disabling a managed definition restores original operators from their backups. A module-created
operator with no owners is removed from the server definition table. If players already own it, the
definition is retained using the selected native template with custom drops and abilities removed, so
their records remain valid.

## Client and server workflow

Select the matching game client and host server configuration. The panel imports operator and item
definitions read-only; credentials remain on the host.

After saving:

1. For a new ability runtime or changed ability configuration, stop the selected BattleServer hosts
   through the owner's normal operations, then check and install the matching runtime components.
2. Apply the saved catalog to the server. Runtime-backed capabilities require the matching installed
   files to pass verification before application.
3. Generate and install the matching client update for names, portraits, dialogue, voice, and other
   client resources while that client is closed.
4. Restart affected game processes through the owner's normal operations and verify live runtime
   status.
5. For `product` entries, open the shop module, create the disabled draft, review it, and separately
   apply or publish it.

Players use their normal launcher and do not need Python or Manager. Command-line development and
offline inspection require Python, Pillow, and pyodbc.

## Use in game

Buy a listed operator through the normal shop and confirm its price and term in the native dialog.
Free-for-all operators are granted through the normal login/operator-page flow instead.

Open the game's operator page and use its existing page controls to find the owned entry. Name it
when prompted, then select it for use through the native controls. Saving a Manager draft does not
equip an operator for a player; match effects use the operator actually equipped in game.

## Developer notes

The server Mod's developer setting `TraceSettlements` records at most sixteen reward input/target
comparisons per process. It is off by default and omits account IDs and nicknames.

The complete asset-free package schema, strict ability object, Manager API, missing-resource behavior,
and native extension ABI are documented in the
[operator pack and extension protocol](operator-pack-protocol.md).

The native headers are
[`include/sdgo/operator_effects.h`](../include/sdgo/operator_effects.h) and
[`include/sdgo/operator_reward_filter.h`](../include/sdgo/operator_reward_filter.h). A package can carry
finite JSON payloads for registered namespaces, but cannot register a handler or execute code. Handler
authors define and version their own payload contract; the framework does not provide a universal
parameter-schema registry.

The client mod inserts operator records through verified native calls and uses the native CRT allocator.
The server adapter manages the operator tables and wraps the native read/drop procedures with backups,
installed-definition hashes, and conflict checks. Drops are sampled by the server; client configuration
cannot choose its own rewards.
