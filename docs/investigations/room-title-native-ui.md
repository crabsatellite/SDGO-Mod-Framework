# Room-title native UI evidence

Pinned client:

- `GOnline.exe`
- SHA-256 `e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92`
- Image base `0x00400000`, no relocations.

The address claims below are bounded observations from that exact executable.
`mode10-waitroom-disassembly.txt`, `static-wordskip-mode10-disassembly.txt`,
and `room-details-disassembly.txt` contain the corresponding native bytes.

## Mode 10 and the shared wait-room title bar

- `0x009F5590` creates the single-mission request. At `0x009F5796` it writes
  `0A 02` to packet `+6`, so the request is category 2, mode 10.
- `0x009F5BC0` accepts a room record whose packet mode at `+0x10` is 10.
  The copy at `0x009F5C78` writes the native room id to mission record `+0`;
  `0x009F5C84` copies category to `+4`; `0x009F5C8B` copies mode 10 to `+5`.
- PvP and mission rooms use the same title-bar object and vtable
  `0x00EBCE9C`. The adjacent vtable `0x00EBCF14` is a map-selection component
  and is deliberately not used.
- The shared title bar stores a PvP room pointer at `self+0x108` and a mission
  room pointer at `self+0x10C`. Native code consistently checks `+0x108`
  first, then `+0x10C` (`0x00A45910`, `0x00A459E0`, and `0x00A53CBE`).
- Mission population `0x00A45A70` receives the mission record at event
  `+0x2C`, stores it at `self+0x10C`, and renders it through `0x00A324C0`.
  The mode-10 fixture therefore uses the normal mission waiting-room title
  path.

`native_ui::ReadTitleBarRoom` mirrors the native pointer precedence, validates
the shared vtable and readable room id, and returns the matching legacy title
from PvP `room+0x7A` or mission `room+0x54`. This gives the controller an
evidence-backed domain and native id instead of inferring a battle domain from
the event type.

## Rename entry and 0x089E domain

- The common `TitleChangeBtn` click path is `0x00A53B0D` through
  `0x00A53D46`. It requires waiting-room state `self+0x100 == 2`, rejects only
  mission category 4, and handles control `self+0x7C` with event `0x101`.
  It selects PvP `self+0x108+0x7A` or mission `self+0x10C+0x54`, then opens
  the same title-edit service with constructor `0x00A0DD70`. There is no
  host/leader test in this bounded click path.
- Shared edit init `0x00A1E160` loads `uirs\0882.uir` / `TitleDlg`, finds the
  type-8 `TitleEdit`, stores it at service `+0x50`, and initializes its text.
  The Mod's shared `PvpTitleEdit` init hook widens that edit and restores the
  current metadata title for either domain.
- Confirm `0x00A46A20` reads the edit at service `+0x50`, constructs event
  `0x0BDB` through `0x00A3B540`, and emits it to the common title-bar handler
  `0x00A53670`.
- The native packet constructor `0x0072DE80` builds opcode `0x089E`, puts the
  subtype byte at packet `+6`, and the title at `+8`.
- Battle request `0x00750A50` calls that constructor with subtype 1 at
  `0x00750B73`; mission request `0x00750BC0` calls it with subtype 2 at
  `0x00750CE3`.

The in-room full-title view does not depend on the button fitting 128
characters. `TitleChangeBtn` is the entry; opening it creates the shared edit,
and the init hook puts the full metadata title into that edit. Native anchors
cover both PvP and mission/mode 10.

`0x00A46380` compares an incoming title with the owned cache at
`self+0x110`. For a changed title it updates that cache before dispatching the
service request. A rejected retry must therefore restore the confirmed full
title (or the returned legacy title when no extension record exists) to
`self+0x110` before re-entering the native handler.

## Full title in room lists

- PvP lobby renderer `0x008D9CB0` and mission row renderer `0x009EC2A0` are
  hooked after their native renderers. Projection is keyed by the native room
  id rather than row order.
- Selected-room details are hooked after the native renderers at
  `0x008CDC00` (PvP) and `0x008CD760` (mission). They use the verified native
  vector bounds and find the type-3 `RoomNameStatic`.
- Host `CUIStaticText::SetWordSkip` is `0x00C6B3B0`. Its mouse/update/render
  path uses flags at `+0x288..+0x28B` and the measured full size at
  `+0x28C/+0x290` to expand overflowing text while hovered. The Mod enables it
  only on exact static-text vtables. It does not write the same offsets on
  buttons.

This gives mission rows and both selected-room detail panels a native
hover-expanded full title. Button text projection remains useful for the
ordinary title bar and PvP rows, but no claim is made that a fixed-width button
can display every 128-character title without opening the shared edit or the
static detail view.

## Tests

```powershell
cmake --build build --config Release --target room_titles_native_ui_test room_titles
.\build\Release\room_titles_native_ui_test.exe
```

The fixture covers both title domains, a mission record with mode byte 10, shared title-bar identity
and legacy-title recovery, full-title projection, selected-room vector bounds, exact static vtables,
one-time WordSkip activation and rejection of an out-of-range details index.

The native byte signatures used by the shipping Mod are in `symbols/GOnline-54BE1568-00C97000.ini`;
`tools/verify_symbols.py` validates them without starting the client.

## Already-focused create-room input (client 0.2.1, 2026-11-02)

The native edit has two relevant limits. `CUIEditBox +0x2A0` stores UTF-16
bytes, while the shared input at pointer `0x010887E0` caches UTF-16 code units
at `+0xFC` and applies them to its Win32 EDIT at `+0x134`. Focus setup
`0x00C61840` reads the edit's byte limit at `0x00C6186C`, divides by two and
calls `0x00C47F90` under the critical section at `0x00FF3368`.

The old post-init hook only changed `+0x2A0`. An edit already focused during
initialization could therefore retain the old 20-character live input limit
until another focus transition. Version 0.2.1 also calls the native input-limit
setter under that same lock when the verified room edit's focus byte `+0x165`
is set. The setter's `clear` argument is zero: only the limit is refreshed,
preserving text and selection. An unfocused room edit never changes another
control's active input limit.

The fixture covers keyboard character 21, 128-character mixed input, preserved
text/selection, all four dialog init hooks and repeated opens, unfocused-input
isolation and a missing shared input. With the optional supported client path,
it replays the pinned 92-byte setter at `0x00C47F90` on a hidden fixture EDIT.
Its reached SendMessageW import is redirected to User32; the clear branch and
its game-string calls are never executed.

```powershell
cmake -S . -B build -DSDGO_ROOM_TITLE_NATIVE_CLIENT="<supported client>\GOnline.exe"
cmake --build build --config Release --target room_titles_native_ui_test room_titles
ctest --test-dir build -C Release -R "^room_titles_(native_input|native_ui|client|transport|client_state)_offline$" --output-on-failure
```

## Real creation services and deferred controls (client 0.2.3)

The three RTTI-backed creation vtables are `CServiceMakeRoom` at `0x00EA6414`,
`CServiceMakeRoomAI` at `0x00EA6484`, and `CServiceMakeRoomMatchmaking` at
`0x00EA64F4`. The first two use root `+0x50` and cached edit `+0x54`.
Their early init at `0x008D0380` only loads a hidden root. Updates
`0x008E5AA0` / `0x008E5BB0` change lifecycle `+0x44` from 3 to 4, populate
controls through `0x008CF890` / `0x008D0B30`, then set default text and focus.
The limit and saved draft must therefore be applied after that update returns,
only for the initialization transition, never over later user typing.

The team form initializes its visible controls at `0x008E4700`, root `+0x50`.
The old `0x00EA3CD4` / `0x008E0C60` pair belongs to the separate rule-selection
service (`uirs\1306.uir`), not the name edit. The native room list opens the
ordinary/AI forms through `MakeRoomBtn` and the team form through `MakeTeamBtn`.

Ordinary/AI submit uses `0x0525` (80 bytes, length/title at `+0x12/+0x13`);
its `0x0918` ACK is `0x3CE1` bytes with Battle room ID at `+6`. Team and mission
creation continue to use the existing `0x0640` carrier. Native short fields stay
bounded; full metadata is committed only on the corresponding native ACK.

The retained `--room-titles-only` GameTest drives original dialog notifications,
committed characters in the real Win32 EDIT, cancellation/reopen, and exact
server-confirmed full titles. It does not synthesize rooms, title replies or
inventory. Offline fixtures also cover missing controls before the first update,
the state-3 initialization, focused input refresh and no draft rewrite on later
updates. Runtime receipts and private accounts remain outside Git.

## Independent width truncation

The focused edit update at `0x00C629F0` calls `0x00C627E0` after synchronizing
the shared Win32 input. This `void __thiscall(Edit*, HostWString24*)` routine
measures the text against `edit[+0x25C] - edit[+0x254]` and replaces an overflowing
value with a fitting prefix. Raising `+0x2A0` alone does not disable it.

The Mod bypasses this width policy only for an initialized room-name edit whose
registered service, live service vtable, expected edit identity and 512-byte
limit still match. Other edits run the original function, including unrelated
edits with the same limit. It leaves `+0x29F` unchanged because that flag also
changes Enter and multiline handling. The native single-line geometry has no
horizontal scroll offset; retaining the full value does not enlarge the field.
The offline regression covers an overflowing room name, an unrelated edit and
service-storage reuse. Native GameTest checks complete text after later updates
and requires the exact full title in the server confirmation.

## Title confirmation and native command queues

Native loading can defer response dispatch on the client main thread beyond
five seconds. Capability and title requests retain their nonce for a bounded
30-second window. A delayed matching response can complete the request; an
unrelated or superseded nonce still cannot authorize a title or battle start.

The inline title event `0x0BDB` reaches `0x00A53670`, then `0x00A46380`.
The latter clones a title command into the native queue at `0x00994DE0`.
Returning from the UI callback does not prove that the transport already sent
it. A changed title keeps its draft until the original send observes it and the
server confirms it. An unchanged native cache remains a no-op. Room identity is
validated from the native room id and service layout independently of whether
the optional short display projection can currently be converted.

## Native channel changes and provider identity

Agent session `+0x444` holds the current Battle connection. Both `0x07FB` and
`0x0640` resolve it at dispatch time; a successful Battle `0x09DF` can replace
that handle between the lobby Hello and room creation. Capability cannot be
assumed to belong to the same Battle merely because the client-to-Agent
connection remains unchanged.

A validated fresh `SetTitle` create, whose epoch, room, generation and revision
are all zero, is an explicit protocol offer from the Agent-authenticated pilot
to the newly selected provider. It establishes capability only when this
provider has no existing capability for that pilot. An existing Agent route
cannot be implicitly rebound, because restarting its capability round would
erase completed request deduplication. Existing-room updates keep their normal
negotiation, owner, generation and revision checks.

The client can adopt a different provider epoch only for an awaiting fresh
create with the exact request nonce, complete title and initial revision. Its
snapshot must also match the native carrier, room ID and Team/Battle domain:
`0x0640` uses `0x0A33`, while `0x0525` uses `0x0918`. Expired creates, unrelated
snapshots and existing-room edits cannot authorize that change. Old-provider
room caches are removed when the matching fresh creation is confirmed.
Each client process seeds its monotonic request range from the Windows system
RNG. A reused pilot and Agent route must not restart at request `1`: another
Battle provider can still remember an earlier login's same-title create, and
replaying that old native room reply would leave the new waiting room without
its current model. In-process retries keep their exact nonce for deduplication.

Native model category `4` has no reachable inline rename input: population
`0x00A45A70` initially disables `TitleChangeBtn`, and event handler `0x00A53830`
explicitly rejects that category even if a later host refresh enables the
button. This model byte is copied from the `0x0A33` server payload; it is not
the factory object type that selects the Team manager. Native tests confirm
the original Team manager/Main graph and exercise its inline title input when
the native model category permits it. Compiled legacy settings adapters remain
separate ABI/protocol regressions.

Modern Mission and Team Main states dispatch `0x0BE1` to the shared original
leave business at `0x00741950`. Their validated manager/state/context graph is
sufficient; they need not publish the legacy profile-room pointer. Mission
uses manager/Main vtables `0x00E89D28` / `0x00E86824`, while Team uses
`0x00E89CC8` / `0x00E867C4`.

The modern close path sends `0x05C4` with its native room ID at `+6` and accepts
`0x09B7` for that same ID. The title cache records the actual outgoing close
intent and clears only the matching connection, epoch, room and generation.
An unrelated room, a reused generation or a rejected `0x04D2` close cannot clear
the current confirmed title. These fields identify a room, not a pilot.
