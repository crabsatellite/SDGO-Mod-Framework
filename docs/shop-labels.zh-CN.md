# 商城租赁与中文选项修复

[English](shop-labels.md) · [返回功能目录](../README.zh-CN.md)

`GOnline-54BE1568-00C97000` 的客户端修复。商城的销售选项名（例如泰文 `7วัน`）在当前客户端中会变成问号。
本修复在鼠标悬停详情、购买窗口和赠送窗口中明确显示「租赁 7 天」「永久」等标签，并可显示服主商城目录导出的中文选项名。
价格、币种、期限和购买流程不变。

| mod | 运行在 | 负责 |
|---|---|---|
| `shop_labels` | 客户端 | 整个功能 |

## 安装

它属于基础修复，随客户端框架一起安装，玩家通过服主提供的客户端包或更新获得。

也可以使用独立的 `SDGO-ShopLabels-Patch` 包。解压到任意目录，关闭**目标目录里的**游戏，然后运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Install.ps1 -Target "D:\Game"
```

已安装框架时只添加本 Mod 和它的符号段，保留现有 core、`sdgo.ini`、其他 Mod 和符号；未安装框架时补齐包内的
loader/core，已有的第三方 `d3d9.dll` 会被串联。目标版本不匹配时不会安装。

卸载本 Mod（配置移到 `sdgo/shop-backups/`，共享框架和其他 Mod 保留）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Install.ps1 -Target "D:\Game" -Uninstall
```

## 设置

在 SDGO Mod Manager 的“商城物品显示”中修改，保存后重启游戏。

| Manager 名称 | 键 | 默认值 | 含义 |
|---|---|---|---|
| 显示租赁物品标签 | `ShowRental` | 1 | 显示实际租赁天数 |
| 显示永久物品标签 | `ShowPermanent` | 1 | 永久机体明确标为永久 |
| 使用自定义中文名称 | `UseUnicodeOverrides` | 1 | 从 `display.ini` 读取选项名 |

`display.ini` 是[商城商品管理](shop-catalog.zh-CN.md)导出的 UTF-16LE 文件。使用独立包时，也可以安装时直接导入：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Install.ps1 -Target "D:\Game" -ConfigFile "D:\Share\runtime-display.ini"
```

某个选项启用了租赁或永久标签时，显示该标签；否则才使用自定义名称，且仅在选项 ID、类别、期限、价格和币种都仍然匹配时使用。

这不是全局字体或聊天编码修复，也无法恢复数据库里已经变成问号的商品名。活动扭蛋的币种显示由独立的
[活动扭蛋币种显示修复](capsule-currency-fix.zh-CN.md)处理。

生效时日志为 `rental/permanent labels enabled; <n> Unicode option overrides; prices and currency untouched`。

## 开发者说明

原生 GridData 查询只通过 `AL` 返回一个字节，`EAX` 高位可能残留指针值，调用声明必须使用字节返回类型。

客户端用代码页 874（泰文）转换销售选项名，因此变成问号。Mod 挂钩 `shop.buy_options.populate`、
`shop.gift_options.populate` 和 `shop.tooltip.option` 记录正在绘制的选项行，再挂钩 `text.ansi_to_wide`，只替换这些行的
CP874 转换。租赁标志来自选项类别的客户端元数据（GridData type `0x22` 的 `+0x6D` 字节，通过 `shop_labels.grid_data` / `shop_labels.grid_find` 查询），
期限来自选项的数值有效期。其它文字转换交回原函数。

数量型物品直接显示数值数量；悬停价格表使用「租赁7天」「30天」等紧凑文字，购买/赠送下拉框保留完整标签和原生格式化价格。
