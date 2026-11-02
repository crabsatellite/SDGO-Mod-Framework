# Agent relay log fix

Core fix for the Agent server. One of the Agent's built-in relay log messages prints an integer
IP address as if it were text, which makes the Agent read invalid memory while relaying traffic.
The fix corrects that log format before relay traffic starts.
中文：[agent-relay-log-fix.zh-CN.md](agent-relay-log-fix.zh-CN.md)

| mod | host | part |
|---|---|---|
| `agent_relay_log_fix` | Agent server | the whole feature |

## Use

Install the server package on Agent hosts with `install_server.ps1 -Role Agent`; see
[Setup](setup.md). The fix is on by default and SDGO Mod Manager lists it under **修复归档**
(fix archive). To switch it off while troubleshooting, click **显示开发信息** (show developer info),
clear its switch and restart the Agent. Owner and player networking setup is in the
[FRP guide](frp-distribution.md).

## Settings

None.

## Logs

| log line | meaning |
|---|---|
| `corrected native relay printf IP ...` | fix applied |
| `native relay log already uses an integer IP specifier` | already correct, nothing changed |
| `native relay log format differs; no patch applied` | unexpected Agent build; nothing changed |
| `required symbol agent.relay_log_format ...` | unsupported Agent build; the mod stays off |
