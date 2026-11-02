# Agent 中继日志修复

Agent 服务器的基础修复。Agent 自带的一条中继日志把整数 IP 地址当作文字输出，会让 Agent 在转发流量时
读取无效内存。本修复在中继开始前改正这条日志格式。
English: [agent-relay-log-fix.md](agent-relay-log-fix.md)

| mod | 运行在 | 负责 |
|---|---|---|
| `agent_relay_log_fix` | Agent 服务器 | 整个功能 |

## 使用

在 Agent 宿主使用 `install_server.ps1 -Role Agent` 安装服务器部分，见[安装说明](setup.zh-CN.md)。
本修复默认启用，在 SDGO Mod Manager 的“修复归档”中列出。排查问题需要临时关闭时，点“显示开发信息”，
再关闭它的开关并重启 Agent。服主和玩家的联机配置见 [FRP 分发指南](frp-distribution.zh-CN.md)。

## 设置

无。

## 日志

| 日志 | 含义 |
|---|---|
| `corrected native relay printf IP ...` | 已修正 |
| `native relay log already uses an integer IP specifier` | 已经正确，未做修改 |
| `native relay log format differs; no patch applied` | Agent 版本与预期不同，未做修改 |
| `required symbol agent.relay_log_format ...` | Agent 版本不受支持，本 mod 不启用 |
