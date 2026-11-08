# Agent 房间视图错误转发修复

[English](agent-room-view-error-fix.md) · [返回功能目录](../README.zh-CN.md)

修复房间操作时偶尔弹出与实际情况无关的错误提示。

## 使用

这是 AgentServer 的自动修复，默认启用，没有可调设置，在 SDGO Mod Manager 的「修复归档」中列出。
把它安装到所有承载玩家的 AgentServer，然后重启这些 AgentServer 进程；客户端不需要配套组件。
房间操作确实失败时，客户端仍会收到并显示原生错误。

## 开发者说明

原版 AgentServer 会把原生房间视图错误回复 `0x0880` 路由给正确的已登录玩家，
但只转发 BattleServer 八字节回复的前四个字节。错误字段因此缺失，客户端会继续读取无关内存。
本 Mod 只在已验证的 Agent 转发调用处保留完整的八字节回复；目标玩家、原生错误码、发送标志、
房间逻辑和其他数据包都保持原版行为。
