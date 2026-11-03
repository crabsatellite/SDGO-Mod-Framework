# Original constructor resource bindings

`sdgo_resource_bindings` is a static importer component. It changes an owned
resource argument at the existing missile initializer's string-construction
call. It does not create a second controller, replace projectile rules, or hook
effect update, attachment, detach, transform or particle lifetime.

The original initializer has already retained its type-10 weapon record and
effect object at this point. The source adapter reads the entry weapon ID from
the original frame and compares all 1,871 record bytes with the immutable
prepared snapshot. The resource therefore belongs to the weapon at birth,
independently of the owner's subsequent displayed form. The original native
string constructor and effect loader consume the redirected path normally.

## Trusted preparation boundary

The typed input is in `sdgo/resource_bindings.h`; the wire manifest is described
by `resource-bindings.schema.json`. All candidates are prepared together before
installing a hook. The importer supplies an immutable `Authority` backed by its
actual allocation registry and published native resource overlay. It must prove
that each target ID is allocated to that owner and record hash, does not belong
to the original baseline, and that every declared resource path/hash is really
published. A candidate's own flags or claims are not allocation evidence.

Resource paths use distinct owner-prefixed names. Target record IDs and SHA-256
must match the actual complete record bytes. Duplicate owners/IDs/resources,
missing publication, changing authority revisions and incompatible constructor
branches are rejected before activation. The native empty-model, non-split
fallback branch requires an explicit effect; an explicit model must not invent
one. The native branch reads the original initializer byte, rather than a
guessed wider classification.

The component verifies publication witnesses; it is not a ZFX/model parser.
The source adapter must also validate the complete typed resource closure and
format compatibility. A valid hash alone is not rendering acceptance.

## Native integration

Pass the immutable prepared `Bindings` to `InstallFixedEffectHook`. It obtains
the original initializer and instruction site through the verified SDK symbol
API, checks their exact relative location and the entire original initializer
fingerprint, and installs one narrow `HookSite`. Native content manifests cannot
supply addresses or weaken this profile.

The detour preserves integer registers, flags, x87/SSE state and Win32 last-error,
changes only the already-pushed resource pointer, then executes the normal
trampoline. Unowned weapons retain their exact original pointer. The prepared
set and path storage stay alive for the module's process lifetime; no hot
replacement/unload API is exposed.

An owned record/argument/context mismatch cannot resume a partially initialized
object. A mandatory candidate-client closure callback is invoked. If it returns,
the adapter terminates the current candidate process. It never substitutes the
baseline effect to hide a missing owned binding and never claims native object
rollback. Preparation failures return an error without installing the hook.

## Verification scopes

The policy fixture tests real target record bytes and SHA-256 against a neutral
allocation/publication authority. It includes cross-owner, baseline-ID,
publication, branch and mutation failures.

Optional original-client replay executes the authenticated original path-setup
instructions and the real framework/MinHook instruction detour in an owned
private child. It checks resource selection, registers/flags/x87/SSE/last-error,
the original retained effect pointer, baseline pass-through, a second owner and
immediate fatal-context termination. The string-constructor callee is a bounded
fixture capture and execution stops before the native effect loader. No original
entry/TLS/import initialization, game window, renderer or native effect lifetime
runs. This is ABI/call-site evidence, not original-game rendering or gameplay
acceptance.
