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

## Messaging boundary

This component lets the player open, view, and compose in the native
conversation for an offline friend. The current server explicitly rejects an
offline whisper with result `0x1407`; it does not queue that message for later
delivery. The client Mod does not suppress or reinterpret that server result.
