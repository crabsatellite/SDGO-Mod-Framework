# 原生机体包接口

[返回机体族导入说明](unit-import.zh-CN.md)

任何来源按协议做好的包均可提交导入；是否可用取决于协议校验和当前支持的运行能力。来源拆包和来源格式转换不属于公开框架。

本接口用于 SDGO Mod Manager 的机体内容制作。通用工具不附带具体机体、原始记录或模型；
服主提供的数据、原生模型截图与发布内容保存在自己的工作区。

## 机体资料项目与共享素材

`sdgo.unit-project` / `schema_version: 1` 是制作项目协议，独立于玩家运行包。
可以先创建不含机体记录的 `metadata-only` 项目，填写资料和上传图片；此时不会登记机体或修改宿主。
后续制作包可同时包含原始记录、typed definitions、资料与素材，不要求素材一直保留在原电脑的路径上。

项目根 `project.json` 声明稳定 `project_key`、`name`、`metadata`、`media`、`assets`、
制作 definitions、presentation 和精确 `files` SHA-256 清单。
`assets/<sha>.png` 携带原图及规范化制作图，所有素材都必须在当前项目内且哈希匹配。
声明自身不列入自己的hash清单；缺文件、重复身份、资源被修改或外部素材指针会拒收。
制作项目不包含 DLL、主机连接配置、账户、运行收据或临时绝对路径；不能把它作为运行包安装。
再次遇到相同稳定项目号时要求明确更新，不默默覆盖作者项目。

`metadata` 提供 `description`、`short_description`、`gameplay_notes`、`aliases`、
`search_examples`、`tags`、`series`、`model_number`、`english_name`、`sources` 和可选的
`declared_unit_id`。介绍与本服说明分开；资料出处记录标题、网址及官方/作者/参考类型。
`visibility` 为 `private`：这里的具体机体与服主资料只进入作者内容，不进入通用框架默认数据。
搜索使用名称、简介、别名、作品、型号和标签；示例用于提示，不凭示例制造并不存在的匹配。
这些字段是 Manager 元数据，不能当作已证明的原生图鉴字段或技能行为。

`media.activity_poster` 同时引用规范图 `asset_sha` 和原图 `source_sha`，保留来源与文件名。
既有 shop/capsule 图片通道同样在项目素材库可用。活动图通过共用生成器生成361×322派生版，原图保留。

Manager 提供：

- `POST /api/providers/units/api/units/projects` 创建资料项目。
- `GET /api/providers/units/api/units/projects` 读取可检索的项目素材库。
- `GET /api/providers/units/api/units/drafts/<id>/project` 导出完整制作项目。
- 既有 asset 接口的 `role=activity_poster` 上传海报。
- 扭蛋侧 `GET /api/providers/capsules/api/projects` 复用相同目录；
  `POST /api/providers/capsules/api/projects/media` 将选定版本的图片及元数据复制进扭蛋制作目录，
  随其语义导出携带。来源版本与素材哈希不符即拒绝。

素材可在机体尚未完成登记时用于制作；此操作不会将其加入可抽奖池。
`usable_for_authoring` 与宿主定义、运行验证分别声明，不把资料可读当作机体已可玩。
独立活动机草稿按同等级、同货币原生机台的均值提供售价、稀有奖配置频率与陪跑奖励建议，
保留来源机台与精确有理数。原生序列、重复回避与出货限制仍另行验证，不能将配置频率冒充独立抽样概率。
生成草稿不编译安装、不上架；修改海报会使已关联草稿需要重新核对素材。

## 制作输入

RAR/ZIP 中提供原始机体 BIN（每条 4976 字节）、武器 BIN（每条 1871 字节）及其原生资源。
编号、名称、变形目标和已知字段从记录内部读取，不用文件名代替身份。
文件夹可包含 `mdrs`、`anrs`、`txrs`、`fxrs`、`sprs`、`sdrs`、`scp`；
记录保留未知字节，直接依赖必须能从包或所选客户端解析到。不会运行包内安装程序或数据库脚本。

主包上限 32 MiB，展开上限 128 MiB、512 个文件。上传 PNG 上限 8 MiB，边长 1–2048。
重复身份、路径穿越、链接、损坏核心布局和缺少核心模型会被拒收。新 ID、资源或旧武器版本冲突
不会被隐式覆盖，也不通过二进制全文替换来盲目换号。

### 分片资源制作库

大资源库可使用 `sdgo:authoring-resource-depot-index/1`，由
`owner.units.resource_depot.ResourceDepot` 读取。入口 `index.json` 必须绑定外部确认的 SHA-256；
元数据采用带独立摘要、行数和有序选择区间的 JSONL 页，每页最多 1 MiB。
资源使用 `blobs/<sha256>.bin`，每片最多 4096 文件、48 MiB 原始字节和 64 MiB 压缩字节。
读取器先校验整片，再流式校验所需 blob；最多缓存四个元数据页和两个压缩分片，
不把完整制作库或原始资源同时读入内存，也不按 ZIP 路径直接展开文件。

同一逻辑路径的不同内容保留各个版本，服主资源域编译时需明确别名，不能按最后出现的版本覆盖。
基线复用声明仍需在实际目标上重新核验字节。原始 SCP 只作为不透明源资源运输，
执行或登记需要独立的目标脚本注册协议；资源库不得签发编号、SQL、原生登记或可玩性权限。
这个开发者读取接口只提供制作数据；它尚不等于 Manager 导入入口、运行时多归档安装器或机体验收。

### 技能与浮游记录 profile

`owner.units.profiles.skill_target_1132` 验证 kind 13 的882→1132字节投影，保留完整源／目标哈希和逆变换。
其独立 `validate_relocated_record` 接口使用 `sdgo.native-type13-relocated-record/1`：明确绑定源、目标技能编号，
默认只允许投影后目标记录的前4字节换号，并保留源、原投影、最终目标三份哈希；原有未换号接口的协议不变。
可选的 `linked_skill_rewrite` 仅接受明确的源／目标关联技能编号，固定验证源偏移304与目标偏移354，
恢复关联编号后仍逐字节验证完整投影；调用者不能指定其他改写位置。
`owner.units.profiles.funnel_target_732` 验证 kind 11 的完整732字节源／目标记录；换号只允许改动开头的
32位编号，其他字段保留原字节，不能根据字段名猜测转换。两种 profile 均不发放原生注册权限。

它们的 `admission.native_registration_blocked` 保持 true。制作数据的布局、哈希或换号通过，
不能代替资源与服务端依赖闭包、原生对象初始化、战斗行为及退出／变形／死亡后的生命周期验收。
未知的技能调度值保留在源数据中并阻止登记，不自动清零或截断。Manager 的正式发布路径仍要求完整的消费者准入。

## 服务端 definitions JSON

补充定义接口使用 `schema_version: 1`，允许以下顶层字段：

- `root_unit_id`：明确选择进入机库的根形态，必须属于该包。
- `behavior_profile`：`native-single-form-v1` 或 `native-linked-forms-v1`。
- `item_template_id`：客户端和服务端都存在的永久机体物品类别模板。
- `units`、`weapons`：按内部编号匹配的类型化服务端字段。

[最小字段示例](examples/unit-family.example.draft.json) 只演示结构，不含可安装记录或默认机体。
完整列名、类型和范围以 `owner/units/model.py` 的 `schema()` 与 `field_value()` 为准，
实际 SQL 列校验在 `owner/units/repository.py`。缺少字段保持为缺项，由服主填入或明确选模板补齐；
模板仅补空值，不用模板覆盖已恢复的源数据。`UserGradeLimit` 保留 SQL `int` 范围，不按字节截断。

单形态要求变形目标为零。当前双形态 profile 要求完整、互相匹配的两条关系；
三形态以上的有向图需要新的 profile，不会删掉多余形态后当作兼容。
库存根形态在发布后保持稳定。原始 BIN/RAR 始终保留，编译只写回已验证的共享字段。

技能依赖默认必须存在。只有已验证可同步编码的技能 1/2 支持服主逐项明确移除，效果会丢失并记录来源；
不凭编号编造技能，不自动清空未知技能或脚本。新增依赖 GDT、技能/脚本行为及版本资源域需单独扩展适配器。

## 客户端内容与展示

### 设计图制作声明

definitions 可选字段 `blueprints` 接受设计图列表；每个机体族最多16份。
服务端接口仍使用 `schema_version: 1`。读取导入面板的
`capabilities.blueprints` 判断制作和原生注册是否开放，不通过修改 source-only 标记跳过门禁。
`native_registration` 表示使用宿主兼容模板的结构化配方发布能力；此路径支持原生编译、事务登记及原生合成。
完整上传展示模板独立使用 `client_templates.native_registration_authorized`，当前仍为 false，
未完成目标资源和消费验证的完整模板只能保存和导出，不能确认发布。

每份声明提供：

- `schema_version: 1`、`id`、`enabled`、`name`、`output_unit_id`。
- `source.kind` 为 `source-recovered`、`authored` 或 `unknown`。恢复项还须提供
  `domain`、`record_id`、`record_sha256`；未知项保留在草稿，不能作为完整来源发布。
- `recipe_template_id` 与 `item_template_id` 是宿主现有的配方／图纸物品兼容 donor。
  它们与机体的永久物品模板不同，也与下面上传的客户端展示记录分开；
  donor 不会被上传记录替换，不用于套用另一台机体的材料或战斗数值。
- 可选 `recipe_sprite` 指定配方卡面的 `sprs/名称.spr` 路径，最多29个ASCII字符。
  上传原始配方时保留其卡面引用。该资源及依赖必须来自批准的项目文件或当前客户端；
  缺失时阻止发布，不静默换成模板图片。作者不指定此字段时，明确沿用所选显示模板。
- `item_category_id` 是新的图纸类别。未指定目标ID时，工作区从独立范围分配配方和图纸类别，
  删除未发布草稿也不会复用已预留的身份。
- `materials` 发布使用3至5个有序 `{unit_id, minimum_level}`。原始6/7槽保留为未开放的来源声明。首项为原生关键机体；
  原生合成保留它并按原有规则调整等级，其余材料消耗。当前不接受重复材料ID或产物自引用。
- `minimum_player_level`、`repay_point` 必须明确填写；零保持为零，缺项不自动补零。
  `RepayPoint` 是原生删除图纸的返回点数，不是本扩展定义的合成收费。
- `internal_name` 为目标SQL可无损保存的ASCII内部名称；`name` 用于客户端本地化展示。
  多份启用配方必须明确选定一份 `encyclopedia_primary`；它只选择图鉴展示关联，不能证明图纸可以获得。

可选的 `client_templates` 为声明自身绑定一份完整客户端展示清单：

```json
{
  "client_templates": {
    "manifest_path": "blueprints/209000/client-templates.json",
    "manifest_sha256": "<lowercase sha256>"
  }
}
```

清单协议为 `sdgo.unit-blueprint-client-templates/1` 或 `/2`，包含 `owner`、两份 `templates` 和
完整 `resources`。两份 typed 记录必须明确为 kind 33 / 1197字节的
`client_recipe_template` 与 kind 34 / 614字节的 `client_item_template`；
不能按长度把614字节记录猜成 kind 85。每份模板分别绑定不同的 `source_path`、
`prepared_path`、源／准备 SHA-256、源记录编号和审核过的 SPR 改写区间。
kind 34 可选 `display_name_mode: preserve-template` 保留图纸物品自己的名称，或用
`explicit` 携带明确名称；配方名称与图纸物品名称互不强制相同。

准备记录只能在清单批准的 SPR 载体区间与源记录不同；项目导入时逐字节确认其他区域不变。
编译从准备记录出发，只写目标编号、配方名称、材料数量／编号／等级、产物、最低玩家等级、
RepayPoint，以及图纸物品的目标编号、可选明确名称和配方引用。每条输出记录给出源、准备、
编译 SHA-256 与连续差异区间收据，未知字节必须保留。

`resources` 为每个直接引用路径绑定项目成员和 SHA-256；项目导出／再导入必须保留清单、
两对完整记录及资源原字节，缺项、修改或跨声明冲突均拒收。版本1只证明记录可见的直接引用和
审核改写，`resource_closure_verified` 保持 false。

版本2额外要求 `resource_dependencies` 列表，每项为 `{path, member, sha256}`，仅接受 TXR。
验证器从准备记录的 SPR 出发，逐帧读取原生10000/11000布局的第一个纹理载体，检查全部 TXR 的
哈希并解码 PNG、DDS 或 TGA（含 `ZOATEX1` 包装）。第二个字符串和未分配字段保留为不透明证据，
不根据看似路径的内容推断依赖。缺纹理、多余依赖、成员重用、冲突、非法 UV 或超预算均拒绝。
单纹理最多16,777,216像素，闭包最多67,108,864像素、128份资源和128MiB原始字节。

派生的 `runtime_resources` 仅包含准备记录的 SPR 及其纹理；原始版本专用素材随制作项目保留，
不会自动安装为游戏资源。版本2可返回 `resource_closure_verified: true`，但这只覆盖设计图展示资源的
静态闭包，不证明模型、动画、特效、原生交互或战斗行为。
独立验证结果的 `native_registration_authorized` 和 `native_runtime_accepted` 仍为 false。
发布编译器还须检查框架支持的原生 profile：当前只开放验证通过的版本2；版本1拒绝编译为运行包。
编译成功的发布收据可标记 `native_registration_authorized: true`，但
`native_runtime_accepted` 仍为 false，不能用通用消费者测试替代每份包自身的实机验收。
修改清单标志不能跳过既有资源要求或原生登记门禁。
旧包没有 `client_templates` 时仍沿用 donor 编译路径。

对应 schema 与检查在 `owner/units/blueprints.py`；目标1197字节布局适配在
`owner/units/blueprint_codec.py`。GDT容器的第三word不能当作逻辑记录kind。
新增设计图的非玩家定义计划包含 `tbl_MixTreeFile`、`tbl_MixTreeInfo` 和材料列表。
不能调用会清空全局派生表的旧重建过程来安装单个作者配方。
已发布配方可停用并保留已持有图纸所需登记，不能通过硬删除草稿破坏玩家资产。

### 获取途径预览

`GET /api/providers/units/api/units/acquisition` 返回已确认机库根形态的获取建议和来源版本，
不改变机台、价格、概率或玩家库存。分析复用扭蛋模块的最小闭包，图纸物品与全部材料均需可获取；
无起点的循环配方不会自行变为可达。变形态不能当作另一台可持有材料。

新增的 S 级及以上库存根分别生成独立活动机台意图；A/B/C 优先完整图纸路线，否则使用既有
原生等级组合和概率模板进入普通扭蛋。已存在的普通扭蛋获取可复用。
源数据、配置可达、客户端登记及实际合成/抽取验收分别记录。

确认后输出的 `definitions.json` 是 `sdgo.unit-family` 发布声明，不是制作阶段的 definitions 补丁。
客户端原生目录使用 `SDGOUN1` 容器、类型化记录、长度与校验值，安装器核对完整文件清单和所有权。
发布包同时携带安装入口使用的根符号片段与 Mod 目录中的完整 `symbols.fragment.ini`，两者字节必须一致。
局部符号随 Mod 安装并由框架校验，不能只更新 DLL 或只追加旧的表加载符号。
机体、武器、物品类别分别使用原生 kind `2`、`10`、`0x22`。
机体一览使用 `0220.gdt` 对应的 kind `0x55`，614 字节，按原生连续 global ID 枚举；
每个库存根一条，`+6` 为机体 ID，`+10` 为可选的 kind `0x21` 设计图 ID。
无对应设计图时使用原生空配方分支，不借用其他机体的设计图。

原生游戏不会从工作目录自动读取新增模型。编译器在 `unit_catalog` 运行目录生成
`resource-overlay.ini`、`resources.zdx` 和 `resources.zpk`，使用既有原生归档编码器；
INI 的 schema 为 1，分别声明 index/archive 文件名及 SHA-256。归档只包含清单中批准的
模型、动作、纹理、特效、图片、声音和游戏脚本资源。安装器核对归档条目与 `owned_paths` 的精确对应。
松散副本只供工具读取，游戏不会读取它们。

机体目录独立启用时自行提供资源读取；与扭蛋目录同时启用时复用单一资源 broker，
通过版本化 `SdgoGetResourceOverlayBrokerV1` 接口登记独立资源包。
每包保留自己的资源类型范围和哈希，路径重叠会拒绝登记。旧版扭蛋目录没有该接口时，
需先更新框架；不会叠加冲突 hook 或跳过资源登记继续注入机体表。

普通商城八行列表读取 kind `0x55` 条目 `+14` 的已持有图标，画布为 122×122；导入页的商品图标预览与编译器共用这一输出。
三处机体图片消费者分别是记录 `4654`（另一列表图及新增图鉴图标源图）、`4754`（SecretStore 二维详情图）和 `4854`（详情/自定义候选图）。
它们不是同一个显示字段。固定扭蛋卡面调用既有卡框生成器，自定义候选的等级框由原生控件绘制。
一览另有 122×122 的正常/未持有图标。借用的原版图片需要人工核对是否仍对应新模型；
没有合适图片时，可以使用原生模型准备/选择动作截图，再通过图片通道生成相应画布。

图标确认记录当前内容版本及生成图片哈希；可选的游戏内预览调用回执和 GameTest 结果另行记录。

## 事务与获取模块

在线确认在应用锁和同一 SQL 事务中更新非玩家机体/武器/名称/物品类别定义与本模块账本。
启用收集扭蛋时同时登记机体族根形态。外部修改、缺失引用、有损字符串和读取不一致会拒绝提交。
数据已提交但本地发布未完成时，从事务收据恢复。

导入不发放机体、不创建商品、不修改机台槽位、价格或概率。既有商城和扭蛋模块消费已确认的同版登记，
依据同一游戏数据库指纹核对来源。服主通过这些模块配置获取途径，然后分发配套客户端内容。
个人设置、连接凭据、真实玩家数据与整套游戏文件不进入通用工具或内容源码仓库。

## 扩展接口

活动制作数据属于胶囊目录协议：`project_assets` 携带项目资料与已校验的 PNG，
`activity_drafts` 保存尚未应用的模板草稿，`activity_machines` 保存审核后的活动机台。
机台分别绑定稳定的 `project_key`、机库根 ID、原生机体物品类别及海报哈希，
可记录 `gp_rare`、`token_rare` 或 `token_99` 模板身份。模板不是默认发行的机体内容。
`activity_preferences` 可携带各类模板的服主价格及目标频率覆盖；未设置时查询宿主原生参考。
它与机体、奖池和图片一样属于服主数据，通用发行物不携带任何服主的具体配置。
服务端投影包含 schema、机台/根形态/项目身份、修订、货币、价格、原生状态模板及奖池；
每个奖项用 `category_id`、`amount`、`entries` 表达，类别与数量组合唯一，频率合计 10000。
同类别各数量档合计最多 5000；机体、图纸和已登记的原生消耗品分别沿用 11、15、22 类发放路径。
客户端投影通过原生 0099 活动表和数字 SPR/TXR 引用展示项目海报。
安装器核对各自所有权；服务器五表与独立账本在既有部署事务提交前读回验证。
正常变化的共享游标不参与定义哈希，原生条件和质数配置仍参与完整性检查。

源恢复/导出器提供带 source domain、原始行、SHA-256、资源闭包和消费者证据的中间表示；
作者端负责类型约束、缺项、冲突、编译、事务和 Manager 集成；原生适配器负责宿主布局与实际运行验证。
新 profile 或依赖表不能靠猜值、用旧版本资源全局覆盖或忽略缺失行为来实现。

Type-13 技能扩展目前只有源码层的布局 profile 与带来源的依赖中间表示，初始化、服务端字段和运行行为尚无实现，因此不开放导入注册。
`owner/units/profiles/skill_target_1132.py` 验证完整的 882 字节源记录和 1132 字节目标记录、两端 SHA-256 及逆变换。
投影为 `source[:105] + zero[50] + source[105:881] + zero[200] + source[881:882]`；
末字节位于目标偏移 `1131`，插入区间为 `[105,155)` 和 `[931,1131)`。
调度值必须保留原值；已观察的分支域为 `0..3`，域外值不能截断或归零。
`owner/units/dependencies.py` 单列图标、特效、关联技能、调度及两端引用；
即使依赖文件已齐全，两层仍返回 `native_registration_blocked=true`，编译器和原生消费者也不会接受 kind `13`。
