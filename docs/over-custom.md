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
| `oc_custom_client` | Client | Brings the client package's own OC windows (allocation window, OVER CUSTOM challenge window and confirmation popup) to the factory 经验值转换 page for the active unit and sends allocation, challenge and reset requests; in battle it adds the OC speed and agility gains to the local unit |

`oc_custom_server` needs `friend_unit_gift_server` on the Agent and World servers, and `oc_custom_client` needs `friend_unit_gift_client`, because the OC requests ride that authenticated carrier. The gift feature itself can stay disabled.

## Setup

1. Enable **启用 OC 六维强化** under **OC 六维强化 · World 服务端** in SDGO Mod Manager and set **生效的强化体系** to `oc`. Restart the World servers; the schema and the per-unit growth mapping install on the first request.
2. Keep **OC 六维强化 · 对战服务端** enabled on every Battle server and restart them.
3. Players need the client Mod from the owner's client package; it is enabled by default and does nothing until the server reports `oc` mode.

## Playing

The client package still ships the original OC layouts, and the Mod uses them as they are: the allocation window `uirs\0794.uir`, the challenge window `uirs\0813.uir`, the confirmation popup `uirs\0819.uir` and the OVER CUSTOM button of the OC-era upgrade page. The window texts are the client's own original OC strings.

Set the unit you want to enhance as the active unit and open the factory. With the server in `oc` mode the tab bar gets back the OC-era **终极强化** tab right after **经验值转换**; the other tabs move along.

- **终极强化** switches to the 经验值转换 page and opens the OC allocation window on the right. The factory inventory it covers is hidden meanwhile; any other tab leaves and restores it. Its top line shows **强化点数** and the remaining normal + Over points. Each of the six stats (推进力 speed, 耐久力 HP, 攻击力 attack, 防御力 defense, 机动力 agility, 必杀技 special) shows its allocated normal + Over points.
- A stat's arrow button opens the original confirmation 「在使用强化点数之前再仔细考虑下如何？确定要强化这项性能吗?」. After confirming, the server checks the budget and the stat caps from the original tables (speed 100, HP 23800, attack 169, defense 169, agility 100, special 7950) and stores one point. Once the normal points are spent and the OC level is 9 or higher, the arrows allocate Over points. Without a free point, or above a cap, the original messages 「无法使用强化点数.」 or 「不能再使用.超过了最大能力值.」 appear.
- Normal points follow the OC level: one point per level from level 5 up to the unit's normal maximum (level 6 to 8 depending on the unit). The OC level follows the native level while it is below 9.
- The long button at the bottom resets points. Below OC level 9 it reads 「基本强化点数还原」 and resets normal points; from level 9 it reads 「终极强化还原」 and resets Over points. The confirmation shows the G coin cost; confirming charges it and clears that allocation. 「初始化费用不足.」 appears when the coins are short.
- The OVER CUSTOM button under the experience gauge glows when the unit can take the challenge, and otherwise shows as a dim label. The challenge needs the normal maximum level and a full experience bar. The button opens the challenge window with the unit name, battle count, target level (OVER CUSTOM1 to OVER CUSTOM EX), the condition, the failure rule, and the account's challenge items usable at the current level; up to three can be checked (for example success-rate or level-protection items). 「开始」 opens the 「进行终极强化.使用道具 : …」 confirmation and then rolls the original probability. Success grants Over points and the reward item and shows 「[ OVER CUSTOMn ]强化成功!」. Failure drops the OC level back to the normal maximum, unless a level-protection item was used, and shows the original failure message. The native level and experience never change.

Every save refreshes the active-unit record, so it applies from the next battle. In `oc` mode the current custom skill parts stay equipped in the database but are hidden from the records and have no effect in battle. Switching back to `current` restores them. With the server in `current` mode the page stays native and shows no OC window.

## Scope and compatibility

- The World server writes the OC payload into the owned-unit records of the unit pages, the active-unit reply, the unit-info reply, the user info and the end-of-battle user update. Every native copy of those records keeps it, including the room pilots the Battle server builds from.
- A saved allocation, reset or challenge applies from the next battle without logging in again: the World server pushes the updated user info to the Battle server that holds the player's channel, and to the room pilot if the player is already in a room.
- The Battle server applies HP (first form), attack, defense and special damage per form. Speed and agility are client-side movement values: when a battle starts the client Mod adds the active record's deltas to the local unit's dash level and motive power, at the same place the current system's stat bonus lands, for both forms of a transformable unit. Each unit's movement is computed by its own client, so other players see the result of that player's client.
- The OC-era EX abilities (extra abilities at OVER CUSTOM EX) are outside this Mod; the Ex button of the allocation window stays disabled.
- GP shown in the client refreshes on the next native GP update after a reset.
- The per-unit growth rows come from the original OC catalogue for 472 units; the remaining current units use the balanced row of their grade.

## Troubleshooting

- No OC window on the 经验值转换 page: the server does not run OC mode (the World Mod is disabled, its mode is `current`, or the schema failed to install; the World log reports `oc schema` messages), or the friend-gift client or server Mod is missing; the OC carrier needs both. The client log's `oc original windows` lines report whether the windows loaded and bound.
- Battle values unchanged: check the Battle server log for `oc applied uid=...` lines after the room starts; the Battle Mod logs every applied allocation. Speed and agility appear as `oc mobility applied` lines in the client log.
- The OVER CUSTOM button stays dim: the unit has not reached its normal maximum level, or it is already OVER CUSTOM EX.

## Developer notes

**Record carrier.** The owned-unit record (`_UnitData`, 0x99 bytes) has a 25-byte reserved gap at +21..+45. In `oc` mode the World Mod writes `'O' 'C'`, version 1, flags (1 = OC mode, 2 = profile present), the OC level, the normal and Over point totals, six signed 16-bit stat deltas (speed, HP, attack, defense, agility, special), a revision and a checksum there, and clears the four custom-skill slots at +138. The World send hook (`oc_custom.world_send`) patches packets 0x8F2, 0x9B4, 0x699 and 0x5C1 after the request handlers that build them.

**Lobby requests.** The OC commands use the friend-gift carrier's opaque extension range 0x1000..0x10FF: 0x1000 mode, 0x1001 profile, 0x1002 normal allocation, 0x1003 Over allocation, 0x1004 challenge, 0x1005 reset cost, 0x1006 reset, 0x1007 the account's challenge items for the next challenge (type 3 `tbl_EtcItems` entries with a success bonus or level protection whose level range covers the current OC level, at most 10). The server registers through `SdgoFriendUnitGiftRegisterExtensionV1`; the client sends through `SdgoFriendUnitGiftClientExtensionSendV1`. While a handler runs, `SdgoFriendUnitGiftCurrentRequestV1` returns the native World request object that carried it.

**Battle-side copies.** A Battle server requests the user info (request 0x6B3, reply 0x5C1) when the player enters its channel and keeps the account's active record; room pilots are copied from it. Its 0x5C1 handler updates that cached user outside a room, or the pilot inside a room and broadcasts 0x861. After a successful allocation, reset or challenge the World Mod replays the native user-info handler (`oc_custom.world_req_user_info`) for the account to every peer that received its user info, except the agent that carried the request, using a copy of the request object with the new destination and packet buffer. The end-of-battle user update (request 0x68D, `oc_custom.world_req_battle_end_user`, reply 0x4A9 with the record at +0x2B) is filled as well, because the Battle server stores that record as the account's active record. The client refreshes its own active record with the same user-info request after a save.

**Battle.** `oc_custom.battle_set_unit_stat` hooks the native stat dispatcher (`__stdcall`, seven arguments, `RET 0x1C`). After the original runs, the deltas are added per form and clamped to the grade caps table (`oc_custom.battle_grade_caps`); special damage is capped against the fatal weapon's damage size (`oc_custom.battle_weapon_lookup`).

**Client windows.** `CServiceFactoryRoomUnitUpGrade` (service `ServiceFactoryUnitUpGrade`) embeds its page component at +0xA8. The current `GOnline.exe` no longer has the OC service classes, but the package keeps the OC layouts complete: `0794` (allocation), `0813` (challenge) and `0819` (confirmation) match the OC-era client byte for byte or control for control. The current `0799` dropped the OC-era `OverCustomBtn`; its sprites `sprs\8525-8528.spr` are still in the package.

| Symbol | Role |
| --- | --- |
| `oc_custom.client_upgrade_init` | Page component Init: loads `uirs\0799.uir` and copies the active-unit record to component +0x6C. `__thiscall` with one unused stack argument (`RET 4`), on the resource loader thread. The Mod loads the OC layouts right after it on the same thread |
| `oc_custom.client_upgrade_update` | The service's per-frame update (vtable slot 12), game thread. The Mod adopts the loaded windows here and re-creates `OverCustomBtn` on the page |
| `oc_custom.client_upgrade_callback` | The service's UI callback (vtable slot 14), `__stdcall(service, event, second, control)`; button clicks arrive as 0x101, check boxes as 0x401 |
| `oc_custom.client_upgrade_vtable` | Service vtable, checked before the windows are adopted |
| `oc_custom.client_tab_menu_update`, `oc_custom.client_tab_menu_callback`, `oc_custom.client_tab_menu_vtable` | The factory tab menu (`ServiceFactoryRoomMenu`): per-frame update, UI callback and vtable. The Mod puts `OverCustomRadio` back among the seven native radios (cached at service +0xD4), after 经验值转换, with the native radio skins and caption style and text 126631 as its caption; the bar is reflowed at 125 px per tab. Pressing it selects the 经验值转换 page through the native 0x301 event first and then shows the allocation window; a native tab leaves |
| `oc_custom.grid_singleton` | The client table manager (`DAT_01005608`). +4 is a `std::map` keyed by id whose values are {type, record}; type 0x3B is the UI text table (UTF-16 from record +5), type 2 the unit table |

The windows are created by the service's own loader (vtable +0x58, `Load(name, path)`, the call the page uses for `0799`), so the service owns, draws and destroys them and their control events reach its UI callback. The service files dialogs by name, so each window has its own registry name. Controls are looked up by their original names and types (`FindControl`: static 3, button 4, icon 1, check box 10); static texts and check boxes use vtable +0x88 (`SetText`, `SetCheck`), and a check box keeps its state at +0x21C. While an OC window is open, the other dialogs of the page's parent frame that lie mostly under it (the inventory) are hidden and restored to their previous visibility when it closes or the page goes away. `OverCustomBtn` (18,213, 205x30, sprites 8526-8528) and its dim label `BackIcon1` (22,217, 84x24, sprite 8525) are re-created from the OC-era `0799` records. The window texts come from the original OC strings in text ids 126262..127192 (for example 126631 「终极强化」, 126638..126643 the six stat names, 126644 「强化点数」, 126646\126647 the resets, 126683..126685 the challenge confirmations).

**Speed and agility in battle.** The unit stat setup `FUN_00676C70` copies DashLevel (+0x21E) and MotivePower (+0x221) of the unit table to the battle unit at +0xB64 and +0xB6A, adds the current system's stat bonus there and calls `FUN_00675850` to derive the movement values from those levels. `oc_custom.client_unit_derive_call` (a site hook on that call) adds the deltas to the local unit's two levels and their restore copies +0xD5C and +0xD5E before the derive step. The local unit is the one at unit manager +0x88. At battle start a unit can be set up before `oc_custom.client_add_player` (`AddPlayer`, `__thiscall(manager, unit, local, player)`, `RET 0xC`) marks it local; the deltas are then added at that point and `oc_custom.client_unit_derive` recomputes the movement values.

**GameTest.** `tools\game_test.py prepare` accepts `--oc-allocate STAT:POINTS[,STAT:POINTS...]` (with `--play-unit-uid`: save normal points directly before an ordinary PvP battle), `--oc-panel` and `--oc-challenge`. When speed (0) or agility (4) is allocated, `oc.mobility` checks after the battle starts that the local unit's dash level and motive power rose by exactly the saved deltas. `oc.panel` opens the factory 经验值转换 page from the unit manager after the battle, waits for the original OC windows and drives their original controls by name, delivering each UI event through the client Mod's `SdgoOcCustomClientClickV2` to the page service's native callback: reset (`ResetBtn`, then `OKBtn` in the confirmation), one speed point (`SpeedUpBtn`), one agility point (`AgiUpBtn`), and with `--oc-challenge` OVER CUSTOM, an item check box and the challenge start. Frames of each step are kept under `visual\` in the run folder.
