# SDGO Mod 框架

为 SD敢达 Online 客户端和私服提供基础修复、玩法扩展和管理工具。
非官方爱好者项目，不包含游戏文件。[English](README.md)

[安装说明](docs/setup.zh-CN.md) · [SDGO Mod Manager](docs/mod-manager.zh-CN.md) · [最新发布](https://github.com/crabsatellite/SDGO-Mod-Framework/releases/latest)

| [对战 AI：队友与对手](docs/pvp-ai.zh-CN.md) | [任务 AI：合作僚机](docs/coop-wingmen.zh-CN.md) |
|:---:|:---:|
| [![对战中的 AI 队友与对手](docs/pvp-ai-demo.webp)](docs/pvp-ai.zh-CN.md) | [![参与合作任务的 AI 僚机](docs/coop-wingmen-demo.webp)](docs/coop-wingmen.zh-CN.md) |

[玩法扩展](#玩法扩展) · [玩家辅助与界面](#玩家辅助与界面) · [服主管理与内容编辑](#服主管理与内容编辑) · [联机与更新](#联机与更新) · [基础修复](#基础修复) · [开发诊断](#开发诊断)

## 开始使用

**玩家：** 先取得兼容的原版客户端，在 Manager 安装框架并导入服主配置档，取得服主选定的 Mod、共享配置和素材。
绑定后照常双击 `GOnline.exe`，Manager 显示更新进度，校验完成后启动游戏。更新保留个人设置，详见[安装与启动前更新](docs/owner-updates.zh-CN.md)。

**服主：** 从客户端目录顶部的「0_更新与管理」或包内的 **SDGO Mod Manager.exe** 打开管理界面。
默认跟随基础整合包；选择「服主 DIY」后自行维护配置、内容和兼容性。
[本服程序包](docs/owner-programmes.zh-CN.md)与[服主配置档](docs/owner-updates.zh-CN.md)分别维护程序和内容，玩家只需跟随一个配置地址。
「仅本地」扩展留在服主本机，不会打包分发。

兼容性基于 B 站 UP 主屮仔丿发布的 **SD敢达 Ultimate Repack**（[视频 BV1Qa4y1z7eW](https://www.bilibili.com/video/BV1Qa4y1z7eW/)），
具体程序版本见[支持的游戏版本](docs/setup.zh-CN.md#支持的游戏版本)。本项目不提供原版客户端或服务端。

未标注阶段的功能为 **Stable**，Manager 默认安装 Stable。**Beta** 表示基本流程仍在开发，需在 Manager 切换到 Beta 通道；
**Experimental** 为开发诊断工具，不随常规更新安装。具体设置、兼容性与排错见各功能介绍页。

## 玩法扩展

新增战斗规则、AI、成长与奖励，以及玩家之间的互动。

| 功能 | 简介 |
|---|---|
| [对战 AI](docs/pvp-ai.zh-CN.md) <!-- catalog: `pvp_ai`、`match_ai` --> | 房名写 `!ai` 加入 AI 对手和队友；可让真人自动同队，选择双方补位、只补对面或指定数量 |
| [任务 AI 僚机](docs/coop-wingmen.zh-CN.md) <!-- catalog: `coop_ai`、`mission_ai` --> | 任务房名写 `!ai`，AI 按玩家生命额度复活；`!ai3 inf` 开启 AI 无限复活 |
| [战斗 AI 难度](docs/battle-ai.zh-CN.md) <!-- catalog: `battle_ai`、`battle_ai_server` --> | 在房名中选择 Easy／Medium／Hard 三档对战 AI 难度 |
| [任务行为版本](docs/mission-behavior.zh-CN.md) <!-- catalog: `mission_behavior`、`mission_behavior_server` --> | 统一选择任务 AI 与刷怪使用 OC 原版行为或现有行为，客户端与全部 Battle 频道同步 |
| [AI 机师互动台词（可选）](docs/ai-dialogue.zh-CN.md) <!-- catalog: `ai_dialogue` --> | AI 机师按战况和机体人设说台词、给队友建议；需自填 DeepSeek 密钥，默认关闭 |
| [每局随机机体](docs/room-random.zh-CN.md) <!-- catalog: `room_random`、`room_random_client` --> | 房名写 `!rand`，每局从各自机库随机选择出击机体或卡组 |
| [完整对战 All Map](docs/all-maps.zh-CN.md) <!-- catalog: `all_maps` --> | 房间选择 All Map 时从完整原生对战地图目录抽取，任务专用图仍留在任务模式 |
| [OC 六维强化](docs/over-custom.zh-CN.md) <!-- catalog: `oc_custom_client`、`oc_custom_server`、`oc_custom_battle` --> | OC 六维加点、挑战与洗点，房间可选择 OC、G改 2.0 或服主提供的 G改 1.0；原改造数据独立保留 |
| [好友机体赠送（可选）](docs/friend-unit-gift.zh-CN.md) <!-- catalog: `friend_unit_gift_client`、`friend_unit_gift_server` --> | 右键好友（在线或离线均可），在机体管理中选机赠送，对方在保管库接受或退还；默认关闭，由服主开启 |
| [收藏扭蛋](docs/collection-capsule.zh-CN.md) <!-- catalog: `collection_capsule` --> | 优先补图鉴的活动机台；服主可设除外名单，图鉴获取说明随实际奖池自动更新；价格和次数可配置，默认每周 15 次 |
| [战斗贡献奖励](docs/battle-rewards.zh-CN.md) <!-- catalog: `battle_rewards` --> | 按贡献发放 GB/MB，可配置人数倍率与按账号去重的聚会奖励 |
| [每日双倍时间](docs/daily-rewards.zh-CN.md) <!-- catalog: `daily_rewards` --> | 每天固定时段（默认 18:00–22:00）战斗 GB 和经验翻倍 |

<a id="long-room-title-commands"></a>

房名里的玩法与 AI 参数可组合使用，见[房名指令](docs/room-commands.zh-CN.md)。

## 玩家辅助与界面

帧率、显示、机库与领取操作等日常使用功能。

| 功能 | 简介 |
|---|---|
| [FPS 上限](docs/fps-limit.zh-CN.md) <!-- catalog: `fps_limit` --> | 提高渲染帧率上限，保留原生游戏速度 |
| [机库管理](docs/hangar-manager.zh-CN.md) <!-- catalog: `hangar_manager` --> | 在机体管理页整理机库、预览并兑换重复机体，支持导入机体 |
| [保管库一键领取](docs/storage-claim.zh-CN.md) <!-- catalog: `storage_claim` --> | 保管库页内一键领取当前页签的全部物品或礼物 |
| [自动机库容量](docs/hangar-capacity.zh-CN.md) <!-- catalog: `hangar_capacity` --> | 登录时把每个账号的机库保障到至少 1200 格（WorldServer） |
| [商城汉化与中文检索](docs/shop-localization.zh-CN.md) <!-- catalog: `shop_localization` --> | 汉化商品名称、说明、选项和涂装机体预览，可用中文名称搜索并组合等级、币种筛选 |
| [原生界面中文汉化](docs/ui-localization.zh-CN.md) <!-- catalog: `ui_localization` --> | 汉化原版泰文按钮与较长系统提示，保留简单英语和原版图片；自定义内容随内容包分发 |
| [长房名](docs/room-titles.zh-CN.md) <!-- catalog: `room_titles`、`room_titles_agent`、`room_titles_server` --> | 普通、AI、队伍及任务房的建房、设置和改名入口支持最长 128 字 |
| [登录机体展示](docs/login-showcase.zh-CN.md) <!-- catalog: `login_showcase` --> | 选择登录页面展示哪些机体：按等级、全部、最近注册或指定名单，默认 SS/SR/SU |

## 服主管理与内容编辑

在 SDGO Mod Manager 编辑和发布服务器内容，供玩家安装使用。

| 功能 | 简介 |
|---|---|
| [商城商品管理](docs/shop-catalog.zh-CN.md) <!-- catalog: `shop_catalog` --> | 在 Manager 中上架、改价、下架服主自己的商城商品（WorldServer） |
| [扭蛋机编辑器](docs/capsule-catalog.zh-CN.md) <!-- catalog: `capsule_catalog` --> | 在 Manager 中新增、修改、下架扭蛋机，设置奖池、价格和权重，为未使用的独立活动机改号 |
| [机体族导入（Beta）](docs/unit-import.zh-CN.md) <!-- catalog: `unit_catalog`, `unit_catalog_server` --> | 在 Mod Manager 导入任何来源按协议制作的机体包或整套阵容，补齐定义并生成两端数据、图鉴和商城／扭蛋资源 |
| [副官与能力管理](docs/operator-assistant.zh-CN.md) <!-- catalog: `operator_catalog`、`operator_catalog_server` --> | 在 Manager 编辑副官外观、能力、补给掉落和商城草稿，导入分享包并发布配套内容 |
| [AI 机体目录（可选）](docs/ai-catalog.zh-CN.md) <!-- catalog: `ai_catalog` --> | 把服务器可出击的机体加入 AI 机体池 |
| [公告与展示规则](docs/announcements.zh-CN.md) <!-- catalog: `announcements` --> | 登录时显示一次资讯简报；可选在游戏内按时段和事件展示服主公告 |
| [新账号起始配置](docs/new-account-profile.zh-CN.md) <!-- catalog: `new_account_profile` --> | 设置新账号的起始货币、军衔和赠送机体 |
| [节日与生日赠礼](docs/calendar-gifts.zh-CN.md) | 在 Manager 配置节日、生日或指定日期的机体与道具赠礼，发到符合条件玩家的保管库 |

## 联机与更新

连接服务器、安装程序，以及同步服主选定的配置与内容。

| 功能 | 简介 |
|---|---|
| [FRP 联机](docs/frp-distribution.zh-CN.md) <!-- catalog: `net_route`、`agent_udp_pool` --> | 通过服主的 Sakura FRP 等 TCP/UDP 转发连接服务器 |
| [Mod 安装与更新](docs/mod-manager.zh-CN.md) | 在 Manager 安装、启停和配置官方或导入的模组，选择 Stable／Beta 更新通道 |
| [服主配置与自动更新](docs/owner-updates.zh-CN.md) | 导入服主配置地址，同步共享设置和自定义内容；启动前显示进度并完成校验 |
| [本服程序包](docs/owner-programmes.zh-CN.md) | 服主选定官方基线与本服补丁版本，和内容一起交付给玩家 |

## 基础修复

修复已有功能的崩溃、卡死、断线与显示错误。

| 功能 | 简介 |
|---|---|
| [中文输入法候选与提交修复](docs/ime-fix.zh-CN.md) <!-- catalog: `ime_fix` --> | 恢复候选窗请求，按旧拼音范围提交中文，保留英文和数字前后缀 |
| [输入法卡死修复](docs/findwindow-fix.zh-CN.md) <!-- catalog: `findwindow_fix` --> | 防止讯飞等输入法让整合包客户端卡死或崩溃 |
| [启动崩溃修复](docs/startup-crash-fix.zh-CN.md) <!-- catalog: `startup_crash_fix` --> | 修复开游戏时读取界面随机闪退 |
| [房间窗口初始化保护](docs/waitroom-window-fix.zh-CN.md) <!-- catalog: `waitroom_window_fix` --> | 修复建房成功后客户端偶尔直接退出 |
| [原生数据与生命周期修复](docs/native-data-guard.zh-CN.md) <!-- catalog: `native_data_guard` --> | 修复机体数据缺失或机体管理页等待数据时的闪退，并让闪退日志记录正确现场 |
| [机体列表三维预览加载保护](docs/hangar-preview-guard.zh-CN.md) <!-- catalog: `hangar_preview_guard` --> | 模型或图形缓冲加载失败时，退回原生二维缩略图 |
| [特殊攻击目标选择保护](docs/special-target-guard.zh-CN.md) <!-- catalog: `special_target_guard` --> | 随机目标数量非法时，使用已有原生失败结果阻止越界读取 |
| [场景登记生命周期修复](docs/scene-lifecycle-fix.zh-CN.md) <!-- catalog: `scene_lifecycle_fix` --> | 修复场景对象切换区域后偶发的清理闪退 |
| [多人加载同步修复](docs/loading-sync-fix.zh-CN.md) <!-- catalog: `loading_sync_fix` --> | 修复多人开局时偶发卡在加载画面（BattleServer） |
| [异常断线登录恢复](docs/login-recovery.zh-CN.md) <!-- catalog: `login_recovery`, `login_recovery_client` --> | 异常断线后重新登录被拒或卡在等待时，清理旧会话并自动有限重试 |
| [好友列表修复](docs/friend-list-fix.zh-CN.md) <!-- catalog: `friend_list_fix`、`friend_list_client_fix` --> | 保留离线好友、当前名字和军衔，恢复主页面 USER LIST 的中文菜单及原生聊天界面 |
| [阿普萨拉斯 III 变形卡死修复](docs/transform-input-fix.zh-CN.md) <!-- catalog: `transform_input_fix` --> | 缺少变形动画时恢复原生状态退出，解除移动与攻击锁定 |
| [军衔回退修复](docs/rank-progression-fix.zh-CN.md) <!-- catalog: `rank_progression_fix` --> | 防止战斗结算降低已有军衔（WorldServer） |
| [首次登录大厅音乐](docs/lobby-music-fix.zh-CN.md) <!-- catalog: `lobby_music_fix` --> | 恢复首次登录时的大厅音乐 |
| [活动扭蛋币种显示](docs/capsule-currency-fix.zh-CN.md) <!-- catalog: `capsule_currency_fix` --> | 9301 号机在列表和购买确认框中显示为 99 代币 |
| [商城租赁与中文选项修复](docs/shop-labels.zh-CN.md) <!-- catalog: `shop_labels` --> | 显示租赁/永久、实际数量和中文选项名 |
| [商城图片文字汉化](docs/shop-art-localization.zh-CN.md) <!-- catalog: `shop_art_localization` --> | 汉化受支持涂料图片内的军衔文字 |
| [Agent 中继日志修复](docs/agent-relay-log-fix.zh-CN.md) <!-- catalog: `agent_relay_log_fix` --> | 修复 Agent 中继日志读取无效内存 |
| [Agent 房间视图错误转发修复](docs/agent-room-view-error-fix.zh-CN.md) <!-- catalog: `agent_room_view_error_fix` --> | 修复房间操作时弹出与实际情况无关的错误（AgentServer） |

## 开发诊断

供开发者分析协议、定位问题和执行原生自动化测试。

| 功能 | 简介 |
|---|---|
| [发包日志](docs/net-probe.md) <!-- catalog: `net_probe` --> | 记录客户端发出的封包，用于分析协议 |
| [扭蛋价格探针（Experimental）](docs/capsule-price-probe.md) <!-- catalog: `capsule_price_probe` --> | 只读记录扭蛋列表价格标签的刷新 |
| [GameTest（Experimental）](docs/game-test.zh-CN.md) <!-- catalog: `game_test_client`、`game_test_server` --> | 原生登录、入房、Ready、单客户端与多客户端协同对战，以及页面检查 |

## 项目文档

[构建与开发接口](docs/framework.md) · [贡献与 AI 披露规则](docs/contributing.zh-CN.md) · [配置与排错](docs/setup.zh-CN.md)

MIT，见 [LICENSE](LICENSE)。第三方许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

框架、SQL、服主内容和个人数据的归属见[内容边界](docs/content-boundaries.zh-CN.md)。
