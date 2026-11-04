# Scene registration lifecycle fix

[简体中文](scene-lifecycle-fix.zh-CN.md) · [Feature index](../README.md)

The original client removes a scene object from its current spatial node before
allocating the list node needed by its destination. If that native allocation
fails, the scene can still point to the old owner even though the old owner no
longer tracks it. Later cleanup can then follow a retired owner.

`scene_lifecycle_fix` allocates the destination's detached native list node
first. Only after allocation succeeds does it remove the old registration,
append the prepared node and assign the new owner. Normal movement keeps the
original native list shape and ownership result.

## Use and compatibility

The client component is enabled automatically and has no settings. Install the
server owner's client package or update, then restart the game. Server
configuration is not required.

The Mod supports the declared original client build only. It verifies the host
profile, required native symbols and complete reparent-function fingerprint
before installing its hook. A different build is rejected instead of being
patched speculatively.

## Native behavior preserved

- Moving to the current owner remains a no-op.
- The original native list allocator, removal routine and owner setter remain
  in use.
- A native allocation exception propagates unchanged.
- A null allocation result leaves the old registration and owner unchanged.
- The native list-capacity error retains its original message and exception
  path.

This repair closes a concrete ownership gap. It does not identify every native
scene-cleanup crash as an instance of that gap.

## Developer diagnostics

`SdgoSceneLifecycleGetStatusV1` returns read-only counters for reparent calls,
allocation attempts, committed transfers, same-owner calls, initially unowned
scenes, null allocations and capacity errors. The export does not mutate scene
state or provide a test-only control path.

If the Mod reports a missing symbol or fingerprint mismatch, use the supported
original client build supplied by the server owner. Include the client log when
reporting a failure on that build.
