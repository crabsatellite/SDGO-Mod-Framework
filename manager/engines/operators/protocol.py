"""Developer-facing, asset-free operator interchange and extension contract."""
from .legacy_pack import FORMAT, FORMAT_VERSION, LAYER_VERSIONS


def _bonus(gp=0, unit_exp=0, energy_save=0):
    return {
        "gp_percent": gp,
        "unit_exp_percent": unit_exp,
        "energy_save_percent": energy_save,
    }


def describe():
    inherit_abilities = {
        "schema_version": 1,
        "mode": "inherit",
        "general": _bonus(),
        "victory": _bonus(),
        "defeat": _bonus(),
        "timed": [],
        "pilot_exp_percent": 0,
        "mission_parts": None,
        "extensions": {},
    }
    custom_abilities = {
        "schema_version": 1,
        "mode": "custom",
        "general": _bonus(gp=10),
        "victory": _bonus(gp=20, unit_exp=10),
        "defeat": _bonus(unit_exp=5),
        "timed": [
            {
                "start": "18:00",
                "end": "22:00",
                **_bonus(gp=20, unit_exp=20),
            }
        ],
        "pilot_exp_percent": 10,
        "mission_parts": True,
        "extensions": {},
    }
    return {
        "format": FORMAT,
        "format_version": FORMAT_VERSION,
        "manifest": "operator-pack.json",
        "layers": dict(LAYER_VERSIONS),
        "distribution": {
            "scope": "client-content",
            "bundle_with_manager": False,
        },
        "summary": (
            "副官素材包只作为服主提供的客户端内容；Manager EXE、公开框架发行包与源码仓库"
            "只包含通用导入器、协议和能力执行代码，不捆绑港服粤语或其他实际素材。"
        ),
        "limits": {
            "archive_bytes": 128 * 1024 * 1024,
            "expanded_bytes": 256 * 1024 * 1024,
            "file_bytes": 64 * 1024 * 1024,
            "entries": 4096,
        },
        "resource_roots": {
            "gdrs": ".gdt",
            "sprs": ".spr",
            "txrs": ".txr",
            "sdrs": ".zsd",
        },
        "required_features": {
            "supported": [],
            "rule": (
                "version 1 当前不接受任何非空 required_features；它不能用来登记能力扩展。"
            ),
        },
        "ability_schema": {
            "schema_version": 1,
            "exact_keys": [
                "schema_version",
                "mode",
                "general",
                "victory",
                "defeat",
                "timed",
                "pilot_exp_percent",
                "mission_parts",
                "extensions",
            ],
            "bonus_keys": [
                "gp_percent",
                "unit_exp_percent",
                "energy_save_percent",
            ],
            "extension_name_pattern": "[a-z][a-z0-9_.-]{0,63}",
            "extension_registry": "catalog.registered_ability_extensions",
            "inheritance": (
                "省略 abilities 或使用 null 表示完整继承模板。显式 inherit 对象必须包含全部字段，"
                "且数值为 0、timed/extensions 为空、mission_parts 为 null。"
            ),
        },
        "extension_sdk": {
            "effect_handler_header": "include/sdgo/operator_effects.h",
            "reward_filter_header": "include/sdgo/operator_reward_filter.h",
            "handler_export": "SdgoOperatorEffectsRegisterExtensionHandlerV1",
            "handler_registration_calling_convention": "cdecl",
            "handler_callback_calling_convention": "stdcall",
            "max_extensions_json_bytes": 16384,
            "contract": (
                "extensions 的顶层键都是必需命名空间。素材包不能登记或执行代码；"
                "必须由已加载且受信任的原生 Mod 登记完全相同的处理器名称。"
            ),
            "payload_schema": (
                "框架只验证有限 JSON；没有全局参数 schema 注册器。处理器作者必须自行定义、"
                "版本化并记录该命名空间的 payload 合同。"
            ),
            "failure": (
                "任一必需命名空间没有处理器时，整个副官 profile 返回 NO_CHANGE，"
                "不会只应用内置能力。"
            ),
        },
        "manager_api": {
            "base_path": "/api/providers/operators",
            "revision_header": "X-Catalog-Revision",
            "routes": [
                "GET /api/operators/protocol",
                "GET /api/operators",
                "POST /api/operators/import-preview",
                "POST /api/operators/import",
                "GET /api/operators/packs/{sha256}/portrait",
                "GET /api/operators/{id}/share",
                "PUT /api/operators/{id}",
            ],
            "preview_result_key": "pack_sha256",
            "import_body_key": "sha256",
        },
        "sections": [
            {
                "title": "身份、边界与兼容性",
                "lines": [
                    (
                        "format 固定为 sdgo.operator-pack，format_version 为整数 1；"
                        "pack_id 必须匹配 [a-z0-9][a-z0-9_.-]{0,95}。"
                    ),
                    (
                        "layers 必须精确登记 resources、abilities、acquisition、storefront 的"
                        "当前版本。未知层版本和非空 required_features 均拒绝。"
                    ),
                    (
                        "distribution 必须精确为 client-content / bundle_with_manager=false。"
                        "港服粤语语音、图片及其 .opack 只能随服主客户端内容交付，不能进入"
                        "Manager EXE 或公开框架 Release。"
                    ),
                    (
                        "ZIP 路径大小写不冲突，不含绝对路径、父目录、链接或加密条目。"
                        "files 必须精确列出每个资源的整数 bytes 与真实 SHA-256。"
                    ),
                    (
                        "资源目录之外的条目只会列入 ignored_files，不会执行、安装或重新导出；"
                        "分享包不能携带脚本、DLL 或其他执行器。"
                    ),
                ],
            },
            {
                "title": "resources 资源层",
                "lines": [
                    (
                        "原生输入路径只接受 gdrs/数字.gdt、sprs/数字.spr、txrs/数字.txr、"
                        "sdrs/数字.zsd。resources.profile 精确包含 dialogue、七个 sprites 和"
                        " intro_voice；空字符串表示该角色沿用目标 source_template。"
                    ),
                    (
                        "SPR 接受 version 10000/11000、kind 20、1–1024 帧；所有帧的尺寸、"
                        "UV、原生跨边界与镜像坐标均保留。SPR 引用的每个 TXR 依赖应逐个打包。"
                    ),
                    (
                        "SPR 材质引用独占 32-wchar 字段，最多 31 个 UTF-16 代码单元及结尾空字符；"
                        "后续独立文本和剩余帧字节保持原样。编译后的材质使用受限短别名 "
                        "sdgo/optex/<operator_id>/<numeric_index>.txr，由相同编号的副官资源包提供。"
                    ),
                    (
                        "710 字节台词表保留 event(+0)、event_variant(+50)、caption(+100)，"
                        "以及 +502/+602 两个独立 50-wchar 语音变体。提取器遍历全部台词行；"
                        "39 个已知 row/variant 只用于可用性报告，不是固定文件数或完整事件上限。"
                    ),
                    (
                        "provided_only 会清空未提供的台词语音引用；native_fallback 才允许"
                        "保留目标客户端原生语音引用。带 locale 的包默认 provided_only。"
                    ),
                    (
                        "预览分别报告 base_dependencies、voice_events、sprite_frames、"
                        "ignored_files 等信息。来源提取报告还区分缺失、无效、角色不可用和"
                        "依赖角色；无效格式始终拒绝，默认也拒绝任何缺失依赖。"
                    ),
                    (
                        "导入后，已提供资源编译为逐副官 ZDX/ZPK 归档，由共享原生资源代理提供虚拟路径；"
                        "松散文件本身不是运行时安装。资源位于该新副官自己的客户端命名空间；"
                        "不会覆盖 gdrs/0004.gdt、模板 SPR/TXR/ZSD 或其他副官文件。"
                    ),
                ],
            },
            {
                "title": "abilities 能力层",
                "lines": [
                    (
                        "省略 abilities/null 表示完整继承模板。显式对象必须包含全部字段；"
                        "mode 为 inherit 或 custom，inherit 不能夹带休眠的自定义值。"
                    ),
                    (
                        "general/victory/defeat 分别编译到 BattleResult/TeamWin/TeamLose。"
                        "每组精确包含 gp_percent、unit_exp_percent、energy_save_percent，"
                        "且必须是 0–100 的整数；布尔值不算整数。"
                    ),
                    (
                        "gp_percent 对应原生 PointAdd，unit_exp_percent 对应 UnitExpAdd，"
                        "energy_save_percent 对应 EnergySave。pilot_exp_percent 是独立运行时"
                        "能力，不能当成 PointAdd。"
                    ),
                    (
                        "timed 最多四项，按 start 升序；HH:MM 严格有效，start/end 原生边界"
                        "均包含，暂不支持跨午夜。重叠可保存，但原生按 AddOn1→AddOn4 首个"
                        "命中项生效。"
                    ),
                    (
                        "mission_parts 为 null（继承）、true 或 false。ChangeSp、ServiceType"
                        "等未确认的原生字段继续从模板继承，不按字段名推测语义。"
                    ),
                    (
                        "编译器优先复用完全相同的原生 SkillType=1 行。原版 SkillNo 1–27"
                        "受保护；新增 SkillNo/TimeID 只有在对应原生能力已验证时才允许。"
                    ),
                ],
            },
            {
                "title": "命名空间扩展 SDK",
                "lines": [
                    (
                        "extensions 是命名空间到有限 JSON payload 的对象；名称必须先在目录"
                        " registered_ability_extensions 中登记，并由服务端已启用的匹配版本"
                        "执行器提供。素材包本身不能完成任何登记。"
                    ),
                    (
                        "原生处理器 ABI 见 include/sdgo/operator_effects.h。回调收到当前目标值、"
                        "可写掩码和非 NUL 结尾的 payload 字节；所有指针仅在回调期间有效。"
                    ),
                    (
                        "处理器只能在 writable_mask 内返回驾驶员经验、机体经验或 GP 目标值。"
                        "任务阶段先消费机体经验，最终阶段消费驾驶员经验和 GP；扩展不得重复"
                        "结算或用延迟补发替代原生 EXP。"
                    ),
                    (
                        "共享结算过滤 ABI 见 include/sdgo/operator_reward_filter.h。能力配置"
                        "在 BattleServer 启动时载入；安装文件、主机重启和实时状态读取是三种"
                        "不同状态。"
                    ),
                ],
            },
            {
                "title": "acquisition 获取层与 storefront 商城层",
                "lines": [
                    (
                        "acquisition 可携带 delivery（all_players/product）和 drops。"
                        "每条掉落使用已登记 category_id、chance_bps 0–10000、"
                        "min_amount/max_amount 1–65535；单次原生触发至多命中一条，合计"
                        "不得超过 10000，余量是不掉落。"
                    ),
                    (
                        "storefront 只预填 name、label、正整数 price、GB/MB currency、"
                        "validity。它不创建商品、不应用商城数据库，也不能要求 enabled=true。"
                    ),
                    (
                        "只有已启用且 delivery=product 的副官登记会出现在商城导入列表。"
                        "商城创建动作始终生成 enabled=false 的草稿；正式上架仍由服主在商城"
                        "模块检查并单独应用。"
                    ),
                ],
            },
            {
                "title": "Manager API 与状态",
                "lines": [
                    (
                        "GET /api/operators/protocol 不依赖已启动的副官工作区。其他路由位于"
                        " /api/providers/operators 前缀下。"
                    ),
                    (
                        "POST /api/operators/import-preview 接收 path，或 zip(base64)+filename；"
                        "它规范化并缓存包但不修改副官目录，返回 pack_sha256。"
                    ),
                    (
                        "POST /api/operators/import 的请求字段名是 sha256，值取上一步"
                        " pack_sha256；可另传 name、template_id 和 source_template。"
                    ),
                    (
                        "template_id 选择技能/动作模板，source_template 选择资源 profile"
                        "回退模板，两者可以不同。导入只创建独立副官草稿，不应用服务端、"
                        "不安装客户端，也不创建商城商品。"
                    ),
                    (
                        "写入沿用 X-Catalog-Revision、Manager 会话令牌和同源保护。"
                        "配置保存、服务端应用、客户端包安装、BattleServer 重启与实时状态"
                        "读取必须分别报告。"
                    ),
                ],
            },
        ],
        "example_kind": "schema-illustration-not-importable",
        "example_notes": [
            (
                "files 中的 bytes/sha256 是占位说明；作者必须替换为 ZIP 内每个真实资源的"
                "整数长度和 64 位小写十六进制 SHA-256。"
            ),
            (
                "示例的 abilities 是完整且可由 schema 接受的显式 inherit 对象；"
                "若想只继承模板，也可以完全省略 abilities。"
            ),
            (
                "SPR、台词表引用的 TXR/ZSD 依赖必须各自列入 files；不要把此结构示例"
                "误当作已验证素材包。"
            ),
        ],
        "example": {
            "format": FORMAT,
            "format_version": FORMAT_VERSION,
            "pack_id": "example.operator",
            "name": "自定义副官",
            "layers": dict(LAYER_VERSIONS),
            "required_features": [],
            "source_template": 60001,
            "distribution": {
                "scope": "client-content",
                "bundle_with_manager": False,
            },
            "resources": {
                "profile": {
                    "dialogue": "gdrs/1.gdt",
                    "sprites": ["sprs/1.spr"] * 7,
                    "intro_voice": "sdrs/1.zsd",
                },
                "locale": "zh-HK",
                "voice_policy": "provided_only",
            },
            "files": {
                "gdrs/1.gdt": {
                    "bytes": "<exact integer>",
                    "sha256": "<64 lowercase hex characters>",
                },
                "sprs/1.spr": {
                    "bytes": "<exact integer>",
                    "sha256": "<64 lowercase hex characters>",
                },
                "txrs/1.txr": {
                    "bytes": "<exact integer>",
                    "sha256": "<64 lowercase hex characters>",
                },
                "sdrs/1.zsd": {
                    "bytes": "<exact integer>",
                    "sha256": "<64 lowercase hex characters>",
                },
            },
            "abilities": inherit_abilities,
            "acquisition": {"delivery": "product", "drops": []},
            "storefront": {
                "name": "自定义副官",
                "label": "永久副官",
                "currency": "GB",
                "price": 1000,
                "validity": 1,
            },
        },
        "custom_abilities_example": custom_abilities,
    }
