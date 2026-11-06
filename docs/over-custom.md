# Over-custom six-stat enhancement (Beta)

[简体中文](over-custom.zh-CN.md) · [Back to the feature index](../README.md) · [Friend unit gifts](friend-unit-gift.md)

Brings back the original six-stat over-custom (OC) enhancement next to the current custom-parts system. Each owned unit gets an OC profile with normal points (speed, HP, attack, defense, agility, special), Over points earned through the OC challenge, and a reset that costs GP. The rules are the original 2009 stored procedures, ported to the Mod's own tables so the current custom-parts data in the game database is never written.

Both data sets coexist. The server owner chooses which one is active:

| Mode | Battles use | Owned-unit records | OC profiles |
| --- | --- | --- | --- |
| `current` (default) | Current custom parts (`UnitSkill_1..4`) | Native, unchanged | Stored, inactive |
| `oc` | OC allocation deltas | OC payload in the reserved gap; the four custom-skill slots are blanked in the records the World server sends | Active |

Switching the mode or disabling the Mods leaves every table in place. Nothing is deleted or rewritten in the native tables, so removing the Mods restores the native behaviour immediately.

## Components

| Mod | Host | Role |
| --- | --- | --- |
| `oc_custom_server` | World server | Installs the `sdgo_mod_oc_*` tables and procedures in the game database, answers lobby requests through the friend-gift carrier and, in `oc` mode, fills the OC payload into every owned-unit record it sends |
| `oc_custom_battle` | Battle server | Adds the record's OC deltas to HP, attack, defense and special damage after the native per-form stat builder, within the native per-grade caps |
| `oc_custom_client` | Client | Shows the OC panel on the factory upgrade page for the active unit and sends allocation, challenge and reset requests |

`oc_custom_server` needs `friend_unit_gift_server` on the Agent and World servers, and `oc_custom_client` needs `friend_unit_gift_client`, because the OC requests ride that authenticated carrier. The gift feature itself can stay disabled.

## Setup

1. Enable **启用 OC 六维强化** under **OC 六维强化 · World 服务端** in SDGO Mod Manager and set **生效的强化体系** to `oc`. Restart the World servers; the schema and the per-unit growth mapping install on the first request.
2. Keep **OC 六维强化 · 对战服务端** enabled on every Battle server and restart them.
3. Players need the client Mod from the owner's client package; it is enabled by default and does nothing until the server reports `oc` mode.

## Playing

Open the factory upgrade page with the unit you want to enhance as the active unit. The panel shows the OC level, the remaining normal and Over points and six rows of `base + normal × gain + over × gain = total`.

- `+` and `-` change the pending allocation; **确认加点** sends the whole vector. The server validates the budget and the stat caps (speed 100, HP 23800, attack 169, defense 169, agility 100, special 7950) and stores the result; it applies from the next battle.
- Normal points follow the OC level: one point per level from level 5 up to the unit's normal maximum (level 6 to 8 depending on the unit). The OC level follows the native level while it is below 9.
- **编辑 Over 点** switches the rows to Over points. Over allocation requires OC level 9 or higher and all normal points spent.
- **OC 挑战** rolls the original challenge for the next OC level (9 to 15) when the normal maximum is reached and the experience bar is full. Success grants Over points and the reward item; failure drops the OC level back to the normal maximum unless a level-protection item was used. The native level and experience never change.
- **洗点** first shows the GP cost from the original table, then a second click within fifteen seconds charges the GP and clears the normal or Over allocation.

In `oc` mode the current custom skill parts stay equipped in the database but are hidden from the records and have no effect in battle. Switching back to `current` restores them.

## Scope and compatibility

- The World server writes the OC payload into the owned-unit records of the unit pages, the active-unit reply, the unit-info reply and the login user info. Every native copy of those records keeps it, including the room pilots the Battle server builds from.
- The Battle server applies HP (first form), attack, defense and special damage per form. Speed and agility are client-side movement values; their deltas travel in the record and are shown in the panel, but the client Mod does not change movement in this version.
- GP shown in the client refreshes on the next native GP update after a reset.
- The per-unit growth rows come from the original OC catalogue for 472 units; the remaining current units use the balanced row of their grade.

## Troubleshooting

- Panel says the server does not run OC mode: the World Mod is disabled, its mode is `current`, or the schema failed to install. The World log reports `oc schema` messages.
- Panel never leaves "waiting for the capability handshake": the friend-gift client or server Mod is missing; the OC carrier needs both.
- Battle values unchanged: check the Battle server log for `oc applied uid=...` lines after the room starts; the Battle Mod logs every applied allocation.
