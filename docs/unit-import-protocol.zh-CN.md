# 原生机体包接口

[返回机体族导入说明](unit-import.zh-CN.md)

任何来源按协议做好的包均可提交导入；是否可用取决于协议校验和当前支持的运行能力。来源拆包和来源格式转换不属于公开框架。

本接口用于 SDGO Mod Manager 的机体内容制作。通用工具不附带具体机体、原始记录或模型；
服主提供的数据、原生模型截图与发布内容保存在自己的工作区。

## 新增技能与导入入口

完整协议发布物支持目标客户端原来没有的自定义技能：kind `13` 原生记录、独立图标图集与资源、
配套服务端定义和引用它的机体一起导入。目标已有的原生消费者解释这些数据；新增编号不等于新增执行逻辑。
无需因为目标目录里没有该编号，就删除技能、改用原版编号或给每个技能另写 Mod。

区分三种产物：单机体编辑器的草稿、可交换的制作项目附件，以及已经编译好的两端运行发布目录。
单族确认对新增依赖的限制，不能推导为完整协议包不支持技能记录。
完整发布目录通过 Manager「批量阵容服务端应用」写入共享技能及机体定义，玩家安装匹配的客户端包；
只有制作附件时，先补齐并编译发布物。仅有图片或 SQL 行均不构成完整技能。

按虚构、非默认发行的“训练护甲”例子，见[自定义技能：记录、图标和两端导入](unit-skill-import.zh-CN.md)。
该说明包含原生记录、SPR/TXR 路径、图集和服务端 JSON、机体引用、编译顺序及 Manager 操作，
不使用任何服务器的私有机体、素材、记录或来源工具。

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
`declared_unit_id`。可选 `ace_pilot` 记录王牌模式使用的 `display_name` 与 `kind`：
`named` 表示有依据的具名驾驶员，`role` 表示无法确定具名人物时使用的阵营／身份称号。该字段只随作者项目和服主内容发布，
不会把自定义机体资料写进通用框架；未填写时旧项目保持原义。机体被选为王牌后，游戏从出战起整场以该名称替代机体名称，复活后继续使用；它不是命中后出现的提示文字。 队伍公告只说明红队或蓝队出现王牌，不包含驾驶员名或机体名。显示名同时受 48 个 Unicode 字符与原生 64 个 UTF-16 code unit 限制。
审核后的制作项目可用 `tools/export_unit_ace_pilots.py` 汇编为独立的 `owner-pilots.json`，服主内容发布器负责把它作为受管内容交付，通用程序包只带基础目录。
介绍与本服说明分开；资料出处记录标题、网址及官方/作者/参考类型。
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
保留来源机台与精确有理数。这是配置频率，不是独立抽样概率；原生序列、重复回避与出货限制照常生效。
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
这个开发者读取接口只提供制作数据，不是 Manager 导入入口，也不是运行时多归档安装器。

### 特效纹理的资源域准备

`owner.units.effect_resources` 提供独立的制作辅助接口。`texture_carriers` 按完整 ZFX
v1–v5 布局读取 emitter 的 32 字节纹理字段，以首个 NUL 为字符串终点；不扫描字段尾部来增加依赖。
未终止的名称、超出预算或无法完整解析的字段／尾部都会拒绝处理。
另有严格限定的 `native-final-emitter-subdivision-eof-v1` 原生边界：v5、无网格 emitter、
最后一个 type 1 emitter 恰好在 subdivision 数量字段前结束。该宿主读取失败时沿用构造器中的零值，
制作器保留原始短文件并标明默认字段，不补字节；任意更早的截断、半个数量字段或省略网格尾部均不属于此边界。
`scope_textures(owner, effects, read_texture, destination_read)` 要求调用者明确选择来源版本，
为特效及纹理生成服主资源域名称，并重新解析检查输出引用。目标路径已有不同字节时拒绝生成，
不会用同名资源覆盖原客户端。仅改写实际的纹理名称字段，新的终止符之后保留原存储字节。
输入纹理叶名可含 `+`；发布路径仍须使用安全的服主资源域名称，并同步改写有类型的引用。

特效 TXR 检查支持原生 PNG、DDS、TGA 和 JPEG，完整解码但保留原始文件字节。
同一逻辑名称的不同纹理内容保持独立身份；ZFX 的模型依赖需要单独的模型资源域适配器。
输出含来源及结果摘要、明确的字段位置和依赖边，不包含原生登记或可玩性许可。
这是开发者制作接口，尚未作为 Manager 自动导入步骤开放。

### 初始化脚本制作声明

项目可用 `native_programs` 数组携带 `{manifest_path, manifest_sha256}` 绑定。
声明及全部 SCP 文件必须在项目的精确文件清单内。Manager 会保留它们、显示脚本组数量并随项目导出；
修改文件、缺少源武器或函数名不匹配时拒绝导入。单族编辑器的确认发布仍拒绝携带新增脚本的项目；
完整协议包可携带按下述接口编译、与最终目录绑定的程序 sidecar，二者不是同一入口。

声明 schema 为 `sdgo.unit-native-programs/1`，必填：

- `owner`、`revision`：独立的函数命名空间及内容修订摘要。
- `vm_abi=lua50-x86-double64`、`phase=after-original-root-loader`、`root_profile=closure-publications-only-v1`。
- `programs`：每项含 `resource`、项目内 `member`、`sha256` 和完整 `exports`。
- `weapon_initializers`：每项含 `weapon_id`、完整源武器 `weapon_sha256` 和 `export`；
  函数必须与该 1871 字节武器记录 `+1771` 的 UTF-16 初始化名称一致。

版本 2（[schema](unit-native-programs-v2.schema.json)）使用 `weapon_callbacks` 代替
`weapon_initializers`，每项另外必填 `role`：`ranged` 对应 `+1569`，`special_ranged`
对应 `+1669`，`initializer` 对应 `+1771`。均检查实际 100 字节 UTF-16 字段，不能传任意偏移。
同一所有者可绑定同一武器的不同回调，不能让多个所有者分别接管同一武器。
运行侧容器保留 `SDGOPR1` 标识，header version 为 2，各绑定额外携带角色码；版本 1 沿用原布局。
这些字段保存函数名，不是脚本文件路径；其他原生回调、共享状态和资源仍需各自明确绑定。

`owner.units.native_programs` 完整解析目标 Lua 5.0 二进制块；根代码只能发布不捕获外部局部变量的函数并返回。
普通文本、LuaJIT 块、未声明导出、原版全局名覆盖及未绑定武器不会获得执行许可。
校验只覆盖程序格式、导出和武器绑定，不检查脚本的实际行为。

函数体中的完整资源路径常量同样是依赖，不能只声明程序文件及武器 BIN 中的路径。
`literal_resource_references(program_bytes)` 按目标 Lua 5.0 序列化结构读取这些常量，不扫描调试源文件名；
`validate_resource_dependencies(bundle, weapons, read_resource, catalog_sha256=...)` 验证目录、武器和程序绑定，
从实际资源归档／兼容原版读取字节并生成摘要。Manager 的批量预检和应用会调用这项检查，
缺资源、归档摘要变化或目录不配套时，在 SQL 写入前拒绝。
资源须随项目和发布物运输；动态拼接路径、动作／模型节点以及原生固定分支仍需要专项依赖声明和运行适配。
完整的虚构案例、三种回调位置及原生测试步骤见[特殊武器与变形后的攻击](unit-skill-import.zh-CN.md#特殊武器与变形后的攻击)。

底层 `compile_bundle` 生成 `SDGOPR1` 运行侧文件，绑定完整目录 SHA-256、程序摘要及具体武器字节。
原生 `unit_catalog` 的可选消费者再次验证这些条件，在机体目录完整登记后、原版根脚本返回时注册；
初始线程身份缺失、已有函数冲突或部分执行失败会拒绝并关闭候选客户端。
该消费者只注册脚本，不授予资源、SQL、编号保留或机体准入。
单族 `Workspace` 发布路径尚不输出这个文件；面向完整协议发布物的构建器使用 `compile_bundle` 生成它。

### 技能与浮游记录 profile

制作项目可用 `native_content` 数组携带 `{manifest_path, manifest_sha256}`，指向
`sdgo.unit-native-content/1`（[schema](unit-native-content.schema.json)）。它把完整原生记录、资源、
服务端技能／浮游定义、技能图集、身份绑定请求和来源证据组织成同一个有界项目。
所有成员逐项核对 SHA-256，原始来源和准备记录分别保留。已知记录检查固定布局；
未知类型或调度值保留完整字节并列为待适配项，不能凭包内许可字段放行。

已知记录布局包括 kind 11／732 字节、kind 12／1559 字节和 kind 13／1132 字节；
kind 12 使用 `native-type12-1559-v1`，面板单列「武器特殊效果」数量。
布局支持只表示完整记录的运输／校验，不能推导该特殊效果的资源、原生消费者及战斗已经通过。
单族编辑器仍将此类附件保存为制作项目；完整运行发布走批量协议入口。

Manager 的「配套原生资料」可补充 ZIP，或明确更新指定资料组。附件 ZIP 包含一个
`manifest.json` 或 `native-content.json` 及其引用成员。补传后使用独立传输命名空间，保留原清单与成员映射，
不修改机体／武器原始记录、不覆盖此前发布。内容改变后预览确认失效；失败或旧修订不提交部分草稿。
项目导出保留完整配套资料并检查重新导入的容量限制。
来源证据不会因恰好具有记录长度而被自动识别为另一条武器或运行资源。
这些附件支持制作项目保存和重新导入，不能直接作为玩家包安装。
完整运行发布物另外组织客户端目录、资源归档、sidecar 和服务端计划，见[组合例子](unit-skill-import.zh-CN.md#5-编译完整发布目录)。

`POST /api/providers/units/api/units/drafts/<id>/native-server-definition` 使用既有会话和
`X-Catalog-Revision`，请求为 `{manifest_path, kind, id, definition}`；`kind` 为 `skill` 或 `funnel`。
`definition` 可选完整哈希绑定的 `template`、完整 `explicit`，或制作阶段的 `draft`。
`draft` 使用 `{kind:"draft", origin, values}`，允许缺项但仅接受既有列名及有限 SQL 类型，
保存时从对应完整客户端记录补入已证实的共享字段，其余不设默认值。
字段齐全时转为 `explicit`；默认验证器、数据写入器和运行编译器拒绝 `draft`。
资料组可暂缺部分服务端条目，每条缺项仍单列为发布阻塞。
模板来自宿主定义目录，不能选取已属导入账本的行；保存时按完整行哈希及共享字段复核。
客户端技能末字节不是服务端 `BattleMode` 的默认值。改变已有原生行为绑定对应的模板会拒绝，
不会通过移除绑定来放行；只更新数据修订不发出安装许可。
编辑保留原始清单与服务端资料作为证据，独立保存作者版本并使旧预览确认失效。

模型资源经过纹理或特效路径改写后，机体记录中的模型摘要也必须对应最终字节。
发布编译器从实际发布的 ZOA 模型自动生成 `+672` 的固定 64 字节 UTF-16 字段：
把模型 MD5 视为四个小端 32 位字，各输出八位大写十六进制，再拼接；此字段不追加 NUL。
原始机体和模型资料保留。`owner.units.model_integrity.rebind_model_digest` 另提供先验证完整源模型、
再更新准备记录的严格接口；这一摘要不能替代资源闭包或实际出击检查。

服务端技能制作数据使用 `sdgo.unit-server-dependencies/1`
（[schema](unit-server-dependencies.schema.json)）。`owner`、`revision` 和 `skills` 必填；
每项用 `id`、完整 1132 字节客户端记录的 `client_sha256` 及 `definition` 绑定数据。
`definition.kind=template` 要求既有技能 `id` 和完整目录行摘要，复制所有列后只改变目标编号。
`kind=explicit` 要求全部 `tbl_SkillRev` 字段及 `origin`，不补缺失列或猜填零。

`owner.units.server_dependencies.apply` 只在调用方已开启的目录事务中工作，共用目录应用锁，
核对实际 SQL 类型、模板版本、客户端共享字段和关联技能，随后写入定义及独立所有权账本。
已有原生或其他内容的行不能被接管；同版重复提交复用原数据，外部修改或有损字符串读回会拒绝。
更新内容需要新修订，提交或回滚仍由调用方管理。

制作数据保持 `native_registration_authorized=false` 和 `actor_accepted=false`，表示单独的资料或写入回执不授予运行权限。
目录的兼容字段 `pending_skill_ids` 列出技能依赖账本中的编号；它本身不是完整发布物的不可用清单。
单族编辑器仍据此限制确认，批量服务端应用则核对本次声明的完整客户端记录与服务端定义，
并允许机体引用成功安装的自有技能。不要只检查这个字段就判断所有新增技能无法导入。
完整发布目录在 `server-plan.json.skills` 携带这份声明；`owner.units.batch_server` 会在共享依赖事务中
调用同一写入器，再按依赖顺序应用引用这些技能的机体族。此入口支持完整的新增技能定义。

浮游制作数据使用 `sdgo.unit-server-funnels/1`（[schema](unit-server-funnels.schema.json)），
由同一事务接口以 `kind='funnel'` 写入完整的六列定义和独立所有权账本。
732 字节客户端记录的 `FunnelProp`、动作时间、攻击延迟、攻击次数及可夺取标记必须与服务端逐值对应；
不能截断客户端的 32 位攻击次数来迁就服务端列。目录用 `pending_funnel_ids` 保留尚未完成的运行准入。

技能图集制作声明为 `sdgo.unit-skill-atlases/1`（[schema](unit-skill-atlases.schema.json)）。
`owner.units.skill_atlases` 核对完整 1132 字节技能记录、独立 SPR、每帧的 TXR 引用与文件摘要，
并检查记录 `+426` 的帧编号。编译的 `skill-atlases.bin` 以 `SDGOAT1`、版本 1、128 字节头绑定完整机体目录和原生资源归档的摘要。
原生组件按实际登记的技能记录选择图集，分别处理详情控件和战斗技能初始化路径；
未匹配的原版技能仍使用原版图集。纹理和帧坐标沿用原生解析及绘制，不覆盖全局图集。
该组件处理图集资源；完整协议包把其输出与技能记录、服务端定义一起安装。
单独保存图集不会解除单族编辑器的发布限制，也不提供额外的服务端特殊行为。
SQL `BattleMode` 与客户端末尾调度字段是独立输入，不按相同名称互相推导。
原生处理器可能按技能编号选择特殊行为；复制一行 SQL 不会自动保留这种行为，需另行处理原生身份绑定。
开发者可使用[原生技能身份选择器](skill-identity.md)在独立所有权校验后只改专用分支的局部选择值，
保留实际技能记录和网络同步编号；机体其他行为与资源仍需各自适配。

服务端运行请求使用 `sdgo.unit-skill-identities/1`
（[schema](unit-skill-identities.schema.json)），只声明所有者、当前安装修订及目标／源编号和客户端记录摘要。
可信控制器通过 `owner.units.skill_identity_runtime.prepare_from_cursor` 在目录事务中独立读回完整 SQL 定义、
所有权、原版编号集合和模板，核对配对字段后生成候选文件及独立的宿主安装记录。
`unit_catalog_server` 默认关闭且不携带内容；它校验实际服务器程序和两个文件，使用原生登记表的真实借用指针，
保留目标记录及同步编号，只改变受支持分支的局部选择值。内容项目不能自行提供生效许可。
安装记录位于宿主 `sdgo/install-state`，不作为项目资源导入；定义改变后需由控制器重新生成并读回。
该身份选择器是固定编号分支的专项适配，不是所有自定义技能的必选组件。
其支持集合不是整个导入协议的技能白名单；使用已有原生数据驱动行为的技能按完整记录、资源与服务端定义导入。

`owner.units.profiles.skill_target_1132` 验证 kind 13 的882→1132字节投影，保留完整源／目标哈希和逆变换。
其独立 `validate_relocated_record` 接口使用 `sdgo.native-type13-relocated-record/1`：明确绑定源、目标技能编号，
默认只允许投影后目标记录的前4字节换号，并保留源、原投影、最终目标三份哈希；原有未换号接口的协议不变。
可选的 `linked_skill_rewrite` 仅接受明确的源／目标关联技能编号，固定验证源偏移304与目标偏移354，
恢复关联编号后仍逐字节验证完整投影；调用者不能指定其他改写位置。
`owner.units.profiles.funnel_target_732` 验证 kind 11 的完整732字节源／目标记录；换号只允许改动开头的
32位编号，其他字段保留原字节，不能根据字段名猜测转换。两种 profile 均不发放原生注册权限。

这些 profile 的 `admission.native_registration_blocked` 保持 true：布局检查本身不签发安装或运行许可，
不能仅凭 profile 通过就绕开完整发布物的资源、引用及两端定义检查。
它不是运行目录编译器拒绝 kind `13` 的全局标记。完整记录进入 `native_catalog`，
配套资料分别进入图集、资源和服务端编译／应用接口；单族编辑器限制仍独立存在。
未知的技能调度值保留在源数据中并阻止登记，不自动清零或截断。

## 服务端 definitions JSON

补充定义接口使用 `schema_version: 1`，允许以下顶层字段：

- `root_unit_id`：明确选择进入机库的根形态，必须属于该包。
- `behavior_profile`：`native-single-form-v1`、`native-linked-forms-v1`、`native-one-way-forms-v1`、
  `native-entry-forms-v1`（入口形态，变入已发布双向形态之一；第二跳需要原生形态图运行时）、
  `native-self-transform-v1`（单条记录，变形目标为自身）或 `native-three-stage-forms-v1`
  （同一机体族内恰好三条完整记录，根 → 中间态 → 终态 → 0）。
- `item_template_id`：客户端和服务端都存在的永久机体物品类别模板。
- `item_text`：可选，`{"description": "...", "usage": "..."}`。编译时写入新物品类别（kind 34，614字节）
  的 `+0xD6` 和 `+0x19E`，即商城详情与扭蛋结果里的「性能」「使用」栏。两个原生字段各容纳100个 UTF-16 单元
  （含结尾零），因此每段最多99个单元，不接受控制字符和换行。缺省或为空时写入空字段；模板物品自带的这两段文字
  描述的是另一台机体，不会沿用。制作项目只在填写了文字时导出这个字段。
- `units`、`weapons`：按内部编号匹配的类型化服务端字段。

[最小字段示例](examples/unit-family.example.draft.json) 只演示结构，不含可安装记录或默认机体。
完整列名、类型和范围以 `owner/units/model.py` 的 `schema()` 与 `field_value()` 为准，
实际 SQL 列校验在 `owner/units/repository.py`。缺少字段保持为缺项，由服主填入或明确选模板补齐；
模板仅补空值，不用模板覆盖已恢复的源数据。`UserGradeLimit` 保留 SQL `int` 范围，不按字节截断。

单形态要求变形目标为零。双向 profile 要求完整、互相匹配的两条关系；单向 profile 要求终态的变形目标为零；
自变形要求单条记录指向自身；入口形态要求单条记录指向已发布双向形态之一。
三阶段要求三条记录从选定根依次可达，无外部目标或环，终态目标为零，并通过批量应用安装服务端形态图。
自引用不能证明第二状态或可用的 R 键动作；独立双状态应使用完整互指记录及配套动作。
超出这些适配器范围的有向图需要匹配的 profile，不会删掉多余形态后当作兼容。
Manager 的 `form_graph` 报告使用 `sdgo.unit-transform-graph/1`，从完整4976字节记录
的 `+316` 四字节字段读取目标，逐条绑定记录 SHA-256。报告单列弱连通分组、有向环、
选定根路径、同组未从该根到达的其他入口及外部目标；不补反向边，不从连通分组推断库存身份。
生成或导出报告不会开放运行登记。
库存根形态在发布后保持稳定。原始 BIN/RAR 始终保留，编译只写回已验证的共享字段。

技能依赖必须由目标已有目录或完整协议包提供。只有已验证可同步编码的技能 1/2 支持服主逐项明确移除，效果会丢失并记录来源；
不凭编号编造技能，不自动清空未知技能或脚本。目标已有原生消费者的新增数据按完整发布物导入；
未知布局、目标缺少的行为或额外固定编号分支才需要相应适配器。

## 公开预设配方与自动阶段规则

[`mods/unit_catalog/default-presets.json`](../mods/unit_catalog/default-presets.json) 是随 Mod 提供的公开示例，
schema 为 `sdgo.unit-default-presets/1`，从兼容目标客户端的完整原生模板组合新记录，不携带原版模型包。
它与编辑器的 definitions JSON 不同：配方条目使用 `profile`，生成的机体族定义使用 `behavior_profile`；
两者的 profile 值相同，配方文件不能直接当 definitions 上传。

制作新配方时保留完整结构，修改 `owner`、各条目的 `key`／`family_key`、新编号、全部引用、文字和字段修改。
`templates` 逐项绑定原版记录的 kind、ID 与 SHA-256；更换模板须更新摘要。当前配方生成器要求每个预设引用
两个已声明的普通技能，并校验其原生编号类别。制作单形态可保留这两个技能、令唯一形态的 `target` 为 0，
并移除该形态的阶段规则；双形态和三阶段按上节关系提供完整记录。不能用补零占位代替记录或资源。

开发者入口：

- [`preset_recipe.load(path)`](../owner/units/preset_recipe.py) 读取并校验作者配方；
  `compose(recipe, baseline, catalog)` 核对目标原版摘要及服务端定义并生成两端数据。
- [`preset_publication.build(output, baseline, repository, module_binary, recipe=recipe)`](../owner/units/preset_publication.py)
  生成新候选目录并返回 `publication` 路径。`baseline` 使用目标客户端的 `assets.Baseline`，
  `repository` 使用配置到 WorldServer INI 的 `repository.Repository`，`module_binary` 指向已安装的
  `unit_catalog.dll`；参数来自服主自己的配置。生成本身不应用 SQL、不安装客户端。
- 输出包含 `server-plan.json`、`form-graph-requests.json`、登记资料和 `package` 客户端安装目录；
  在 Manager 走既有[批量应用](unit-import.zh-CN.md#把批量阵容应用到服务端)并安装对应客户端内容。
  新作者内容单独发布；默认自动登记只负责内置配方，不手改受管 `presets` 安装目录或伪造回执。

自动阶段与形态数分开声明。配方的 `stage_rules` 为以下对象的列表；这是结构片段，不是可独立安装的包：

```json
[
  {"unit_id": 29001, "target_unit_id": 29002, "skill_id": 74001, "max_hp_percent": 85},
  {"unit_id": 29002, "target_unit_id": 29003, "skill_id": 74002, "max_hp_percent": 45}
]
```

这些虚构编号仍须检查目标占用。每个起点最多一条规则，目标必须等于其完整机体记录的原生变形目标。
技能必须在起点的技能 1／2 槽，并以完整 kind 13 记录包含在同一目录中；`Activate=2`、`ActivateValue` 与
`max_hp_percent` 一致，客户端调度字节为 0，起点与目标使用标准变形条件。阈值为 1–99 的整数。
[`stages.encode(rules, catalog, records)`](../owner/units/stages.py) 核对这些条件，生成绑定完整目录摘要的
`stage-rules.bin`，随同版客户端内容交付；只增加一段未编译的 JSON 不会开启行为。
运行时等待实际原生技能生效与耐久条件满足后发送原版变形请求，保留原生资格检查，不恢复耐久。
没有规则的普通单／双形态按原生行为运行；profile 本身不产生自动升级、额外模型或新动作。

## 客户端内容与展示

### 设计图制作声明

definitions 可选字段 `blueprints` 接受设计图列表；每个机体族最多16份。
服务端接口仍使用 `schema_version: 1`。读取导入面板的
`capabilities.blueprints` 判断制作和原生注册是否开放，不通过修改 source-only 标记跳过门禁。
`native_registration` 表示使用宿主兼容模板的结构化配方发布能力；此路径支持原生编译、事务登记及原生合成。
完整上传展示模板独立读取 `capabilities.blueprints.client_templates.native_registration_authorized`；
当前框架支持资源闭包校验通过的 v2 展示模板发布，v1 仍仅用于制作交换。
单独模板验证结果中的同名许可字段保持 false，因为它不自行授权安装；不要把该回执误读为框架能力开关关闭。

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
静态闭包，不检查模型、动画、特效、原生交互或战斗行为。
独立验证结果的 `native_registration_authorized` 和 `native_runtime_accepted` 仍为 false。
发布编译器还须检查框架支持的原生 profile：当前只开放验证通过的版本2；版本1拒绝编译为运行包。
编译成功的发布收据可标记 `native_registration_authorized: true`，但
`native_runtime_accepted` 仍为 false；每份包上线前仍应在测试服实际检查。
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
源数据、配置可达、客户端登记及实际合成／抽取结果分别记录。

确认后输出的 `definitions.json` 是 `sdgo.unit-family` 发布声明，不是制作阶段的 definitions 补丁。
客户端原生目录使用 `SDGOUN1` 容器、类型化记录、长度与校验值，最多 8192 条记录、负载 64 MiB；安装器核对完整文件清单和所有权。
发布包同时携带安装入口使用的根符号片段与 Mod 目录中的完整 `symbols.fragment.ini`，两者字节必须一致。
局部符号随 Mod 安装并由框架校验，不能只更新 DLL 或只追加旧的表加载符号。
机体、武器、物品类别分别使用原生 kind `2`、`10`、`0x22`。
机体一览使用 `0220.gdt` 对应的 kind `0x55`，614 字节，按原生连续 global ID 枚举；
每个库存根一条，`+6` 为机体 ID，`+10` 为可选的 kind `0x21` 设计图 ID。
无对应设计图时使用原生空配方分支，不借用其他机体的设计图。

原生游戏不会从工作目录自动读取新增模型。编译器在 `unit_catalog` 运行目录生成
`resource-overlay.ini`、`resources.zdx` 和 `resources.zpk`，使用既有原生归档编码器；
INI 的 schema 为 1，分别声明 index/archive 文件名及 SHA-256。归档只包含清单中批准的
模型、动作、纹理、特效、图片、声音和游戏脚本资源。安装器核对归档条目与清单
`resource_overlay.paths` 的精确对应；没有该列表的旧包继续按 `owned_paths` 核对。
松散副本只供工具读取，游戏不会读取它们；只带归档、不带松散副本的包（`owned_paths` 为空）
同样被接受。归档上限为 2 GiB，单个资源 64 MiB。

归档可以拆成分段：INI 另写 `parts=N`（2 至 32）和 `part_chunks=C`。第 0 段是 `resources.zpk`，第 k 段是
`resources.<k>.zpk`；除最后一段外，每段恰好 C 个 520 字节的块，最后一段不超过 C 块。索引里的起始块号是全局块号，
第 k 段保存全局块 `[k*C, (k+1)*C)`；任何条目都不能跨段，段内块头的前后链接按段内块号编写。`archive_sha256`
是按段顺序连接后的整体 SHA-256，因此与同一字节流写成单个文件时的身份相同。读取器按 `起始块 / C` 选择分段，
并把起始块换算成段内块号后交给原生文件流。C 由打包方选择，例如 C=180000 时每段约 94 MB。

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

### 按类型重编号与宿主范围

`owner.units.relocation.plan` 接受 SHA-256 绑定的编号请求、目标占用快照和逐条源字节。
版本1使用共同编号区间；`sdgo:record-relocation-request/2` 另支持
`allocation.kindPolicies[{kind, ranges, maxTargetId}]`，让机体等记录使用各自的合法范围。
区间仍共享全局占用集合，不会因为类型不同而重复使用同一个全局编号。
源键区分同号的不同版本；只改身份字及明确声明、核对过原值的四字节引用，其余源字节不变。

控制器可通过独立的 `target_domains={kind:(minimum, maximum)}` 限制当前宿主范围。
包内区间与宿主范围取交集，空交集会拒绝；保留源编号及外部引用也受该范围检查。
当前原生服务端的机体编号范围为 `10001..30000`，不能把武器／技能的大编号池直接用于机体。
目录计划和数据写入器都在写 SQL 前检查机体及变形目标的范围。
输出保留输入、源／生成记录摘要和变更区间，不发出编号预约或运行安装许可。
宿主安装时仍需重新检查当前占用、所有权和实际表约束。

重编号后，机体定义、变形引用、两端字段、编号派生的展示资源和脚本常量需要一起刷新；
重编号计划不会自动刷新这些派生内容，上线前仍应在测试服实际出击检查。

### 形态图控制器请求与宿主许可

`owner.units.form_graph_runtime` 处理 [`sdgo.unit-form-graphs/1`](unit-form-graphs.schema.json)。
请求只携带 owner、revision、roots 和逐条完整机体记录的 SHA-256，不接收安装许可或准入标记。
有向目标由原始记录 `+316` 读取，并与服务端所有已核对的共享标量一起验证，不从弱连通关系补反向边。
库存入口必须另有真实机体物品行和匹配账本；只拥有形态定义不足以声明一个入口。
同源原版节点依靠独立的两端基线摘要只读复用，不能借此接管其他内容所有者的记录。

SQL 控制器要求调用者已有可提交事务，取得既有应用锁，并以 `HOLDLOCK` 读取固定定义和完整账本。
记录身份、`RecordKey`、`OriginalJson`、修订及安装摘要均参与快照约束，`UpdatedAt` 明确仅作审计时间。
既有只读文本的 NUL 填充按原值保留，所有快照标量类型与范围仍需校验；新写入的所有者行使用更严格的写入验证。
编译输出使用内容 `SDGOUF1` 和独立宿主 `SDGOUA1` 容器，分别有界并绑定完整 SHA-256、宿主版本和同一图负载。
许可留在受信任的宿主安装状态中；它不是内容成员，也不是可以由项目自报的能力。
`GraphBindings::resolveTarget` 仅在有向边和根路径验证通过后返回目标；`inspectNode` 只供资料检查。
指纹不替代部署后对数据库、客户端资源、原生缓存和实体生命周期的读回；这一策略层本身不执行变形。

独立宿主许可 v2 `SDGOUA2` 使用 160 字节头，绑定内容摘要、复合快照指纹、宿主摘要、
原生定义区摘要及条目数。其后是与内容候选逐字节相同的形态图，随后按编号递增存储
`u32 ID + 114 字节原生战斗定义`；每个不同节点必须恰好一条，内部编号和变形目标须与图一致。
这 114 字节是宿主服务端表的预期数据，不替代客户端 kind `2` 的完整 4976 字节记录。
这些字节来自受信任控制器的完整 SQL 行及等级关联，不接受内容包自报的运行许可。
`compile_native_files` 检查所有节点的完整服务端行摘要；`prepare_native_from_cursor` 在既有事务／锁内
读取同一批记录和 `tbl_UnitGradeInfo`，不修改等级配置。缺失关联拒绝编译。
复合指纹同时约束原有机体／入口快照及等级快照。
独立性依靠控制器管理的固定安装路径和写入边界；文件摘要不是数字签名，
内容项目及其导出包不得写入宿主 `install-state`。

`owner.units.native_battle_definition` 描述原生表复制的 `0x72` 字节数据区；
`UserGradeLimit` 位于 `0x6A`，等级表的有符号 `LevelRating` 位于 `0x6E`。
`DailyLimit`、`MonthLimit`、`SallyReady` 等 SQL 字段保留在完整行约束中，不能填入这两个位置。
结构尾部的分配填充和链表链接不进入文件。所有者仍须完成实际原生目录读回，
并在使用时核对记录及当前生命周期。v1 不提供这些见证，v2 本身也不授予实际变形准入。

`unit_catalog_server` 的形态输入与技能输入各自成对验证，缺少任意一半会拒绝加载。
`HumanUnitCapture` 只为已登记的库存入口保留原始构造数据；同步回调复制有界 POD，
不分配、阻塞或调用游戏原生函数。异步排空按调用、线程、房间代际和玩家代际匹配前后阶段，
仅在成功返回且开始范围已接受后提供快照。释放是幂等的，新开始始终获得新代际。
状态读取 `SdgoReadUnitFormRuntimeStateV1` 只提供本进程的诊断标量及内容／许可摘要，
不输出源记录，不授予变形权限。`Current` 必须在原始房间锁内于任何后续实体操作前重新检查；
它只检查生命周期 token，不能替代当前实体、目录及效果状态核验。

### 获取制作数据

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

### 新增 profile 与技能扩展

包的提供者负责提供带 source domain、原始行、SHA-256、资源闭包和消费者证据的中间表示；
作者端负责类型约束、缺项、冲突、编译、事务和 Manager 集成；原生适配器负责宿主布局与实际运行验证。
新 profile 或依赖表不能靠猜值、用旧版本资源全局覆盖或忽略缺失行为来实现。

Type-13 技能使用布局 profile、依赖中间表示、原生目录及配套资源／服务端定义接口。
`owner.units.compiler.native_catalog` 接受完整 kind `13` 记录；客户端验证机体→技能和技能→关联技能引用，
使用原生登记与资源消费者。完整批量发布物可携带新增技能及显式服务端定义。
单族制作项目的 `Workspace.publish` 仍限制新增依赖；不能把该入口的拒绝解释为协议整体不支持。
`owner/units/profiles/skill_target_1132.py` 验证完整的 882 字节源记录和 1132 字节目标记录、两端 SHA-256 及逆变换。
投影为 `source[:105] + zero[50] + source[105:881] + zero[200] + source[881:882]`；
末字节位于目标偏移 `1131`，插入区间为 `[105,155)` 和 `[931,1131)`。
调度值必须保留原值；已观察的分支域为 `0..3`，域外值不能截断或归零。
`owner/units/dependencies.py` 单列图标、特效、关联技能、调度及两端引用；
这些制作校验器仍返回 `native_registration_blocked=true`，因为它们不独立签发运行许可，
而不是因为所有新增技能都需要重写行为。已由目标原生程序解释的条件、属性与效果可以按数据组合；
目标没有的消费者或额外固定编号分支则需相应适配。图标、记录和 SQL 各自通过不等于某个新行为已经实现。
完整组合步骤见[自定义技能导入](unit-skill-import.zh-CN.md)。
