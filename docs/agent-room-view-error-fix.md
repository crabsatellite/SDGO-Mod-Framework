# Agent room-view error forwarding fix

[简体中文](agent-room-view-error-fix.zh-CN.md) · [Back to the feature index](../README.md)

Fixes occasional error dialogs during room actions that have nothing to do with what actually happened.

## Use

This automatic AgentServer fix is enabled by default, has no settings and is listed under **修复归档** (fix archive)
in SDGO Mod Manager. Install it on every AgentServer that serves players, then restart those AgentServer processes;
clients need no matching component. If a room operation really fails, the client still receives and shows the
original native error.

## Developer notes

The original AgentServer routes native room-view error reply `0x0880` to the correct authenticated client, but
forwards only the first four bytes of the eight-byte BattleServer reply, so the client reads unrelated memory for the
missing error field. The Mod preserves the complete eight-byte reply at that one verified Agent forwarding call; the
destination player, native error code, send flags, room logic and every other packet are unchanged.
