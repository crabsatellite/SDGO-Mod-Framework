# 阿普萨拉斯 III 变形卡死修复

[English](transform-input-fix.md) · [返回功能目录](../README.zh-CN.md)

`transform_input_fix` 修复阿普萨拉斯 III 在变形后持续卡住、无法移动或攻击的问题。它是自动生效的客户端修复，安装后重启游戏即可，无需设置；在 SDGO Mod Manager 的「修复归档」中查看。

## 适用范围

支持原版客户端 `GOnline-54BE1568-00C97000`，处理机体 `24787/24788` 两形态之间的异常变形。机库里的机体、等级、经验、强化和涂装沿用原数据。正常变形和其他机体仍走原生流程。

## 工作方式

从 `24788` 形态出击时，原生流程可能在切换模型后请求该模型缺少的 `change_ma`；反向也可能请求缺少的 `change_ms`。动画接口返回空句柄，但旧动画事件仍在到达，导致变形更新一直等待。

修复只在这对机体的变形模式为 3、请求句柄为空、旧动画事件仍存在且已锁定目标形态时介入，使用原生等待与变形退出流程完成收尾。形态提交、武器切换和清理由原生函数完成。缺失的过渡动画仍使用现有资源。

## 排查

确认客户端已经更新并重启。`sdgo/logs/sdgo.log` 中有 `Apsalus III missing-animation transform input recovery active` 表示修复已加载；
`Apsalus III null transform animation` 表示本次变形触发了恢复。若仍出现问题，请记录出击形态、变形方向、对战模式及客户端版本。

开发者可参阅[英文说明](transform-input-fix.md#native-behavior)中的原生函数位置，回归测试见[原生 GameTest](game-test.zh-CN.md)。
