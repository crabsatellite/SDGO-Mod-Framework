# Scene registration lifecycle fix

[简体中文](scene-lifecycle-fix.zh-CN.md) · [Back to the feature index](../README.md)

Fixes an occasional cleanup crash after a scene object moves to another area.

## Use

`scene_lifecycle_fix` is an automatic client fix with no settings, listed under **修复归档** (fix archive) in
SDGO Mod Manager. Restart the game after installing the server owner's client package or update; no server
configuration is needed.

The Mod supports the declared original client build only. Before hooking it verifies the host profile, the
required native symbols and the complete function fingerprint, and a different build is rejected rather than
patched speculatively. If the log reports a missing symbol or fingerprint mismatch, use the supported client from
the server owner; include the client log when reporting a failure on that build.

## Developer notes

The original client removes a scene object from its current spatial node before allocating the list node its
destination needs. If that native allocation fails, the scene can still point to the old owner even though the old
owner no longer tracks it, and later cleanup follows a retired owner.

The Mod allocates the destination's detached native list node first. Only after allocation succeeds does it remove
the old registration, append the prepared node and assign the new owner. Normal movement keeps the original list
shape and ownership result, and these native behaviors are preserved:

- Moving to the current owner remains a no-op.
- The original native list allocator, removal routine and owner setter remain in use.
- A native allocation exception propagates unchanged.
- A null allocation result leaves the old registration and owner unchanged.
- The native list-capacity error keeps its original message and exception path.

`SdgoSceneLifecycleGetStatusV1` returns read-only counters for reparent calls, allocation attempts, committed
transfers, same-owner calls, initially unowned scenes, null allocations and capacity errors. The export does not
mutate scene state or provide a test-only control path.
