# Wait-room window initialization guard

[简体中文](waitroom-window-fix.zh-CN.md) · [Back to the feature index](../README.md)

Fixes the client sometimes closing right after a room is created.

## Use

`waitroom_window_fix` is an automatic client fix with no settings, listed under **修复归档** (fix archive) in
SDGO Mod Manager. Restart the game after installing the server owner's client package or update; no server
configuration is needed.

The Mod activates only on the supported original client; a different build is left unhooked and the log says why.
If room entry still fails, include the client log and the room-entry steps in the bug report.

## Developer notes

Windows can request a window's icon while the native waiting-room service is still attaching its view, and the
original message callback dereferences that empty view. For `WM_GETICON` while the view is absent, the Mod
returns the callback's usual unhandled result so the application's default window path answers the query. Once
the view is attached, the original callback receives its original arguments; other messages keep the original
path. The hook requires the supported native client profile and the complete original callback fingerprint.
