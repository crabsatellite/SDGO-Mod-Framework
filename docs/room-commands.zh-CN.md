# 房名组合指令

[English](room-commands.md)

core 提供统一的 `FindRoomCommand` 公共接口；`room_random`、`pvp_ai`、`coop_ai` 使用同一套解析规则。
各 Mod 分别读取自己的命令与参数，不消费、不删除、不改写房名。后续 Mod 可复用这个接口。

| 房名 | 效果 |
|---|---|
| `!rand !ai` | 真人每局从各自机库随机；AI 按该模式默认规则补位 |
| `!rand !ai3` | 真人随机；PvP 请求 3 个敌方 AI，合作任务请求 3 个僚机，人数仍受空位限制 |
| `!ai3 inf !rand` | 任务中请求 3 个无限复活的 AI 僚机，真人每局随机 |
| `!ai3 player` | 任务中请求 3 个僚机，按原生玩家生命额度复活（默认） |
| `!ai3 lv4 !rand` | 同上，AI 使用 4 级机体池 |
| `!rand !ai e2 a1 lv4` | PvP 请求 2 个敌人、1 个队友，AI 使用 4 级池；真人每局随机 |
| `!ai e2 a1 lv4 !rand` | 与上一行相同，指令顺序可交换 |
| `!ai enemy !rand` | 保留真人分队，本方不补 AI、对面填满剩余空位；真人每局随机机体 |
| `!ai fill !rand` | 双方按当前空位补 AI；真人每局随机机体 |
| `!ai coop !rand` | 真人开局自动同队，本方不补 AI，对面补满剩余名额；真人每局随机机体 |
| `!ai coop fill` | 真人自动同队，双方补位；三名真人在 8 人房得到 4v4 |
| `!ai coop e4 a0` | 真人自动同队，只加 4 个敌人；三名真人时为 3v4 |
| `!ai e5 a0 !rand` | 精确增加 5 个敌方 AI，不增加 AI 队友；与各自机库随机组合 |
| `!rand !ai0` | PvP 只随机真人，不添加 AI；合作任务的 `!ai0` 沿用原有的默认补位含义 |

命令之间以空格分开，英文大小写不敏感。`lv4`、`e2`、`a1` 要放在 `!ai` 后、下一个 `!指令` 前。
例如 `!ai !rand lv4` 中的 `lv4` 属于 `!rand`，不会改变 AI 等级；应写成 `!ai lv4 !rand`。
未知的 `!指令` 同样划分参数范围，避免以后增加 Mod 时发生参数串用。
同名命令重复时使用第一个有效命令。`!aim`、`房间!ai3`、`!ai3x`、`!rand!ai3` 不会当作 `!ai`。

任务的 `inf`、`player` 要写在 `!ai` 参数段内。真人等待复活时 AI 照常复活；全部真人生命耗尽立即任务失败，存活 AI 不能维持任务。详见[任务 AI 僚机](coop-wingmen.zh-CN.md)。

PvP 中敌友相对开局时房主最终所在阵营。真人规则与 AI 补位独立：默认或显式 `versus` 保留原生手选／随机；
`coop` 在开局时让所有真人跟房主当前选边同队，并优先于原生随机设置。AI 在真人分队完成后补入。
`eN` / `aN` 是额外 AI 数量，`e*` / `a*` 是自动填空位。当前全房总量上限为 8，数量无法完整满足则取消开局。
朋友一起打电脑直接用 `!ai coop`，本方不补 AI、对面填满全房剩余名额，支持 1v7、3v5 和 4v4；
`coop fill` 则双方补位，`coop e4 a0` 固定四个敌人。`!rand` 只随机机体，不控制队伍。
`coop` 与 `versus` 不能同时使用；省略 AI 补位参数时，前者默认只补对面，后者默认双方补位。
`!ai`、`!ai fill`、`!ai e* a*` 是同一种双方补位策略；`!ai enemy` 等同于 `!ai e* a0`。
不带 `*` 的数量规则只加指定数量，未写的敌方或队友数量为 0。两种命名策略不能与数量参数混用。

配齐[战斗 AI](battle-ai.zh-CN.md)后，对战的 `!ai` 参数段还支持 `easy`、`medium`、`hard`，
例如 `!ai3 hard`，默认 Medium。`lv1`–`lv5` 仍选择 NPC 池；实际机体等级 Medium 为 Lv12，
Hard 为 EX13。`!ai !rand hard` 中的 `hard` 不会改变 AI 难度。

原生房名最多 **20 字节**。可选的[长房名 Mod](room-titles.zh-CN.md)通过配套客户端、Agent 和 Battle 扩展到 128 个 Unicode 标量 / 512 字节 UTF-8；更新后的三个规则 Mod 读取完整标题。未启用扩展时，组合仍受原生限制。上表两个完整 PvP 组合各为 19 个 ASCII 字节。
未启用扩展时，加入中文房名仍需留意原生编码与字节数，使用短命令 `!rand` 可节省空间。

## 配合长房名使用

| 场景 | 完整房名示例 | 规则 |
|---|---|---|
| 任务 | `周末朋友任务房，慢慢打一起练习 !ai3 lv4 !rand` | 请求 3 个 4 级僚机，真人每局随机 |
| 对战 | `朋友练习局，慢慢打欢迎新手 !ai e2 a1 lv4 !rand` | 2 个敌人、1 个队友，4 级池，真人每局随机 |
| Hard 对战 | `朋友高手练习局，一起来挑战 !ai e2 a1 lv4 hard !rand` | 同样的人数和池，使用 Hard 战斗 AI |

三端房名 Mod 默认关闭：在 Manager 中为客户端启用 `room_titles`，为所有相关 Agent 启用 `room_titles_agent`，
为所有相关 Battle 启用 `room_titles_server`；再更新已使用的 `coop_ai`、`pvp_ai`、`room_random`。
使用战斗 AI 时，同时更新 `battle_ai`、`match_ai`、`battle_ai_server`。
玩家正常启动游戏。房主照常设置房名，等服务端确认后再开始；失败时文字保留，可修改重试。
长房名不会自动安装 AI、扩充空位、解锁库存或创建新的指令，仍按对应玩法 Mod 的设置执行。
仅选择 `room-titles` 功能包不包含 AI / 随机机体 Mod；使用完整框架包或同时选入所需玩法。
安装与全文查看入口见[长房名说明](room-titles.zh-CN.md)。

随机 Mod 只处理真人；AI 的机体池、数量、队伍与复活继续由 AI Mod 控制。三个服务端 Mod 使用不同的
Hook 地址，core 仍执行每个 Hook 只能有一个所有者的规则。AI 所需的客户端 Mod 继续按原有安装说明配置。

## 开发接口

公共 C ABI 位于 `include/sdgo/sdgo_api.h`。`SdgoRoomCommand` 返回原房名中的命令和参数字节范围，
可选数字后缀通过 `SDGO_ROOM_COMMAND_ALLOW_COUNT` 启用，例如 `!ai3`。解析有界、无状态、可并发调用。
API 1 的可选字段通过 `api->size` 检查，不改变既有字段的布局。
已有每日奖励 Mod 使用的 `HookSite` 扩展保持原位置，房名接口位于其后，因此每日奖励 DLL 无需重编译。

建议通过 `include/sdgo/room_commands.h` 调用：

```cpp
SdgoRoomCommand command{};
if (sdgo::room_commands::Find(api, title, titleLength, "ai",
                            SDGO_ROOM_COMMAND_ALLOW_COUNT, &command)) {
    // command.arguments_offset / arguments_length only cover this command.
}
```

有新 core 时使用公共服务；旧 core 缺少该可选字段时，SDK 使用完全相同的公共解析实现。
现有 Mod 需要重新构建才能采用新规则。配套安装包同时提供新 core、随机 Mod，以及已安装 AI Mod 的 DLL 更新，
保留 AI 的 `mod.ini`、`config.ini` 与服主设置；不会自动启用或新增尚未安装的 AI Mod。
若宿主已装有可选的 `ai_catalog`，更新后的 AI Mod 继续读取该进程内提供的机体池；不安装或修改其数据。
未安装此提供者时，继续使用 AI Mod 原有等级池。
