---
name: sdgo-native-testing
description: Run and diagnose original SDGO native GameTest using isolated loopback environments, shared five-account database templates, real client/server flows and verified cleanup. Use for SDGO Mods and native test-framework work, not Minecraft GameTest or ordinary gameplay.
---

# SDGO 原生测试 guide

目标是使用当前测试框架验证用户指定的功能。默认只做测试、收集证据和清理；测试本身不扩大功能开发或生产数据修改范围。先读当前源码 worktree 的 `AGENTS.md`。常见仓库根为 `E:\Dev\sd-re\SDGO-d3d9-proxy`，并行开发 worktree 位于 `E:\Dev\sd-re-worktrees`；在实际含有本 skill 对应工具版本的 worktree 内运行命令。

## 框架里有什么

| 组件 | 职责 |
| --- | --- |
| `tools/game_test.py` | 准备正常客户端副本、封存运行输入和哈希、启动、校验回执、生成 JSON/JUnit 报告 |
| `mods/game_test_client` | 真实客户端主线程上的登录、页面、机体选择、建房、战斗、任务等请求和回包断言 |
| `mods/game_test_server` | 真实 BattleServer 内的被动观察，按运行 ID、房间 nonce、真实玩家和战斗事件关联 |
| `tools/game_test_native_environment.py` | 从已有模板复制私有原生后端、SQL 数据库和账号配置，运行测试并清理 |
| `tools/game_test_environment.py` | SQLite 跨进程租约、独立回环 IP/UDP 池、资源归属日志、证据归档、遗留回收 |
| `tools/game_test_sql_bridge.py` | 回环 TCP 到 `network=none` 私有 SQL 容器的透明字节传输，不编造 SQL 或游戏回复 |
| `tools/game_test_fixtures.py` | 仅供初次制作公共模板时初始化并核验五账号及机体；普通测试不调用创建流程 |

模板是本地私有、带文件 SHA-256 清单的只读输入，含原生服务程序、11 个完整 SQL 快照、共享资源和五个已初始化账号。数据库、账号配置和游戏原始文件不进入 Mod 源码 Git。查询 `environment templates` 获取现有模板路径，不能从历史记录猜服务地址或密码。

每次 `environment run` 初始创建：唯一租约和临时目录、独立 `127/8` 地址和 UDP 池、带归属标签的 SQL 容器及卷、还原的 11 库、重定位后的原生服务副本、私有账号配置副本、正常客户端测试副本以及运行 ID/哈希/回执目录。它绑定模板中的已有账号，**不会注册账号、抽机体或写真实玩家数据库**。副本中的账号 UID 与模板相同，账号锁文件归本环境所有，不会锁住另一个环境。

## 普通测试

1. 检查本 worktree Git 状态和构建配置，保留他人的修改及运行环境。Windows 用 PowerShell、x86 MSVC；Python 最低 3.10，原生后端需 Docker Desktop 和 `pyodbc`。沿用已缓存的固定 SQL 镜像，不为排错启动全局 LiteSQL 或弹 UAC。
2. 构建目标 Mod、`game_test_client`，涉及服务端观察时也构建 `game_test_server`。构建会自动写入 `build/stage/Release`，没有名为 `stage` 的构建目标。
3. 发现模板、查看并恢复**已无活跃拥有者**的遗留环境：

   ```powershell
   python tools/game_test.py environment templates
   python tools/game_test.py environment list
   python tools/game_test.py environment recover
   ```

   模板缺失时定位或恢复公共模板。不要改用 `--create-test-account`、全局账号配置或其他 agent 的服务来绕过。
4. 从已有模板运行。`--template` 可省略，默认用最新登记模板；明确指定便于复现。按需要选择一次 bootstrap 或完整测试：

   ```powershell
   python tools/game_test.py environment run --template <existing-template> --account 3 --bootstrap-only
   python tools/game_test.py environment run --template <existing-template> --account 3 --include-mod <mod_id>
   ```

   `--include-mod` 把已构建候选 Mod 及资源放进副本，合并所需符号，并在启动前冻结哈希。不要运行后替换 DLL、改数据库或修改被封存的输入。普通对战保留完整结算和返回流程，超时默认 600 秒。
5. 阅读命令返回的 `evidence` 下 `result.json`、`client-run/receipts/report.json`、原生 `events.jsonl` 和 `cleanup.json`。结果必须绑定本次源代码、二进制/资源、账号、真实登录和房间；缺失、超时或原生异常按失败报告。编译、模拟/回放或 bootstrap 通过不能代替完整战斗。
6. 结束后再次 `environment list`，本次环境必须为 `closed`，`cleanup.json` 为 `clean`，临时目录/私有容器/卷及拥有的进程已清除。保留模板和紧凑证据。出现 `agent-action.json` 或 `cleanup_required` 时由 agent 执行回收并读回结果；最终答复写清仍未清理的环境 ID，不能把清理留给用户而不说明。

多个进程可以同时运行上述命令；每次都新建环境，不把别人的活跃后端当共享测试服务，不按进程名批量结束进程。恢复依据 PID 加创建时间、租约和 Docker 标签，不能凭目录年龄删除资源。中断后重新执行 `recover`，活跃拥有者会被跳过。

## 现有五个账号

| 槽位 | 用途 | 默认机体 |
| --- | --- | --- |
| 1 | 阿普萨拉斯 III 异常初始形态回归 | 24788 |
| 2 | 阿普萨拉斯 III 正常形态对照 | 24787 |
| 3 | 通用战斗 | 10003 |
| 4 | 远程战斗 | 22042 |
| 5 | 第二参与者/其他场景 | 22097 |

每个账号预装 `10003,22040,22042,22097,24402,24787,24788`。实际身份、UID 和归属读模板 `accounts` 和本次 `fixture-binding.json`，不要硬编码真实玩家 UID。24788 是明确声明的异常形态测试资产，不是正常购买形态。

阿普萨拉斯完整回归：

```powershell
python tools/game_test.py environment run --template <existing-template> --account 1 --include-mod transform_input_fix --apsalus-roundtrip
python tools/game_test.py environment run --template <existing-template> --account 2 --include-mod transform_input_fix --apsalus-roundtrip
```

该开关在副本 observer 的 `mod.ini` 中封存，调用原始变形准入业务并观察两次实际形态提交/退出，随后完成正常战斗；不要靠进程环境变量偷偷注入未封存场景。没有加候选修复的 before 测试可以失败；如模板已安装该修复，必须明确从 before 副本禁用并重新封存，不能把已修复的模板叫作 before。

## 排错和扩展

- 先读 `progress.json`、native 日志和精确失败步骤。核实本次独立 IP、登录端口、账号绑定、服务 readiness，再诊断网络。`0x80070005` 建防火墙规则失败只说明那条远程路径权限不足，不能推断回环、Docker 或全部测试不可用。
- Battle 启动集合必须同时满足原生 `ServerStartUp.xml` 的 Run 配置和数据库 `tbl_ChannelInfo.AddrPT`；不得因目录存在而启动未登记 Battle 频道，也不得放宽原生行数校验。
- 测试隐藏、静音并隔离输入；真实客户端 sockets 和服务端监听要读回为回环。不得操作用户桌面或以交互客户端代替失败的后台测试。
- 现有模板和现有账号优先。**只有已确认场景缺少必要公共数据时**，才读 [公共模板数据维护](references/template-data.md)。普通 bug 验证不能顺手注册新账号、补库存或开发功能。
- 修复后的交付沿用仓库 `AGENTS.md` 和用户当前授权。本项目已授权通过相关完整 GameTest 后常规合入/push 和本地部署；完整运行时发布和实际读回是独立证据，不从测试通过推断已部署。
