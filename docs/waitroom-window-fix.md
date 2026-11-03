# Wait-room window initialization guard (Beta)

[简体中文](waitroom-window-fix.zh-CN.md) · [Feature index](../README.md)

Windows can request a window's icon while the native waiting-room service is
still attaching its view. The original message callback dereferences that empty
view, which can close the client immediately after a room is created.

`waitroom_window_fix` returns the callback's usual unhandled result for
`WM_GETICON` while the view is absent. The application's existing default window
path then processes the query. Once the view is attached, the original callback
receives its original arguments. Other messages retain the original path.

The client component is enabled automatically and has no settings. Mod Manager
lists it in the automatic-fix archive. Restart the game after installing the
server owner's client package or update; no server configuration is needed.

The hook requires the supported native client profile and the complete original
callback fingerprint. A different callback is rejected and logged. If room entry
still fails, include the client log and the room-entry steps in the bug report.
This Beta is awaiting its original-client room/battle/return check.
