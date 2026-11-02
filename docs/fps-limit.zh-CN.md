# FPS 上限（Beta）

让客户端按现有 `config.xml` 的 FPS 配置运行，启动时不再强制覆盖为 60。
例如原有 `updatefps="160"` 会用于设置 160 FPS 上限。

| mod | 宿主 | 作用 |
|---|---|---|
| `fps_limit` | 客户端 | 保留原生读取器解析的 `Framework/Timer@updatefps` |

## 使用

在 **SDGO Mod Manager** 启用「FPS 上限」，调整 FPS 后关闭并重新启动该客户端。
Manager 直接读写客户端已有的 `config.xml`；Mod 不另存一份 FPS 数值。
安装时保留当前配置，包括已有的 160。
使用与本次 Mod 配套的新 Manager；更新客户端前先关闭旧 Manager。
客户端 Git 更新同时附带统一的 Manager 程序。

## 配置

| 位置 | 默认 | 含义 |
|---|---|---|
| `config.xml` 的 `Framework/Timer@updatefps` | 60 | 整数 1–1000；重启游戏后生效 |

缺失或无效的数值回退到原生默认的 60。FPS 上限不保证机器一定达到该帧率。
原有渲染同步和垂直同步配置保持原样，也可能进一步限制呈现帧率。
该 Mod 不改变分辨率或游戏时间来源。

## 日志与排查

`fps_limit: config.xml updatefps=160; effective FPS cap=160`

| 现象或日志 | 含义 |
|---|---|
| 符号校验失败，跳过 Mod | 客户端版本不同或指令被改动，未安装 FPS hook |
| `invalid value; using 60` | 解析出的数值不在 1–1000 范围内 |
| 保存成功，游戏没变化 | 重新启动选定的客户端 |
| 生效上限为 160，但实测更低 | 检查性能负载、呈现同步和实际渲染帧率 |

## 实现与验证范围

原生 XML 读取器将数值放进 `CApplication+0x80`，随后的一条指令会将其覆盖成 60。
Mod 校验 `fps.config.force60` 的原始字节，通过框架跳过完整的十字节覆盖指令，
保留 CPU 寄存器、标志及浮点状态。原生启动流程仍按 `1.0f / FPS` 初始化计时器。
版本或指令不匹配时停用，不猜地址。

框架/Mod 原生夹具检查、真实游戏渲染和联机时序分别验收。
高帧率完整战斗验收仍属后续检查，Beta 不代表这一部分已通过。

真实 core / 正式 Mod DLL 的 11 项夹具检查已覆盖 160/120/60、上下界、无效值回退、
禁用和符号拒绝，并检查寄存器、标志、浮点状态、LastError 和无关内存保护。
运行 `ctest --test-dir build -C Release -R "^fps_limit_runtime$" --output-on-failure` 可重现。
