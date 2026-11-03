# 每局随机机体（Beta）

[English](room-random.md) · [返回功能目录](../README.zh-CN.md)

房名里加一个独立的 `!rand` 或 `!random`，房间里的真人玩家在每次开局时都会从**各自机库**重新分配本局机体，
无需退房或重新建房。命令不区分大小写，例如 `!rand`、`朋友局 !rand`、`!rand !ai3`。

| mod | 运行在 | 负责 |
|---|---|---|
| `room_random` | 对战服务器 | 开局时为每位真人抽取本局机体或卡组 |
| `room_random_client` | 客户端 | 把本局结果同步到客户端的开局数据 |

两部分都需要：对战服务器装 `room_random`，所有参战客户端装 `room_random_client`。

## 房名指令

可与 AI 组合，例如 `!ai !rand`、`!ai3 lv4 !rand`、`!rand !ai e2 a1 lv4`。命令之间用空格分开，AI 参数紧跟 `!ai`，
放在下一个 `!指令` 之前；完整语法见[房名组合指令](room-commands.zh-CN.md)。AI 继续使用 AI Mod 自己的机体池。

原生房名上限为 20 字节。启用[长房名三端配套](room-titles.zh-CN.md)并更新 `room_random` 及使用中的 AI Mod 后，
上限为 128 个 Unicode 标量 / 512 字节 UTF-8，第 20 字节之后的指令也会读取，例如
`朋友练习局，慢慢打欢迎新手 !ai e2 a1 lv4 !rand`。

移除命令后，之后的开局恢复原生出击流程。

## 本局规则

- 普通、死亡竞技、队长、任务等单机体模式：开局抽一台，本局复活沿用它，下一局重新抽。
- 原生多机体模式 6/7：从各自机库选择机体副本，最多 4 格，**恰好 10 点**（通常 C=2、B=3、A=4、S=5），
  例如 `5+5`、`5+3+2`、`4+4+2`、`4+3+3`、`4+2+2+2`、`3+3+2+2`。同一库存序号不能占多格，同型号的不同副本可以同时使用。
- 房间等级范围和仅 C 限制作用于所有真人的候选池；原生的准备、队伍、模式、战舰等检查照常进行。
- 禁用、过期、耗尽能量、训练或不可出击的机体不会被抽到。锁定只是防出售，锁定的可出击机体可以抽到。
- 有其他可行结果时，单机体不会连续抽到同一型号，卡组不会连续抽到相同的型号组合；只有一种可能时才会重复。
- 任一玩家没有可用机体，或在房间限制下凑不满 10 点时，本次开局整体取消，不会部分随机，也不会借用别人的机体。
  仅 C 卡组最多 8 点，因此无法满足。

单机体模式先按型号等概率抽取，再在其合规副本中选择；卡组在所有满足条件的库存副本组合中均匀抽取。
抽到的机体保留各自的等级、特装、技能、涂装、纹章、经验和机体序号。随机结果只用于本局，不写入数据库中的
出击选择（`GoUserUnitNum`）、仓库、卡组或锁定状态；战后能源、经验等仍由原生服务器处理。

## 安装

朋友服玩家关闭客户端后运行 `update.bat`，再用原启动器进入游戏，客户端部分无需额外配置。

服务端支持 `BattleServer-548A891D-0046C000`（SHA256
`f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4`），需要已安装 SDGO Mod Framework。
先正常停止**要安装的那个** BattleServer，再运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\install_room_random.ps1 `
  -ServerRoot "D:\SDGO-Server\Server\BattleServer1" `
  -DatabaseIni "D:\SDGO-Server\Server\WorldServer\WorldServer.ini"
```

每个会接收房间的 BattleServer 都要安装。安装脚本更新 core、本 Mod，以及已安装的 `pvp_ai` / `coop_ai` DLL，
使三者使用同一套房名解析；保留它们的清单和设置，不会安装缺少的 AI Mod，也不会停止或启动服务。
只补齐缺少的符号，遇到地址冲突则拒绝安装；被覆盖的文件备份到 `sdgo/backups/room_random/`。
增量包会自动使用自己的 `server/`；源码工作树默认使用 `build/stage/Release/server`，也可用 `-Source` 指定。

## 设置

在 SDGO Mod Manager 的“每局随机机体”中设置“原生服务器 INI 路径”，即 WorldServer INI。Mod 只保存这个路径，
不复制数据库凭据；不要把它或私人 `config.ini` 放进客户端分发包。驱动名、查询超时和单玩家机库读取上限是开发设置，
点“显示开发信息”后显示。配置保存在 `sdgo/mods/room_random/config.ini`：

```ini
[settings]
DatabaseIni=D:\SDGO-Server\Server\WorldServer\WorldServer.ini
Driver=SQL Server
QueryTimeoutSeconds=2
MaxRowsPerPlayer=4096
```

`DatabaseIni` 可为绝对路径，或相对于本 Mod 目录的路径。查询使用本机 32 位 SQL Server ODBC 驱动，默认是 Windows 自带的
`SQL Server` 驱动。功能 ID 为 `room-random`，分类安装器可用 `-Features room-random` 选择；正常使用时两端都应保持启用。

## 开发者说明

旧的随机脚本从固定账号列表挑选机体，只在 `tblg_OnUser.f_room/f_team=0` 时更新 `GoUserUnitNum`；房间缓存不会可靠刷新，
卡组也不会每局生成。本 Mod 改为在原生开局校验前，对全房真人执行一次只读、参数化的机库查询，先为所有人算出完整方案，
再交给原生人类战斗构造器使用；中途失败整体恢复。原生释放战斗驾驶员后恢复房间内原来的出击数据，复活不会重抽。
不调用会更新到期状态的机库存储过程。卡组用动态规划枚举所有恰好 10 点的组合。

四条原生真人开局路径都调用同一套校验和构造器：普通/任务、AI 11、AI 12、13/14。13/14 另有独立的外层入口，
同样单独建立每局上下文。断线或中止跳过资源释放回调时，只有确认原生开局已清空真人的战斗机体记录，才会退役上一轮缓存，
避免下一次开局报 `ERR_BATTLE_START_FAIL`；仍有活跃机体时拒绝覆盖。

客户端同步：服务端完成所有真人构造后，沿原生广播链先发送 `0x0861` 完整机体记录，模式 6/7 还先发送 `0x098B` 完整卡组记录，
然后才是原本的 `0x06C3` 开战通知；类型 3 的备用发包机体编号也同步为随机结果。广播只发给真人和观战客户端，不执行选机请求，也不写数据库。

原客户端的 3D 房间把这些通知放进界面命令队列，并且用另一份冻结的开战快照创建机体，所以只更新可见缓存不够。
`room_random_client` 在原生消息分发器入口之后同步房间缓存（与 `match_ai` 的函数入口 Hook 分开注册），并在真人构造入口
核对本局开战消息、房间和玩家身份后，传入含库存序号、技能、涂装及完整卡组的私有记录副本；原快照和永久出击选择不变。
3D 房间不填充旧版账户房间指针，客户端从当前 3D 管理器取得房间模型并调用原生槽位接口；任务与普通对战的记录位置不同。
原生真人构造函数是静态的四参数函数（`RET 10h`），`ECX` 只是临时寄存器，不是房间对象；3D 开战命令还可能在构造前推进应用状态。
因此 Mod 使用实际开战消息中观察到的房间模型，并在构造时重新核对其 vtable 和房间身份。
通知缺失、房间或玩家变化、开战记录不同或卡组不完整时，保留原生输入并在日志中说明原因。

日志：服务端 `native loadout notices queued` 表示通知已排队；客户端 `battle start cache` 和
`human construction ... snapshot ... -> ...` 分别记录房间缓存和实际构造输入。

## 开发测试

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32 -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure

# 只读机库查询，仅在宿主本机运行
build\Release\room_random_inventory_probe.exe "D:\Server\WorldServer.ini" 1234

# 使用本机原生 EXE 执行服务端选机和客户端缓存复制指令
build\Release\room_random_test.exe "D:\Server\BattleServer.exe" "D:\Client\GOnline.exe"
powershell -File tests\room_random_client_runtime_test.ps1 -GameExe "D:\Client\GOnline.exe"
powershell -File tests\room_random_install_test.ps1 -NativeExe "D:\Server\BattleServer.exe"
```
