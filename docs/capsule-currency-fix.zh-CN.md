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

## 原理

活动机列表加载完成后，Mod 找到 9301 这一条，确认它仍是 99 价格、记录格式符合预期，再把列表和购买确认框
共同读取的客户端内存记录中的币种改为代币。数据不符合预期时不做任何修改。

只改客户端内存中的显示，不修改游戏文件、服务器数据、价格、奖池或扣款逻辑。
开发细节见 [mods/capsule_currency_fix/README.md](../mods/capsule_currency_fix/README.md)。
