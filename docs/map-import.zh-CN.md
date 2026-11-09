# 地图与任务导入

[English](map-import.md)

把在游戏外制作、按**地图包协议**准备好的地图和任务登记到游戏里：新的地形、物件、出生点、天空、小地图和音乐，以及带阶段目标、刷怪、人物对话、开场/过场/终场动画和横幅特效的 PvE 任务。导入后的地图出现在对战房间的地图列表中，任务出现在任务房间的任务列表中，与原有地图、任务并存。

这个功能只负责“导入”：它读取符合协议的数据，生成游戏需要的记录和资源，再安装到客户端与服务端。它不拆解、不导出、也不修改游戏原有的地图和任务。

## 组成

| 部分 | 作用 |
| --- | --- |
| 客户端 Mod `map_catalog` | 启动时登记导入的地图行和任务记录，提供地形/模型/贴图资源，加载任务脚本（过场、阶段事件），并为导入任务启用人物对话框 |
| Manager「地图与任务导入」 | 登记地图包、修改名称/模式/服务器、编译、安装客户端包、检查并写入服务端 |
| 地图包协议 | `map.json` 描述地图；`missions/*.json` 描述任务。格式见下文 |

## 在 Manager 中使用

1. 在模块设置里填写**地图工作区目录**（可留空，使用 Manager 数据目录）、**配套客户端目录**（编译基准与安装目标）和 **WorldServer INI**。
2. **登记地图包**：填写包含 `map.json` 的目录。登记时会校验全部文件摘要并把地图包复制进工作区；同一 `map_key` 重新登记会替换文件，保留之前的修改。
3. 在地图卡片上调整：中文/英文名称、开放的对战模式、开放的服务器类型、要发布的任务、是否发布、排列顺序。点「保存」。
4. 点「**编译全部启用的地图**」。所有启用的地图和任务被编译成一个客户端包和一个服务端计划。
5. 关闭客户端后点「**安装客户端包**」。也可以「下载客户端包」发给其他玩家（服主提供的客户端包或更新）。
6. 点「**检查服务端**」查看将新增、更新的行和冲突；没有冲突时点「**应用到服务端**」，再「读回服务端」确认。
7. 重启 World/Battle 服务和客户端后生效。

编号在编译时自动分配：地图编号接在客户端原有地图之后，任务的各类记录接在原有任务记录之后，并且彼此连续（游戏按连续编号读取列表，中间不能留空）。因此移除或调整一张地图的顺序，会让排在后面的地图和任务重新编号；重新编译、安装并应用即可，服务端会更新并清理本工作区写入过的旧行。

服务端写入在一个事务里完成。已存在、但不是由地图导入写入的同键行会让整个应用被拒绝，不会覆盖原有数据。

## 地图包协议（`map.json`）

地图包是一个文件夹：一个 `map.json`，以及它引用的 glTF 2.0 模型（`.glb`/`.gltf`）、PNG 图片和可选的 MP3。所有文件都必须在 `files` 中登记 SHA-256。

**坐标系**与游戏世界相同：X 向东，Y 向上，Z 向北，原点在地形中心，单位为游戏单位。高度图和遮罩图覆盖整块地形，第 0 行是北边、第 0 列是西边。glTF 资源保持自身约定（右手系，-Z 朝前），编译时会转换。`rotation_y` 为俯视顺时针角度：0 朝北，90 朝东。

| 字段 | 说明 |
| --- | --- |
| `schema`、`schema_version` | 固定为 `sdgo.map-package`、`1` |
| `map_key` | 地图唯一标识，3..64 个小写字母、数字、`.`、`_`、`-` |
| `name`、`description` | `{"zh": …, "en": …}`；名称最多 48 个字符 |
| `modes` | 对战模式：`normal`、`deathmatch`、`grid`、`tag`、`captain`、`ai`、`ai_battleship`（可为空，只用作任务地图） |
| `servers` | 服务器类型：`training`、`beginner`、`expert`、`clan` |
| `terrain.half_extent` | 地形半宽：4096、8192 或 16384 |
| `terrain.height` | `image`（257×257 灰度 PNG）、`min`、`max` |
| `terrain.layers` | 1..8 层地表：`texture`（PNG）、`mask`（第一层可省略）、`tiling` |
| `terrain.lighting` | `sun_direction`（y 必须小于 0）、`sun_color`、`ambient`、`bake_shadows`（烘焙光照与阴影） |
| `terrain.fog`、`terrain.sky` | 雾的颜色与起止距离；天空 `zenith`/`horizon`/`ground` 颜色 |
| `terrain.play_area` | 可活动范围 `half_x`、`half_z` 和高度上限 `ceiling` |
| `objects` | 最多 1024 个物件：`model`（包内 glTF）或 `native_model`（目标客户端已有的模型），`position`、`rotation_y`、`scale`、`collision`（`auto`、`none` 或碰撞盒列表）、可选 `id`、`only`（`pvp`/`mission`）、`initially_hidden` |
| `spawns` | `team_a`、`team_b` 各 6 个，`shared` 1..12 个，`grid` 模式另需 `grid_respawn_a`/`grid_respawn_b`；每个点 `position` 与 `facing` |
| `sectors` | 任务分区出生点（`{name: {spawns: [...]}}`），供任务切换区域使用 |
| `item_patterns` | 道具刷新组，`red` 与 `blue` 组数相同 |
| `presentation` | `preview`（地图预览 PNG）、`minimap`（小地图 PNG）、`bgm`（包内 MP3 或 `{"native": "BGM\\NNNN.mp3"}`） |
| `files` | 包内每个文件的 SHA-256 |

## 任务协议（`missions/*.json`）

任务文件放在地图包的 `missions/` 下并在 `files` 中登记。一个任务对应任务列表中的一项，可以有 1..4 个难度。

| 字段 | 说明 |
| --- | --- |
| `schema`、`schema_version` | 固定为 `sdgo.map-mission`、`1` |
| `mission_key`、`name`、`description` | 任务标识、名称和说明 |
| `briefing.zh` | 任务说明面板，最多 5 行、每行 49 字 |
| `difficulties` | 每档：`level`、`required_rank`、`time_limit`（秒）、`lives` |
| `bgm` | 任务音乐（与地图 `presentation.bgm` 写法相同） |
| `npcs` | 登场单位：`template`（目标服务器已有的 NPC），或 `unit` + `base`（用任意机体套用一个 NPC 模板）；可改 `hp`、`boss`、`name`、`ai`（AI 脚本名，例如对战 AI 的 `VSAI_Infighter_01`） |
| `lines` | 人物对话：`portrait`（客户端已有头像 `sprs\\NNNN.spr`）、`speaker`、`text` |
| `banners` | 横幅：`image`（512×128 PNG），在过场中以特效显示 |
| `sectors` | 区域切换：`warp`（1..8 个传送点）、`facing`、可选 `rect`（活动范围） |
| `cutscenes` | 过场动画，见下表 |
| `phases` | 1..8 个阶段，按顺序推进，见下表 |
| `opening`、`ending` | 开场、通关时播放的过场名 |

**阶段**（`phases[]`）：

| 字段 | 说明 |
| --- | --- |
| `key`、`objective` | 阶段标识；目标说明显示在任务目标栏 |
| `goal` | `kill`：击破指定单位若干台（可多条同时满足）；或 `survive`：坚持若干秒 |
| `fail` | `time_limit`：限时；`lose`：指定友方单位被击破即失败 |
| `spawns` | 刷怪：`npc`、`positions`、`at`（阶段开始后秒数）、`every`（间隔）、`side`（`enemy`/`ally`）、`min_free` |
| `slots` | 同屏单位上限 `ally` + `enemy` ≤ 10 |
| `enter` | 进入本阶段时：`cutscene`（过场）、`lines`（对话）、`sector`（切换区域）；第一阶段请使用 `opening` |
| `reward` | 阶段奖励 `points`、`unit_exp` |
| `next` | 下一阶段（默认按顺序） |

**过场**（`cutscenes{}`）：`length`（1..60 秒），`actors`（演员：`npc`、`at`、`facing`），`camera`（从 `t=0` 开始的关键帧 `pos`/`look`，编译成平滑曲线），以及 `events`：

| 事件 | 说明 |
| --- | --- |
| `anim` + `actor` | 播放机体动作（编译时检查该机体模型确有此动作），可设 `loop` |
| `move_to` + `actor` | 演员移动到指定点 |
| `banner` | 在镜头前显示横幅，可设 `distance` 或 `at` |
| `line` | 显示人物对话 |
| `effect` | 播放客户端已有特效 `{fx, at, scale}` |
| `sound` | 播放客户端已有音效 |
| `shake` | 镜头震动 `[幅度, 秒]` |

过场期间玩家不可操作，结束后自动交还控制。

编译时会检查摆放与镜头：出生点、刷怪点、过场演员和传送点不能位于物件内部；过场镜头不能低于地面或位于物件内部，看向目标的视线也不能被地形或物件挡住（客户端已有模型按它自带的碰撞体计算，门框、拱门等镂空结构保持通透）。报错会给出过场名、时间和镜头坐标。

## 生效与兼容

- 需要启用客户端 Mod `map_catalog`，并在客户端和服务端都安装同一版本的发布物；只更新一端会出现地图或任务缺失。
- 服务端表在服务启动时读取，应用后需重启 World/Battle 服务；客户端包安装后重启客户端。
- 导入任务在任务房间中以原生任务模式运行，人物对话使用游戏自带的人物对话框（带头像和名字）。
- 引用的原生模型、头像、特效、音效和 BGM 必须在目标客户端中存在；编译时会检查。

## 排错

- 客户端日志 `sdgo/logs/sdgo.log` 中，`map_catalog: native map rows registered` 与 `native mission records registered` 表示登记成功；`imported resource loaded: trns\…` 表示战斗确实使用了导入的地形；`imported mission script call: …` 与 `imported cutscene finished: …` 记录任务阶段事件和过场的执行。
- 任务开不起来或单位缺失时，在 Mod 设置中打开「写入游戏原生调试日志」，客户端 `app_log` 会记录找不到的记录编号。
- 编译报错会指出具体字段（例如刷怪点低于地面、引用了未定义的台词、演员模型没有该动作）。
- 「检查服务端」报告冲突时，说明目标数据库中已有同编号的非导入数据，应用会被拒绝；请确认客户端与服务端来自同一套原始数据。
