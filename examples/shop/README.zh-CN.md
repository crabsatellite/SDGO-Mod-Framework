# 商城导入模板：四类物品、五份作者样例

[English](README.md) · [商城操作](../../docs/shop-catalog.zh-CN.md)

本目录是通用模板和可运行的作者示例代码，不包含某个服务器的商品、机体素材或已分配编号。
`templates.json` 描述永久机体、租赁机体、副官、设计图和消耗品五种选择条件。
构建器只从目标目录已经在售、价格有效的原生单物品选项选择来源，沿用其价格、GB/MB 和期限／数量，
用现有商城 SDK 分配新的商品身份。所有新增样例均为 `enabled=false`，不执行 SQL、不发货、不改变当前销售方案。

每份也有独立的公开配方：[永久机体](recipes/unit-permanent.json)、[租赁机体](recipes/unit-rental.json)、
[副官](recipes/operator.json)、[设计图](recipes/blueprint.json)、[消耗品](recipes/consumable.json)。
只生成一种时传 `--recipes examples/shop/recipes/blueprint.json`；省略时按五份完整顺序生成。
尚未在售的新导入物品没有可继承的实际售价时，应在商城明确填写并审阅自己的交易条件，
不能把其他机体的价格猜作它的默认值。本工具的保守样例不会为这种情况补价或自动上架。

## 从目标目录生成可导入文件

在框架源码环境中使用现有工具读取自己的非玩家配置快照。以下路径只是占位；
`shop.json` 必须来自准备导入的同一个商城工作区，不要拿其他服主的 catalog 身份替换它。

```powershell
python tools/shop_catalog.py snapshot --server-ini "D:\Server\WorldServer\WorldServer.ini" --output "D:\PrivateShop\snapshot.json"
python examples/shop/build_examples.py --snapshot "D:\PrivateShop\snapshot.json" --catalog "D:\MyShopWorkspace\shop.json" --output "D:\PrivateShop\new-examples"
```

不下载原版、不恢复数据库，也不创建账号。快照和解析后的输出含目标服务器的实际定义，只保存在服主工作区；
公开框架只保存本目录的中性配方、代码和说明。输出目录必须尚不存在；缺少某类有效现货时整体拒绝生成，
不猜物品编号、不补任意低价、不降级为免费商品，也不覆盖已有作者输出。

## 每份样例是什么

```text
new-examples/
  examples.json
  01-unit-permanent/       # 永久机体
  02-unit-rental/          # 租赁机体
  03-operator/             # 副官
  04-blueprint/            # 设计图
  05-consumable/           # 消耗品
  *.shop-authoring.zip    # 各目录对应的作者交换 ZIP
```

每个编号目录包含：

- `shop.json`：可在当前工作区“商城商品 → 导入配置”读取的完整目录；保留已有商品及 catalog 身份。
- `catalog.ini`：同一目录的原生配置投影，供开发者检查；生成不等于应用。
- `selection.json`：读回所选目标现货的商品／选项／类别和交易条件，方便对照价格、币种与期限。
- `shop-example.json`：作者样例清单和逐文件 SHA-256，不是玩家安装器的 `manifest.json`。
- `README.md`：本份样例的应用边界。

第 2 份保留第 1 份样例，第 3 份保留前两份，以此类推；每一步只新增一个未启用商品。
可以只导入第 5 份得到全部五个草稿，或按顺序逐份查看。当前工作区发生新的作者编辑后，
应重新导出最新 `shop.json` 并在新目录生成，不能把旧快照反复导入来覆盖后续编辑。
同名样例 key 已存在时拒绝再次生成；不要删除已预约身份来重复分配编号。

## 导入、预览与明确上架

1. 解压作者 ZIP，在同一个商城工作区选择其 `shop.json`，使用“导入配置”，不是“游戏功能 → 导入配置包”。
2. 确认原有商品未改变，新增样例均未启用，交易条件与 `selection.json` 一致。
3. 在测试环境需要验证上架时，明确启用指定样例，保存后“应用到游戏”，再读回实际商品、子项和价格。
4. 在原生游戏中重开商城，检查名称、图标、价格、期限，打开购买／赠送预览后取消。
   这验证的是列表和预览，不是扣款发货；完整购买另需隔离环境的明确测试。
5. 下架该样例并应用，核对从在售列表消失；保留商品身份及玩家历史。

工具生成的普通玩家显示更新 ZIP 由 Manager 在应用后另行导出。它只安装客户端组件和显示数据，
不会替服主上架商品或执行服务端 SQL；上述作者 ZIP 也不能当玩家运行包安装。

正式服只保存这些模板和未启用草稿即可，不必为了查看例子新增在售商品。
若只是更新模板文件，无须改现有价格、概率、奖励或运行开关，也不需要重启游戏服务。
若打开原生商品预览时异常退出，应记录框架版本、商品和选项身份、触发步骤及日志，
先检查目标物品和客户端资源配对，不通过清空商城、重建玩家库或关闭所有 Mod 来代替定位。
