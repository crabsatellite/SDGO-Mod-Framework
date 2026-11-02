# 商城租赁与中文选项修复

[English](shop-labels.md) · [返回功能目录](../README.zh-CN.md)

`GOnline-54BE1568-00C97000` 的客户端修复。商城的销售选项名（例如泰文 `7วัน`）在当前客户端中会变成问号。
本修复在鼠标悬停详情、购买窗口和赠送窗口中明确显示「租赁 7 天」「永久」等标签，并可显示服主商城目录导出的中文选项名。
价格、币种、期限和购买流程不变。

| mod | 运行在 | 负责 |
|---|---|---|
| `shop_labels` | 客户端 | 整个功能 |

## 安装

它属于基础修复，随客户端框架一起安装；朋友服玩家通过 `update.bat` 获得。

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

只有选项 ID、类别、期限、价格和币种都匹配时才使用自定义名称，因此自定义文字不会遮盖实际的租赁状态；
不匹配时显示数值期限标签。

这不是全局字体或聊天编码修复，也无法恢复数据库里已经变成问号的商品名。活动扭蛋的币种显示由独立的
[活动扭蛋币种显示修复](capsule-currency-fix.zh-CN.md)处理。

生效时日志为 `rental/permanent labels enabled; <n> Unicode option overrides; prices and currency untouched`。
