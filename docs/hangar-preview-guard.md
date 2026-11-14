# Hangar preview loading guard

This automatic fix protects the native unit-list and main-hangar 3D preview loading paths. It checks failed vertex and index-buffer uploads and rejects a failed model before activation. An affected list cell uses the game's existing 2D thumbnail; the inventory entry and selection remain available. The main display follows its original empty-model branch when the upload fails.

## Installation and behavior

Install the component through Mod Manager or the server owner's client package or update, then restart the client. It has no gameplay settings and appears in Manager's automatic-fix archive.

Successful uploads retain the original native instructions. Failure handling is scoped to a unit-list preview request or the verified main-hangar setter caller: it skips invalid copies and unmatched unlocks, finishes native file parsing, releases the new unregistered resource through its original destructor, and reports load failure. The preview's temporary strings, candidate model, renderer critical section and row job each retain their native cleanup path. The main-hangar caller already checks for a null model and releases its own renderer lock. The guard does not release that lock or issue a list-thumbnail operation; later native loading attempts remain available.

The native thumbnail fallback applies only to the failed cell. It does not modify model files, unit definitions, account data, inventory order, combat or graphics-driver settings.

## Scope and diagnostics

This component does not intercept general heap frees or swallow unrelated access violations. It does not establish the producer of an already corrupted model or repair unrelated heap corruption.

Warnings identify the failed preview and operation. Developers can use `sdgo/hangar_preview_guard_observer.h` to read the scoped failure, fallback and cleanup counters. Test-only GPU fault injection belongs to GameTest, not the production component.

The additive `SdgoHangarPreviewGuardGetStatusV2` observer keeps V1 unchanged and separately counts main-hangar requests, successes and failures. Actual-DLL tests cover failed and empty 32-byte vertex uploads, index uploads, false model results, source/root/candidate cleanup, native lock ownership and a later successful main preview. These tests do not imply that every device-loss or graphics-driver failure is recoverable.

For an isolated normal-PvP regression, GameTest SDK 0.1.44 can opt into `MainHangarPreviewFault=1` with this component 0.2.0 or later. Its one-shot VB32 creation failure is gated by the production observer's thread-local main-hangar scope. The battle-return case requires exactly one injection, completed native root/source/candidate cleanup, a subsequent successful main-preview load and the real lobby return. It refuses page-only, mission and FPS scenarios. Leave the setting at its default zero for other tests; it is not a player configuration.

Focused tests load the production DLL and cover both vertex layouts, index uploads, native ownership, source closure, lock pairing and the setter's byte return ABI. Native acceptance injects one vertex-buffer creation failure into the shipped call path, verifies cleanup and same-slot recovery through real account return/relogin, and compares the complete inventory's identities, slots, levels and protection flags. Asynchronous detail-cache readiness is observed separately. Frame readback verifies the actual management page; a transient fallback may recover before a screenshot is taken.
