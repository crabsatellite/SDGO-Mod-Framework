# 活动扭蛋机 9301 币种显示修复

活动扭蛋机 9301 的客户端记录是 `Type=0, Price=99`，界面因此显示 `99 G币`；服务器
`tbl_ItemMachinePrice` 的同一机台是 `Type=2, Price=99`，实际按代币扣费、按代币退款。
所以修复只改客户端显示，不改服务器扣款。

## 运行方式

Mod 通过 named symbol 和 `HookCode` 挂接 `CapsuleMachineUI::LoadCapsuleItemData`。原函数完成后，
检查刚生成的活动机显示向量：

1. `begin <= end <= capacity`，记录大小为 179 字节；
2. 槽位数等于 `页数 × 8`，且不超过 4096；
3. 恰好一条 `MachineID=9301`，价格仍为 99，币种为已知错误值 0 或已正确的 2。

购买确认框不读取这份副本，而是重新查找 GridData type `0x3C`，从注册记录 `+9` 取币种、`+15` 取价格。
因此还要求条目的 Grid ID 为 `145093`、`grid.find` 返回该 type `0x3C` 记录、两处内存可写且 179 字节一致。
全部满足且币种为 0 时，才把显示副本和注册记录各一个 `Type` 字节改为 2；否则不做修改。

## 文件

- `capsule_currency_fix.cpp`：Hook 入口和内存检查。
- `capsule_currency_logic.*`：可单元测试的向量检查。
- `capsule_currency_fix_test.cpp`：补丁、幂等和各类异常记录的单元测试。
- `verify_source_fixture.py`：只读核对底包 `0099.gdt` 中 9301 的 `Type0/Price99` 前提。
- `symbol-proposal.ini`：可离线验证的符号提案，条目已并入 `symbols/GOnline-54BE1568-00C97000.ini`。

## 离线核对

从仓库根目录执行：

```powershell
python tools/verify_symbols.py "D:\Game\GOnline.exe" mods\capsule_currency_fix\symbol-proposal.ini
python mods\capsule_currency_fix\verify_source_fixture.py "D:\Extracted\gdrs\0099.gdt"
```

玩家安装和检查步骤见[使用说明](../../docs/capsule-currency-fix.zh-CN.md)。
