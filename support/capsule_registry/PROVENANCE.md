# Capsule registry support provenance

The three native sources in this directory began as a working-tree snapshot
copied from the repository's `investigate/capsule-machines` worktree, whose
checked-out base commit was `5838080`. That worktree had uncommitted capsule
runtime work, so the commit alone does not identify the imported bytes. The
first hashes below identify that input.

| Imported file | SHA-256 |
|---|---|
| `capsule_catalog.cpp` | `498b1a44f89fa16df153abd709beafa9e43f3df547b5165e81ea5ae7c7299cb0` |
| `capsule_runtime.cpp` | `59bc5afc16c98d89a92eeffdd8b7e574811867f3f8c48cbace7771661e252acf` |
| `capsule_runtime.h` | `95a3c76fb55126599ed46a47a2bf8a02785c9f1f2d8a69f654169ba8b425adb4` |

The collection activity update extends that provider to compose `0031.gdt`
unit-item categories and `0099.gdt` activity machines through the same
`CZoaPack::OpenFile` hook. It adds activity-count readback but deliberately
does not count item-category records, so unrelated dynamic type-`0x22`
registrations remain valid.

The six-table provider ships as component version `1.1.1`. Its startup log
references the stable ASCII marker `SDGO_CAPSULE_REGISTRY_6TABLE_V2`, which
allows packagers to reject an older four-table DLL before publishing a
registry containing activity machines or unit-item categories.

The acquisition extension also exposes `SDGO_COLLECTION_ACQUISITION_V1`. It consumes hashed, owner-generated initial-form metadata and updates only the native type-85 completed-product explanation at byte 214. Original source text and blueprint fields are retained; no game archive is rewritten.

| Current file | SHA-256 |
|---|---|
| `capsule_catalog.cpp` | `01884256ee891872aeb318b17ca7eccca5cea45c158d39ba3418926b49d7b43c` |
| `capsule_runtime.cpp` | `69b8909fa89516437324a1bb042cfdd2f3d36d6c7fb45a9efd69673e0d99725f` |
| `capsule_runtime.h` | `ac6a11a2907489384a59a403175c96c06a584e53ed0f1d8955cc17755a9e1f80` |
| `acquisition_hints.h` | `eeaae0d3f43b9d4b658bd1a52be20c3456d99f6e403e35408165d300744f4bcc` |

The source repository is MIT licensed. The project root `LICENSE` applies to
these sources. `Install-Client.ps1` is the matching standalone installer
implementation and is maintained here as packaging support, outside the three
source-hash assertions above.
