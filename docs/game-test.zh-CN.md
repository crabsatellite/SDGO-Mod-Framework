# 原生 GameTest

默认复用本机从生产复制、数据库完全隔离的原生 `test`，不用 Docker。
并发有冲突时复制 test 并配置独立端口和原生 SQL，完成后删除整个任务副本。
普通测试绑定已有账号与机体；保留已有代码和有效结果，测试程序/数据库副本只留本地。
流程、命令和数据维护见 [`sdgo-native-testing` guide](../skills/sdgo-native-testing/SKILL.md)。

## 无需登录的 FPS 参数验证

构建 `game_test_client`、`fps_limit`、`sdgo_core`、`d3d9_shim` 和 `sdgo_data` 后，
以正常客户端为只读模板运行矩阵：

```powershell
python tools/game_test_fps.py --client-template D:\Games\SDGO --stage build/stage/Release --work-dir build/fps-runs --results-dir build/fps-results
```

矩阵使用同一个 GameTest 后台桌面、真实 D3D9 设备、静音 Miles 驱动和回环网络策略，
停在原生登录页，不读取账号配置，并清空测试副本的已保存登录资料。
依次对照「禁用 Mod、配置 160」与启用后的 30、60、120、160。
原生登录页仍需要连接兼容的真实后端；可通过 `--target-host`、`--login-port`
选择已有本地测试入口，不会因此创建账号或提交凭据。
整轮默认超时为 600 秒，为原生资源加载预留时间，可用 `--timeout` 调整；
这不改变频率断言或采样窗口。
后台进程使用与普通游戏相同的 Windows 正常优先级；宿主 CPU 满载时，低于正常优先级会使
原生加载及计时采样长期得不到调度。后台桌面与静音隔离继续生效。

每轮读取真实应用的 FPS 字段及已初始化的计时器间隔，再以独立的 QueryPerformanceCounter
测量原生更新和成功 D3D9 Present 的频率，连续采样三段，每段三秒，保留全部窗口及平均值。
逻辑更新须保持原生 60 Hz 的 80–110%，不随渲染上限提高；成功 Present 须有正值，平均不超过启用上限的 110%。
最小提交间隔须符合实际限速周期，允许 0.5 毫秒与周期 10% 两者较大的调度误差。
正式 Mod 的观察接口独立确认逻辑/渲染目标与实际周期；30 FPS 对照须真正触发等待。
禁用对照不检查渲染上限。渲染低于上限时照实记录，不承诺达到该吞吐量。
断言和结果沿用 GameTest 的 JSON/JUnit；该场景验证登录页原生渲染，战斗计时和画质另行测试。

运行器先导出报告、原生事件及后台环境回执，再核对本轮进程已退出并删除所属客户端副本。
模板和其他测试保留；换环境复测时指定新的工作及结果目录即可。
单独验证一档也可使用 `game_test.py prepare --fps-cap 160` 后执行通常的 `run`；
添加 `--fps-mod-disabled` 可取得原生强制 60 的对照。

完整原生对战回归可在常规已有账号的准备命令中加入 `prepare --fps-gameplay-cap 160`。
运行前会封存渲染上限与观察器配置。实际 AI 移动后，场景核对 60 Hz 逻辑更新；
选择高于 60 的上限时，渲染还必须实测超过 60 FPS，之后继续正常对战、结算和返回流程。

### 重建本地后端

`tools/game_test_backend.py` 管理复制的原生服务和独立 SQL 容器。需要 Docker、兼容的
本地原生服务端、server stage，以及已校验的数据库备份（`backend.json` 和 `backups` 目录）。
server stage 须构建 `winmm_shim`、`sdgo_core`、`sdgo_data`、`pvp_ai` 和 `game_test_server`。
缺少加载器、观察器或符号时，会在创建 SQL 资源和恢复数据库之前报告预检失败。
使用新的工作目录和未占用的 IPv4 回环地址，例如：

```powershell
python tools/game_test_backend.py prepare --source-server D:\SDGO\Server --stage build/stage/Release --backup-source D:\Tests\verified-backend --work build/fps-backend --target-ip 127.93.0.1
python tools/game_test.py prepare --client-template D:\Games\SDGO --stage build/stage/Release --run-dir build/fps-probe --target-host 127.93.0.1 --login-port 5001 --fps-cap 160
python tools/game_test_backend.py serve --work build/fps-backend --client-run build/fps-probe --stage build/stage/Release --startup-xml D:\SDGO\ServerStartUp\ServerStartUp.xml --lifetime 1800
```

出现 `backend_ready` 后，在另一终端运行矩阵并指定相同的 `--target-host`。
服务启动重试和数据库探测均有超时；启动集合同时读取原生启动 XML 和频道表。
FPS 套件无需账号；其他套件默认复用私有账号配置，后端观察器另需通过
`serve --host-pilot-id <账号实际原生驾驶员 ID>` 指定对应账号，不在工具中固定账号 ID。
缺少已校验备份时，只有显式
`--allow-live-copy-only` 才允许向源 SQL 发起 COPY_ONLY 备份。

```powershell
python tools/game_test_backend.py stop --work build/fps-backend
python tools/game_test_backend.py cleanup --work build/fps-backend --remove-runtime
```

清理先核对所属服务已退出及 SQL 容器/卷身份，再删除私有凭据和复制的运行环境，保留公开回执。
单独准备的 `fps-probe` 仅属于该次测试，进程退出、证据导出后再删除。共享备份和源服务端保留，
后续可换目录、地址或机器复测。

开发用可选 Mod，默认关闭。测试在真实客户端进程内执行，通过正常业务请求
与真实服务端通信。外部运行器负责准备独立客户端、启动和收集结果，不操作桌面。

每个场景按“发起请求 → 等待真实回包 → 断言状态”执行。登录、服务器数据、
建房和战斗加载均走正常游戏流程，不伪造机库、房间、机体记录或服务端回包。

测试账号与注册重试规则保存在本地私有配置中。缺少步骤、断言失败、未处理的原生异常
和超时都会判为失败，结果输出为 JSON 和 JUnit XML。仅安装 Mod 不会自动运行测试。

测试默认使用独立后台桌面，副本中的 BGM、音效和语音全部静音。后台窗口与前台
归属有独立回执；不满足条件就失败。旧 D3D9 的设备在专用图形线程上创建，仅使用
不可见、不可激活的代理窗口；真实游戏页面仍留在后台桌面。禁止全屏，逐窗口验证
代理身份和前台归属。原生 Miles 驱动使用 WaveOut，按驱动验证主音量为零。逻辑测试副本关闭原生
后处理效果，同时将各项原生特效等级设为零，并读取实际运行标志；该配置用于逻辑与协议测试，画质和后处理应在可见客户端中单独测试。
本机直连测试只绑定回环地址，不创建公网监听，
不修改系统防火墙或通知设置。非本机测试需要预先具备其专用防火墙规则的管理权限；
准备失败时不会弹出提权窗口或回退到交互运行。

默认复用 `%LOCALAPPDATA%\SDGO-GameTest\account.ini`，也可用 `--account-profile`
指定已有配置。普通重跑不会注册新账号；只有显式 `--create-test-account` 才允许
首次创建持久账号配置，且不会覆盖已有文件。

新账号初始化包括昵称、新手测试退出、首次机体抽取和必要的游戏内确认。
这些步骤同样等待正常服务端结果，随后才运行对战场景。

开发者可通过 [`game_test.h`](../include/sdgo/game_test.h) 注册按帧执行的测试，
在原生主线程等待实际业务事件并进行断言。

构建后，用已有正常客户端作为模板。模板需要安装对战 AI，目标服务器需要启用
`pvp_ai`。运行器只启动独立客户端，不重启现有服务器。

```powershell
python tools/game_test.py prepare --client-template D:\Games\SDGO --stage build/stage/Release --run-dir build/gametest/pvp --mode-sequence 1,3,9
python tools/game_test.py run --run-dir build/gametest/pvp --timeout 3600
```

仅首次没有测试账号配置时，在 `prepare` 后加 `--create-test-account`；以后直接
复用同一配置。首次准备独立客户端，后续对同一 `run-dir` 添加 `--reuse-client`，
在确认进程已停止、文件完整后复用现有客户端，每轮生成新的运行 ID。
上一轮回执与二进制哈希集中保存，旧任务及重复文件进入回收站；不会再复制整套客户端。
被其他任务引用的只读归档基线不能作为复用目标。同一账号禁止并发运行。
复用功能需要开发环境安装 `Send2Trash`，缺少时会停止，不会降级为永久删除。
连接地址默认 `127.0.0.1:5001`，可用 `--target-host` 和 `--login-port` 修改；
服务器和频道由原客户端正常流程自动选择。
默认 `--network-policy direct` 仅在测试副本关闭继承的地址转发，并在登录前核对
该进程实际建立的 TCP 连接。要专门测试服主的 FRP 路由，显式选择 `preserve`。

示例依次测试普通、死亡竞赛和队长模式。每种模式都经过
建房、真实开局、AI 状态断言和正常结束返回；也可指定单个模式排查。
Grid（`6`）和 Tag（`7`）会通过原生业务切换 VS 频道，并要求账号已有完整战舰编队。
缺少编队或服务端拒绝会判为失败。
回执位于运行目录的 `receipts/report.json` 和 `receipts/junit.xml`。

准备账号时可单独使用 `--bootstrap-only`、`--battleship-only`、`--gift-requests-only`
或 `--deck-only`，这些场景不计为对战通过。领舰走原生教学；赠送走现有账号的
正常邮件业务，需目标服务器启用正常赠送服务并独立验证处理结果。编队从私有
账号配置的 `[deck] unit_uid0` 至 `unit_uid3` 读取四台实际持有的机体，等待原生
保存和出击舰回包。准备完成后重新登录同一账号运行对战，不补写机体或数据库记录。

使用 `--pages-only` 可验证真实登录后的机体管理页面、库存读取、两种兑换预览和
取消。测试逐台比较机体 UID、槽位及出击机体；存在兑换候选时不会提交兑换。
页面场景不计为对战通过。
该场景还通过原生按钮打开保管库，等待消息循环和真实列表回包，验证确认领取流程；
空库通过只代表空库路径，不代表物品到账。扭蛋检查核对五张原生目录表，进入实际
扭蛋页面后检查 9301 的列表与确认数据源货币、以及收藏机的原生价格，不会购买。
该场景要求安装机库、保管库、操作员与 AI 目录、扭蛋目录和货币修复 Mod，
以及服务端 1200 格机库 Mod。
目录检查从真实原生表逐条读回配置的操作员与 NPC，核对名称、类别引用、NPC
字节和来源机体哈希；这些目录检查不代表商店交互或任务通关。

要覆盖商店页面，在 `--pages-only` 中同时传入 `--shop-assertions` 和
`--shop-display`。两份开发用断言文件只描述服主已有的现货，复制到测试副本后
按路径、SHA-256 和 revision 绑定。场景读取原生名称和永久／租赁选项，打开购买与
赠送预览后取消，并比较完整机库与钱包。它不会安装商品、购买或赠送。
查询先打开原生搜索页，再点击该页的 `FilterBtn` 并等待原生查询完成；仅切换
`AllSerchRadio` 会打开空页，不作为商品查询成功的证据。

`--play-unit-uid` 可在页面、对战或任务场景前，通过原生机库选择账号实际持有的
机体。框架先等待真实完整机体资料，再调用出击业务；必须收到原生成功回包并读回
相同 UID，才算完成换机。`--random-loadout` 使用房名中的原生 `!rand` 对战命令。

`--mission-mode 5` 或 `8` 注册任务烟雾测试：真实频道与任务选择、建房、开局、
三名等级 2 的友军刷出、移动、目标获取和原生战斗状态。场景不宣称任务通关、
结算或主动退出；缺少可进入的原生任务或频道会失败，不会补造准入数据。

`--gift-requests-only` 的私有账号配置可以用 `[gift] body0` 至 `body3` 指定服主
已有的正常邮件命令，或继续用 `page0/position0` 指定机体赠送。收件人须填写原生
角色昵称。账号权限、命令处理和发放由服务器负责；测试仍等待独立处理回执，
不会直接发币、写库存或用发送成功代替到账。凭据与命令配置保留在本地。

服务端被动验证使用 `tools/game_test_server_report.py --run-dir build/gametest/pvp
--events <server-observer-events.jsonl> --host-pilot <角色ID>`。矩阵使用多个 Battle
主机时重复传入 `--events`。每个模式都必须具备同次运行、相同房名与角色、正确
队伍和完整四阶段原生生命周期，结果写入 `receipts/server-report.json`。
单场服务端摘要不能代替整个客户端矩阵的验证。

两个组件均作为开发类 Mod 接入 Mod Manager。Python 运行器是执行有限测试任务的
开发命令，不是玩家启动器或常驻服务。账号配置和运行回执不得放入分发包或源码仓库。

开发者明确需要可见录制时，`prepare` 加 `--visible --sfx-only`，构建 `game_test_audio` 目标，再运行
`python tools/game_test_record.py --run-dir build/gametest/pvp --output D:\Videos\SDGO-GameTest`。
仅测试副本关闭 BGM、启用音效。每场通过开局和 AI 断言后录制最多 25 秒，画面和声音
只取自该测试游戏进程。失败、静音或黑屏片段会清理；保留素材仍需检查交战情况与构图，
AI 断言通过本身不代表画面适合剪辑。


使用 `--room-titles-only` 可在真实登录后检查普通、AI、队伍和任务建房，以及当前三维等候室中可用的房内改名。
每个窗口通过实际原生编辑器输入 21 个键盘字符、
超过 20 字符的中文和 128 字符标题，再验证取消重开与服务端确认的完整文本。
中英文字符通过实际 Win32 EDIT 的 `WM_CHAR` 消息队列输入，让原生 IME 过滤器处理后，再读回游戏自己的编辑框文本；测试不使用文字赋值函数。
若原生好友聊天窗口延迟初始化并清空共用输入框，测试记录该原生调用，取消重开房名窗口后重新执行全部断言，最多重开两次。
测试观察器不修改输入上限；系统输入法候选窗和剪贴板不在此场景范围内。
当前客户端建房成功进入原生三维等候室；队伍视图保留原来的改名权限条件。
二进制中仍包含旧式二维队伍/协作设置窗口，但现代会话没有进入那些旧场景的正常路径。
这些旧控件保留 C++、原生布局和协议回归，不以构造隐藏服务或重放伪造回包宣称端到端通过。
内联改名框没有确认按钮。测试向其已核对的原生输入查询提供一次 Enter 或 Esc，随后仍由原生窗口处理、构包并等待真实服务器确认；其他窗口和按键查询不变。
复用本地原生 test 后端，使用 `prepare --room-titles-only --include-mod room_titles`，
明确指定测试回环地址、登录端口和已有账号配置，再使用 `run --timeout 1200`。
后续迭代使用停止的所属客户端及 `--reuse-client`，不重新还原数据库。
场景通过原生换机流程选择该账号已有的默认机体；配套服务端候选只安装到所属停止的 Agent/Battle 副本，
保留原有配置、合并符号并核对文件哈希。输入矩阵通过原生离房流程退出各个房间。
最后重新创建长房名 AI 房，验证真实开战回包和原生 `GAME_MAIN`，以及完整标题仍被保留；
此开战 smoke 不宣称战斗表现、结算或自然返回，结束时由运行器退出所属测试客户端。
完整运行要求后端已启用配套长房名
Agent/Battle Mod；窗口输入通过与服务端标题确认分别断言。该场景不代表所有模式的
战斗或双客户端显示验收。测试账号配置和回执继续留在本地，不进入玩家分发包。
