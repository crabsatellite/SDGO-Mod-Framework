# SDGO mod framework

Core fixes and optional extensions for SD Gundam Online clients and private servers.
Unofficial fan project; no game files are included. [简体中文](README.zh-CN.md)

[Installation](docs/setup.md) · [SDGO Mod Manager](docs/mod-manager.md) · [Latest release](https://github.com/crabsatellite/SDGO-Mod-Framework/releases/latest)

Manage features through **SDGO Mod Manager** and use your normal game launcher.
This page is a feature index. Configuration, gameplay instructions, examples and verification details are in each feature's documentation.
Beta and Experimental labels indicate release channels.

## Core fixes

<!-- mod-category:fixes -->

| Feature | Overview | Documentation |
|---|---|---|
| IME candidates and commit fix <!-- catalog: `ime_fix` --> | Restore Chinese candidates and replace the old preedit range without residual letters | [Use](docs/ime-fix.md) |
| Activity capsule currency <!-- catalog: `capsule_currency_fix` --> | Show machine 9301 as 99 coins in the list and purchase confirmation | [Use](docs/capsule-currency-fix.md) |
| Input method freeze fix <!-- catalog: `findwindow_fix` --> | Stop input methods such as iFlytek from freezing or crashing the repack client | [Use](docs/findwindow-fix.md) |
| Startup crash fix <!-- catalog: `startup_crash_fix` --> | Stop random crashes on the loading screen at game start | [Use](docs/startup-crash-fix.md) |
| Agent relay log fix <!-- catalog: `agent_relay_log_fix` --> | Fix an Agent relay log that reads invalid memory | [Use](docs/agent-relay-log-fix.md) |
| Battle rank regression fix <!-- catalog: `rank_progression_fix` --> | Stop battle settlement from lowering a pilot's rank (WorldServer) | [Use](docs/rank-progression-fix.md) |
| Friend list fix <!-- catalog: `friend_list_fix`, `friend_list_client_fix` --> | Retain offline friends, current names and the native conversation entry | [Use](docs/friend-list-fix.md) |
| New-account starter profile <!-- catalog: `new_account_profile` --> | Configurable starting GP, cash, rank and unit gifts instead of the stock 9,999,999 / rank 15 | [Use](docs/new-account-profile.md) |
| Shop rental and Unicode labels <!-- catalog: `shop_labels` --> | Readable rental/permanent labels and Chinese option names in the shop | [Use](docs/shop-labels.md) |

<!-- /mod-category:fixes -->

## Gameplay extensions

<!-- mod-category:gameplay -->

| Feature | Overview | Documentation |
|---|---|---|
| Mission AI wingmen <!-- catalog: `coop_ai`, `mission_ai` --> | `!ai` in a co-op room title adds AI wingmen to the team | [Use](docs/coop-wingmen.md) |
| PvP AI (Beta) <!-- catalog: `pvp_ai`, `match_ai` --> | `!ai` in a PvP room title adds AI opponents and teammates | [Use](docs/pvp-ai.md) |
| AI mech catalog (Beta, separate addon) <!-- catalog: `ai_catalog` --> | Add the server's sortie-ready units to the AI unit pools | [Use](docs/ai-catalog.md) |
| Battle contribution rewards (Beta) <!-- catalog: `battle_rewards` --> | GB/MB from wins, kills, damage, assists and Boss contribution | [Use](docs/battle-rewards.md) |
| Collection capsule (Beta) <!-- catalog: `collection_capsule` --> | 100,000 GP activity machine that fills missing catalog units first | [Use](docs/collection-capsule.md) |
| Per-round random loadouts (Beta) <!-- catalog: `room_random`, `room_random_client` --> | `!rand` in a room title draws each player's unit or deck from their own hangar every round | [Use](docs/room-random.md) |
| FPS cap (Beta) <!-- catalog: `fps_limit` --> | Use the existing config.xml FPS limit instead of the forced 60 | [Use](docs/fps-limit.md) |
| Extended room titles (Beta) <!-- catalog: `room_titles`, `room_titles_agent`, `room_titles_server` --> | Mission and PvP room titles up to 128 characters, with commands anywhere in the title | [Use](docs/room-titles.md) |
| Battle AI difficulty (Beta) <!-- catalog: `battle_ai`, `battle_ai_server` --> | Easy / Medium / Hard PvP bots, selectable in the room title | [Use](docs/battle-ai.md) |
| Automatic hangar capacity (Beta) <!-- catalog: `hangar_capacity` --> | Raise every account to at least 1200 hangar slots at login (WorldServer) | [Use](docs/hangar-capacity.md) |
| Hangar manager (Beta) <!-- catalog: `hangar_manager` --> | One-click sorting and duplicate exchange on the unit-management page, with server-side GB batches | [Use](docs/hangar-manager.md) |
| Operator supplies and manager (Beta) <!-- catalog: `operator_catalog` --> | Five free supply officers, plus operator names, portraits and drops | [Use](docs/operator-assistant.md) |
| Capsule catalog editor (Beta) <!-- catalog: `capsule_catalog` --> | Create, edit and retire capsule machines, pools, prices and weights in the Manager | [Use](docs/capsule-catalog.md) |
| Daily double rewards (Beta) <!-- catalog: `daily_rewards` --> | Double battle GB and EXP in a daily window (default 18:00–22:00) | [Use](docs/daily-rewards.md) |
| Storage claim all (Beta) <!-- catalog: `storage_claim` --> | Claim every item or gift in the current storage tab with one confirmation | [Use](docs/storage-claim.md) |

| Chinese shop localization and search (Beta) <!-- catalog: `shop_localization` --> | Translate shop text and hangar confirmation names; combine Chinese search, rank and payment filters | [Guide](docs/shop-localization.md) |

<!-- /mod-category:gameplay -->

<a id="long-room-title-commands"></a>

Additional reference: [room-title commands](docs/room-commands.md).

## Networking and hosting

<!-- mod-category:network -->

| Feature | Overview | Documentation |
|---|---|---|
| FRP connectivity (Beta) <!-- catalog: `net_route`, `agent_udp_pool` --> | Connect through the owner's Sakura FRP or other TCP/UDP forwarding | [Use](docs/frp-distribution.md) |
| Shop catalog manager (Beta) <!-- catalog: `shop_catalog` --> | List, price and retire the owner's own shop offers from the Manager (WorldServer) | [Use](docs/shop-catalog.md) |

<!-- /mod-category:network -->

## Developer tools

Optional development and diagnostic features.

<!-- mod-category:development -->

| Feature | Overview | Documentation |
|---|---|---|
| Packet logger <!-- catalog: `net_probe` --> | Log the client's outgoing packets for protocol work | [Use](docs/net-probe.md) |
| Capsule price probe (Experimental) <!-- catalog: `capsule_price_probe` --> | Read-only log of capsule list price label refreshes | [Use](docs/capsule-price-probe.md) |
| Native GameTest (Experimental) <!-- catalog: `game_test_client`, `game_test_server` --> | Automated login, room, battle and page scenarios in the real client and server | [Use](docs/game-test.md) |

<!-- /mod-category:development -->

## Project documentation

[Build and developer API](docs/framework.md) · [Contribution and AI disclosure rules](docs/contributing.zh-CN.md) · [Setup and troubleshooting](docs/setup.md)

MIT: [LICENSE](LICENSE). Third-party notices: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
