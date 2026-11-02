# 军衔回退修复

[English](rank-progression-fix.md) · [返回功能目录](../README.zh-CN.md)

`WorldServer-548A86E3-0010C000` 的基础修复：战斗结算不再降低角色军衔。玩家无需更新客户端。

## 问题

原生过程 `dbo.sp_SetBattleExp_V4`、`dbo.sp_SetGridBattleExp_V2` 判断是否变更军衔的条件写错了：
经验对应军衔的子等级更高时，会覆盖更高的大军衔。例如军衔 **15-1**、经验 30,090 的角色获得 20 经验后变成 **5-2**。

## 安装

停止目标 WorldServer，用 World 角色安装服务器包，然后按原方式启动：

```powershell
powershell -ExecutionPolicy Bypass -File .\install_server.ps1 -ServerRoot "D:\SDGO\Server\WorldServer" -Role World
```

本修复默认启用，在 SDGO Mod Manager 的“修复归档”中列出。每个参与战斗结算的 WorldServer 都需要加载它，
仅共用数据库是不够的。

Mod 只读取 WorldServer `[DB Game]` 中的数据库名，复用服务器现有的数据库会话，不读取密码。
该数据库账号需要能读取两个原生过程，并创建 Mod 自己的 schema、状态表和过程。

## 修改内容

第一次遇到对应的战斗结算时，Mod 在自己的 schema `sdgo_rank_progression_fix` 下生成两个过程的修正副本，
只改动军衔比较：

```sql
IF(@UserLevelNumber < @newlevel OR
   (@UserLevelNumber = @newlevel AND @UserLevelStep < @NewLevelStep))
```

之后这两类结算调用改走修正副本。经验、奖励、战斗次数、正常晋级、原生 15-5 与排名军衔限制以及排行榜重算保持不变。
原生 `dbo` 过程不被修改。安装前已经丢失的军衔不会自动恢复。

## 日志

| 日志 | 含义 |
|---|---|
| `rank progression routing installed ...` | 已加载，等待第一次战斗结算 |
| `rank policy active for normal battle` / `Grid/Tag` | 该类结算已改用修正过程 |
| `rank policy unavailable; native command retained: ...` | 修正副本未能准备，本次调用仍使用原生过程 |
| `host INI needs [DB Game] DBName ...` | WorldServer INI 缺少数据库名，本 Mod 不生效 |

## 关闭

在 Manager 点“显示开发信息”，于“修复归档”关闭开关并重启 WorldServer。之后结算重新使用原生过程；
Mod 留在数据库里的闲置 SQL 对象可以保留。

## 开发者说明

Hook 符号为 `rank.execute`、`rank.execute_rows`、`rank.connection_layout`、`rank.command.battle` 和
`rank.command.grid`。只有两个完全匹配的原生命令字符串会被重定向；参数绑定、输出处理、经验、奖励和战斗次数
仍由原生流程处理，调用后恢复命令描述中的原始指针。

副本由当前数据库中的原生定义生成，只替换过程名称和军衔比较式。两个过程在同一事务和应用锁内准备，
检查归属、比较式版本、定义指纹和 SQL 选项；未知定义、他人的同名 `sdgo_rank_progression_fix` schema 或被外部修改的副本
都会拒绝覆盖。分发包不携带原生 SQL。借用会话上已有的事务不会被 Mod 提交或回滚。
## 开发测试

`ctest` 运行离线 C++ 测试。可选的数据库测试会创建隔离的临时库并使用合成角色，需要两个原生过程定义的备份：

```powershell
powershell -ExecutionPolicy Bypass -File tests\rank_progression_db_test.ps1 -ServerIni "D:\SDGO\Server\WorldServer\WorldServer.ini" -OriginalBackup "D:\Maintenance\original-rank-procedures.json" -BuildDir build
```
