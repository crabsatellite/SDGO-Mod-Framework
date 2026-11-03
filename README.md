# SDGO mod framework

Core fixes and optional extensions for SD Gundam Online clients and private servers.
Unofficial fan project; no game files are included. [简体中文](README.zh-CN.md)

[Installation](docs/setup.md) · [SDGO Mod Manager](docs/mod-manager.md) · [Latest release](https://github.com/crabsatellite/SDGO-Mod-Framework/releases/latest)

Server owners and maintainers install, configure and [update](docs/mod-manager.md#updates) features with
**SDGO Mod Manager**; the **0_MOD管理（服主专用）** entry at the top of the client folder opens it. Ordinary
players keep using their normal game launcher, update the way their server owner provides, and should not
change Mods themselves, so their client stays matched to the server.
Manager can also configure addons marked **local-only**. They stay in the selected installation;
public package and export tools reject them. See the [Manager guide](docs/mod-manager.md).
This page is a feature index. Configuration, gameplay instructions, compatibility and troubleshooting are in each feature's documentation.

Beta and Experimental after a feature name mark its release channel; unlabeled features are Stable:

- **Stable**: the basic workflow works; later problems are fixed in new versions. Manager updates install Stable by default.
- **Beta**: the basic workflow is still under development, currently only unit family import. Switch Manager updates to the Beta channel to install it.
- **Experimental**: development diagnostics. Development tools are not installed by Manager updates.

## Core fixes

<!-- mod-category:fixes -->

| Feature | Overview | Documentation |
|---|---|---|
| IME candidates and commit fix <!-- catalog: `ime_fix` --> | Restore Chinese candidates and replace the old preedit range without residual letters | [Use](docs/ime-fix.md) |
| Activity capsule currency <!-- catalog: `capsule_currency_fix` --> | Show machine 9301 as 99 coins in the list and purchase confirmation | [Use](docs/capsule-currency-fix.md) |
| Input method freeze fix <!-- catalog: `findwindow_fix` --> | Stop input methods such as iFlytek from freezing or crashing the repack client | [Use](docs/findwindow-fix.md) |
| Startup crash fix <!-- catalog: `startup_crash_fix` --> | Stop random crashes on the loading screen at game start | [Use](docs/startup-crash-fix.md) |
| Apsalus III transform input fix <!-- catalog: `transform_input_fix` --> | Release the native transform lock when a form has no transform animation | [Use](docs/transform-input-fix.md) |
| Agent relay log fix <!-- catalog: `agent_relay_log_fix` --> | Fix an Agent relay log that reads invalid memory | [Use](docs/agent-relay-log-fix.md) |
| Battle rank regression fix <!-- catalog: `rank_progression_fix` --> | Stop battle settlement from lowering a pilot's rank (WorldServer) | [Use](docs/rank-progression-fix.md) |
| Friend list fix <!-- catalog: `friend_list_fix`, `friend_list_client_fix` --> | Retain offline friends and current names; restore the built-in messenger UI | [Use](docs/friend-list-fix.md) |
| New-account starter profile <!-- catalog: `new_account_profile` --> | Configurable starting GP, cash, rank and unit gifts instead of the stock 9,999,999 / rank 15 | [Use](docs/new-account-profile.md) |
| Shop rental and Unicode labels <!-- catalog: `shop_labels` --> | Readable rental/permanent labels, numeric quantities and Chinese shop options | [Use](docs/shop-labels.md) |

<!-- /mod-category:fixes -->

## Gameplay extensions

<!-- mod-category:gameplay -->

| Feature | Overview | Documentation |
|---|---|---|
| Mission AI wingmen <!-- catalog: `coop_ai`, `mission_ai` --> | `!ai` in a co-op room title adds AI wingmen to the team | [Use](docs/coop-wingmen.md) |
| PvP AI <!-- catalog: `pvp_ai`, `match_ai` --> | `!ai` in a room title adds AI opponents and teammates; optionally group all humans, fill both sides, fill opponents only or add exact counts | [Use](docs/pvp-ai.md) |
| AI mech catalog (separate addon) <!-- catalog: `ai_catalog` --> | Add the server's sortie-ready units to the AI unit pools | [Use](docs/ai-catalog.md) |
| Battle contribution rewards <!-- catalog: `battle_rewards` --> | GB/MB from battle contribution, supervised wallet delivery and historical GB compensation | [Use](docs/battle-rewards.md) |
| Collection capsule <!-- catalog: `collection_capsule` --> | 100,000 GP activity machine that fills missing catalog units first | [Use](docs/collection-capsule.md) |
| Per-round random loadouts <!-- catalog: `room_random`, `room_random_client` --> | `!rand` in a room title draws each player's unit or deck from their own hangar every round | [Use](docs/room-random.md) |
| FPS cap <!-- catalog: `fps_limit` --> | Raise the rendering ceiling while keeping native gameplay speed | [Use](docs/fps-limit.md) |
| Extended room titles <!-- catalog: `room_titles`, `room_titles_agent`, `room_titles_server` --> | Enter up to 128 characters in normal, AI, team and mission room creation, settings and rename controls | [Use](docs/room-titles.md) |
| Battle AI difficulty <!-- catalog: `battle_ai`, `battle_ai_server` --> | Easy / Medium / Hard for PvP AI, chosen in the room title | [Use](docs/battle-ai.md) |
| Automatic hangar capacity <!-- catalog: `hangar_capacity` --> | Raise every account to at least 1200 hangar slots at login (WorldServer) | [Use](docs/hangar-capacity.md) |
| Hangar manager <!-- catalog: `hangar_manager` --> | One-click sorting and duplicate exchange on the unit-management page, with server-side GB batches | [Use](docs/hangar-manager.md) |
| Operator supplies and manager <!-- catalog: `operator_catalog` --> | Five free supply officers, plus operator names, portraits and drops | [Use](docs/operator-assistant.md) |
| Capsule catalog editor <!-- catalog: `capsule_catalog` --> | Create, edit and retire capsule machines, pools, prices and weights in the Manager | [Use](docs/capsule-catalog.md) |
| Daily double rewards <!-- catalog: `daily_rewards` --> | Double battle GB and EXP in a daily window (default 18:00–22:00) | [Use](docs/daily-rewards.md) |
| Storage claim all <!-- catalog: `storage_claim` --> | Claim every item or gift in the current storage tab with one confirmation | [Use](docs/storage-claim.md) |
| Chinese shop localization and search <!-- catalog: `shop_localization` --> | Translate shop names, descriptions and options; search by Chinese name combined with rank and payment filters | [Use](docs/shop-localization.md) |
| Friend unit gifts (optional) <!-- catalog: `friend_unit_gift_client`, `friend_unit_gift_server` --> | Give an owned unit to a friend as-is; the friend accepts or returns it in Storage. Off until the owner enables it | [Use](docs/friend-unit-gift.md) |
| Unit family import (Beta) <!-- catalog: `unit_catalog` --> | Import units and portable metadata/artwork, complete paired definitions, and author dedicated activity pools with GP or token presets in Mod Manager | [Use](docs/unit-import.md) |

<!-- /mod-category:gameplay -->

Owners can also use the separate Manager module [holiday and birthday gifts](docs/calendar-gifts.md) to put units and items into players' Storage on 14 common holidays, birthdays or chosen dates.

<a id="long-room-title-commands"></a>

Additional reference: [room-title commands](docs/room-commands.md).

## Networking and hosting

<!-- mod-category:network -->

| Feature | Overview | Documentation |
|---|---|---|
| FRP connectivity <!-- catalog: `net_route`, `agent_udp_pool` --> | Connect through the owner's Sakura FRP or other TCP/UDP forwarding | [Use](docs/frp-distribution.md) |
| Shop catalog manager <!-- catalog: `shop_catalog` --> | List, price and retire the owner's own shop offers from the Manager (WorldServer) | [Use](docs/shop-catalog.md) |

<!-- /mod-category:network -->

## Developer tools

Optional development and diagnostic features.

<!-- mod-category:development -->

| Feature | Overview | Documentation |
|---|---|---|
| Packet logger <!-- catalog: `net_probe` --> | Log the client's outgoing packets for protocol work | [Use](docs/net-probe.md) |
| Capsule price probe (Experimental) <!-- catalog: `capsule_price_probe` --> | Read-only log of capsule list price label refreshes | [Use](docs/capsule-price-probe.md) |
| Native GameTest (Experimental) <!-- catalog: `game_test_client`, `game_test_server` --> | Native login, room entry, Ready, single-client and coordinated multiplayer battles, and page checks | [Use](docs/game-test.md) |

<!-- /mod-category:development -->

## Project documentation

[Build and developer API](docs/framework.md) · [Contribution and AI disclosure rules](docs/contributing.zh-CN.md) · [Setup and troubleshooting](docs/setup.md)

MIT: [LICENSE](LICENSE). Third-party notices: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
