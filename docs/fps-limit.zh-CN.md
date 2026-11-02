# FPS 上限（Beta）

让客户端按现有 `config.xml` 的 FPS 配置运行，启动时不再强制覆盖为 60。
例如原有 `updatefps="160"` 会用于设置 160 FPS 上限。

| mod | 宿主 | 作用 |
|---|---|---|
| `fps_limit` | 客户端 | 用 `Framework/Timer@updatefps` 同时设置逻辑更新频率和渲染上限 |

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
同一个值控制原生逻辑更新频率，并在原生 Present 包装函数处限制渲染提交上限。
垂直同步和渲染负载仍可能进一步限制呈现帧率。
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
保留 CPU 寄存器、标志及浮点状态。1.0.2 还在原生空闲循环边界检查计时器间隔和独立渲染模式，
仅当页面切换重置了数值时恢复 `1.0f / FPS` 和独立渲染；稳定运行时不反复写入。
累计时间及时间来源仍由原生代码管理。

校验过的 `fps.device.present` 包装函数在调用原生 Present 前，通过 QueryPerformanceCounter
及匿名可等待计时器限速（系统支持时使用高精度计时器）。每次从实际提交起点安排下一次期限，
卡顿后不突发追帧。保留原生图形对象、窗口、HRESULT 和 LastError 行为。
只读观察接口 `SdgoFpsLimitGetSnapshotV1` 为测试提供配置的逻辑/渲染目标、实际限速周期和等待次数。
版本或指令不匹配时停用，不猜地址。

[原生 GameTest 矩阵](game-test.zh-CN.md) 在真实登录页依次验证「禁用 Mod、配置 160」，
以及启用后的 30、60、120、160。保留每段采样的完整平均值，检查逻辑更新频率、成功的
D3D9 提交、最小提交间隔及正式 Mod 的限速周期；30 FPS 对照必须真正触发等待。
实际渲染低于上限时照实记录，不将上限解释为性能保证。
高帧率完整战斗验收仍属后续检查，Beta 不代表这一部分已通过。

真实 core / 正式 Mod DLL 的 11 项夹具检查已覆盖 160/120/60、上下界、无效值回退、
禁用和符号拒绝，并检查寄存器、标志、浮点状态、LastError、无关内存保护、
原生 Present 调用约定、实际等待与卡顿恢复。
运行 `ctest --test-dir build -C Release -R "^fps_limit_runtime$" --output-on-failure` 可重现。
