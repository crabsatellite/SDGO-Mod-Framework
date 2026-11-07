# SDGO mod framework

Core fixes and optional extensions for SD Gundam Online clients and private servers.
Unofficial fan project; no game files are included. [简体中文](README.zh-CN.md)

The game version is the client and server of the **SD Gundam Online Ultimate Repack** published by Bilibili
uploader 屮仔丿 ([video BV1Qa4y1z7eW](https://www.bilibili.com/video/BV1Qa4y1z7eW/)). The framework is verified and tested against that pack's
executables; supported builds are listed under [Installation](docs/setup.md#supported-game-builds). This project
does not distribute the pack.

[Installation](docs/setup.md) · [SDGO Mod Manager](docs/mod-manager.md) · [Latest release](https://github.com/crabsatellite/SDGO-Mod-Framework/releases/latest)
Dual-source client updates track framework programs and owner unit/configuration content separately, with one verified installation transaction.

Server owners and maintainers install, configure and [update](docs/mod-manager.md#updates) features with
**SDGO Mod Manager**; the **0_更新与管理** entry at the top of the client folder opens it. Players can update
an owner-provided complete client in Manager and keep using their normal game launcher. Shared Mod configuration
remains the server owner's responsibility.
Owners can also manage their own **local-only** addons in Manager; those are never packaged or exported.
Installations follow the official curated starter pack by default. Owners explicitly choose DIY when taking over
maintenance. Once an owner starts DIY, they maintain its configuration,
scripts, resources, compatibility and releases. [Owner-managed updates](docs/owner-updates.md) let owners review
upstream components and publish their confirmed pack; players follow only that owner's source.
Manager checks dependencies, data impact and content retirement, separates saved settings from observed activation, and can update an owner-provided complete client while preserving existing configuration and content. See [Manager safety and updates](docs/mod-manager.md#dependencies-data-and-activation).

This page is a feature index. Configuration, gameplay instructions, compatibility and troubleshooting are in each feature's documentation.

Beta and Experimental after a feature name mark its release channel; unlabeled features are Stable:

- **Stable**: the basic workflow works; later problems are fixed in new versions. Manager updates install Stable by default.
- **Beta**: the basic workflow is still under development. Current Beta features include unit family import. Switch Manager updates to the Beta channel to install them.
- **Experimental**: development diagnostics. Development tools are not installed by Manager updates.

## Core fixes

<!-- mod-category:fixes -->

| Feature | Overview | Documentation |
|---|---|---|
| IME candidates and commit fix <!-- catalog: `ime_fix` --> | Restore Chinese candidates and replace the old preedit range without residual letters | [Use](docs/ime-fix.md) |
| Activity capsule currency <!-- catalog: `capsule_currency_fix` --> | Show machine 9301 as 99 coins in the list and purchase confirmation | [Use](docs/capsule-currency-fix.md) |
| Input method freeze fix <!-- catalog: `findwindow_fix` --> | Stop input methods such as iFlytek from freezing or crashing the repack client | [Use](docs/findwindow-fix.md) |
| Startup crash fix <!-- catalog: `startup_crash_fix` --> | Stop random crashes on the loading screen at game start | [Use](docs/startup-crash-fix.md) |
| First login lobby music <!-- catalog: `lobby_music_fix` --> | Restore the lobby music on first login | [Use](docs/lobby-music-fix.md) |
| Wait-room window initialization guard <!-- catalog: `waitroom_window_fix` --> | Fix the client sometimes closing right after a room is created | [Use](docs/waitroom-window-fix.md) |
| Native data and lifecycle guards <!-- catalog: `native_data_guard` --> | Fix crashes when unit data is missing or unit management is still waiting for data; keep crash logs accurate | [Use](docs/native-data-guard.md) |
| Scene registration lifecycle fix <!-- catalog: `scene_lifecycle_fix` --> | Fix an occasional cleanup crash after a scene object moves to another area | [Use](docs/scene-lifecycle-fix.md) |
| Multiplayer loading synchronization <!-- catalog: `loading_sync_fix` --> | Fix multiplayer starts that occasionally hang on the loading screen (BattleServer) | [Use](docs/loading-sync-fix.md) |
| Native login recovery <!-- catalog: `login_recovery`, `login_recovery_client` --> | WorldServer stale-session cleanup plus bounded client reconnect, pending-request cleanup and return to native login input | [Use](docs/login-recovery.md) |
| Apsalus III transform input fix <!-- catalog: `transform_input_fix` --> | Release the native transform lock when a form has no transform animation | [Use](docs/transform-input-fix.md) |
| Agent relay log fix <!-- catalog: `agent_relay_log_fix` --> | Fix an Agent relay log that reads invalid memory | [Use](docs/agent-relay-log-fix.md) |
| Battle rank regression fix <!-- catalog: `rank_progression_fix` --> | Stop battle settlement from lowering a pilot's rank (WorldServer) | [Use](docs/rank-progression-fix.md) |
| Friend list fix <!-- catalog: `friend_list_fix`, `friend_list_client_fix` --> | Keep offline friends, current names and ranks; Chinese actions in the home USER LIST and the built-in messenger | [Use](docs/friend-list-fix.md) |
| New-account starter profile <!-- catalog: `new_account_profile` --> | Configurable starting GP, cash, rank and unit gifts instead of the stock 9,999,999 / rank 15 | [Use](docs/new-account-profile.md) |
| Shop rental and Unicode labels <!-- catalog: `shop_labels` --> | Readable rental/permanent labels, numeric quantities and Chinese shop options | [Use](docs/shop-labels.md) |
| Chinese shop artwork labels <!-- catalog: `shop_art_localization` --> | Translate rank text baked into supported paint artwork | [Use](docs/shop-art-localization.md) |

<!-- /mod-category:fixes -->

## Gameplay extensions

<!-- mod-category:gameplay -->

| Feature | Overview | Documentation |
|---|---|---|
| Announcements <!-- catalog: `announcements` --> | Show a news digest once at login; optional in-game owner notices by time and event | [Use](docs/announcements.md) |
| Mission AI wingmen <!-- catalog: `coop_ai`, `mission_ai` --> | `!ai` adds mission wingmen with the player life allowance; `!ai3 inf` allows unlimited AI respawns | [Use](docs/coop-wingmen.md) |
| Mission behavior profiles <!-- catalog: `mission_behavior`, `mission_behavior_server` --> | Select paired OC-default or current GCO mission behavior for one client and every Battle channel in a server group | [Use](docs/mission-behavior.md) |
| PvP AI <!-- catalog: `pvp_ai`, `match_ai` --> | `!ai` in a room title adds AI opponents and teammates; optionally group all humans, fill both sides, fill opponents only or add exact counts | [Use](docs/pvp-ai.md) |
| AI mech catalog (separate addon) <!-- catalog: `ai_catalog` --> | Add the server's sortie-ready units to the AI unit pools | [Use](docs/ai-catalog.md) |
| Battle contribution rewards <!-- catalog: `battle_rewards` --> | Contribution GB/MB, configurable human multipliers and optional per-account gathering rewards | [Use](docs/battle-rewards.md) |
| Collection capsule <!-- catalog: `collection_capsule` --> | Missing-unit activity machine with configurable price and limits, plus native encyclopedia acquisition hints; 15 per week by default | [Use](docs/collection-capsule.md) |
| Per-round random loadouts <!-- catalog: `room_random`, `room_random_client` --> | `!rand` in a room title draws each player's unit or deck from their own hangar every round | [Use](docs/room-random.md) |
| Complete PvP All Map <!-- catalog: `all_maps` --> | Make the room's All Map choice draw from the complete native PvP map catalog while keeping mission-only maps in missions | [Use](docs/all-maps.md) |
| FPS cap <!-- catalog: `fps_limit` --> | Raise the rendering ceiling while keeping native gameplay speed | [Use](docs/fps-limit.md) |
| Extended room titles <!-- catalog: `room_titles`, `room_titles_agent`, `room_titles_server` --> | Enter up to 128 characters in normal, AI, team and mission room creation, settings and rename controls | [Use](docs/room-titles.md) |
| Battle AI difficulty <!-- catalog: `battle_ai`, `battle_ai_server` --> | Easy / Medium / Hard, shared enemy contacts and routes through map propulsion devices | [Use](docs/battle-ai.md) |
| Automatic hangar capacity <!-- catalog: `hangar_capacity` --> | Raise every account to at least 1200 hangar slots at login (WorldServer) | [Use](docs/hangar-capacity.md) |
| Hangar manager <!-- catalog: `hangar_manager` --> | One-click sorting and duplicate exchange on the unit-management page, with server-side GB batches | [Use](docs/hangar-manager.md) |
| Operator supplies and manager <!-- catalog: `operator_catalog` --> | Five free supply officers, plus operator names, portraits and drops | [Use](docs/operator-assistant.md) |
| Capsule catalog editor <!-- catalog: `capsule_catalog` --> | Create, edit and retire machines, pools, prices and weights; renumber unused dedicated activities in the Manager | [Use](docs/capsule-catalog.md) |
| Daily double rewards <!-- catalog: `daily_rewards` --> | Double battle GB and EXP in a daily window (default 18:00–22:00) | [Use](docs/daily-rewards.md) |
| Storage claim all <!-- catalog: `storage_claim` --> | Claim every item or gift in the current storage tab with one confirmation | [Use](docs/storage-claim.md) |
| Chinese shop localization and search <!-- catalog: `shop_localization` --> | Translate shop names, descriptions, options and paint-page unit previews; search by Chinese name combined with rank and payment filters | [Use](docs/shop-localization.md) |
| Friend unit gifts (optional) <!-- catalog: `friend_unit_gift_client`, `friend_unit_gift_server` --> | Right-click a friend (online or offline), pick a unit in unit management, and the friend accepts or returns it in Storage. Off until the owner enables it | [Use](docs/friend-unit-gift.md) |
| Over-custom six-stat enhancement (Beta) <!-- catalog: `oc_custom_client`, `oc_custom_server`, `oc_custom_battle` --> | The original OC normal/Over points, challenge and reset on the factory upgrade page, stored beside the current custom parts; the owner picks which rule set battles use | [Use](docs/over-custom.md) |
| Unit family import (Beta) <!-- catalog: `unit_catalog`, `unit_catalog_server` --> | Import unit archives, edit skill/funnel dependency fields, retain script declarations, author blueprints and activity pools in Mod Manager, import protocol-ready batches of single, reciprocal, one-way and self-transform families plus third-form entry variants, and apply that roster to the server from Mod Manager | [Use](docs/unit-import.md) |

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
