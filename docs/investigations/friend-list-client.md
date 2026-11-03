# Native client friend-list investigation

`friend_list_client_fix` supports only this verified `GOnline.exe`:

- TimeDateStamp `0x54BE1568`
- SizeOfImage `0x00C97000`
- SHA-256 `e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92`

## Client expectations

The `0x04B3` friend-list response starts its 56-byte entries at packet
`+0x0B`. Each entry contains:

| Entry offset | Meaning |
|---:|---|
| `+0x00` | stable account/pilot identity |
| `+0x0A` | 17-byte encoded nickname |
| `+0x1B` | relationship state; `7` is accepted friend |
| `+0x1C` | presence; `0` offline, `1` available, `3` in room |

The initial response handler caches every accepted-friend row, including
presence `0`. Later updates match the stable identity and remove a row only
when its relationship is no longer accepted. Presence notifications update
the same row without clearing its identity or nickname.

The client therefore expects the server to return an offline friend with the
real identity and nickname and numeric zero presence. A complete offline row
is displayable without a client list filter.

## Conversation opener defect

The native selected-conversation path first rejects a missing tree selection,
then compares the selected row's presence at `0x00964545`. Its conditional
branch at `0x00964549` skips the existing conversation UI when presence is
zero.

The Mod hooks only that verified branch. It changes the branch decision only
for a row with all four properties:

- nonzero identity,
- nonempty nickname,
- relationship state `7`,
- presence `0`.

No-selection, category, pending, unnamed, online, and in-room paths retain
their native behavior. The Mod then uses the shipped conversation window and
send controls.

The test loads the production Mod, installs its real naked detour through
MinHook on an executable copy of the native branch sequence, and checks the
open/blocked outcomes. It also verifies stack balance, general registers, and
that the detour preserves the comparison flags except for clearing ZF in the
one accepted-offline case. Optional artifact testing pins both exact byte
signatures and rejects a one-byte mismatch.

## Messenger layout defect

The supported executable assigns the path `uirs\1364.uir` before creating both
the messenger service's auxiliary panel and `MessengerPopup`'s chat panel. In
the supported complete client archive, that resource is a 5,251-byte
`Exchange_Pop_up` layout. It has the parts-exchange controls and none of the
messenger `Edit`, `Slider`, or `Static0` through `Static19` controls. The popup
therefore gets a null result for its native `Edit` lookup and the unmodified
initializer dereferences it at `0x00961FF4`.

The same archive contains `uirs\0553.uir`, a 10,646-byte friend-chat layout
with all of those required controls. It is the only layout in the archive with
the complete messenger control set and has no native path reference in this
executable.

The parts-exchange initializer at `0x00565DA0` legitimately uses
`uirs\1364.uir`, so a global path replacement would break another native UI.
The client fix hooks the two verified native CALL instructions at `0x00959911`
and `0x00961F1E`. Each detour substitutes the stable DLL-owned path
`uirs\0553.uir` only when both argument guards hold:

- the source pointer is the verified original `uirs\1364.uir` literal;
- the native length is exactly 13 wide characters.

The detours restore registers and flags, then replay the original CALL through
separate trampolines. The shared `std::wstring::assign` entry remains available
to localization Mods. Every mismatch and every other caller, including the
exchange return at `0x00565DF5`, remains unchanged. The core owns and queues
the site hooks and rolls them back if activation fails. No original game asset
is copied into this repository; the
complete-client publisher supplies the existing original `0553` resource.
Neither the executable nor either `GPackData` archive is modified.

The production-DLL test installs the actual CALL-site detours through MinHook,
executes direct-call fixtures for both accepted sites and a third
exchange caller, and checks source-pointer and length mismatch rejection,
return value, stack balance and callee-saved registers. It also installs an
independent global assign hook before and after this Mod to verify coexistence.
Optional artifact testing pins the assign ABI, both CALL/return sites, the
shared path bytes and the distinct exchange call.

## Messaging boundary

This component lets the player open, view, and compose in the native
conversation for an offline friend. The current server explicitly rejects an
offline whisper with result `0x1407`; it does not queue that message for later
delivery. The client Mod does not suppress or reinterpret that server result.
