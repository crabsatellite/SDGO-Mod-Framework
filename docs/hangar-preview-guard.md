# Hangar preview loading guard

This automatic fix protects the native unit-list 3D preview loading path. It checks failed vertex and index-buffer uploads and rejects a failed model before activation. The affected cell uses the game's existing 2D thumbnail; the inventory entry and selection remain available.

## Installation and behavior

Install the component through Mod Manager or the server owner's client package or update, then restart the client. It has no gameplay settings and appears in Manager's automatic-fix archive.

Successful uploads retain the original native instructions. Failure handling is scoped to a unit-list preview request: it skips invalid copies and unmatched unlocks, finishes native file parsing, releases the new unregistered resource through its original destructor, and reports load failure. The preview's temporary strings, candidate model, renderer critical section and row job each retain their native cleanup path.

The native thumbnail fallback applies only to the failed cell. It does not modify model files, unit definitions, account data, inventory order, combat or graphics-driver settings.

## Scope and diagnostics

This component does not intercept general heap frees or swallow unrelated access violations. It does not establish the producer of an already corrupted model or repair unrelated heap corruption.

Warnings identify the failed preview and operation. Developers can use `sdgo/hangar_preview_guard_observer.h` to read the scoped failure, fallback and cleanup counters. Test-only GPU fault injection belongs to GameTest, not the production component.

Focused tests load the production DLL and cover both vertex layouts, index uploads, native ownership, source closure, lock pairing and the setter's byte return ABI. Native acceptance injects one vertex-buffer creation failure into the shipped call path, verifies cleanup and same-slot recovery through real account return/relogin, and compares the complete inventory's identities, slots, levels and protection flags. Asynchronous detail-cache readiness is observed separately. Frame readback verifies the actual management page; a transient fallback may recover before a screenshot is taken.
