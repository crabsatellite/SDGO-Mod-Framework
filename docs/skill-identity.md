# Native skill identity selectors

`sdgo_skill_identity` is a linkable importer component. It preserves an imported
skill's allocated ID while selecting an audited stock branch in the server's
supplemental enable/disable dispatchers. It carries no content or installation
configuration and is not a separately enabled Mod.

## Mapping and ownership

`sdgo/skill_identity.h` separates content candidates from the importer's trusted
`Authority`. A candidate binds owner, revision, allocated target ID, stock source
ID and client/server/template SHA-256 values. The authority independently checks
the allocation, complete published definitions and original source identity.
The candidate cannot grant itself ownership or adopt a baseline destination.

The current native profile supports only the stock selector group that sets or
clears the supplemental actor flag. Other semantic groups require their own
native profile. It does not turn arbitrary skill data into a stock skill.

`Bindings::resolve` additionally requires the actual borrowed definition pointer
returned by the native registry for that destination ID. Matching IDs in a copy
or an unrelated actor buffer do not qualify. The source definition must still
exist. A rejected owned context returns failure instead of running a partial
special branch. Ordinary stock skills use their original path.

## Native integration

An importer calls `InstallNative` during `SdgoModLoad`, supplying immutable
bindings and a process-lifetime native lookup adapter. The two exact symbol
sites and continuations are:

| Symbol | RVA | Role |
| --- | --- | --- |
| `skill_identity.enable_selector` | `0x89449` | `MOV ECX,[EAX]; MOV [EBP-0x1C],ECX` |
| `skill_identity.enable_continue` | `0x8944E` | first original selector comparison |
| `skill_identity.disable_selector` | `0x89789` | same original load/store pair |
| `skill_identity.disable_continue` | `0x8978E` | first original selector comparison |

The named sites must be provided by the importer's verified symbol fragment.
The adapter checks the host identity, both complete dispatcher/table regions
and all recorded HIGHLOW relocation sites before queuing either hook. A failed
installation must make the importing Mod decline so the framework rolls back
its queued hooks. Replacing a live mapping is unsupported.

The adapter preserves registers, flags, last error and x87/SSE state. It changes
only the dispatcher-local selector and resumes at the original comparison.
The borrowed definition and its first DWORD remain unchanged; the generic
state-notification path continues reading the allocated ID. This is not a
global ID rewrite or a temporary mutation of shared records.

The original flag is at actor offset `0x1CDE`. Matching this branch does not
establish all downstream gameplay, packet, effect, transformation or cleanup
behavior. The importer must still connect its resource/server admission and
exercise the actual actor.

## Developer checks

`skill_identity_policy` uses neutral allocation and registry fixtures. Set
`SDGO_SKILL_IDENTITY_SERVER` to the matching original server to add controlled
instruction replays at the preferred and a rebased image address. These map
authenticated bytes into an owned child and invoke only the audited dispatcher
functions against scratch storage. They exercise original and allocated IDs,
enable/disable symmetry, pointer rejection, code/table changes and state
preservation. They do not run the original server entry point, create a room,
send a packet or establish actual actor behavior.
