# Native battle data guards

[中文](native-data-guard.zh-CN.md) · [Feature index](../README.md)

## What it does

The client Mod `native_data_guard` protects three native data paths:

- Funnel mount names accept indices 1–6. Other indices return the game's empty result, so an invalid mount cannot read outside the name array.
- Missing name records use `未命名` (unnamed) in the two native display formats. Valid names keep their original records.
- When a model has no optional transform metadata, its base-model loading continues through the original finalization and completion steps.

These protections do not supply missing models, add units, change weapons, or alter battle rewards. They cover the named data paths; unrelated client faults need their own diagnosis.

## Use

Install the server owner's client package or update. The Mod starts automatically on the supported client and appears in Mod Manager's automatic-fix archive. It has no gameplay settings. Restart the client after installing or updating it.

The supported host is `GOnline-54BE1568-00C97000`. The framework checks the named instruction signatures before activation. A different client or a conflicting patch must be reviewed before use.

## Logs

The framework log contains `native data guards active` when activation succeeds. Missing names and optional transform records produce bounded warnings with their data identifiers. Valid data does not produce a warning per frame.

For another crash, retain the current native exception log and both current and previous framework logs before restarting repeatedly. Match the session and timestamps; copied client folders can contain historical exception records from another installation.

## Developer notes

The funnel lookup's original condition admits any signed index because it uses an OR instead of the intended bounded range. A signature-checked two-byte branch correction sends invalid indices to its existing empty-result path.

The two display-name callers are checked by return address. Their silent preflight reproduces the native type-2/type-24 lookup key; a missing record receives a packed immutable text record with text at byte 13. Other callers and valid records retain native lookup behavior.

The model-loading guard acts after primary model loading, at the optional transform-record read. It preserves the live x86 context and continues at the original finalizers on a null record. It does not skip the entire loading job or replace a unit with a different one.

`native_data_guard_actual_dll` exercises the built DLL and its actual site stub. When this Mod is installed, the native GameTest bootstrap also exercises the host's funnel and name-format functions before continuing through its authenticated room/battle flow. `SdgoNativeDataGuardGetStatusV1` provides read-only guard counters to observers.
