# 新账号起始资料修复

[English](new-account-profile.md) · [返回功能目录](../README.zh-CN.md)

`WorldServer-548A86E3-0010C000` 的服务端修复，默认启用。原服注册新账号会发放 9,999,999 GP 和点券，
初次建档又把军衔写成 15/1。启用后，新玩家以 **5,000 GP、0 点券、下士 1 阶** 起步，首次创建角色时
服务器自动赠送的机体 **最多保留一台**。玩家客户端无需安装。

## 安装

停止 World 服务器，用 World 角色安装服务器包，然后重新启动：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File install_server.ps1 -ServerRoot D:\SDGO\WorldServer -Role World
```

每个负责创建新玩家的 World 实例都需要安装；Battle、Agent、Store 服务器和玩家客户端不需要。
安装后检查 `sdgo/logs/sdgo.log` 中有 `new-account defaults active: ...`。

## 设置

在 SDGO Mod Manager 的“新账号默认配置”中修改，保存后重启 World。配置保存在
`sdgo/mods/new_account_profile/config.ini`。

| Manager 名称 | 键 | 默认值 | 含义 |
|---|---|---|---|
| 初始 GB | `InitialGP` | 5000 | 初始 GP |
| 初始 MB | `InitialCash` | 0 | 初始点券 |
| 初始军衔 | `RankNumber` | 5 | 初始军衔，Manager 中可选 1..15 |
| 初始军衔子等级 | `RankStep` | 1 | 初始子等级，1..5 |
| 初始机体数量上限 | `InitialUnitLimit` | 1 | 首次建档时保留的自动赠送机体数 |

起始经验从服务器 `tbl_Levelname` 中对应军衔和子等级的行读取，该行必须存在。

`InitialUnitLimit` **不是**玩家最多能拥有的机体数。若服务器在首次创建角色时一次自动赠送 100 台，
设为 `1` 保留一台，设为 `0` 全部不保留。建档前已有的机体、正常的初始机体选择，以及之后的购买、扭蛋和奖励
都不受影响。服务器在这一步本来不送机体时，Mod 也不会补送。

## 修改内容

- 新账号注册时使用配置的资金。原服首次登录需要重试的行为保持不变。
- 首次创建角色时设置配置的军衔、对应经验和资金；过去已注册但从未建档的账号同样适用。
- 已有角色不会被重置，资金、经验、军衔、机体和奖励保持原样。
- 创建失败时整体回滚，不会留下初始化一半的角色。
- 服务器数据库会话不可用时，创建以数据库错误失败，不会退回原服的 9,999,999 赠送。

原服的初始道具、技能和副官保留。Mod 不修改存储过程本身，而是在已核对的 World 调用外围用数据库事务写入配置值。

Mod 只从 World INI 读取 `[DB StoreBilling] DBName`，复用服务器现有的数据库会话，不读取密码。
Common、Game、Billing 数据库需与原服一样位于同一 SQL 实例。

## 关闭

在 Manager 中关闭它的开关并重启 World，之后的新账号恢复原服规则，已有角色保持现状。
`install_server.ps1 -Uninstall` 会卸载整个框架，不是只卸载本 Mod。

## 开发测试

`ctest` 运行 `new_account_policy_test`。SQL 测试需要一个可丢弃的 SQL Server 容器：

```powershell
python tests/run_new_account_sql_test.py --container <isolated-sql-container> --credential-file <local-private.json> --out build/starter-sql-check
```
