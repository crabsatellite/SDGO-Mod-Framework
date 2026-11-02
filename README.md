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
| IME input fix <!-- catalog: `ime_fix` --> | Commit Chinese, Japanese and Korean IME text at the correct length | [Use](docs/ime-fix.md) |
| Activity capsule currency <!-- catalog: `capsule_currency_fix` --> | Show machine 9301 as 99 coins in the list and purchase confirmation | [Use](docs/capsule-currency-fix.md) |
| Input method freeze fix <!-- catalog: `findwindow_fix` --> | Stop input methods such as iFlytek from freezing or crashing the repack client | [Use](docs/findwindow-fix.md) |
| Startup crash fix <!-- catalog: `startup_crash_fix` --> | Stop random crashes on the loading screen at game start | [Use](docs/startup-crash-fix.md) |
| Agent relay log fix <!-- catalog: `agent_relay_log_fix` --> | Fix an Agent relay log that reads invalid memory | [Use](docs/agent-relay-log-fix.md) |
| Battle rank regression fix <!-- catalog: `rank_progression_fix` --> | Stop battle settlement from lowering a pilot's rank (WorldServer) | [Use](docs/rank-progression-fix.md) |
| New-account starter profile <!-- catalog: `new_account_profile` --> | Configurable starting GP, cash, rank and unit gifts instead of the stock 9,999,999 / rank 15 | [Use](docs/new-account-profile.md) |
| Shop rental and Unicode labels <!-- catalog: `shop_labels` --> | Readable rental/permanent labels and Chinese option names in the shop | [Use](docs/shop-labels.md) |

<!-- /mod-category:fixes -->

## Gameplay extensions

<!-- mod-category:gameplay -->

| Feature | Overview | Documentation |
|---|---|---|
| Mission AI wingmen <!-- catalog: `coop_ai`, `mission_ai` --> | `!ai` in a co-op room title adds AI wingmen to the team | [Use](docs/coop-wingmen.md) |
| PvP AI (Beta) <!-- catalog: `pvp_ai`, `match_ai` --> | `!ai` in a PvP room title adds AI opponents and teammates | [Use](docs/pvp-ai.md) |
| AI mech catalog (Beta, separate addon) <!-- catalog: `ai_catalog` --> | Custom AI rosters | [Use](docs/ai-catalog.md) |
| Battle contribution rewards (Beta) <!-- catalog: `battle_rewards` --> | Combat reward management | [Use](docs/battle-rewards.md) |
| Collection capsule (Beta) <!-- catalog: `collection_capsule` --> | Collection gameplay | [Use](docs/collection-capsule.md) |
| Per-round random loadouts (Beta) <!-- catalog: `room_random`, `room_random_client` --> | Randomized mission and PvP loadouts | [Use](docs/room-random.md) |
| Extended room titles (Beta) <!-- catalog: `room_titles`, `room_titles_agent`, `room_titles_server` --> | Room title extensions | [Use](docs/room-titles.md) |
| Battle AI difficulty (Beta) <!-- catalog: `battle_ai`, `battle_ai_server` --> | Combat AI behavior | [Use](docs/battle-ai.md) |
| Automatic hangar capacity (Beta) <!-- catalog: `hangar_capacity` --> | Ensure at least 1200 native slots at login | [Use](docs/hangar-capacity.md) |
| Hangar manager (Beta) <!-- catalog: `hangar_manager` --> | Server sorting and GB batch exchange; 0.4.0 adds immediate GB preview and atomic server credit | [Use](docs/hangar-manager.md) |
| Operator supplies and manager (Beta) <!-- catalog: `operator_catalog` --> | Operator content management | [Use](docs/operator-assistant.md) |
| Capsule catalog editor (Beta) <!-- catalog: `capsule_catalog` --> | Capsule content management | [Use](docs/capsule-catalog.md) |
| Daily double rewards (Beta) <!-- catalog: `daily_rewards` --> | Scheduled reward events | [Use](docs/daily-rewards.md) |
| Storage claim all (Beta) <!-- catalog: `storage_claim` --> | Batch storage reward collection; 0.1.1 fixes lobby clicks and hover between confirmation clicks | [Use](docs/storage-claim.md) |

<!-- /mod-category:gameplay -->

<a id="long-room-title-commands"></a>

Additional reference: [room-title commands](docs/room-commands.md).

## Networking and hosting

<!-- mod-category:network -->

| Feature | Overview | Documentation |
|---|---|---|
| FRP connectivity (Beta) <!-- catalog: `net_route`, `agent_udp_pool` --> | Network connectivity | [Use](docs/frp-distribution.md) |
| Shop catalog manager (Beta) <!-- catalog: `shop_catalog` --> | Shop content management | [Use](docs/shop-catalog.md) |

<!-- /mod-category:network -->

## Developer tools

Optional development and diagnostic features.

<!-- mod-category:development -->

| Feature | Overview | Documentation |
|---|---|---|
| Packet logger <!-- catalog: `net_probe` --> | Protocol diagnostics | [Use](docs/net-probe.md) |
| Capsule price probe (Experimental) <!-- catalog: `capsule_price_probe` --> | Capsule diagnostics | [Use](docs/capsule-price-probe.md) |
| Native GameTest (Experimental) <!-- catalog: `game_test_client`, `game_test_server` --> | Native integration testing | [Use](docs/game-test.md) |

<!-- /mod-category:development -->

## Project documentation

[Build and developer API](docs/framework.md) · [Contribution and AI disclosure rules](docs/contributing.zh-CN.md) · [Setup and troubleshooting](docs/setup.md)

MIT: [LICENSE](LICENSE). Third-party notices: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
