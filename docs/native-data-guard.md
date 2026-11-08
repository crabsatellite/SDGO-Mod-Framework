# Native data and lifecycle guards

[简体中文](native-data-guard.zh-CN.md) · [Back to the feature index](../README.md)

## What it does

Fixes client crashes when unit data is missing or unit management is still waiting for data, and keeps crash logs accurate. The client Mod `native_data_guard` handles these cases:

- Funnel mount names accept indices 1–6. Other indices return the game's empty result, so an invalid mount cannot read outside the name array.
- Missing name records use `未命名` (unnamed) in the two native display formats. Valid names keep their original records.
- If a unit's primary information record is unavailable, the detail page skips that refresh before either its normal or transform readers can dereference null. A later native selection or refresh uses the original display path once the record exists.
- A missing derived record during unit-information refresh follows the game's existing zero-denominator path and retains the default percentage instead of reading a null pointer.
- When a model has no optional transform metadata, its base-model loading continues through the original finalization and completion steps.
- Native exception reporting walks a separate context copy, so its exception address, access type, module range and registers remain the original pre-walk values.
- When unit management waits for inventory data, the service keeps its own native-cloned message instead of retaining the dispatcher's temporary object.
- Model material lookups check the actual native container before reading a slot. Missing optional decal materials advance to the next slot while retaining the model's original commit and finalization path. This shared lookup protects unit presentations used by the encyclopedia, capsule previews and other native pages.
- Room cameras accept only the native 0–29 sample range. An invalid drag or slider result retains the prior selection. A missing model or invalid camera state skips that frame's camera update; later ready frames use the original update normally.
- While the initial login inventory is requesting capacity or receiving pages, page-opening intents wait while the current native packet receiver finishes its work. The encyclopedia's separate request waits for the same native completion flag and opens automatically afterward. A different page or inventory owner cancels a stale request. Waiting follows real packet completion rather than a fixed number of seconds.

These protections do not supply missing models, add units, change weapons, or alter battle rewards. They cover the named data paths; unrelated client faults need their own diagnosis.

## Use

Install the server owner's client package or update. The Mod starts automatically on the supported client and appears under **修复归档** (fix archive) in Mod Manager. It has no gameplay settings. Restart the client after installing or updating it.

The supported host is `GOnline-54BE1568-00C97000`. The framework checks the named instruction signatures before activation. A different client or a conflicting patch must be reviewed before use.

## Logs

The framework log contains `native data guards active` when activation succeeds. Missing names, information records and optional transform records produce bounded warnings. Valid data does not produce a warning per frame. A native exception also records the original pre-walk registers and access address in the framework log.

`base information unavailable` identifies a skipped primary-record refresh and includes the unit ID. After loading, select the unit again or reopen unit management. If the warning persists, the server owner needs to check the unit's client resources; the guard does not create missing content.

For another crash, retain the current native exception log and both current and previous framework logs before restarting repeatedly. Match the session and timestamps; copied client folders can contain historical exception records from another installation.

`wait-room camera sample` identifies a rejected camera index. `wait-room camera data is not ready` means the current update was deferred until a later frame. Both warnings are bounded. Missing material slots are counted by the developer observer without logging every lookup.

## Developer notes

The funnel lookup's original condition admits any signed index because it uses an OR instead of the intended bounded range. A signature-checked two-byte branch correction sends invalid indices to its existing empty-result path.

The two display-name callers are checked by return address. Their silent preflight reproduces the native type-2/type-24 lookup key; a missing record receives a packed immutable text record with text at byte 13. Other callers and valid records retain native lookup behavior.

The model-loading guard acts after primary model loading, at the optional transform-record read. It preserves the live x86 context and continues at the original finalizers on a null record. It does not skip the entire loading job or replace a unit with a different one.

The information-page guard acts at the first read after the derived-record lookup. A null result supplies a zero record only to the displaced native read, so the following code takes the game's existing default-percentage branch. Valid records and the original formula are unchanged.

The whole-detail entry separately preflights the primary type-2 key at receiver `+0x114` with the native silent lookup. The shipped error path otherwise retains a null base record, reads it at `+0x13C` in transform mode, or passes it to later detail readers. A miss returns without modifying the receiver or invoking those readers. Successful lookups delegate once to the original function; there is no failure latch, so later callbacks can refresh normally. The original secondary transform lookup and its base-record fallback remain unchanged.

The unit-management lifecycle guard acts only on the verified deferred-message slot. It invokes the message's native clone virtual before the dispatch callback returns, deep-copies that owned message when the service is copied, and releases a still-pending copy during replacement or teardown. Replacement, service copy and teardown each use independent ownership; the shipped loading-completion path continues to clone the message into its queue and destroy the service-owned copy once.

The exception-report guard wraps the native writer and, only for that thread and invocation, redirects the writable `DBGHELP!StackWalk` `ContextRecord` to an independent copy. The original writer retains its stack trace and report flow, while the walk can no longer mutate the registers printed afterward.

`native_data_guard_actual_dll` exercises the built DLL and its actual site stubs, including deferred-message save, replacement, service copy, normal completion and teardown. When this Mod is installed, the native GameTest bootstrap also exercises the host's funnel, name-format and information-refresh functions before continuing through its authenticated room/battle flow. The information refresh uses private display receivers and checks the original default-percentage output without editing a live unit or table. `SdgoNativeDataGuardGetStatusV1` and `V2` preserve their existing observer ABIs; `SdgoNativeDataGuardGetStatusV3` adds read-only counters for deferred native message copies, replacements, copy failures, native completions and currently outstanding owned messages.

The actual-DLL test also covers missing primary records in both detail modes, an unavailable grid, an empty lookup result, unchanged page state, and resumption on a later valid callback. The native bootstrap invokes the real whole-detail entry with private receivers and an unavailable key in both modes. `SdgoNativeDataGuardGetStatusV4` adds the skipped primary-record count while retaining the earlier exports.

The material guard validates the two native CModel vector triplets and delegates valid lookups, including the separate shared-resource branch, once to the original implementation. Its decal-result site skips only the unavailable slot and continues at the original loop increment, before the original finalizers. It does not replace materials or alter asset identifiers. `native_material_guard_actual_dll` executes the built lookup and x86 result-site branches with empty, short, malformed and valid containers.

The wait-room camera reads a 30-sample table. Both the selection setter and frame update enforce that range; no out-of-range value is written back and no fallback sample is invented. The camera model must also exist before the native update runs. The same update is reached by the native wait-room main states rather than being restricted to a single menu button.

`SdgoNativeDataGuardGetStatusV5` adds unavailable-material, skipped-decal, rejected-camera-index and skipped-camera-update counters while retaining V1–V4. The native bootstrap exercises the genuine hooked entries with private material, selection and unloaded-room receivers; it does not modify a live unit, camera selection or data table.

The initial-inventory guard reads the native `CUnitInfo+0x20` completion flag. It acts at the bottom-menu business and the separate encyclopedia entry, before navigation commands and network requests can replace the initial packet consumer. MyRoom's native inventory bootstrap remains available. The guard does not coalesce inventory requests, fabricate replies or accept `0x279A` and other server errors. Existing scene-transition and music hooks retain their separate ownership.

A pending menu action retains only the application, inventory owner, source page and action number. After normal native frame updates and validation, it resolves the currently registered menu and control and delegates through the original menu callback, preserving its parent and starter-busy checks. A rebuilding, temporarily disabled or busy menu can wait; changing the application, inventory or page cancels stale intent. The encyclopedia opens once at the same completion boundary. No temporary message or widget pointer is retained.

`SdgoNativeDataGuardGetStatusV6` adds waiting, resume and catalog-cancellation counters while retaining earlier exports. Actual-DLL tests cover capacity and pagination phases, MyRoom bootstrap, busy callbacks, rebuilt menus and changed page/owner identities. The separate native `early_navigation_before_inventory` scenario checks genuine terminal replies, completion flags, guard triggers, target readiness and normal recovery.
