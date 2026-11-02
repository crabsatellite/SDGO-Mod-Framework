# Apsalus III transformation input recovery

`transform_input_fix` fixes the Apsalus III transformation state that can remain stuck and block movement and attacks. It is an automatic client fix: install it, restart the game, and view its status in SDGO Mod Manager's repair archive. No settings are required.

## Compatibility

The supported original client is `GOnline-54BE1568-00C97000`. Recovery is limited to the `24787/24788` pair. Inventory, levels, experience, upgrades and paint retain their original data. Normal transformations and other units keep their native behavior.

## Native behavior

An alternate-form sortie can swap models before requesting a missing `change_ma` animation; the reverse edge can similarly request a missing `change_ms`. The animation request returns zero while previous animation events remain, so the original transformation update can wait indefinitely.

The Mod calls the original update once. It intervenes only for the exact unit pair in transformation mode 3, a zero requested animation handle, retained animation events and a valid latched target. It uses the native empty-event WAIT path and normal transformation release. The native functions retain responsibility for form commits, weapons and cleanup. Existing animation resources are used.

The supported native functions are `TransformUpdate` at `0x006D9AD0`, `TransformRelease` at `0x006D8E40`, `CUnit::Transform` at `0x006798F0`, and the state signal at `0x00BE3920`. Symbol signatures bind the implementation to the supported executable. The movement FSM is `CUnit+0x9C8`; the native transform-request business reads its owner at `+0x21C`.

## Troubleshooting and testing

Check that the client is updated, the fix is installed and the game has restarted. Include the starting form, transformation direction, battle mode and client version in a bug report. A log entry containing `Apsalus III null transform animation` identifies a recovered missing-animation transition.

The native regression exercises a real transformation round trip, native movement and primary-attack consumers, and the full battle/return flow. Follow the [GameTest guide](../skills/sdgo-native-testing/SKILL.md) for the isolated local test workflow. Offline ABI and original-function replay checks supplement gameplay verification.
