# 活动扭蛋币种显示修复

把活动扭蛋机 9301 在列表和购买确认框中的 **99 G币** 纠正为 **99 代币**。服务器本来就按代币扣费
（`tbl_ItemMachinePrice` 为 `Type=2, Price=99`），只是客户端记录写成了 G币。
English: [capsule-currency-fix.md](capsule-currency-fix.md)

| mod | 运行在 | 负责 |
|---|---|---|
| `capsule_currency_fix` | 客户端 | 整个功能 |

## 使用

无需操作：它属于基础修复，随客户端框架一起安装；朋友服玩家通过 `update.bat` 获得。
只需要这一项修复的客户端，可在游戏关闭时用 Manager 的「导入客户端配置包」导入
`SDGO-CapsuleCurrencyFix-<版本>.zip`。它在 Manager 的「修复归档」中列出。安装后需重启客户端，
正在运行的游戏不会热加载。

它与商城中文选项修复、可选的扭蛋机编辑器相互独立，可以同时使用。

## 设置

无。

## 检查

1. 进入「活动扭蛋机」，93 号机（ID 9301）显示 **99 代币**，其他机台币种不变。
2. 打开它的购买确认框后取消，币种、价格以及当前/预计余额都应使用代币。
3. `sdgo\logs\sdgo.log` 中同时有 `capsule_currency_fix: loaded ...` 和一条结果行。

| 日志 | 含义 |
|---|---|
| `activity machine 9301 display currency corrected from type 0 ...` | 列表副本和客户端注册记录已纠正 |
| `... is already type 2 (token); no change made` | 已经正确，未做修改 |
| `activity machine 9301 display fix refused (<原因>)` | 某项检查未通过，未写入任何数据，需要排查 |
| `required symbol capsule.activity.load_items ...` | 客户端版本不受支持，本 Mod 不启用 |

只有 `loaded` 行只能说明 DLL 已加载，不能说明页面已纠正。

## 原理

Mod 挂接 `CapsuleMachineUI::LoadCapsuleItemData`。原函数生成活动机列表后，检查向量边界、
179 字节记录、`页数 × 8` 的槽位形状（不超过 4096）、唯一的 9301 条目、价格 99，以及币种为 0（错误）或 2（已正确）。

确认框不读取这份列表副本：它会重新查找 GridData type `0x3C`，从注册记录 `+9` 取币种、`+15` 取价格。
因此还要求该条目的 Grid ID 为 `145093`、原生 `grid.find` 返回同一条 type `0x3C` 记录、两处内存可写且
179 字节完全一致，之后才各修改一个币种字节。其他情况一律不改。

不修改磁盘 GDT、catalog overlay、服务器数据表、价格、奖池、钱包数值、扣款或退款逻辑。

## 验证范围

单元测试覆盖单字节修改、重复执行，以及重复、缺失、价格错误或未知币种记录的拒绝。DLL 测试用模拟注册表
实际执行 Hook，覆盖当前 94 台机器 / 96 槽列表和 100000 G币收藏机。独立包经过 Manager 导入和归档读回。
确认框读取路径来自静态分析；实际画面和真实抽取仍由玩家手动验收。
实现说明见 [mods/capsule_currency_fix/README.md](../mods/capsule_currency_fix/README.md)。
