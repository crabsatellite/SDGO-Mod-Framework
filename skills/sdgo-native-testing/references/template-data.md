# 公共测试模板与数据维护

仅在首次建立公共模板，或确证现有资产无法覆盖必要场景时使用本参考。平时只复制现有模板做测试。账号、机体和其他公共资产应保存在初始数据库中，不能让每个 agent 重走注册、昵称、新手流程和抽机体。

## 初次模板

`tools/game_test.py environment template` 是一次性引导工具。输入必须是已停止、归属明确、带完整 `backend.json`、11 个已校验 COPY_ONLY `.bak`、原生服务和启动 XML 的私有 prototype。不能指向玩家在线数据库或另一个活跃测试环境。

```powershell
python tools/game_test.py environment template --prototype <stopped-owned-prototype> --output <private-template-cache>/native-v1 --client-template <normal-owner-client> --stage build/stage/Release
```

允许输出根为本 worktree 的 `build/game-test-templates` 或 `%LOCALAPPDATA%\SDGO-GameTest\templates`。已存在的目标会拒绝覆盖；模板不是每轮测试的产物。

首次引导在私有 SQL 副本中使用原生 `sp_Account_Regist`、`sp_UpdateUserInfo`、`sp_CreateUserUnit` 建立五个声明的 fixture 账号，补齐昵称/教程完成/默认出击/测试钱包。五账号各有公共机体清单，原生 UID/序列号由过程分配。核验归属、默认 UID 和账号数量后，重新 COPY_ONLY 备份、VERIFYONLY，封存文件哈希。账号凭据仅留私有配置。真实玩家数据和原始快照不被改写。

这不是常规失败的重试方法；创建完初始模板后，之后的测试和模板修订都继续用这五个身份。不要生成新的随机账号替换登录失败的旧账号。

## 如何增加新的公共数据

若已有五账号模板缺少原生新手战舰/合法编组/出击操作员，可运行明确的维护命令：

```powershell
python tools/game_test.py environment upgrade --template <existing-template> --output <private-template-cache>/native-v3
```

它只还原旧模板、调用原生新手战舰/编组/出击及已有操作员选择存储过程、封存新版并清理；保留原来的五个账号和每台机体 UID，新增账号数为零。编组按原生 Star/MinCost/MaxCost 选择已有机体，优先四台；大型 MA 的容量限制允许更少成员。此命令不能用来给普通测试临时补库存；已有完整模板就直接测试。

先确认模板实际缺少什么：机体 ID、必需的原始模型/表项、库存类型或某场景资源。已有数据足够时直接选择对应账号/机体做测试，停止数据维护。

需要扩充时，以现有 sealed 模板为基线制作新版本，保留旧版本和五个账号身份：

1. 通过相同 `Registry`/`ManagedEnvironment` 租约机制分配一个临时维护副本，使用 `NativeDriver.start_sql` 和 `restore` 还原现有模板的 11 库。复用模板 `accounts` 和私有账号配置；**不要调用 `build_accounts`，也不要把初次引导命令当作模板升级命令**。除上述原生战舰补齐外，其他新增数据需要写一个有明确清单的受控维护脚本，没有通用的任意 `add-data` 入口。
2. 用已有账号 ID 和原生存储过程加所需数据。机体用 `GOnlineGame.dbo.sp_CreateUserUnit` 分配 UID/序列号，先查 `tbl_ItemCategory` 的真实 CatalogueID/RefIndex/TimeLimit；不能复制其他玩家记录或随意修改全局 identity。模型、资源、Mod 配置同步进新版私有模板的相应目录。
3. 将模板 `accounts[*].units` 清单与实际库存对应，保留原有角色、默认单位和 UID，除非新场景明确需要改默认。当前 `ROSTER` 是精确核验；新增公共机体时应同时更新声明清单和版本兼容校验，而不是关闭 `verify_accounts`。确认仍为五个既有账号，新增数为零，旧资产未丢失。
4. 对 11 库做新的 COPY_ONLY 备份、CHECKSUM/VERIFYONLY，重算快照及全部文件哈希，分配新的模板版本/ID和独立输出路径，明确记录变更的数据、原因、基线和验证结果。新模板应通过现有五账号核验和真实登录/机库读取，再运行受影响的完整场景。
5. 只在验证通过后登记新版作为默认。旧模板保持可复现。`ManagedEnvironment` 负责停止临时进程、删除拥有的 SQL 容器/卷及临时目录；检查 `cleanup.json`/registry 的 closed 状态。公共模板在临时根之外，不能随环境清除。成功测试和成功模板维护的记录在校验清理结果后删除；失败诊断只临时保留到问题解决。

脚本、schema、fixture 清单与教程可以提交 Mod 源码库。原始服务程序、模型资源、数据库备份、账号配置、密码/哈希和真实玩家记录只能留本地私有模板，不提交源码或附在报告中。完成扩充后回到测试任务，避免把测试请求扩大为不必要的功能开发。
