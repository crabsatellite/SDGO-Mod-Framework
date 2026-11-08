# Native skill identity selectors

[Back to unit family import](unit-import.md)

`sdgo_skill_identity` is a linkable importer component. It preserves an imported
skill's allocated ID while selecting an audited stock branch in the server's
supplemental enable/disable dispatchers. It carries no content or installation
configuration and is not a separately enabled Mod.

The `unit_catalog_server` companion integrates this SDK with the original server
registry. It is disabled by default and ships without content or an installation
authority. It does not open ordinary Manager publication of new skill dependencies.

## Installed definition authority

The developer request [`sdgo.unit-skill-identities/1`](unit-skill-identities.schema.json)
contains an owner, the installed revision, and target/source/client-hash bindings.
`owner.units.skill_identity_runtime.prepare_from_cursor` reads complete definitions
and ownership in the selected catalog transaction. It requires an unchanged,
same-owner installed destination and the exact complete stock template, checks
the paired client/server fields, and keeps unrelated baseline text unchanged.
Unknown selectors and candidate admission flags are refused.

The trusted controller generates `skill-identities.bin` and a separate host
installation authority. The candidate uses `SDGOSI1`, version 1 and a 64-byte
header; the authority uses `SDGOSA1` and a 128-byte header. It pins the complete
candidate SHA-256, original-ID inventory, native host profile and server snapshot
fingerprint, and repeats the authorized allocation payload. A content package
cannot install its own authority through project resources.

The companion reads the candidate in its Mod directory and the controller-owned
`sdgo/install-state/unit-skill-authority.bin` on the server. It checks the actual
server image, both files and both dispatcher bodies before installation. The
process-lifetime lookup resolves actual borrowed definition pointers through
the original registry. A changed candidate or missing authority declines the
component. The controller must regenerate and read back the paired installation
when its definitions change; the companion does not query or modify player data.

`SdgoReadUnitSkillRuntimeStateV1` is a read-only observation interface for the
installed hashes, server fingerprint and native alias/rejection counters.

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

The original flag is at actor offset `0x1CDE`. Selecting this branch covers only
that flag; downstream gameplay, packets, effects, transformation and cleanup are
handled elsewhere. The importer still connects its resource/server admission, and
the unit should be tried in real battles on a test server.

## Developer checks

`skill_identity_policy` uses neutral allocation and registry fixtures. Set
`SDGO_SKILL_IDENTITY_SERVER` to the matching original server to add controlled
instruction replays at the preferred and a rebased image address. These map
authenticated bytes into an owned child and invoke only the audited dispatcher
functions against scratch storage. They exercise original and allocated IDs,
enable/disable symmetry, pointer rejection, code/table changes and state
preservation. They do not run the original server entry point, create a room,
send a packet or exercise actual actor behavior.
