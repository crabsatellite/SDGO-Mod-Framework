# SDGO mod framework

Core fixes and optional extensions for SD Gundam Online clients and private servers.
Unofficial fan project; no game files are included. [简体中文](README.zh-CN.md)

[Installation](docs/setup.md) · [SDGO Mod Manager](docs/mod-manager.md) · [Latest release](https://github.com/crabsatellite/SDGO-Mod-Framework/releases/latest)

Server owners and maintainers install, configure and [update](docs/mod-manager.md#updates) features with
**SDGO Mod Manager**; the **0_MOD管理（服主专用）** entry at the top of the client folder opens it. Ordinary
players keep using their normal game launcher, update the way their server owner provides, and should not
change Mods themselves, so their client stays matched to the server.
Every Manager page says what each button saves and when the change takes effect, and asks before leaving unsaved changes.
Manager can also configure addons marked **local-only**. They stay in the selected installation;
public package and export tools reject them, including case-varied paths. See the [Manager guide](docs/mod-manager.md).
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
| Wait-room window initialization guard <!-- catalog: `waitroom_window_fix` --> | Keep window icon queries safe while a room view is attaching | [Use](docs/waitroom-window-fix.md) |
| Native data and lifecycle guards <!-- catalog: `native_data_guard` --> | Guard missing data reads, retain deferred unit-management messages and preserve original crash context | [Use](docs/native-data-guard.md) |
| Scene registration lifecycle fix <!-- catalog: `scene_lifecycle_fix` --> | Preserve spatial ownership when destination-node allocation fails | [Use](docs/scene-lifecycle-fix.md) |
| Native login recovery <!-- catalog: `login_recovery` --> | Complete authenticated old-session cleanup and retry login once on WorldServer | [Use](docs/login-recovery.md) |
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
| Announcements <!-- catalog: `announcements` --> | Login-only daily digest with server-local timezone defaults; optional scheduled notices with cached news, technology, free-game and history cards | [Use](docs/announcements.md) |
| Mission AI wingmen <!-- catalog: `coop_ai`, `mission_ai` --> | `!ai` adds mission wingmen with the player life allowance; `!ai3 inf` allows unlimited AI respawns | [Use](docs/coop-wingmen.md) |
| PvP AI <!-- catalog: `pvp_ai`, `match_ai` --> | `!ai` in a room title adds AI opponents and teammates; optionally group all humans, fill both sides, fill opponents only or add exact counts | [Use](docs/pvp-ai.md) |
| AI mech catalog (separate addon) <!-- catalog: `ai_catalog` --> | Add the server's sortie-ready units to the AI unit pools | [Use](docs/ai-catalog.md) |
| Battle contribution rewards <!-- catalog: `battle_rewards` --> | Contribution GB/MB, configurable human multipliers and optional per-account gathering rewards | [Use](docs/battle-rewards.md) |
| Collection capsule <!-- catalog: `collection_capsule` --> | Missing-unit activity machine with configurable price and successful-draw limits; 15 per week by default | [Use](docs/collection-capsule.md) |
| Per-round random loadouts <!-- catalog: `room_random`, `room_random_client` --> | `!rand` in a room title draws each player's unit or deck from their own hangar every round | [Use](docs/room-random.md) |
| FPS cap <!-- catalog: `fps_limit` --> | Raise the rendering ceiling while keeping native gameplay speed | [Use](docs/fps-limit.md) |
| Extended room titles <!-- catalog: `room_titles`, `room_titles_agent`, `room_titles_server` --> | Enter up to 128 characters in normal, AI, team and mission room creation, settings and rename controls | [Use](docs/room-titles.md) |
| Battle AI difficulty <!-- catalog: `battle_ai`, `battle_ai_server` --> | Room-selected Easy / Medium / Hard; shared teammate vision, collision-aware pursuit and native weapon handoff | [Use](docs/battle-ai.md) |
| Automatic hangar capacity <!-- catalog: `hangar_capacity` --> | Raise every account to at least 1200 hangar slots at login (WorldServer) | [Use](docs/hangar-capacity.md) |
| Hangar manager <!-- catalog: `hangar_manager` --> | Native-page sorting and duplicate exchange, server-side GB batches, and no repeated closed-panel rendering | [Use](docs/hangar-manager.md) |
| Operator supplies and manager <!-- catalog: `operator_catalog` --> | Five free supply officers, plus operator names, portraits and drops | [Use](docs/operator-assistant.md) |
| Capsule catalog editor <!-- catalog: `capsule_catalog` --> | Create, edit and retire machines, pools, prices and weights; renumber unused dedicated activities in the Manager | [Use](docs/capsule-catalog.md) |
| Daily double rewards <!-- catalog: `daily_rewards` --> | Double battle GB and EXP in a daily window (default 18:00–22:00) | [Use](docs/daily-rewards.md) |
| Storage claim all <!-- catalog: `storage_claim` --> | Claim every item or gift in the current storage tab with one confirmation | [Use](docs/storage-claim.md) |
| Chinese shop localization and search <!-- catalog: `shop_localization` --> | Translate shop names, descriptions, options and paint-page unit previews; search by Chinese name combined with rank and payment filters | [Use](docs/shop-localization.md) |
| Friend unit gifts (optional) <!-- catalog: `friend_unit_gift_client`, `friend_unit_gift_server` --> | Right-click an online or offline friend, page through owned units in native unit management, and let the friend accept or return the gift in Storage. Reopening Storage binds its new native page. Off by default | [Use](docs/friend-unit-gift.md) |
| Unit family import (Beta) <!-- catalog: `unit_catalog` --> | Import portable unit projects, publish framed blueprints or complete v2 presentation templates with checked textures, and author GP or token activity pools in Mod Manager | [Use](docs/unit-import.md) |

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
