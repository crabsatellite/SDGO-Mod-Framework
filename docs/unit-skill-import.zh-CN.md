# 自定义技能：记录、图标和两端导入

[English](unit-skill-import.md) · [机体导入](unit-import.zh-CN.md) · [完整协议](unit-import-protocol.zh-CN.md)

完整机体协议包可以携带目标客户端原来没有的技能。技能使用新的、经冲突检查的编号，
客户端加载它的原生记录和资源，服务端加载对应定义；已有的原生技能处理器继续解释这些数据。
新增编号本身不要求另写一个技能 Mod，也不要求把技能换成原版编号或从机体上删除。

这里的例子完全虚构：作者给“训练测试机”配置一个“训练护甲”技能，使用原生属性处理方式，
防御字段设为 `5`，其余字段按本例明确填写。编号 `74001`、所有者 `example-training`
和资源名称仅作示例，不是已预约编号、可下载机体或默认发行内容。记录、素材和行为设定由作者提供。

## 选对入口

| 输入与入口 | 当前用途 |
|---|---|
| 单机体 RAR/ZIP 编辑、预览、确认 | 编辑机体与武器；直接确认仍检查服务器技能目录，不能据此判断完整协议包不支持新增技能 |
| 制作项目的 `native_content` /「配套原生资料」 | 保存、编辑、交换技能记录、资源和服务端定义；上传资料或保存草稿不会自动生成可运行发布物 |
| 完整发布目录 /「批量阵容服务端应用」 | 导入已编译的客户端目录、资源和共享技能服务端定义；也可只包含一个机体族 |

已有完整协议包时，按最后一行应用，不要为了通过第一行的检查而清空技能。
只有作者明确选择放弃某个技能及其效果时，才使用单机体面板的移除选项。
目录的 `pending_skill_ids` 是保留名称的依赖账本字段；它不表示批量应用必须拒绝这些技能。
完整发布入口检查实际定义、记录和所有权，不能把单个状态字段当作全局技能支持表。

## 1. 准备一个完整的原生技能记录

先选择**符合用途且未占用**的编号。受支持客户端会把 `1–75000` 分到普通技能槽；新普通技能通常从
`70001–75000` 中查找空位。本例的 `74001` 仍须在目标客户端和服务端检查冲突，不能覆盖已有定义。
原生部件另有普通槽编号区间：`3220001–3225000`、`3473001–3478000`、`3558001–3559000`、
`3613002–3615999`；这些区间只说明编号分类，不能替代部件的记录、依赖和行为检查。

其余编号会进入原生的额外技能槽。例如 `79950` 可以作为 kind 13 记录登记、显示图标，
但直接写进 `SkillNumber_1/2` 时，普通技能的网络触发通知不会应用到这两个槽。单机体和批量应用
都会检查这项类别。完整记录仍可作为制作资料保存；需要改为合适的空闲编号并更新引用／哈希，
或提供对应类别的运行适配后再发布。这是原生消费端的类别规则，**不是技能效果的白名单**。

准备目标宿主兼容的 kind `13`、1132 字节记录，记录编号为 `74001`。
名称、说明、激活条件、属性、图标帧和原生调度字段属于记录内容；不能只给一个编号或一段文字。
作者工具负责生成正确的完整记录，导入器保留并校验它，不能用一块补零的字节代替真实技能。

组合原生技能时，数值和显示文字要一起更新。目标客户端的机体信息页读取字节 `5` 的名称
（50 个 UTF-16LE 单元，含结尾空字符）、字节 `531`／`0x213` 的说明（100 个单元，含结尾空字符），
以及字节 `426` 的小端 `u16` 图标帧。不能只把45%技能改了名称和触发值，却仍显示原技能的85%说明。
随 Mod 提供的演示配方会把自定义说明写入原生记录。

本例的客户端共享字段要与下方服务端定义一致：`Activate=0`、`ActivateValue=0`、`Defense=5`，
其余已声明共享数值为 `0`，`Feature=0`、`UseTarget=0`。选择图标帧 `0`。
这些是虚构例子的作者设定，不是导入未知技能时可套用的默认值。

原生数据驱动的属性、条件与效果可以组合成新技能，前提是目标程序已有对应消费者。
如果技能引用关联技能、特效、纹理或其他原生记录，也要提供完整依赖，或验证目标已有相同版本。
原始编号不存在于目标目录是需要导入依赖的信号，不是删除技能的理由。

制作材料可以这样组织；这不是可直接安装的玩家包：

```text
training-skill/
  manifest.json
  records/training-guard.bin
  sprs/example_training_guard.spr
  txrs/example_training_guard.txr
  skill-atlases.json
  server-skills.json
```

## 2. 提供图标，并绑定到这条技能

`txrs/example_training_guard.txr` 保存作者自己的图片；`.txr` 文件须使用目标支持的实际纹理编码，
不是把任意图片改个扩展名。`sprs/example_training_guard.spr` 是原生 SPR，
本例至少含帧 `0`，该帧的纹理引用指向上述 TXR。可以复用框架的原生 SPR 编码与纹理校验接口。

图片放进文件夹还不够：用 [`sdgo.unit-skill-atlases/1`](unit-skill-atlases.schema.json)
声明技能、SPR 及它全部帧引用的纹理。技能记录 `+426` 的 16 位图标帧编号必须小于 SPR 的总帧数。
可以让多个技能共享同一图集，并各自选择已有帧；不要覆盖全局原版技能图集。

以下 JSON 的 `<...>` 是待计算的摘要占位符，替换为实际文件字节的小写 SHA-256 后才可校验：

```json
{
  "schema": "sdgo.unit-skill-atlases/1",
  "owner": "example-training",
  "revision": "<atlas-revision-sha256>",
  "sprites": [{
    "path": "sprs/example_training_guard.spr",
    "sha256": "<spr-sha256>",
    "textures": [{
      "path": "txrs/example_training_guard.txr",
      "sha256": "<txr-sha256>"
    }]
  }],
  "skills": [{
    "id": 74001,
    "record_sha256": "<complete-1132-byte-record-sha256>",
    "sprite": "sprs/example_training_guard.spr"
  }]
}
```

运行时 `unit_catalog` 按实际技能记录选择该图集，详情控件及战斗技能初始化使用各自的原生路径。
`skill-atlases.bin` 还绑定最终 `catalog.bin`、资源索引和整个资源归档的摘要；
更新记录或图片后要重新编译，不能把旧 sidecar 复制到新目录上。

## 3. 提供完整的服务端定义

使用 [`sdgo.unit-server-dependencies/1`](unit-server-dependencies.schema.json)。
可以选择哈希固定的完整原生模板，也可以明确填写全部 `tbl_SkillRev` 字段。
客户端记录不能推导出所有服务端字段；服务端 `BattleMode` 与客户端末尾调度字节是独立输入。

本例 `server-skills.json` 使用完整的作者定义，不依赖某个服务器私有模板：

```json
{
  "schema": "sdgo.unit-server-dependencies/1",
  "owner": "example-training",
  "revision": "<server-skills-revision-sha256>",
  "skills": [{
    "id": 74001,
    "client_sha256": "<complete-1132-byte-record-sha256>",
    "definition": {
      "kind": "explicit",
      "origin": "authored",
      "values": {
        "SkillID": 74001,
        "SkillName": "Training Guard",
        "Activate": 0,
        "ActivateValue": 0,
        "MoveLv": 0,
        "DashLv": 0,
        "QuickLv": 0,
        "MotiveLv": 0,
        "BoostIncreaseRate": 0.0,
        "BoostPower": 0,
        "Attack": 0,
        "Defense": 5,
        "RadarRadius": 0,
        "SpIncreaseRate": 0,
        "Scale": 1.0,
        "AutoBulletChargeRate": 0.0,
        "BulletChargeRate": 0.0,
        "AtkSpdRate": 0.0,
        "ActiveResult": 0,
        "Feature": 0,
        "UseTarget": 0,
        "BattleMode": 0,
        "PartSpLevel": 0
      }
    }
  }]
}
```

`revision` 标识本声明的内容版本；作者应按固定规则从内容计算，改变内容时更新，
不能把 `revision` 自身的字节递归纳入同一摘要。文件引用中的 SHA-256 则始终计算完整最终文件。
`client_sha256` 和图集里的 `record_sha256` 必须是同一条最终技能记录的摘要。
换号时同步记录首 DWORD、机体引用、关联技能引用和服务端 `SkillID`，随后重新计算所有受影响摘要。

## 4. 将制作资料连起来

制作附件的根 `manifest.json` 使用 [`sdgo.unit-native-content/1`](unit-native-content.schema.json)：

```json
{
  "schema": "sdgo.unit-native-content/1",
  "owner": "example-training",
  "revision": "<native-content-revision-sha256>",
  "records": [{
    "kind": 13,
    "id": 74001,
    "profile": "native-type13-1132-v1",
    "member": "records/training-guard.bin",
    "sha256": "<complete-1132-byte-record-sha256>"
  }],
  "resources": [{
    "path": "sprs/example_training_guard.spr",
    "member": "sprs/example_training_guard.spr",
    "sha256": "<spr-sha256>"
  }, {
    "path": "txrs/example_training_guard.txr",
    "member": "txrs/example_training_guard.txr",
    "sha256": "<txr-sha256>"
  }],
  "server_skills": {
    "manifest_path": "server-skills.json",
    "manifest_sha256": "<complete-server-skills-json-sha256>"
  },
  "skill_atlases": {
    "manifest_path": "skill-atlases.json",
    "manifest_sha256": "<complete-skill-atlases-json-sha256>"
  }
}
```

`member` 是附件内的成员名，`path` 是游戏资源虚拟路径，两者职责不同。
机体项目的 `native_content` 引用这个清单的成员名与完整文件摘要；附件里的全部引用文件必须同时提供。
机体客户端记录和服务端定义的 `SkillNumber_1` 或 `SkillNumber_2` 都引用 `74001`。
保存或重新导入制作项目保留这些资料；普通单族确认仍有自己的发布检查，不能把附件上传成功当作运行安装。

## 5. 编译完整发布目录

面向已准备好原生数据的包构建器，公开的导入端 API 包括：

| API | 输入和结果 |
|---|---|
| `owner.units.native_content.validate_project_bindings` | 校验附件成员、记录、摘要和配套资料 |
| `owner.units.server_dependencies.prepare` | 校验完整服务端定义、共享字段、目标占用及所有权；不写数据库 |
| `owner.units.compiler.native_catalog` | 把最终机体、武器、技能等记录组合成 `catalog.bin`，支持 kind `13` |
| `owner.native_archive.build_archive` / `build_archive_file` | 将声明的资源编译成原生覆盖归档及索引 |
| `owner.units.skill_atlases.compile_bundle` | 将图集声明与最终目录、索引、归档摘要编译为 `skill-atlases.bin` |
| `owner.units.batch_server.load_publication` | 读取并校验发布目录的服务端计划、客户端目录及形态请求；客户端安装器另外校验文件清单和资源 |

顺序是先确定完整记录与资源，再编译目录和归档，最后编译绑定这些摘要的 sidecar。
图集编译的核心调用为：

```python
from hashlib import sha256
from owner.units.skill_atlases import compile_bundle

# atlas_manifest 来自上述 JSON，skill_record 是作者提供的完整目标记录。
# final_catalog / overlay_index / overlay_archive 是这次完整发布的最终字节。
skill_atlases = compile_bundle(
    [atlas_manifest],
    sha256(final_catalog).hexdigest(),
    sha256(overlay_index).hexdigest(),
    sha256(overlay_archive).hexdigest(),
    {74001: skill_record},
    {
        "sprs/example_training_guard.spr": sprite_bytes,
        "txrs/example_training_guard.txr": texture_bytes,
    },
)
```

分片归档使用所有分片按顺序拼接的摘要，即归档编译器返回的 `archive_sha256`。
构建器必须同时保留已有发布者的记录、资源、图集和脚本绑定；上面的单技能片段不是覆盖现有安装的理由。

发布目录与制作附件是不同产物：

```text
publication/
  server-plan.json
  form-graph-requests.json                 # 有形态图请求时提供
  package/
    manifest.json
    client/sdgo/mods/unit_catalog/
      catalog.bin
      definitions.json
      resource-overlay.ini
      resources.zdx
      resources.zpk
      skill-atlases.bin
      ...                                 # 匹配的框架组件和声明的其他成员
```

`server-plan.json` 使用 `sdgo.unit-roster-server-plan/1`，包含 `owner`、`revision`、
`families`、`skills`、`funnels`、`affiliations`。`skills` 放入上面完整的服务端技能声明；
无浮游时 `funnels` 仍带 `sdgo.unit-server-funnels/1`、所有者、修订及空 `funnels` 数组，
无势力时 `affiliations` 为 `[]`。即使只新增一个技能，也需要包含实际引用它的完整机体族计划。
客户端清单绑定全部安装文件及资源覆盖声明，不能用制作附件 ZIP 代替。

## 6. 在 Manager 应用两端

在「机体导入」配置目标服务组的 WorldServer INI，通过「批量阵容服务端应用」选择 `publication/`，
先执行「只读预检」。核对技能行、机体族、资源和目标冲突后，按界面要求停服，再执行「停服后应用…」和「读回核对」。
服务端先通过共享依赖事务写入技能定义，再应用机体族。Manager 不替服主停止或启动原生服务。
玩家安装同一次发布的客户端包或服主提供的客户端更新；客户端安装器不执行服务端 SQL。
重启所选服务组并检查实际技能名称、说明、图标、触发与解除。这里不会自动添加商城、扭蛋或发放机体。

## 哪些情况需要另行适配

- 原生数据驱动的组合走上述记录、资源与服务端定义链路；独立图标只需合法 SPR/TXR 和图集绑定。
- 如果某效果额外按固定技能编号分派，才检查[原生技能身份选择器](skill-identity.md)等专项适配。
  该选择器支持的编号集合不是整个导入协议的技能白名单，也不应强行给普通属性技能添加身份绑定。
- 目标宿主没有某种处理逻辑、使用未知布局／调度值或缺少依赖时，列出具体缺项及所需适配。
  声明文件、图标或 SQL 行不能凭空实现一个目标程序没有的效果。

公开框架只保存通用协议、导入代码及此类虚构说明。作者的原生记录、图片、模型、完整内容包、
私有来源及服务器配置保存在作者自己的内容工作区，不作为框架示例或默认内容发布。
