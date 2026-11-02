# SDGO Mod 框架

为 SD敢达 Online 客户端和私服提供基础修复与可选扩展。
非官方爱好者项目，不包含游戏文件。[English](README.md)

[安装说明](docs/setup.zh-CN.md) · [统一 Mod Manager](docs/mod-manager.zh-CN.md) · [最新发布](https://github.com/crabsatellite/SDGO-Mod-Framework/releases/latest)

通过 **SDGO Mod Manager** 管理功能，游戏仍使用原启动器。
本页仅列出功能概览。具体配置、玩法操作、示例与验证范围见各功能文档。
Beta、Experimental 表示发布阶段。

## 基础修复

<!-- mod-category:fixes -->

| 功能 | 简介 | 文档 |
|---|---|---|
| 输入法提交修复 <!-- catalog: `ime_fix` --> | 中日韩输入法上屏文字按正确长度提交 | [使用说明](docs/ime-fix.zh-CN.md) |
| 活动扭蛋币种显示 <!-- catalog: `capsule_currency_fix` --> | 9301 号机在列表和购买确认框中显示为 99 代币 | [使用说明](docs/capsule-currency-fix.zh-CN.md) |
| 输入法卡死修复 <!-- catalog: `findwindow_fix` --> | 防止讯飞等输入法让整合包客户端卡死或崩溃 | [使用说明](docs/findwindow-fix.zh-CN.md) |
| 启动崩溃修复 <!-- catalog: `startup_crash_fix` --> | 修复开游戏时读取界面随机闪退 | [使用说明](docs/startup-crash-fix.zh-CN.md) |
| Agent 中继日志修复 <!-- catalog: `agent_relay_log_fix` --> | 修复 Agent 中继日志读取无效内存 | [使用说明](docs/agent-relay-log-fix.zh-CN.md) |
| 军衔回退修复 <!-- catalog: `rank_progression_fix` --> | 防止战斗结算降低已有军衔（WorldServer） | [使用说明](docs/rank-progression-fix.zh-CN.md) |
| 新账号起始资料修复 <!-- catalog: `new_account_profile` --> | 可配置的初始 GP、点券、军衔和赠送机体，替代原服的 9,999,999 / 15 级 | [使用说明](docs/new-account-profile.zh-CN.md) |
| 商城租赁与中文选项修复 <!-- catalog: `shop_labels` --> | 商城中显示可读的租赁/永久标签和中文选项名 | [使用说明](docs/shop-labels.zh-CN.md) |

<!-- /mod-category:fixes -->

## 玩法扩展

<!-- mod-category:gameplay -->

| 功能 | 简介 | 文档 |
|---|---|---|
| 任务 AI 僚机 <!-- catalog: `coop_ai`、`mission_ai` --> | 任务房名写 `!ai`，由 AI 僚机补位 | [使用说明](docs/coop-wingmen.zh-CN.md) |
| 对战 AI（Beta） <!-- catalog: `pvp_ai`、`match_ai` --> | 对战房名写 `!ai`，加入 AI 对手和队友 | [使用说明](docs/pvp-ai.zh-CN.md) |
| AI 机体目录（Beta，独立附加包） <!-- catalog: `ai_catalog` --> | 把服务器可出击的机体加入 AI 机体池 | [使用说明](docs/ai-catalog.zh-CN.md) |
| 战斗贡献奖励（Beta） <!-- catalog: `battle_rewards` --> | 按胜负、击杀、伤害、助攻和 Boss 贡献发放 GB/MB | [使用说明](docs/battle-rewards.zh-CN.md) |
| 收藏扭蛋（Beta） <!-- catalog: `collection_capsule` --> | 十万 GP 活动机台，优先抽尚未解锁的图鉴机体 | [使用说明](docs/collection-capsule.zh-CN.md) |
| 每局随机机体（Beta） <!-- catalog: `room_random`、`room_random_client` --> | 任务与对战随机出击 | [使用说明](docs/room-random.zh-CN.md) |
| 长房名（Beta） <!-- catalog: `room_titles`、`room_titles_agent`、`room_titles_server` --> | 房间标题扩展 | [使用说明](docs/room-titles.zh-CN.md) |
| 战斗 AI 难度（Beta） <!-- catalog: `battle_ai`、`battle_ai_server` --> | 战斗 AI 行为 | [使用说明](docs/battle-ai.md) |
| 自动机库容量（Beta） <!-- catalog: `hangar_capacity` --> | 登录时保障至少 1200 格，只增不减 | [使用说明](docs/hangar-capacity.zh-CN.md) |
| 机库管理（Beta） <!-- catalog: `hangar_manager` --> | 服务端排序及 GB 批量回收；0.4.0 支持快速预览和统一入账 | [使用说明](docs/hangar-manager.zh-CN.md) |
| 副官补给与管理（Beta） <!-- catalog: `operator_catalog` --> | 副官内容管理 | [使用说明](docs/operator-assistant.zh-CN.md) |
| 扭蛋机编辑器（Beta） <!-- catalog: `capsule_catalog` --> | 扭蛋内容管理 | [使用说明](docs/capsule-catalog.zh-CN.md) |
| 每日双倍时间（Beta） <!-- catalog: `daily_rewards` --> | 定时奖励活动 | [使用说明](docs/daily-rewards.zh-CN.md) |
| 保管库一键领取（Beta） <!-- catalog: `storage_claim` --> | 自动分页领取奖品；0.1.1 修复大厅点击及鼠标经过取消确认 | [使用说明](docs/storage-claim.zh-CN.md) |

<!-- /mod-category:gameplay -->

<a id="long-room-title-commands"></a>

补充参考：[房名指令文档](docs/room-commands.zh-CN.md)。

## 联机与服主工具

<!-- mod-category:network -->

| 功能 | 简介 | 文档 |
|---|---|---|
| FRP 联机（Beta） <!-- catalog: `net_route`、`agent_udp_pool` --> | 网络联机 | [使用说明](docs/frp-distribution.zh-CN.md) |
| 商城商品管理（Beta） <!-- catalog: `shop_catalog` --> | 商城内容管理 | [使用说明](docs/shop-catalog.zh-CN.md) |

<!-- /mod-category:network -->

## 开发诊断

可选的开发与诊断功能。

<!-- mod-category:development -->

| 功能 | 简介 | 文档 |
|---|---|---|
| 发包日志 <!-- catalog: `net_probe` --> | 协议诊断 | [使用说明](docs/net-probe.md) |
| 扭蛋价格探针（Experimental） <!-- catalog: `capsule_price_probe` --> | 扭蛋诊断 | [使用说明](docs/capsule-price-probe.md) |
| GameTest（Experimental） <!-- catalog: `game_test_client`、`game_test_server` --> | 原生集成测试 | [使用说明](docs/game-test.zh-CN.md) |

<!-- /mod-category:development -->

## 项目文档

[构建与开发接口](docs/framework.md) · [贡献与 AI 披露规则](docs/contributing.zh-CN.md) · [配置与排错](docs/setup.zh-CN.md)

MIT，见 [LICENSE](LICENSE)。第三方许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
