# 活动扭蛋机 9301 币种显示修复

这个 Mod 只修复一个已确认的客户端显示错位：

- 活动扭蛋机 9301 的客户端记录是 `Type=0, Price=99`，所以界面显示 `99 G币`。
- 服务器 `tbl_ItemMachinePrice` 的同一机台是 `Type=2, Price=99`。
- `WorldServer.exe` 的 `CRequest::ReqPickItemMachine` 静态反编译表明，`Type=0`
  经 `SP_PocketGP_IO` 扣 G币，`Type=2` 经 `SP_GameCoin_IO` 扣代币；抽取失败时按同一币种退款。

因此正确修复是让 9301 的客户端界面显示 `99 代币`，而不是更改服务器的真实扣款。

## 运行方式

Mod 通过 core 的 named symbol 和 `HookCode` 挂接
`CapsuleMachineUI::LoadCapsuleItemData`。原函数完成后，Mod 检查它刚生成的
活动机显示向量副本：

1. `begin <= end <= capacity`；
2. 使用 179 字节记录边界；
3. 槽位数等于 `页数 × 8`，且不超过 4096；
4. 恰好存在一条 `MachineID=9301`；
5. 该条目仍为 `Price=99`；
6. 币种只允许已正确的 `Type=2`，或已知错误的 `Type=0`。

确认框并不读取该向量：它在 `0x00795BF0` 重新枚举 GridData type `0x3C`，
从注册记录的 `+9` 选择币种和钱包，`+15` 读取价格。只修显示副本无法修复确认框。

因此还要求列表记录的 Grid ID 为 `145093`，原生 `grid.find` 返回该 type `0x3C` 记录，
且注册记录全部 179 字节与列表副本完全一致、两处内存可写。
全部成立且币种为 0 时，才把显示副本和客户端内存注册记录各一个 `Type` 字节改成 2。
它不修改磁盘 `0099.gdt`、catalog overlay、服务器数据库、价格、奖池、玩家钱包数值、扣款或退款逻辑。
错误价格、未知枚举、重复 9301、损坏的 vector 或其他布局都会 fail closed，保留原数据。

## 文件

- `capsule_currency_fix.cpp`：原生 HookCode 入口和内存保护检查。
- `capsule_currency_logic.*`：可单元测试的严格向量检查；旧显示副本 helper 仅供回归，原生入口另外核对注册记录。
- `capsule_currency_fix_test.cpp`：覆盖补丁、幂等、错误价格、未知枚举、重复条目、
  缺失条目和错误页形状。
- `verify_source_fixture.py`：只读解密并核对当前底包 `0099.gdt` 的 SHA-256、
  93 条记录、12 页/96 槽形状，以及唯一 9301 的 `Type0/Price99` 前提。
- `symbol-proposal.ini`：可离线验证的 symbol 提案；对应条目已并入 `symbols/GOnline-54BE1568-00C97000.ini`。
- 本机逆向调查中保留 WorldServer 静态反编译证据；原始本地证据不随代码仓库分发，也不是运行时抽奖证明。

## 离线验证

从仓库根目录执行：

```powershell
python tools/verify_symbols.py `
  "D:\Game\GOnline.exe" `
  mods\capsule_currency_fix\symbol-proposal.ini

python mods\capsule_currency_fix\verify_source_fixture.py `
  "D:\Extracted\gdrs\0099.gdt"
```

单元 fixture 可由父级 CMake 目标把 `capsule_currency_logic.cpp` 与
`capsule_currency_fix_test.cpp` 一起编译。测试成功只证明记录检查和修改范围；
它不证明游戏内 Hook 已加载，也不证明真实账号扣款。游戏内进入活动扭蛋页和实际抽取由用户手动验收。

DLL 回放测试使用本机已部署表的 94 条解码记录和模拟原生注册 API / 列表对象，覆盖 96 个填充槽、
100000 GP 收藏机、注册记录同步、幂等，以及查找返回不同记录时拒绝写入，另外检查 Manager 导入和归档。
它实际执行 DLL detour，但不执行游戏原生确认控件；确认框读取路径来自静态证据，画面仍由用户验收。
共享扭蛋目录已安装不代表本修复已安装。安装和检查步骤见[修复说明](../../docs/capsule-currency-fix.zh-CN.md)；
游戏日志出现纠正结果后，才进入画面验收。
