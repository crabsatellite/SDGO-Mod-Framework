# Ace AI pilots

[简体中文](ace-ai.zh-CN.md) · [Back to the feature index](../README.md)

Ace AI gives authored pilot identities to eligible AI in two places:

- PvP AI players created for a two-team room.
- Friendly mission wingmen created for the human side.

Enemy mission NPCs are outside this feature. Ace selection does not change human players.
The feature remains Beta while its complete native PvP and mission flows are being accepted.

## Install

The framework package contains the public client and BattleServer parts. Players receive the client part
with the server owner's client package or update.

| Mod | Install on | Responsibility |
|---|---|---|
| `ace_ai` | Every participating client | Pilot catalog, in-game pilot display names and fixed notices |
| `ace_ai_server` | Each participating BattleServer | Authoritative chance, effective AI difficulty, HP and ordinary-attack policy |

Install matching parts from one framework release and restart the affected game and BattleServer processes.
The client has no independent chance roll or attribute policy.

The Ace client requires the bundled `match_ai` shared battle-start/respawn bridge and `battle_ai` controller;
Manager validates these dependencies. Mission Aces also use this shared bridge. It does not add AI to PvP
rooms that have not requested them. Ordinary mission wingmen remain usable independently when Ace is off.
HP display uses the values supplied by the server, with no separate client multiplier.

Opening notices use the game's existing battle chat and the same native entry path as AI dialogue.
They do not require the AI dialogue service or any API configuration.

## What players see

For an Ace selected by the server, the actual in-game display name is the authored pilot name instead of the
machine name from battle entry onward. The same identity is restored after death, respawn and unit changes.
This is the combatant display name used by the game, rather than a damage-hit popup.

At each start, PvP shows at most one fixed notice for each actual display colour that has an Ace:

- `红队出现了王牌驾驶员！`
- `蓝队出现了王牌驾驶员！`

The colour follows the side that the game actually displays after native team assignment. A notice contains
no pilot name, machine, count or difficulty. Friendly mission wingmen use only:

- `我方出现了王牌驾驶员！`

Enemy mission NPCs never produce the Ace mission notice.

## Server-owner settings

Open **Ace AI pilots · Server** in SDGO Mod Manager. Each base difficulty has four independent fields.
All changes require the affected BattleServer to restart.

| Base difficulty | Ace chance | Effective Ace AI | HP | Ordinary attack |
|---|---:|---|---:|---:|
| Easy | 5% | Normal (`Medium`) | 100% | 100% |
| Normal (`Medium`) | 5% | Hard | 100% | 100% |
| Hard | 5% | Hard | 120% | 100% |

Set a tier's chance to `0%` to disable Ace selection only for that base difficulty. `Same` keeps the base AI
difficulty. The native parser also accepts `Normal` as an alias for `Medium`; Manager stores `Medium`.
HP and ordinary attack are percentages, so `100%` is 1×. The ordinary-attack percentage applies only to
ordinary weapon damage. Specials keep their original rules and receive no Ace-specific damage multiplier.

## Pilot associations and owner content

The programme ships the public base catalog at `sdgo/mods/ace_ai/base-pilots.json`. It provides the authored
association layer for the current original machine roster. Generic framework programmes contain no personal
rosters or owner-custom pilot entries.

Clients use `sdgo/mods/ace_ai/base-pilots.json`. BattleServer uses the mirror
`sdgo/mods/ace_ai_server/base-pilots.json`, generated from the same public source, to decide machine eligibility.

An owner who adds custom machines supplies an authored entry for every custom machine and form. Ordinary owner
content places the same locked overlay bytes at `sdgo/mods/ace_ai/owner-pilots.json` and
`sdgo/mods/ace_ai_server/owner-pilots.json`. The overlay is never public Release content and does not belong
under `sdgo/local-mods/`. An owner entry with the same machine id overrides the public base association. The
server fails closed when an eligible association is missing or either catalog is invalid; it does not invent a
pilot name or continue that selection.

The public and owner files use the same bounded JSON shape:

```json
{
  "schema_version": 1,
  "pilots": [
    {"unit_id": 10001, "display_name": "Authored pilot", "kind": "named"},
    {"unit_id": 90001, "display_name": "Owner test pilot", "kind": "role"}
  ]
}
```

`unit_id` is the machine identity used by the battle entry. `display_name` is the exact in-game pilot label.
`kind` is `named` for a named character or `role` for an authored role label. Keep owner artwork, personal
rosters and other private metadata in owner content rather than the generic programme.

## Troubleshooting

| Symptom | Check |
|---|---|
| No Ace appears | The base tier's chance may be `0%`; restart the BattleServer after changing policy |
| Pilot name is unavailable | Confirm that the machine/form has a valid entry in the public base catalog or owner overlay |
| Different clients disagree | Install matching client and server components; clients must use the server-selected identity |
| Mission enemies are unchanged | Expected: the feature covers friendly wingmen only |

## Developer notes

The authoritative policy keys are in BattleServer `sdgo/mods/ace_ai_server/config.ini`, section `[settings]`:
`AceChanceEasy|Medium|Hard`, `AceDifficultyEasy|Medium|Hard`, `AceHpPercentEasy|Medium|Hard`, and
`AceAttackPercentEasy|Medium|Hard`. Chance accepts `0..100`; HP and ordinary attack accept `1..1000`.
Difficulty accepts `Easy`, `Medium`, `Hard` or `Same`; parsing is case-insensitive and `Normal` is a runtime alias
for `Medium`. Invalid difficulty falls back to that base tier. Out-of-range chance falls back to 5, HP to
100/100/120, and ordinary attack to 100.

Both client and server `owner-pilots.json` paths are declared configuration/content inputs, so the Manager
fingerprints their actual bytes when present and also distinguishes a missing overlay. The two public base
catalogs are versioned programme files generated from one source. No catalog is an independent authority for
chance or stat policy.
