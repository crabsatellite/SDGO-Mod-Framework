# 副官分享包与能力扩展协议

[English](operator-pack-protocol.md) · [副官功能说明](operator-assistant.zh-CN.md)

`sdgo.operator-pack` version 1（通常使用 `.opack` 扩展名）用于在服主之间交换副官的客户端资源、
能力草稿、获取规则和商城预填。本文说明 SDGO Mod Manager 导入这类分享包时接受的格式与校验规则、
导入后的编译方式、Manager 接口和能力扩展 ABI，供制作分享包或编写扩展 Mod 的开发者参考。
包内数据不会自行执行，也不会直接应用服务端或商城。

副官语音、图片和其他游戏素材始终属于 **client-content**。这些素材及包含它们的 `.opack` 只能随服主
提供的客户端包或更新交付，不能捆绑进 SDGO Mod Manager EXE、公开框架 Release、源码仓库或通用
服务端包。Manager 和公开框架发行物只提供通用协议、导入器、编译器和能力运行代码。

## 从零组合一个非敏感分享包

虚构案例“训练副官”只使用作者自绘的 `training.png`，所有图像角色暂用同一张图，
台词和语音明确沿用目标模板，能力设为常规 GP 加成 5%，不附带掉落。
`60001` 只是模板编号示例，必须在目标客户端及服务端目录中存在；实际副官的新编号由 Manager 分配。
这不是某位服主的副官素材，也不包含任何游戏原图或语音。

普通服主可以在 Manager 新建副官、上传自己的 PNG、编辑能力并保存。
需要制作可交换 `.opack` 的开发者，在框架源码环境中运行下面的代码；`training.png` 是自己的 1–2048 像素 PNG：

```python
from pathlib import Path
from manager.engines.operators.assets import validate_png, sprite_binary
from manager.engines.operators.legacy_pack import package

image = Path("training.png").read_bytes()
width, height = validate_png(image)
files = {
    "txrs/9001.txr": image,
    "sprs/9001.spr": sprite_binary("txrs/9001.txr", width, height),
}
abilities = {
    "schema_version": 1, "mode": "custom",
    "general": {"gp_percent": 5, "unit_exp_percent": 0, "energy_save_percent": 0},
    "victory": {"gp_percent": 0, "unit_exp_percent": 0, "energy_save_percent": 0},
    "defeat": {"gp_percent": 0, "unit_exp_percent": 0, "energy_save_percent": 0},
    "timed": [], "pilot_exp_percent": 0, "mission_parts": None, "extensions": {},
}
raw = package(
    files,
    {"dialogue": "", "sprites": ["sprs/9001.spr"] * 7, "intro_voice": ""},
    pack_id="example.training.operator", name="训练副官", source_template=60001,
    resources_metadata={"voice_policy": "native_fallback"},
    abilities=abilities,
    acquisition={"delivery": "product", "drops": []},
    storefront={"name": "训练副官", "label": "永久副官", "price": 1000,
                "currency": "GB", "validity": 1},
)
with Path("training-operator.opack").open("xb") as output:
    output.write(raw)
```

`package` 自动生成根 `operator-pack.json`、精确资源大小和 SHA-256，并把资源放入 ZIP。
TXR 在这个例子中保存实际 PNG 编码；SPR 由框架写入合法布局和纹理引用。
七个图像角色使用同一张图只适合演示；正式制作时分别提供合适的图像和原生裁切，不要改变数组顺序。
要替换语音，另提供台词 GDT、出击 ZSD、每行两个语音字段引用的 ZSD，并设 `provided_only`；
本例的 `native_fallback` 明确表示依赖目标原生台词及语音，不是独立本地化语音包。

导入与生效按以下顺序操作：

1. 在“副官与能力”选择目标客户端和服务端定义，点击“导入副官分享包”预览生成的 `.opack`。
2. 检查图像、模板、基础依赖、5% 常规加成和 `product` 发放方式，再确认创建草稿。
3. 自动化客户端先读取当前 `revision`，发送 `import-preview`；确认时用返回的 `pack_sha256` 填入请求字段 `sha256`。
   预览会规范化 ZIP，不能用原 ZIP 的文件摘要或 `source_archive_sha256` 代替这个缓存标识。
4. 保存草稿，分别应用服务端定义和生成／安装副官客户端包；新增定义按 Manager 提示刷新原生服务。
   独立能力需要对应 Battle 运行组件时，另检查其安装与重启状态。
5. 若要销售，在商城读取已应用到同一数据库的副官类别，创建未上架商品草稿，再明确上架并应用。
   本例的 `storefront` 只是预填，`delivery=product` 不会赠送副官或直接开卖。
6. 给玩家分发生成的客户端更新包。`.opack` 用于作者之间交换，不是 Manager 普通“导入配置包”的运行包。

## 四个协议层

| 层 | version 1 的作用 | 导入后的状态 |
|---|---|---|
| `resources` | 台词表、七个图像角色、出击语音和完整依赖 | 编译到新副官自己的客户端命名空间 |
| `abilities` | 原生数值能力、任务部件、驾驶员经验及命名空间扩展 payload | 保存为能力草稿；服务端与运行时另行应用 |
| `acquisition` | 全员免费/商品登记和互斥掉落规则 | 保存到副官目录；服务端另行应用 |
| `storefront` | 商品名称、标签、价格、币种和期限预填 | 仅供商城模块创建未上架草稿 |

`layers` 必须精确等于：

```json
{
  "resources": 1,
  "abilities": 1,
  "acquisition": 1,
  "storefront": 1
}
```

version 1 的 `required_features` 目前是保留字段，只接受空数组。它不是能力扩展登记机制；任何非空值
都会被拒绝。

## ZIP 与清单

ZIP 根目录包含 `operator-pack.json`。原生资源只允许以下路径：

- `gdrs/数字.gdt`
- `sprs/数字.spr`
- `txrs/数字.txr`
- `sdrs/数字.zsd`

路径会统一大小写比较；绝对路径、盘符、空段、`.`、`..`、控制字符、重复路径、符号链接和加密条目
均拒绝。限制为压缩包 128 MiB、解压总量 256 MiB、单文件 64 MiB、最多 4096 个条目。

资源目录之外的文件会显示在预览的 `ignored_files` 中，但不会执行、安装或重新导出。不要在包内放置
脚本、DLL、安装器或其他执行代码。

`files` 必须精确列出 ZIP 中的每一个原生资源，且每项的 `bytes` 与 SHA-256 必须和真实字节一致。
下面是结构示例；尖括号内容必须换成真实整数和 64 位小写十六进制摘要，不能直接导入：

```json
{
  "format": "sdgo.operator-pack",
  "format_version": 1,
  "pack_id": "example.operator",
  "name": "自定义副官",
  "layers": {
    "resources": 1,
    "abilities": 1,
    "acquisition": 1,
    "storefront": 1
  },
  "required_features": [],
  "source_template": 60001,
  "distribution": {
    "scope": "client-content",
    "bundle_with_manager": false
  },
  "resources": {
    "profile": {
      "dialogue": "gdrs/1.gdt",
      "sprites": [
        "sprs/1.spr",
        "sprs/1.spr",
        "sprs/1.spr",
        "sprs/1.spr",
        "sprs/1.spr",
        "sprs/1.spr",
        "sprs/1.spr"
      ],
      "intro_voice": "sdrs/1.zsd"
    },
    "locale": "zh-HK",
    "voice_policy": "provided_only"
  },
  "files": {
    "gdrs/1.gdt": {
      "bytes": "<真实整数>",
      "sha256": "<64 位小写十六进制>"
    },
    "sprs/1.spr": {
      "bytes": "<真实整数>",
      "sha256": "<64 位小写十六进制>"
    },
    "txrs/1.txr": {
      "bytes": "<真实整数>",
      "sha256": "<64 位小写十六进制>"
    },
    "sdrs/1.zsd": {
      "bytes": "<真实整数>",
      "sha256": "<64 位小写十六进制>"
    }
  },
  "abilities": {
    "schema_version": 1,
    "mode": "inherit",
    "general": {
      "gp_percent": 0,
      "unit_exp_percent": 0,
      "energy_save_percent": 0
    },
    "victory": {
      "gp_percent": 0,
      "unit_exp_percent": 0,
      "energy_save_percent": 0
    },
    "defeat": {
      "gp_percent": 0,
      "unit_exp_percent": 0,
      "energy_save_percent": 0
    },
    "timed": [],
    "pilot_exp_percent": 0,
    "mission_parts": null,
    "extensions": {}
  },
  "acquisition": {
    "delivery": "product",
    "drops": []
  },
  "storefront": {
    "name": "自定义副官",
    "label": "永久副官",
    "price": 1000,
    "currency": "GB",
    "validity": 1
  }
}
```

`pack_id` 必须匹配 `[a-z0-9][a-z0-9_.-]{0,95}`。`name` 为 1–48 个 UTF-16 字符。
`distribution` 只能是示例中的精确对象。

## resources：图像、台词和语音

`resources.profile` 精确包含：

- `dialogue`：710 字节记录的台词 GDT；
- `sprites`：固定七项，对应原生七个图像角色；
- `intro_voice`：出击语音。

角色路径可以为空字符串，表示从 `source_template` 沿用该角色。非空路径必须存在于 `files`，类型也
必须匹配。`source_template` 只负责资源 profile 的回退；导入时另选的 `template_id` 负责技能和动作，
两者可以不同。

SPR 支持 version `10000`/`11000`、kind `20`、1–1024 帧。导入器保留每帧的尺寸、UV、原生图集
跨边界和镜像坐标。每个 SPR 引用的 TXR 都应独立列入 `files`。

材质引用使用独立的 32-wchar 字段，最多 31 个 UTF-16 代码单元及结尾空字符。
后续独立的 32-wchar 文本字段和剩余帧字节分别保留，不会被材质路径覆盖。

台词记录保留以下字段：

| 偏移 | 长度 | 含义 |
|---:|---:|---|
| `+0` | 50 wchar | `event` |
| `+100` | 200 wchar | 字幕 |
| `+502` | 50 wchar | 语音变体 0 |
| `+602` | 50 wchar | 语音变体 1 |

导入器遍历所有台词行和两个语音字段，语音文件数和事件数没有固定要求。ZSD 保留原生可播放的 MP3、
WAVE 或 MPEG 同步流字节，不调用外部转码器。
偏移以字节计；当前读取器将 `+0` 的 50 wchar 作为事件字符串，不把它中间的 `+50` 再解释为一段重叠字段。
其余未解释字节保持原样。

`voice_policy`：

- `provided_only`：未打包的台词语音引用会被清空；带 `locale` 的包默认使用此策略。
- `native_fallback`：允许保留目标客户端已有的原生语音引用。

独立本地化包应显式使用 `provided_only` 并打包完整依赖，避免把缺失资源静默替换为目标客户端的
其他语言内容。

预览返回 `base_dependencies`（目标客户端需自备的缺失依赖）、`voice_events`、`sprite_frames` 和
`ignored_files`。格式无效的资源始终拒绝。导入编译后，已提供资源会改写到新副官自己的客户端资源目录，
不覆盖模板资源。

客户端编译器为每名副官生成 `resources.zdx`/`resources.zpk` 和带哈希的 `resource-overlay.ini`，
通过共享原生资源代理向游戏原有文件流提供这些虚拟路径。仅复制生成的松散文件不能完成运行时安装。
编译后的材质使用受限短别名 `sdgo/optex/<operator_id>/<numeric_index>.txr`，以适配原生 SPR 字段；
代理只允许同编号副官资源包提供该别名，其他生成资源仍使用逐副官命名空间。
原版游戏归档保持不变；原始 `.opack` 独立保留，用于分享和恢复。

## abilities：严格能力对象

完全省略 `abilities` 或传 `null` 表示继承模板，也是旧目录的兼容方式。显式能力对象必须精确包含：

```text
schema_version, mode, general, victory, defeat, timed,
pilot_exp_percent, mission_parts, extensions
```

`mode` 为 `inherit` 或 `custom`。显式 `inherit` 仍必须写齐全部字段，并满足：

- 三个数值组全部为 0；
- `timed` 为空；
- `pilot_exp_percent` 为 0；
- `mission_parts` 为 `null`；
- `extensions` 为空对象。

`general`、`victory`、`defeat` 都精确包含三个 0–100 整数：

| 协议字段 | 原生字段/执行边界 |
|---|---|
| `gp_percent` | `PointAdd`，GP 百分比 |
| `unit_exp_percent` | `UnitExpAdd`，机体经验百分比 |
| `energy_save_percent` | `EnergySave` |
| `pilot_exp_percent` | 独立 BattleServer 运行能力，不是 `PointAdd` |

布尔值不会被当成整数。`general`、`victory`、`defeat` 分别编译到 `BattleResult`、`TeamWin`、
`TeamLose`。`ChangeSp`、`ServiceType` 等未确认字段继续从模板继承，协议不为它们杜撰语义。

`timed` 最多四项，按 `start` 升序，时间严格使用 `HH:MM`。开始和结束边界都包含；不支持跨午夜。
重叠时原生按 `AddOn1` 到 `AddOn4` 取第一个命中的时段，因此作者不应依赖后面的重叠项覆盖前项。

```json
{
  "schema_version": 1,
  "mode": "custom",
  "general": {
    "gp_percent": 10,
    "unit_exp_percent": 0,
    "energy_save_percent": 0
  },
  "victory": {
    "gp_percent": 20,
    "unit_exp_percent": 10,
    "energy_save_percent": 0
  },
  "defeat": {
    "gp_percent": 0,
    "unit_exp_percent": 5,
    "energy_save_percent": 0
  },
  "timed": [
    {
      "start": "18:00",
      "end": "22:00",
      "gp_percent": 20,
      "unit_exp_percent": 20,
      "energy_save_percent": 0
    }
  ],
  "pilot_exp_percent": 10,
  "mission_parts": true,
  "extensions": {}
}
```

`mission_parts` 为 `null`（继承）、`true` 或 `false`。

编译器优先复用完全相同的原生 `SkillType=1` 技能行。原版 `SkillNo` 1–27 受保护。只有当前服务端
已经登记对应原生能力时，编译器才会分配新的 `SkillNo` 或 `TimeID`；包作者不能假定任意新数值一定
可部署。

## acquisition 与 storefront

`acquisition.delivery`：

- `all_players`：保存为全员免费规则，服务端应用后按原生登录/副官页面流程补齐永久使用权。
- `product`：只登记副官商品类别，不自动发放，也不自动上架。

`drops` 使用互斥规则。每条规则至少包含：

```json
{
  "category_id": 2220010,
  "chance_bps": 2500,
  "min_amount": 1,
  "max_amount": 3
}
```

`chance_bps` 为 0–10000，全部规则合计不能超过 10000，余量代表不掉落。数量为 1–65535 的闭区间。
一次原生副官掉落触发最多命中一条规则。同一 `category_id` 可出现在多条互斥规则中，用不同概率和数量区间表达常见、惊喜与大奖档。
可选的 `pool` 必须引用目标目录已经登记的随机物品池。

`storefront` 支持 `name`、`label`、`price`、`currency`、`validity`。它只保存商城草稿预填：

- `price` 为正整数；
- `currency` 只能是 `GB` 或 `MB`；
- `validity` 为正整数；
- 实际创建商城草稿时，名称/标签还必须满足商城的中文文本和 1–50 UTF-16 字符规则。

只有已启用、已托管、`delivery=product` 的副官会出现在商城导入列表。副官商品类别必须已经应用到同一
宿主数据库。商城“创建”动作始终生成 `enabled=false` 的草稿，并可应用预填值；它不会执行商城
`apply`。服主仍需检查价格、期限、标签和身份，再单独决定是否上架。

## 命名空间扩展 SDK

`abilities.extensions` 是“命名空间 → 有限 JSON payload”的对象。名称必须匹配
`[a-z][a-z0-9_.-]{0,63}`，先在目录的 `registered_ability_extensions` 中登记，并由服务端已启用、
版本匹配的受信任原生 Mod 提供处理器。

包本身不能登记处理器、加载 DLL 或执行脚本。框架只验证 payload 能序列化为不含 NaN/Infinity 的
JSON；目前没有全局参数 schema 注册器。扩展作者必须在自己的文档中定义 payload 结构和兼容规则，
并使用稳定、版本化的命名空间。

处理器 ABI 位于 [`include/sdgo/operator_effects.h`](../include/sdgo/operator_effects.h)：

- 导出名：`SdgoOperatorEffectsRegisterExtensionHandlerV1`
- 登记函数：`cdecl`
- 回调：`stdcall`
- `ExtensionsHex` 解码后的完整 JSON 最大 16384 字节
- payload 指针不是 NUL 结尾，所有上下文指针只在回调期间有效
- `write_mask` 必须是 `writable_mask` 的子集
- 可返回的目标字段只有驾驶员经验、机体经验和 GP

每个顶层扩展键都是必需命名空间。任一处理器缺失时，整个 profile 返回 `NO_CHANGE`，不会只执行内置
部分。回调收到的 `current_*` 已包含内置能力和先执行的扩展结果，扩展不得自行重复结算。

共享结算过滤 ABI 位于
[`include/sdgo/operator_reward_filter.h`](../include/sdgo/operator_reward_filter.h)。任务结算的早期
阶段只开放机体经验，最终阶段开放驾驶员经验和 GP。扩展应返回目标值，不应通过延迟 SQL 补发替代
原生经验结算。

## Manager API

实际 HTTP 前缀为：

```text
/api/providers/operators
```

内部路由：

| 方法 | 路由 | 作用 |
|---|---|---|
| `GET` | `/api/operators/protocol` | 读取协议、完整 schema 示例和 SDK 边界；不要求已启动工作区 |
| `GET` | `/api/operators` | 读取当前目录、revision、能力扩展登记和已导入包 |
| `POST` | `/api/operators/import-preview` | 规范化并缓存包，不修改副官目录 |
| `POST` | `/api/operators/import` | 从预览缓存创建独立副官草稿 |
| `GET` | `/api/operators/packs/{sha256}/portrait` | 读取包头像预览 |
| `GET` | `/api/operators/{id}/share` | 导出当前包来源副官的 `.opack` |
| `PUT` | `/api/operators/{id}` | 保存副官配置草稿 |

`import-preview` 请求使用以下一种形式：

```json
{"zip": "<base64>", "filename": "operator.opack"}
```

或：

```json
{"path": "<Manager 所在机器上的显式文件路径>"}
```

它返回 `pack_sha256`。下一步 `import` 的字段名仍是 `sha256`：

```json
{
  "sha256": "<上一步 pack_sha256>",
  "name": "显示名称",
  "template_id": 60001,
  "source_template": 60001
}
```

`template_id` 是技能/动作模板；`source_template` 是资源角色回退模板。导入只创建草稿，不应用服务端、
不安装客户端、不创建商城商品。

所有目录写入都要求当前 `X-Catalog-Revision`，并沿用 Manager 会话令牌和同源保护。

## 应用与生效状态

以下状态彼此独立：

1. `.opack` 通过预览和校验；
2. 副官草稿已保存；
3. 服务端目录和原生能力行已应用；
4. 客户端资源包已生成并安装；
5. BattleServer 能力运行文件已安装；
6. 相关主机已重启并载入配置；
7. 实时状态中的 PID、模块版本、目录 revision、已载入 profile 数和 hook 模式与目标一致。

能力配置在 BattleServer 进程启动时载入，写入文件后需重启对应主机，当前进程才会使用新配置。Manager 的
实时状态读取会拒绝陈旧 PID、版本不符、revision 不符或无法对应当前主机进程的 `status.json`。
