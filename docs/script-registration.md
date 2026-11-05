# Original-VM script registration

`sdgo_script_registration` is a static library for an importer or owner module.
It carries no game scripts and does not create a Lua VM. The module supplies
authenticated external chunks and symbol-profile-verified original VM calls.
There is no automatic game hook, live content replacement, or Mod installation.

The typed input and result are in `sdgo/script_registration.h`. The external
manifest is described by `script-registration.schema.json`. A resource provider
resolves each manifest `member` from its sealed content root, verifies the member
and fills `Program.bytes`; the consumer independently rechecks the SHA-256.
Manifest addresses are never executable bindings. Bind native functions through
the framework's verified symbol API before constructing `NativeLuaCalls`.
`BindNativeLuaCalls` resolves the complete named symbol set and leaves no partial
binding on failure. Install `lua.script_load` through `HookCode`; pass the returned
original trampoline rather than the hooked entry to `NativeRegistration`.

## Admission and execution

Each candidate has one owner and revision. Every program must use an independent
`scp/<owner>_*.scp` path and owner-prefixed function exports. All programs are
validated before any executes: byte budgets, actual chunk hashes, exact Lua 5.0
x86 layout, and real root publications must match the declarations.

The registration root may only publish non-capturing closures and return no
values. Function bodies remain the original authored programs; this is not an
interpreter or a gameplay sandbox. Other initialization programs are explicitly
unsupported. This narrow root profile prevents loading a function-definition
carrier from accidentally executing top-level combat or overwriting undeclared
original globals during registration.

Use `NativeRegistration::onOriginalLoad` from the original file-loader hook.
Supply the framework's original trampoline, the game main thread identified by
`ProcessInitialThreadId(api)`, and a mandatory candidate-client close callback. A
zero identity must refuse admission; the current callback thread is not a fallback.
The adapter
passes ordinary loads through. For the original root, it first invokes the
unmodified loader and only then admits registration on that same main thread.
The engine's first state pointer must equal the original state getter. A newly
initialized VM requires a fresh registry instance.

The adapter consumes the original 24-byte x86 native wide-string ABI. Lua calls
are cdecl; the original file loader is thiscall with one native-string pointer
argument and four callee-popped argument bytes. The original buffer loader takes
`(state, bytes, uint32 size, ASCII resource name)` and returns the native
load/protected-call status. Do not substitute a status-less file-loader return
for execution success. The caller must also read back every declared global as
a function with a nonzero closure identity. Root load completion alone is not
registration evidence.

## Conflicts, lifetime and failure

A preexisting export, changed registered function, or mismatched revision is not
overwritten. Exact repeated registration verifies actual function identities and
does not execute chunks again. Different owners can coexist. Replacement or
unload requires a new client VM lifetime; active actors must not lose their
callbacks during an encounter.

An execution or post-execution readback error poisons the candidate registry and
calls the mandatory close/quarantine callback. `clientMustClose` remains true on
subsequent attempts and no more chunks execute. This is **not transactional Lua
rollback**: some exports can already exist in the failing VM. Restoring the
temporary Lua stack does not undo global or native side effects. The owner must
close the candidate client and must not continue unit activation or gameplay.

`Result` distinguishes pre-execution rejection, successful registration,
idempotent readback, restart requirements, and a fatal candidate VM. Preserve the
reason and `executionStarted`/`clientMustClose` in the importer's activation
receipt. An accepted manifest or a copied SCP file cannot stand in for that
receipt.

`onOriginalLoadBatch` admits multiple owners at one original root completion.
It checks every owner's program bytes, resource/export names and existing VM
globals before executing the first owner. A failure after any owner executes
quarantines the whole candidate client. It does not rerun the original root once
per owner or claim to roll back published closures.

## Unit catalog transport

`owner.units.native_programs` validates portable authoring manifests using
`sdgo.unit-native-programs/1` ([schema](unit-native-programs.schema.json)) and
compiles the optional `SDGOPR1` sidecar.
`sdgo/unit_program_format.h` independently consumes that binary format. It binds
the exact emitted catalog SHA-256, every program's bytes/exports and a complete
imported weapon's SHA-256 plus its UTF-16 initializer name at offset 1771.

The unit catalog's optional consumer installs the existing original-loader
adapter only when `native-programs.bin` is present. Native record registration
must finish before the root boundary. `SdgoReadUnitProgramStateV1` exposes a
read-only process-lifetime snapshot of the VM, thread, bundle hash and verified
exports; `sdgo/unit_program_state.h` declares its versioned structure.
This registration does not grant server, allocation, media or actor admission.
Manager can preserve/export declarations in a project and show its script-group
count. Its ordinary publisher remains blocked for projects that require the
unfinished actor admission path.

## Verification

The root CMake project builds this library. Set `SDGO_LUA50_SOURCE` to an existing
official Lua 5.0.3 source tree to enable `script_registration_real_lua50`. The
standalone test project at `tests/script_registration` uses the same production
sources. It exercises actual dumped closures in the official x86 VM, the native
calling convention and root gate, name conflicts, multiple owners, exact
idempotency, VM stack preservation and a partial execution failure that requires
client closure. Its default fixtures contain no private game content.

Those checks cover the consumer contract and VM/ABI fixtures. A target symbol
profile, integration into the original loader and actual original-client use
are separate steps; the offline fixture does not activate anything in the native game.

An optional `SDGO_SCRIPT_NATIVE_CLIENT` enables bounded original-API replay. The
fixture maps only the authenticated original executable bytes into a private
owned child and invokes six original Lua stack/state/type/lookup/identity APIs
against the official VM fixture. It reserves the fixed source image range before
the child's CRT heap exists. No executable entry, TLS callback, import resolver,
game window or game process runs. The original file/root loader and buffer
execution remain separate from that six-API replay; it must not be reported as
an original-client gameplay or native registration pass.
