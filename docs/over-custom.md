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
| `oc_custom_client` | Client | Adds an OC overlay to the factory 经验值转换 page for the active unit and sends allocation, challenge and reset requests |

`oc_custom_server` needs `friend_unit_gift_server` on the Agent and World servers, and `oc_custom_client` needs `friend_unit_gift_client`, because the OC requests ride that authenticated carrier. The gift feature itself can stay disabled.

## Setup

1. Enable **启用 OC 六维强化** under **OC 六维强化 · World 服务端** in SDGO Mod Manager and set **生效的强化体系** to `oc`. Restart the World servers; the schema and the per-unit growth mapping install on the first request.
2. Keep **OC 六维强化 · 对战服务端** enabled on every Battle server and restart them.
3. Players need the client Mod from the owner's client package; it is enabled by default and does nothing until the server reports `oc` mode.

## Playing

Set the unit you want to enhance as the active unit, open the factory and choose the **经验值转换** tab. The page gains an **OC 六维强化** button under the experience gauge. It opens the OC overlay over the item slots; **收起 OC** closes it and shows the native page unchanged.

The overlay shows the OC level, the native level, the remaining normal and Over points and one row per stat: the allocated normal and Over points, then `base + normal gain + Over gain = total`. A total above the stat cap is shown in red.

- `+` and `-` change the pending allocation; **确认加点** sends the whole vector. The server validates the budget and the stat caps (speed 100, HP 23800, attack 169, defense 169, agility 100, special 7950) and stores the result; it applies from the next battle.
- Normal points follow the OC level: one point per level from level 5 up to the unit's normal maximum (level 6 to 8 depending on the unit). The OC level follows the native level while it is below 9.
- **编辑 Over 点** switches the rows to Over points. Over allocation requires OC level 9 or higher and all normal points spent.
- **OC 挑战** rolls the original challenge for the next OC level (9 to 15) when the normal maximum is reached and the experience bar is full. Success grants Over points and the reward item; failure drops the OC level back to the normal maximum unless a level-protection item was used. The native level and experience never change.
- **洗普通点** or **洗 Over 点** first shows the GP cost from the original table, then a second click within fifteen seconds charges the GP and clears that allocation.
- **刷新** reads the profile from the server again.

In `oc` mode the current custom skill parts stay equipped in the database but are hidden from the records and have no effect in battle. Switching back to `current` restores them.

## Scope and compatibility

- The World server writes the OC payload into the owned-unit records of the unit pages, the active-unit reply, the unit-info reply, the user info and the end-of-battle user update. Every native copy of those records keeps it, including the room pilots the Battle server builds from.
- A saved allocation, reset or challenge applies from the next battle without logging in again: the World server pushes the updated user info to the Battle server that holds the player's channel, and to the room pilot if the player is already in a room.
- The Battle server applies HP (first form), attack, defense and special damage per form. Speed and agility are client-side movement values; their deltas travel in the record and are shown in the panel, but the client Mod does not change movement in this version.
- GP shown in the client refreshes on the next native GP update after a reset.
- The per-unit growth rows come from the original OC catalogue for 472 units; the remaining current units use the balanced row of their grade.

## Troubleshooting

- Panel says the server does not run OC mode: the World Mod is disabled, its mode is `current`, or the schema failed to install. The World log reports `oc schema` messages.
- Panel never leaves "waiting for the capability handshake": the friend-gift client or server Mod is missing; the OC carrier needs both.
- Battle values unchanged: check the Battle server log for `oc applied uid=...` lines after the room starts; the Battle Mod logs every applied allocation.
- No **OC 六维强化** button on the page: check the client log for `oc panel` lines. The client Mod logs the page it attached to, or why it could not.

## Developer notes

**Record carrier.** The owned-unit record (`_UnitData`, 0x99 bytes) has a 25-byte reserved gap at +21..+45. In `oc` mode the World Mod writes `'O' 'C'`, version 1, flags (1 = OC mode, 2 = profile present), the OC level, the normal and Over point totals, six signed 16-bit stat deltas (speed, HP, attack, defense, agility, special), a revision and a checksum there, and clears the four custom-skill slots at +138. The World send hook (`oc_custom.world_send`) patches packets 0x8F2, 0x9B4, 0x699 and 0x5C1 after the request handlers that build them.

**Lobby requests.** The OC commands use the friend-gift carrier's opaque extension range 0x1000..0x10FF: 0x1000 mode, 0x1001 profile, 0x1002 normal allocation, 0x1003 Over allocation, 0x1004 challenge, 0x1005 reset cost, 0x1006 reset. The server registers through `SdgoFriendUnitGiftRegisterExtensionV1`; the client sends through `SdgoFriendUnitGiftClientExtensionSendV1`. While a handler runs, `SdgoFriendUnitGiftCurrentRequestV1` returns the native World request object that carried it.

**Battle-side copies.** A Battle server requests the user info (request 0x6B3, reply 0x5C1) when the player enters its channel and keeps the account's active record; room pilots are copied from it. Its 0x5C1 handler updates that cached user outside a room, or the pilot inside a room and broadcasts 0x861. After a successful allocation, reset or challenge the World Mod replays the native user-info handler (`oc_custom.world_req_user_info`) for the account to every peer that received its user info, except the agent that carried the request, using a copy of the request object with the new destination and packet buffer. The end-of-battle user update (request 0x68D, `oc_custom.world_req_battle_end_user`, reply 0x4A9 with the record at +0x2B) is filled as well, because the Battle server stores that record as the account's active record. The client refreshes its own active record with the same user-info request after a save.

**Battle.** `oc_custom.battle_set_unit_stat` hooks the native stat dispatcher (`__stdcall`, seven arguments, `RET 0x1C`). After the original runs, the deltas are added per form and clamped to the grade caps table (`oc_custom.battle_grade_caps`); special damage is capped against the fatal weapon's damage size (`oc_custom.battle_weapon_lookup`).

**Client page.** `CServiceFactoryRoomUnitUpGrade` (service `ServiceFactoryUnitUpGrade`) embeds its page component at +0xA8.

| Symbol | Function |
| --- | --- |
| `oc_custom.client_upgrade_init` | Page component Init: loads `uirs\0799.uir` and copies the active unit record to component+0x6C. `__thiscall` with one unused stack argument (`RET 4`); it runs on the resource loader thread |
| `oc_custom.client_upgrade_update` | Service per-frame update (vtable slot 12), game thread. The overlay controls are created here for the page Init announced |
| `oc_custom.client_upgrade_callback` | Service UI callback (vtable slot 14), `__stdcall(service, event, second, control)`; button clicks arrive as 0x101 |
| `oc_custom.client_upgrade_vtable` | Service vtable, checked before the overlay is built |

The page dialog is shown only while the factory tab menu (`ServiceFactoryRoomMenu`, `CServiceFactoryRoomTabMenu`) has `UnitUpRadio` selected; both services set +0xAC once their page is loaded and active. Overlay buttons use the client's blue button sprites `sprs\17530-17532.spr`, scaled per control from their natural 82×28 size; the backdrop is `sprs\17459.spr` (262×61) scaled the same way.

**GameTest.** `tools/game_test.py prepare` accepts `--oc-allocate STAT:POINTS` (allocate through the client Mod before an ordinary PvP battle with `--play-unit-uid`), `--oc-panel` (after `--oc-allocate`, or with `--pages-only --page-group hangar`: open the factory from the unit manager, select the upgrade tab and drive the overlay through the page's original UI callback: refresh, normal reset, one HP point) and `--oc-challenge` (also press the challenge; it can change the OC level).
