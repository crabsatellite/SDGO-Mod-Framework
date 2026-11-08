# SDGO Mod 框架

为 SD敢达 Online 客户端和私服提供基础修复与可选扩展。
非官方爱好者项目，不包含游戏文件。[English](README.md)

游戏版本基于 B 站 UP 主屮仔丿发布的 **SD敢达 Ultimate Repack** 整合包（[视频 BV1Qa4y1z7eW](https://www.bilibili.com/video/BV1Qa4y1z7eW/)）的客户端和服务端，
框架按这套整合包的程序校验和测试；支持的程序版本见[安装说明](docs/setup.zh-CN.md#支持的游戏版本)。本项目不提供该整合包。

[安装说明](docs/setup.zh-CN.md) · [统一 Mod Manager](docs/mod-manager.zh-CN.md) · [最新发布](https://github.com/crabsatellite/SDGO-Mod-Framework/releases/latest)

服主与维护人员通过 **SDGO Mod Manager** 安装、配置和[更新](docs/mod-manager.zh-CN.md#更新)功能，
客户端目录顶部的「0_更新与管理」可直接打开它。玩家取得兼容原版客户端后，可在 Manager 一键安装服主配置档：按锁定清单组合官方 Mod、服主共享配置和自定义素材，统一校验与回滚。后续内容发布沿用已有程序锁，更换程序需先明确采用。Git、HTTPS 和离线目录使用同一内容契约；完整客户端仓库仅是服主可选备份。
服主自己的「仅本地」扩展也可以放进 Manager 管理，它们不会被打包或导出分发。
绑定服主配置的玩家双击 `GOnline.exe` 时，Manager 会显示更新进度，同步模组程序与服主内容，完整校验后再启动游戏；同时点击会共用一次待启动。详见[启动前更新](docs/owner-updates.zh-CN.md#启动前更新)。
默认跟随我们提供的基础整合包。服主需要自行维护时，明确选择「服主 DIY」接管；接管后由服主维护配置、脚本、素材、兼容性与发布版本。
[服主整合包更新](docs/owner-updates.zh-CN.md)支持审阅并采用上游组件、发布服主确认的版本，玩家只跟随该服主的来源。
Manager 会检查模块依赖、内容退役和数据变更影响，区分配置保存与运行生效；完整来源的程序、资源和 Mod 统一更新，保留个人配置与已有内容。详见[更新与维护保护](docs/mod-manager.zh-CN.md#依赖数据与生效状态)。
本页仅列出功能概览。具体配置、玩法操作、兼容性与排错方法见各功能文档。

功能名后的 Beta、Experimental 表示发布阶段，未标注的为 Stable：

- **Stable**：基本功能可以正常使用，之后的问题通过新版本修复。Manager 更新默认只安装 Stable。
- **Beta**：基本流程仍在开发。当前 Beta 功能包括机体族导入与 OC 六维强化；在 Manager 更新中切换到 Beta 通道后安装。
- **Experimental**：开发诊断工具。开发诊断类功能不随 Manager 更新安装。

## 基础修复

<!-- mod-category:fixes -->

| 功能 | 简介 | 文档 |
|---|---|---|
| 中文输入法候选与提交修复 <!-- catalog: `ime_fix` --> | 恢复候选窗请求，按旧拼音范围提交中文，保留英文和数字前后缀 | [使用说明](docs/ime-fix.zh-CN.md) |
| 活动扭蛋币种显示 <!-- catalog: `capsule_currency_fix` --> | 9301 号机在列表和购买确认框中显示为 99 代币 | [使用说明](docs/capsule-currency-fix.zh-CN.md) |
| 输入法卡死修复 <!-- catalog: `findwindow_fix` --> | 防止讯飞等输入法让整合包客户端卡死或崩溃 | [使用说明](docs/findwindow-fix.zh-CN.md) |
| 启动崩溃修复 <!-- catalog: `startup_crash_fix` --> | 修复读取界面随机闪退，可选展示缩放不可用时仍保留防护 | [使用说明](docs/startup-crash-fix.zh-CN.md) |
| 房间窗口初始化保护 <!-- catalog: `waitroom_window_fix` --> | 修复建房成功后客户端偶尔直接退出 | [使用说明](docs/waitroom-window-fix.zh-CN.md) |
| 原生数据与生命周期修复 <!-- catalog: `native_data_guard` --> | 保护原生数据和延迟消息；初始库存完成后再切换页面，并保留真实崩溃现场 | [使用说明](docs/native-data-guard.zh-CN.md) |
| 场景登记生命周期修复 <!-- catalog: `scene_lifecycle_fix` --> | 修复场景对象切换区域后偶发的清理闪退 | [使用说明](docs/scene-lifecycle-fix.zh-CN.md) |
| 多人加载同步修复 <!-- catalog: `loading_sync_fix` --> | 修复多人开局时偶发卡在加载画面（BattleServer） | [使用说明](docs/loading-sync-fix.zh-CN.md) |
| 首次登录大厅音乐 <!-- catalog: `lobby_music_fix` --> | 恢复首次登录时的大厅音乐 | [使用说明](docs/lobby-music-fix.zh-CN.md) |
| 异常断线登录恢复 <!-- catalog: `login_recovery`, `login_recovery_client` --> | WorldServer 清理旧会话，客户端等待超时有限重连、清理请求锁，结束后恢复原生登录输入 | [使用说明](docs/login-recovery.zh-CN.md) |
| 阿普萨拉斯 III 变形卡死修复 <!-- catalog: `transform_input_fix` --> | 缺少变形动画时恢复原生状态退出，解除移动与攻击锁定 | [使用说明](docs/transform-input-fix.zh-CN.md) |
| Agent 中继日志修复 <!-- catalog: `agent_relay_log_fix` --> | 修复 Agent 中继日志读取无效内存 | [使用说明](docs/agent-relay-log-fix.zh-CN.md) |
| Agent 房间视图错误转发修复 <!-- catalog: `agent_room_view_error_fix` --> | 保留完整错误回复，避免客户端越界读取错误码并弹出数据读取失败 | [使用说明](docs/agent-room-view-error-fix.zh-CN.md) |
| 军衔回退修复 <!-- catalog: `rank_progression_fix` --> | 防止战斗结算降低已有军衔（WorldServer） | [使用说明](docs/rank-progression-fix.zh-CN.md) |
| 好友列表修复 <!-- catalog: `friend_list_fix`、`friend_list_client_fix` --> | 保留离线好友、当前名字和军衔，恢复主页面 USER LIST 的中文菜单及原生聊天界面 | [使用说明](docs/friend-list-fix.zh-CN.md) |
| 新账号起始资料修复 <!-- catalog: `new_account_profile` --> | 可配置的初始 GP、点券、军衔和赠送机体，替代原服的 9,999,999 / 15 级 | [使用说明](docs/new-account-profile.zh-CN.md) |
| 商城租赁与中文选项修复 <!-- catalog: `shop_labels` --> | 显示租赁/永久、实际数量和中文选项名 | [使用说明](docs/shop-labels.zh-CN.md) |
| 商城图片文字汉化 <!-- catalog: `shop_art_localization` --> | 汉化受支持涂料图片内的军衔文字 | [使用说明](docs/shop-art-localization.zh-CN.md) |

<!-- /mod-category:fixes -->

## 玩法扩展

<!-- mod-category:gameplay -->

| 功能 | 简介 | 文档 |
|---|---|---|
| 公告与展示规则 <!-- catalog: `announcements` --> | 登录时显示一次资讯简报；可选在游戏内按时段和事件展示服主公告 | [使用说明](docs/announcements.zh-CN.md) |
| 任务 AI 僚机 <!-- catalog: `coop_ai`、`mission_ai` --> | 任务房名写 `!ai`，AI 按玩家生命额度复活；`!ai3 inf` 开启 AI 无限复活 | [使用说明](docs/coop-wingmen.zh-CN.md) |
| 任务行为版本 <!-- catalog: `mission_behavior`、`mission_behavior_server` --> | 为一个客户端和服务端组全部 Battle 频道统一选择 OC 默认行为或 G改现有行为 | [使用说明](docs/mission-behavior.zh-CN.md) |
| 对战 AI <!-- catalog: `pvp_ai`、`match_ai` --> | 房名写 `!ai` 加入 AI 对手和队友，同步开局、变形和战绩；可让真人同队并选择补位方式与数量 | [使用说明](docs/pvp-ai.zh-CN.md) |
| AI 机体目录（可选） <!-- catalog: `ai_catalog` --> | 把服务器可出击的机体加入 AI 机体池 | [使用说明](docs/ai-catalog.zh-CN.md) |
| AI 机师互动台词（可选） <!-- catalog: `ai_dialogue` --> | 按战况与机体人设生成回应，核验敌友关系后给队友建议；自填 DeepSeek 密钥，默认关闭，广播固定点名对象 | [使用说明](docs/ai-dialogue.zh-CN.md) |
| 战斗贡献奖励 <!-- catalog: `battle_rewards` --> | 按贡献发放 GB/MB，可配置人数倍率与按账号去重的聚会奖励 | [使用说明](docs/battle-rewards.zh-CN.md) |
| 收藏扭蛋 <!-- catalog: `collection_capsule` --> | 优先补图鉴的活动机台，安装时补充图鉴获取说明；价格和次数可配置，默认每周 15 次 | [使用说明](docs/collection-capsule.zh-CN.md) |
| 每局随机机体 <!-- catalog: `room_random`、`room_random_client` --> | 房名写 `!rand`，每局从各自机库随机出击机体或卡组，原生出击任务收束后开局 | [使用说明](docs/room-random.zh-CN.md) |
| 完整对战 All Map <!-- catalog: `all_maps` --> | 房间选择 All Map 时从完整原生对战地图目录抽取，任务专用图仍留在任务模式 | [使用说明](docs/all-maps.zh-CN.md) |
| 登录机体展示 <!-- catalog: `login_showcase` --> | 按等级、全部机体、最近注册或指定名单选择登录展示，默认 SS/SR/SU | [使用](docs/login-showcase.zh-CN.md) |
| FPS 上限 <!-- catalog: `fps_limit` --> | 设置渲染上限，战斗逻辑追赶时仍刷新画面，保留原生游戏速度 | [使用说明](docs/fps-limit.zh-CN.md) |
| 长房名 <!-- catalog: `room_titles`、`room_titles_agent`、`room_titles_server` --> | 普通、AI、队伍及任务房的建房、设置和改名入口支持最长 128 字 | [使用说明](docs/room-titles.zh-CN.md) |
| 战斗 AI 难度 <!-- catalog: `battle_ai`、`battle_ai_server` --> | Easy / Medium / Hard 三档、按实际攻击距离选择武装和形态、共享敌情和地图推进装置寻路 | [使用说明](docs/battle-ai.zh-CN.md) |
| 自动机库容量 <!-- catalog: `hangar_capacity` --> | 登录时把每个账号的机库保障到至少 1200 格（WorldServer） | [使用说明](docs/hangar-capacity.zh-CN.md) |
| 机库管理 <!-- catalog: `hangar_manager` --> | 整理和兑换重复机体，支持导入机体；拦截跨账号删除并保留删除记录 | [使用说明](docs/hangar-manager.zh-CN.md) |
| 副官补给、资源与能力 <!-- catalog: `operator_catalog`、`operator_catalog_server` --> | 五位免费补给副官，以及独立资源包、数值能力、掉落和商城草稿管理 | [使用说明](docs/operator-assistant.zh-CN.md) |
| 扭蛋机编辑器 <!-- catalog: `capsule_catalog` --> | 在 Manager 中新增、修改、下架扭蛋机，设置奖池、价格和权重，为未使用的独立活动机改号 | [使用说明](docs/capsule-catalog.zh-CN.md) |
| 每日双倍时间 <!-- catalog: `daily_rewards` --> | 每天固定时段（默认 18:00–22:00）战斗 GB 和经验翻倍 | [使用说明](docs/daily-rewards.zh-CN.md) |
| 保管库一键领取 <!-- catalog: `storage_claim` --> | 保管库页内一键领取当前页签的全部物品或礼物 | [使用说明](docs/storage-claim.zh-CN.md) |
| 商城汉化与中文检索 <!-- catalog: `shop_localization` --> | 汉化商品名称、说明、选项和涂装机体预览，可用中文名称搜索并组合等级、币种筛选 | [使用说明](docs/shop-localization.zh-CN.md) |
| 好友机体赠送（可选） <!-- catalog: `friend_unit_gift_client`、`friend_unit_gift_server` --> | 右键好友（在线或离线均可），在机体管理中选机赠送，对方在保管库接受或退还；默认关闭，由服主开启 | [使用说明](docs/friend-unit-gift.zh-CN.md) |
| OC 六维强化（Beta） <!-- catalog: `oc_custom_client`、`oc_custom_server`、`oc_custom_battle` --> | 双系统工厂：原生标签保持原样，旁边的「终极强化」标签由客户端包内原有的 OC 布局组成（OVER CUSTOM 等级栏、显示道具 3D 模型的 ITEM 栏、在加点六边形上画出全部六个维度、普通点与 Over 点、OVER CUSTOM 终极强化与洗点），战斗中生效 OC 推进力与机动力；与现行改造部件数据并存，切换后两套数据都保留，由服主选择战斗使用哪一套 | [使用说明](docs/over-custom.zh-CN.md) |
| 机体族导入（Beta） <!-- catalog: `unit_catalog`, `unit_catalog_server` --> | 在 Mod Manager 导入机体包、编辑技能／浮游依赖字段、保存脚本声明，制作设计图和活动奖池，并可导入任何来源按协议做好的整套阵容（单形态、互相变形、单向变形、自变形，以及第三形态的入口形态）（之后的阵容会连同已安装阵容的脚本和技能图标一起承接，单个目录最多 8192 条记录），再由 Mod Manager 应用到服务端，并交给商城、扭蛋机和独立活动机选用（活动海报可随时更换） | [使用说明](docs/unit-import.zh-CN.md) |

<!-- /mod-category:gameplay -->

服主还可以使用独立的 Manager 模块[节日与生日赠礼](docs/calendar-gifts.zh-CN.md)：在 14 个常见节日、玩家生日或指定日期把机体和道具送进每位符合条件玩家的保管库。

<a id="long-room-title-commands"></a>

补充参考：[房名指令文档](docs/room-commands.zh-CN.md)。

## 联机与服主工具

<!-- mod-category:network -->

| 功能 | 简介 | 文档 |
|---|---|---|
| FRP 联机 <!-- catalog: `net_route`、`agent_udp_pool` --> | 通过服主的 Sakura FRP 等 TCP/UDP 转发连接服务器 | [使用说明](docs/frp-distribution.zh-CN.md) |
| 商城商品管理 <!-- catalog: `shop_catalog` --> | 在 Manager 中上架、改价、下架服主自己的商城商品（WorldServer） | [使用说明](docs/shop-catalog.zh-CN.md) |

<!-- /mod-category:network -->

## 开发诊断

可选的开发与诊断功能。

<!-- mod-category:development -->

| 功能 | 简介 | 文档 |
|---|---|---|
| 发包日志 <!-- catalog: `net_probe` --> | 记录客户端发出的封包，用于分析协议 | [使用说明](docs/net-probe.md) |
| 扭蛋价格探针（Experimental） <!-- catalog: `capsule_price_probe` --> | 只读记录扭蛋列表价格标签的刷新 | [使用说明](docs/capsule-price-probe.md) |
| GameTest（Experimental） <!-- catalog: `game_test_client`、`game_test_server` --> | 原生登录、入房、Ready、单客户端与多客户端协同对战，以及页面检查 | [使用说明](docs/game-test.zh-CN.md) |

<!-- /mod-category:development -->

## 项目文档

[构建与开发接口](docs/framework.md) · [贡献与 AI 披露规则](docs/contributing.zh-CN.md) · [配置与排错](docs/setup.zh-CN.md)

MIT，见 [LICENSE](LICENSE)。第三方许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

框架、SQL、服主内容和个人数据的归属见[内容边界](docs/content-boundaries.zh-CN.md)。
