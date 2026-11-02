# 商城商品管理（Beta）

[English](shop-catalog.md)

服主在 SDGO Mod Manager 的「商城商品」面板中新增、查询、编辑、上架、下架和删除自己管理的商品。
玩家继续在游戏内原商城购买，不需要安装管理工具。商城中文选项与租赁标签由独立的[显示修复](shop-labels.zh-CN.md)负责。

`shop_catalog` 安装在 WorldServer（timestamp `548A86E3`、SizeOfImage `0010C000`），属于“联机与服主工具”分类，
只在 Full 包或显式选择商城功能时包含，未配置时默认关闭。`shop_labels` 属于基础修复，随 Base / Full 安装。

## 首次安装服务器组件

关闭目标 WorldServer，在解压包里运行安装器：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Install.ps1 -Target "D:\Server\WorldServer" -ConfigFile .\catalog.ini
```

初次 `catalog.ini` 可以是空商品表；没有导入配置时不启用模块。希望 WorldServer 继续运行时可加 `-DeferActivation`：
只添加尚不存在的 catalog 目录，不更新运行中的模块，DLL 在下次正常重启时加载，商品配置可以立即应用。

## 日常管理

1. 在「商城商品」面板设置现有商城配置目录、`WorldServer` 目录，以及可选的本机客户端目录（用于更新显示标签）。
   数据库连接沿用服主自己的宿主 INI，不在界面填写密码。
2. 在下拉框中选择原生物品，点击「新增商品」；商品编号自动分配。
3. 选中商品后设置商品名称、选项标签、售价、币种（GB / MB）、数量或租赁天数和是否上架，点击「保存商品」。
4. 新增、编辑、上架/下架和删除都先保存为草稿；点击「应用到游戏」并确认后才写入服务器，之后重新打开游戏商城查看。
5. 删除保留商品编号、购买历史和玩家已买物品；「恢复商品」可把删除的商品恢复后再编辑。

「导出配置 / 导入配置」处理可分发的商品 JSON，不含 IP、数据库密码或本机目录。导入同一 catalog 的配置时，
遗漏的旧商品转为已删除并保留编号；导入不能改写已有商品的身份。应用成功后生成的 UTF-16LE 显示标签会写入所选客户端，
朋友服通过 `update.bat` 分发给玩家；未应用的草稿不会混入标签。商品配置只由 Manager 发布，WorldServer 启动时不会自动应用。

## 规则与限制

- 普通商城币种编码为 **GB = 原生 1 / GP，MB = 原生 2 / GC**。售价必须为正整数（原生购买窗口会跳过零价选项）。
- 永久机体 validity=1；租赁为实际天数；其他物品遵循原生数量/有效期。机体必须可出击。
- 只支持原生已有类别，不包含多物品礼包，也不能凭配置创造新机体资源。
- 模块只管理自己拥有的原生商品行，事务应用，不替换原生 SQL 过程，不修改账号、余额、仓库和历史。
  新增 SKU 使用现有单物品模板并保留稳定 option ID，未知或已占用的编号拒绝覆盖，失败回滚。
- 外部工具修改了配置后，Manager 会拒绝覆盖。

## 卸载

在面板下架商品并应用，可只停止销售而保留模块。彻底卸载须先关闭目标 WorldServer：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Install.ps1 -Target "D:\Server\WorldServer" -Uninstall
```

卸载先下架本 catalog 的商品，再备份移出 Mod；数据库不可连接时保留模块供恢复。共享框架和其他模块保留。
