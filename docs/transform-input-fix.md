# Missing-animation transformation input recovery

[简体中文](transform-input-fix.zh-CN.md)

`transform_input_fix` fixes the Apsalus III transformation state that can remain stuck and block movement and attacks. It is an automatic client fix: install it, restart the game, and view its status in SDGO Mod Manager's repair archive. No settings are required.

## Compatibility

The supported original client is `GOnline-54BE1568-00C97000`. Recovery covers the `24787/24788` pair and the same
empty-animation failure when a paired `unit_catalog` reuses an imported chain model. The catalog independently checks
ownership, a living actor, complete native records and an already committed controller target. Inventory, levels,
experience, upgrades and paint retain their original data. Normal transformations keep their native behavior.

## Native behavior

An alternate-form sortie can swap models before requesting a missing `change_ma` animation; the reverse edge can similarly request a missing `change_ms`. The animation request returns zero while previous animation events remain, so the original transformation update can wait indefinitely.

The Mod calls the original update once. It intervenes only for the exact unit pair in transformation mode 3, a zero requested animation handle, retained animation events and a valid latched target. It uses the native empty-event WAIT path and normal transformation release. The native functions retain responsibility for form commits, weapons and cleanup. Existing animation resources are used.
Imported cycles can retain an earlier model's animation events when returning to it. The optional read-only
`SdgoIsImportedTransformRecoveryV1` query admits only an owned, living and already committed chain to the same native
cleanup path. It does not add an animation, force a timeout, resend an accepted request or override server graph authority.

The supported native functions are `TransformUpdate` at `0x006D9AD0`, `TransformRelease` at `0x006D8E40`, `CUnit::Transform` at `0x006798F0`, and the state signal at `0x00BE3920`. Symbol signatures bind the implementation to the supported executable. The movement FSM is `CUnit+0x9C8`; the native transform-request business reads its owner at `+0x21C`.

## Troubleshooting and testing

Check that the client is updated and the game has restarted. `missing-animation transform input recovery active` in
`sdgo/logs/sdgo.log` means the fix loaded; `null transform animation: queued native WAIT and TransformRelease` identifies
a recovered transition. Include the starting form, transformation direction, battle mode and client version in a bug report.

The native regression performs a real transformation round trip and checks native movement and primary attack afterwards; see [native GameTest](game-test.md).
