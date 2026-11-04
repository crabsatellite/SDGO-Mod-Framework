# Native battle data guards

[中文](native-data-guard.zh-CN.md) · [Feature index](../README.md)

## What it does

The client Mod `native_data_guard` protects native data paths, owns a deferred unit-management message safely, and preserves exception reports:

- Funnel mount names accept indices 1–6. Other indices return the game's empty result, so an invalid mount cannot read outside the name array.
- Missing name records use `未命名` (unnamed) in the two native display formats. Valid names keep their original records.
- A missing derived record during unit-information refresh follows the game's existing zero-denominator path and retains the default percentage instead of reading a null pointer.
- When a model has no optional transform metadata, its base-model loading continues through the original finalization and completion steps.
- Native exception reporting walks a separate context copy, so its exception address, access type, module range and registers remain the original pre-walk values.
- When unit management waits for inventory data, the service keeps a native-cloned message instead of retaining the dispatcher's temporary object. Replacement, service copy and teardown each use independent ownership, while the original completion path still queues one clone and releases the service's copy once.

These protections do not supply missing models, add units, change weapons, or alter battle rewards. They cover the named data paths; unrelated client faults need their own diagnosis.

## Use

Install the server owner's client package or update. The Mod starts automatically on the supported client and appears in Mod Manager's automatic-fix archive. It has no gameplay settings. Restart the client after installing or updating it.

The supported host is `GOnline-54BE1568-00C97000`. The framework checks the named instruction signatures before activation. A different client or a conflicting patch must be reviewed before use.

## Logs

The framework log contains `native data guards active` when activation succeeds. Missing names, information records and optional transform records produce bounded warnings. Valid data does not produce a warning per frame. A native exception also records the original pre-walk registers and access address in the framework log.

For another crash, retain the current native exception log and both current and previous framework logs before restarting repeatedly. Match the session and timestamps; copied client folders can contain historical exception records from another installation.

## Developer notes

The funnel lookup's original condition admits any signed index because it uses an OR instead of the intended bounded range. A signature-checked two-byte branch correction sends invalid indices to its existing empty-result path.

The two display-name callers are checked by return address. Their silent preflight reproduces the native type-2/type-24 lookup key; a missing record receives a packed immutable text record with text at byte 13. Other callers and valid records retain native lookup behavior.

The model-loading guard acts after primary model loading, at the optional transform-record read. It preserves the live x86 context and continues at the original finalizers on a null record. It does not skip the entire loading job or replace a unit with a different one.

The information-page guard acts at the first read after the derived-record lookup. A null result supplies a zero record only to the displaced native read, so the following code takes the game's existing default-percentage branch. Valid records and the original formula are unchanged.

The unit-management lifecycle guard acts only on the verified deferred-message slot. It invokes the message's native clone virtual before the dispatch callback returns, deep-copies that owned message when the service is copied, and releases a still-pending copy during replacement or teardown. The shipped loading-completion path continues to clone the message into its queue and destroy the service-owned copy.

The exception-report guard wraps the native writer and, only for that thread and invocation, redirects the writable `DBGHELP!StackWalk` `ContextRecord` to an independent copy. The original writer retains its stack trace and report flow, while the walk can no longer mutate the registers printed afterward.

`native_data_guard_actual_dll` exercises the built DLL and its actual site stubs, including deferred-message save, replacement, service copy, normal completion and teardown. When this Mod is installed, the native GameTest bootstrap also exercises the host's funnel and name-format functions before continuing through its authenticated room/battle flow. `SdgoNativeDataGuardGetStatusV1` and `V2` preserve their existing observer ABIs; `SdgoNativeDataGuardGetStatusV3` adds read-only counters for deferred native message copies, replacements, copy failures, native completions and currently outstanding owned messages.
