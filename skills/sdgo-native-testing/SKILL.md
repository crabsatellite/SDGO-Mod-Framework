---
name: sdgo-native-testing
description: Fix and verify original SDGO Mods in the shared local test copy, using its existing accounts and isolated native database without Docker; copy test only for concurrent conflicts, then finish authorized delivery and cleanup. Not for Minecraft GameTest or ordinary gameplay.
---

# SDGO 原生测试 guide

先读当前源码工作区的 `AGENTS.md`。目标是完成指定修复和相关完整原生 GameTest，继续已授权的部署与 Git 交付。测试不是扩建基础设施的任务。

## 统一的本地 test

- 默认复用 `E:\Dev\sd-re\test`。它由生产客户端和服务端复制而来，含本地客户端、原生服务程序、**独立的原生 SQL 实例和数据文件**、已有账号及机体。生产端只作为复制来源；不得把测试客户端、服务端或维护脚本连接到生产数据库。仅改测试目录名不算数据库隔离。
- **不用 Docker**，不创建 SQL 容器、卷或容器传输通道。旧 Docker 路径只用于回收已有遗留资源，不作为新测试入口。不要为每次重试还原 11 库、创建专用测试服务器或重做公共资产。
- 同一任务一直复用自己的测试客户端和后端，改 DLL、重启或换测试用例都不需要重新复制全套。先核实 test 的本地状态和实际 SQL 文件路径，再开始测试；尚未就绪时定位具体问题，不能回退到生产测试。
- 串行任务可复用统一 test。确需并发或测试状态相互冲突时，复制 test 文件夹（或所需客户端/服务端），给副本配置独立回环地址、端口、SQL 实例及数据目录，再运行。副本不得共享生产或基础 test 的可写数据库文件；账号使用副本里已有身份，不注册新账号。
- 本机其他 agent 的客户端、服务和账号正在使用时，不覆盖其候选 DLL、不抢同一账号、不停止其进程。并发副本标明任务归属，测试完停止所属进程并删除**整个任务测试客户端、服务端和数据库副本**。保留统一 test 与仍在使用的文件。
- 继承已有代码、修复和有效测试结果；只补缺失或被新改动影响的验收，不因迁移测试环境而把所有检查重做。

## 仓库和本地文件的边界

Mod 源码库保存框架、Mod、测试代码、fixture 声明、配置范本和指南。完整客户端、服务端、原始模型/归档、SQL 程序/备份/数据、真实账号资料和凭据都只保存在本地，并受 Git ignore 保护；不能上传到 Mod 仓库、报告附件或测试包。

测试副本与正式运行文件发布是两个用途。后者沿用已授权的客户端/服务端专用仓库与现有发布流程，不能将临时 test 树当作发布来源。

## 现有框架

| 组件 | 用途 |
| --- | --- |
| `tools/game_test.py prepare/run/report` | 准备或复用客户端，绑定现有账号、封存候选输入，后台运行并校验 JSON/JUnit 和原生事件 |
| `mods/game_test_client` | 原生登录、机库、房间、战斗与指定场景观察和断言 |
| `mods/game_test_server`、`tools/game_test_server_report.py` | 观察真实服务端战斗、结算和返回流程 |
| `tools/game_test_backend.py serve` | 启动已配置的本地 `test/server`；复用原生 SQL，按真实频道启动服务，并等待本次启动的原生表加载完成 |
| `tools/game_test_fps.py` | 不登录的 FPS 参数矩阵；战斗速度须另用相关战斗检查 |
| `tools/game_test_local_sql.py`、`tools/native_sql` | 复用复制的原生 SQL；配置只写任务自己的 HKCU 分支，SQL进程内重定向，不安装系统实例或修改生产登记 |
| `tools/game_test_environment.py` | 已有环境归属与回收记录；`environment list/recover` 可检查和回收旧遗留环境 |
| `tools/game_test_fixtures.py` | 已有五账号和公共机体数据的声明/核验；普通测试不调用账号创建函数 |

`environment run/template/upgrade` 的旧实现会创建 Docker SQL，已停用新建。不要把旧模板命令写成当前原生 test 已实现的启动方式。统一 test 的真实启动方式和端点以本地配置为准，不从历史对话猜密码、端口或就绪状态。

## 普通测试

1. 在对应源码 worktree 检查 Git 状态，构建候选 Mod 和 `game_test_client`；需要服务端观察时构建 `game_test_server`。Windows 使用 x86 MSVC，Python 3.10+。构建产物在 `build/stage/Release`，没有名为 `stage` 的构建目标。
2. 选择已有本地 test，读取实际账号绑定与已有机体 UID。账号/机体不足时才按[公共测试数据维护](references/template-data.md)补必要数据，只操作测试数据库。日常测试不注册账号、不重新获得机体、不重走新手资产创建。
3. 使用 `prepare`，将目标地址和私有账号配置明确指向 test。已有停止的本任务客户端用 `--reuse-client`；候选 DLL 用 `--include-mod` 在封存前安装。例如：

   ```powershell
   python tools/game_test.py prepare --client-template <local-test-client> --stage build/stage/Release --run-dir build/game-test-runs/<task> --target-host <test-loopback-ip> --login-port <test-login-port> --account-profile <private-existing-test-account.ini> --include-mod <mod_id>
   python tools/game_test.py run --run-dir build/game-test-runs/<task> --timeout 600
   ```

   需要服务端观察时，在本任务 test 后端使用匹配的观察组件和本次运行 ID，核实原生启动 XML 与 `tbl_ChannelInfo` 共同启用的 Battle 端口。不要因目录存在而启动全部频道，也不要放宽原生数据校验。
   已配置的 `test/server` 使用 `game_test_backend.py serve --work <test/server> --client-run <prepared-run> --stage <stage> --startup-xml <test/server/ServerStartUp/ServerStartUp.xml> --host-pilot-id <existing-account-id>`；运行 `run` 前等它返回 `backend_ready`。TCP 开放不代表 Battle 数据可用：必须等本次启动日志的 `Table Download OK.. Time Procedure Ready..`，不能用旧日志或固定延时替代。已有共享服务正在使用时先复用或复制自己的并发副本。
   客户端迭代可持续复用已就绪的服务进程：从实际 `server-observer-events.jsonl` 首行读取同一会话的 `run_id`、`nonce` 和各 Battle 的 `pid`，传入 `game_test.prepare/reuse_prepared(..., server_session={'run_id': ..., 'nonce': ..., 'pids': [...]})`。框架封存一个带新随机后缀的完整房名，客户端读取该房名；服务端报告按会话、进程和本轮精确房名验证，旧房间记录不能算本轮通过。账号须与观察器的实际 host 绑定一致。服务端候选 DLL 或配置变更时才重启所属服务并建立新会话，保留 SQL、程序与测试数据。
4. 读取并核验本次真实登录、房间、原生事件、JSON/JUnit 与服务端报告。完整战斗保留结算和返回流程；编译、模拟/回放、登录 smoke 或 bootstrap 不能代替相关完整 GameTest。阿普萨拉斯需实测首次 R、变形释放、移动/攻击和再次变回。
5. 成功验证及必要部署读回后，删除这次测试记录、日志、截图、回执和临时研究文件；失败诊断暂留到问题解决后删除。保留测试源码、公共账号资产、配置和统一 test。并发任务结束后额外删除整个所属副本，读回其进程已停止、临时文件已清除。不要清除他人运行中的环境。

已确认可清理的本任务临时文件若被永久删除命令阻断，改用 Windows 回收站，例如
`Microsoft.VisualBasic.FileIO.FileSystem` 的 `RecycleOption.SendToRecycleBin`。先核实绝对路径、任务归属和相关进程已停止，
限定在本任务目录内；移入后确认原路径消失且回收站存在可还原条目。保留统一 test、公共数据和其他任务文件，
不清空回收站，也不在回收站操作失败时回退到永久删除；失败时保留原件并说明实际原因。

测试隐藏、静音并隔离输入，核实实际 sockets 和服务监听为回环。网络错误按精确步骤排查；建防火墙规则的 `0x80070005` 不能推断全部本地测试不可用。原生 INI 保留 `DBServerName`、`DBName` 等键的大小写；`manager.storage.ini_values` 仅用于查询，重写使用 `game_test.read_ini`。

## 开发完成后的交付顺序

在已授权的功能开发或修复任务中，按以下顺序完成交付：

1. 完成代码和相关 GameTest。若框架或环境导致失败，修复相应问题并取得有效测试结果。
2. 将配套运行文件部署到实际客户端和 Manager 框架副本，保留登录、画面等个人设置，核对安装版本及文件。涉及服务端时同步检查对应组件。
3. 按仓库的完整运行文件发布流程提交、推送客户端 `main`，再通过实际更新器核对安装提交与配置。
4. 更新现有 `README.md`、`README.zh-CN.md` 的功能索引，以及该功能已有的中英文独立文档。内容应与刚部署的行为一致，说明用途、操作、默认值、范围、生效方式、兼容性和排错方法。
5. 将源码、测试及上述文档合入最新 `master` 并推送，保留并行提交。文档更新是交付步骤的一部分。

功能通过相关完整 GameTest 并完成实际部署与主线发布后，必须同步更新该功能的文档状态和发布元数据。
已完成验收并正式发布的功能应移除过时的 Beta、测试中或待发布标记，将对应 `channel` 更新为 `stable`，
并对齐中英文 README、功能指南、Mod 清单、Manager 标签、版本后缀及生成/打包模板。
读回实际安装和已发布内容，确认状态一致后才算交付完成。仅编译、夹具或局部场景通过的功能保留原状态；
不能因本次其他功能通过而批量取消 Beta，也不能用一份测试报告代替文档状态更新。

发布状态以已验收版本及其声明的功能范围为单位。后续的新功能、性能优化或候选分支待测，不会取消已发布版本的
有效验收，也不应让整个功能长期保留 Beta。复核历史原始回执、版本、发布读回和用户实测确认；成功记录按要求清理
不等于从未测过，旧文档的 Beta 标记也不能反过来证明未验收。对当前版本尚缺的验收，写清具体流程，并与下一版本的
新增场景分开。验收类型应匹配组件：网络功能核对真实 TCP/UDP 与原生 relay 链路，战斗行为核对相应原生 GameTest。

公开仓库的文档面向玩家、服主或外部开发者。使用可独立理解的说明，避免任务对话、单机部署记录、临时路径、运行 ID、逐次测试计数、内部完成状态和待办式措辞。开发文档可以说明接口、实现与可复现的测试命令，但应写成通用参考。

不新增独立的测试总结、部署报告或日期式验证记录来替代 README 和功能文档。GameTest 自动生成的 JSON、JUnit 和日志留在本地被 Git 忽略的测试输出目录，用于排错；不将它们复制成公开功能文档或最终交付入口。

结束前清理所属临时环境，保留统一 test、公共资产、测试代码和未解决问题的必要诊断；成功测试记录删除。最终回复简要说明功能、部署和提交结果。

测试框架迁移不取消上述已授权交付。公开文档不添加本机端点、临时报告或账号数据。结束时简要说明真实验收、部署/提交和清理状态；未通过的机体问题继续推进，不能因框架已提交就宣布修复完成。
