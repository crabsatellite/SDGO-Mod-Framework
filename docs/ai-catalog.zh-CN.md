# AI 机体目录（Beta，独立附加包）

English: [ai-catalog.md](ai-catalog.md)

`ai_catalog` 把服主数据库中可出击的玩家机体加入 `coop_ai` / `pvp_ai` 的 LV1–LV5 机体池。
服主生成一份目录，安装到对战服务器和所有参战客户端；玩家照常使用原来的启动器。

需要现有的 AI 功能：任务使用 `coop_ai` + `mission_ai`，PvP 使用 `pvp_ai` + `match_ai`。本附加包只扩充随机机体池，
不新增机体选择界面，也不改变原生 AI 对战模式 11/12 的机体名单。例如房名 `!ai e2 a1 lv2` 会从扩充后的 LV2 池
抽取 2 个敌人和 1 个队友。

也可配合[长房名 Mod](https://github.com/crabsatellite/SDGO-Mod-Framework/blob/master/docs/room-titles.zh-CN.md)，例如
`朋友练习局，慢慢打欢迎新手 !ai e2 a1 lv4 !rand`：AI 使用目录里的 4 级池，另装并启用 `room_random` 后真人每局随机。
组合语法见[房名组合指令](https://github.com/crabsatellite/SDGO-Mod-Framework/blob/master/docs/room-commands.zh-CN.md)。

## 服主：在 Manager 中生成和安装

打开 SDGO Mod Manager 左侧的“AI 机体清单”：

1. 选择原生服务器 INI，Manager 直接从数据库读取目录表。
2. 选择配套客户端目录，Manager 会从中读取机体和 NPC 表。
3. 使用 JSON 导出、已解包的 `gdrs` 目录或分级规则文件时，先点“显示开发信息”，这些字段才会出现。
4. “检查”只输出报告；“生成”在新目录中生成可安装的目录。
5. 关闭目标进程后，分别“安装到客户端”和“安装到 BattleServer”。每个参与的 BattleServer 都要安装。
6. “导出玩家包”生成给玩家的客户端增量包。

服务器和所有参战客户端必须使用同一次生成的目录，并同时启用或关闭。没有安装本附加包的客户端不要进入扩展机体房间。

生成目录中的 `catalog.json` 是编号分配记录。在 Manager 中再次生成时会自动以上一次生成的目录为上一版；
使用命令行时**必须手动传入**。已移除机体的编号会继续保留，
避免日后把旧 NPC 编号分给别的机体。不要把某位服主生成的目录提交到代码仓库或当作通用默认值。

## 玩家：安装导出的包

先安装匹配的框架和 AI 功能，关闭游戏，把包解压到 `GOnline.exe` 旁边，然后运行一次：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Install-AiCatalog.ps1
```

它只补齐本 Mod 需要的符号定义，遇到冲突会拒绝写入并保留原文件备份。之后照常用原启动器。玩家无需 Python。

## 分组和服主规则

默认只收 `tbl_Units` 中 `SallyReady=1` 且 HP>0 的机体，分组为 C→LV1、B→LV2、A→LV3、其他 S→LV4、SS/SR→LV5。
这是机体池分组，不是实测的 AI 难度。

规则文件可参考 `backend/ai_catalog.rules.example.json`：

```json
{
  "exclude": [10001],
  "levels": {"10002": 3},
  "templates": {"10002": 3604001}
}
```

编号只是格式示例。`templates` 只能指向服务器和客户端都有的原生 AI NPC 模板。默认按武器类型、等级和 HP 选择原生
AI 脚本，主武器为空时偏向射击模板。AI 使用原有效初始武器，否则用第一个有效武器；这只改 AI 的私有副本，不影响玩家机体。

## 开关与卸载

本附加包默认关闭，在 Manager 中开关；修改后重新启动相应进程，不支持对局中热卸载。关闭后 `coop_ai` / `pvp_ai`
继续使用各自原来的 `LevelN` 设置，不需要恢复数据库或改回等级池。它不随 Base / Full 包自动安装。

## 命令行（可选）

不使用 Manager 时，服主工具需要 Python 3.10+。`--source` 是解压后的独立包根目录，从源码测试时可用
`build/catalog/stage/Release`：

```powershell
python .\backend\ai_catalog.py --db-export "D:\OwnerData\GOnlineGame" --client-gdrs "D:\OwnerData\gdrs" --generate --out ".\my-roster"
python .\backend\install_ai_catalog.py install --role server --target "D:\Server\BattleServer1" --source "." --catalog ".\my-roster\server\sdgo\mods\ai_catalog\catalog.bin"
python .\backend\install_ai_catalog.py install --role client --target "D:\Games\SDGO" --source "." --catalog ".\my-roster\client\sdgo\mods\ai_catalog\catalog.bin"
python .\backend\export_ai_catalog.py --source "." --catalog ".\my-roster\client\sdgo\mods\ai_catalog\catalog.bin" --out ".\players.zip"
```

更新时加 `--previous-catalog ".\my-roster\catalog.json"` 并使用新的输出目录；不加 `--generate` 时只输出 `audit.json`
和 CSV 规划。也可用 `--connection-env SDGO_SQL_CONNECTION` 从本机环境变量中的 ODBC 连接字符串读取（需 `pyodbc`
和 SQL Server 驱动）。`install_ai_catalog.py` 还支持 `disable`、`enable`、`uninstall`、`recover` 和 `--host-exe`
（重命名的同版本服务器副本）。卸载恢复首次安装前的文件；受管文件已被其他更新器改过时会拒绝覆盖。

## 开发者说明

支持 `GOnline-54BE1568-00C97000` 和 `BattleServer-548A891D-0046C000`。运行时不连接数据库、不插入
`tbl_NpcData` / `tbl_UnitNpc`、不修改加密游戏包。

- 服务器只在构造扩展 AI（`battle.build_ai_pilot`）时提供额外的 NPC 记录和私有机体副本，异常或嵌套调用后恢复线程上下文。
  已安装的 `coop_ai` / `pvp_ai` 会获得可选的目录桥接 DLL，它们的 `mod.ini` 和设置不变。
- 客户端在最后一份 NPC 表加载后（`grid.load_npc_table`），把目录记录注册到当前进程的原生查找树和校验表
  （`grid.make_node`、`grid.insert_node`、`grid.register_record`），因此绕过 `grid.find`、直接查树的原生创建函数也能找到。
  记录和节点使用游戏自身的分配器，由正常的表析构释放。
- 注册前预检全部源机体指纹和编号冲突，有冲突或源数据变化时放弃本次注册，不覆盖原记录；创建函数遇到缺失或不同的
  目录记录会拒绝创建。

相关逆向结论：

- 客户端 `FUN_00414090` 依次把 `0237/0242/0243/0244/0246/0245` 作为 type 2 机体数据加载，把
  `0020/0105/0128/0162/0198/0222` 作为 type 41 NPC 数据加载；目录在 `0222.gdt` 加载后注册。
  `FUN_004230D0` 解析 598 字节的 NPC 记录，跳过 security level 非零的记录。NPC 记录布局见
  [任务 AI 僚机](coop-wingmen.zh-CN.md#增加更多僚机机体)。
- 原生客户端创建函数会内联查树，不经过 `grid.find`，因此必须注册到原生树，单纯 detour `grid.find` 不够。
  原生 `Find` 用 AL 返回 bool，失败时 EAX 高位未定义。
- `BattleServer.exe` 的 `0x00485720` 先按 NPC ID 查记录，再按其机体 ID 查数据，任一步缺失即失败，之后还读取机体的
  武器/技能引用，所以服务端和客户端的数据必须配套。
- 普通玩家机体在服务端 `tbl_UnitNpc` 中没有对应项，这由服务端私有副本补上。完整 LV 池若写成单行 `LevelN=`
  会超过 `coop_ai` / `pvp_ai` 1024 字节的 INI 读取缓冲区，因此改由可选 provider 提供等级池。

构建与打包：`cmake --build build/catalog --config Release`，然后 `python tools/package_ai_catalog.py`。
测试：`test_ai_catalog.py`、CTest 的 `ai_catalog_integration`，以及需要自备 EXE 的 `ai_catalog_native_test`（服务器）
和 `ai_catalog_client_map_test`（客户端）。
