# Native GameTest

`prepare --same-room-rematch` checks that the human retains room ownership after
settlement, starts a second full battle in that same room, and leaves only after
the rematch. It cannot combine with random loadouts, pages or account preparation.
The passive server report requires two complete battle lifecycles per mode.

For asymmetric population regression, `prepare` accepts
`--expected-ai 7 --ai-allies 0 --random-teams`: request native human team
randomization and verify seven opponents with no allied AI relative to the host.
Without `--ai-allies`, the existing balanced test layout remains the default.
These are full PvP scenario options; page/mission smoke plans reject them.
Combine with `--random-loadout` to exercise owned units and team placement in the
same sealed native scenario.

## FPS cap without account login

Build `game_test_client`, `fps_limit`, `sdgo_core`, `d3d9_shim` and `sdgo_data`,
then run the FPS matrix using a normal installed client as a read-only template:

```powershell
python tools/game_test_fps.py --client-template D:\Games\SDGO --stage build/stage/Release --work-dir build/fps-runs --results-dir build/fps-results
```

The matrix uses the same private-desktop GameTest launcher, genuine D3D9 device,
muted Miles audio and loopback-only policy. It stops at the native login page,
does not load an account profile, and clears saved login credentials in the
copy. It compares disabled/configured-160 with enabled 30, 60, 120 and 160.
The native login page still needs a reachable compatible backend. Select the
existing local test endpoint with `--target-host` and `--login-port`; this does
not create an account or submit credentials.
The default whole-client timeout is 600 seconds to allow native resource loading;
`--timeout` can override it. The cadence assertions and sampling windows are
unchanged by that startup allowance.
Background processes use normal Windows priority, matching ordinary game
launches. Below-normal priority can starve native loading and timing samples
when the host CPU is saturated; background desktop/audio isolation remains in use.

Each run reads the real application's FPS field and initialized timer interval,
then independently measures native update and successful D3D9 Present cadence
against QueryPerformanceCounter in three three-second windows. All windows and
averages are retained. Logic updates must remain within 80–110% of native 60 Hz,
independently of the configured rendering cap; successful
Present calls must be positive and average no more than 110% of the active cap.
Their minimum spacing must match the actual pacing period within scheduling
jitter (the larger of 0.5 ms and 10%). The production observer independently
confirms both targets and the period; enabled 30 must actually wait. The disabled
control has no rendering ceiling assertion. Rendering below the ceiling is
reported as measured, without a throughput guarantee. The native
GameTest assertions produce JSON and JUnit results. This measures the native
login-page renderer; combat timing and visual quality are separate tests.

The wrapper exports reports, native events and quiet-runtime proofs to the
results directory, then removes each owned client copy after verifying that
its process has exited. The template and other tests are preserved. Use fresh
work/results directories for a later machine or environment. A single case can
also use `game_test.py prepare --fps-cap 160` followed by its ordinary `run`
command; `--fps-mod-disabled` selects the native forced-60 control.

For a full native PvP regression, add `prepare --fps-gameplay-cap 160` to the
normal existing-account battle preparation. It seals the configured render cap
and the observer setting before startup. After real AI movement is observed,
the case checks 60 Hz gameplay and requires rendering above 60 FPS when selecting
a higher cap, then continues the ordinary battle, settlement and return flow.

### Reuse a local native backend

`tools/game_test_backend.py serve` runs an already configured local test server
with its independent native SQL instance. It does not use Docker or recopy and
restore databases on every retry. The backend's `backend.json`, `private.json`
and actual SQL files must identify the same test copy; startup checks the live
SQL process and data-file paths. Operators configure their own environment and existing test accounts;
see the [GameTest API](../include/sdgo/game_test.h) for the component contract.

Build `winmm_shim`, `sdgo_core`, `sdgo_data`, `pvp_ai` and `game_test_server` for
the server stage. This example assumes the test copy already uses `127.93.0.1`
and its private database configuration is ready:

```powershell
python tools/game_test.py prepare --client-template D:\SDGO-Test\client --stage build/stage/Release --run-dir build/fps-probe --target-host 127.93.0.1 --login-port 5001 --fps-cap 160
python tools/game_test_backend.py serve --work D:\SDGO-Test\server --client-run build/fps-probe --stage build/stage/Release --startup-xml D:\SDGO-Test\server\ServerStartUp\ServerStartUp.xml --lifetime 1800
```

Launch selection reads both the native startup XML and channel table. Wait for
each Battle's new `Table Download OK.. Time Procedure Ready..` marker and
`backend_ready` before starting clients; an open TCP port is insufficient.
The FPS suite needs no account. Other suites bind an existing private account
profile, and authenticated observers also require
`serve --host-pilot-id <actual native pilot ID>`. No account ID is embedded.
Client-only scenarios can reuse a ready server session; restart owned services
when their candidate DLLs change.

```powershell
python tools/game_test_backend.py stop --work D:\SDGO-Test\server
```

Verify owned service exit after stopping. Preserve the shared test copy, native
SQL, existing accounts and common assets. After acceptance and required deployment
readback, move successful temporary records and owned concurrent copies to the
Recycle Bin; retain failure diagnostics until resolved. Test runtimes, databases
and private configuration stay local and are not runtime publication sources.

An optional development Mod, disabled by default. Test cases run inside the
real client and use its normal business requests against a real server.
The external runner prepares a separate client, starts it and collects results.
It does not operate the desktop.

A scenario sends a request, waits for the real reply and checks the resulting
room, battle and AI state. Login, server data, room creation and battle loading
use the normal game flow. Tests must not fabricate profiles, rooms or replies.

Test accounts and registration retry policy live in private local configuration.
Missing steps, failed assertions, unhandled native errors and timeouts fail the run.
Results are written as JSON and JUnit XML. Installing this Mod alone does not
start tests or change ordinary play.

Tests default to a separate background desktop with BGM, effects and voice
muted in the copied client. Desktop/window ownership is checked separately and
missing evidence fails the run. A dedicated graphics thread creates the real
D3D9 device through one invisible, nonactivating proxy; actual game pages stay
on the private desktop. Fullscreen is rejected.
If Windows denies access to the locked input desktop, the launcher instead
verifies the private game windows and the single, hidden graphics proxy on
the named Default desktop. Other observation errors or unrecognized windows
still fail the run; inaccessible input-desktop state remains explicitly unknown.
The native Miles WaveOut driver's zero master gain is read back independently. The copied logic-test
profile disables native postprocessing, sets all five native effect levels to
zero and checks the effective runtime flags. These checks do not claim visual or
postprocessing acceptance. Local direct tests bind only loopback and do
not change firewall profiles or notifications. Nonlocal quiet runs require
permission to prepare their scoped application rule; setup failure never opens
a UAC prompt or falls back to an interactive client.

If a protected input desktop denies read access, FPS and local direct PvP tests
can verify the fixed `WinSta0\Default` desktop instead. It must contain only
the same process's exact hidden, nonactivating graphics proxy; game windows
remain on the private desktop, with the foreground, audio and socket checks
still required. The actual input-desktop name and state remain unknown in the
receipt. Unidentified or visible windows and other observation errors fail.

Runs reuse `%LOCALAPPDATA%\SDGO-GameTest\account.ini`, or the file selected with
`--account-profile`. Preparing another run never generates a new account.
Only explicit `--create-test-account` creates the first persistent profile;
existing profiles are never overwritten.

Login inventory paging finishes in its original scene before later cases enter
the hangar. This also applies to existing accounts with several inventory batches;
the framework waits for the native initialized flag and a complete owned-unit scan.

New-account setup covers nickname creation, leaving the beginner trial, the
first unit draw and required in-game confirmations before battle scenarios.

Developers can register tick-driven cases through
[`game_test.h`](../include/sdgo/game_test.h). Callbacks run on the native main
thread and can wait across ticks for actual service events.

Build first, then use an existing normal client with PvP AI installed as the
template. The target server needs `pvp_ai`. The runner starts a separate client
and does not restart existing servers.

```powershell
python tools/game_test.py prepare --client-template D:\Games\SDGO --stage build/stage/Release --run-dir build/gametest/pvp --mode-sequence 1,3,9
python tools/game_test.py run --run-dir build/gametest/pvp --timeout 3600
```

Add `--create-test-account` to `prepare` only when creating the first account
profile. Later runs reuse that profile. After the first preparation, add
`--reuse-client` for the same `run-dir` to validate and reuse its stopped client
with a new run UUID. The previous receipt and binary hashes are consolidated;
retired jobs go to the Recycle Bin instead of keeping another full client.
An archive baseline referenced by another capsule cannot be reused this way.
To switch a stopped client to another existing test identity, combine
`--reuse-client --rebind-existing-account --account-profile <existing-profile>`.
The target must declare `registered` or `existing_account`, without registration
retries. Both profiles are locked and preserved while the same client files are
reused. Bind the server observer to the target account's actual pilot ID too.
For a single host in an eight-player room, exercise automatic commands with
`--expected-ai 7 --ai-allies 3 --ai-policy fill`, or
`--expected-ai 7 --ai-allies 0 --ai-policy enemy`. These test `!ai fill` and
`!ai enemy` directly. Add `--ai-teams coop` to exercise human grouping; omit it
or use `versus` to retain native manual/random teams. These options can combine
with `--random-teams` or `--random-loadout` when the sealed native title fits
20 bytes; split longer combinations into separate scenarios.
Ordinary Grid/Tag runs validate the already-selected fleet without editing it:
one to four distinct owned units, an included leader and at most ten native
points. The explicit `--deck-only` editor scenario still uses its four-unit
fixture and genuine Save/GoOut replies.
Install `Send2Trash` in the developer environment; reuse has no permanent-delete
fallback. The same account cannot run concurrently. The default endpoint is
`127.0.0.1:5001`; change it with `--target-host` and `--login-port`. The original
client selects its server and channel through the normal native flow.
The default `--network-policy direct` disables inherited routing only in the
test copy and verifies that process's actual TCP peer before login. Use
`preserve` explicitly when testing an owner's FRP routes.

The example tests normal, death match and leader match in order.
Each mode covers room creation, real battle startup, AI state assertions and
normal completion and return. Select a single mode when diagnosing a failure.

For focused ordinary PvP checks, `--ai-difficulty Hard --expected-ai 7` seals the
native difficulty command. Add `--ai-layout allied --expected-ai 6` instead to
test five allied AI and one enemy AI. This layout needs a fresh observer session
because its command does not fit the native title limit with a reused session prefix.
`--ai-pool-level 1`–`5` selects an exact pool with `--expected-ai 1`; it cannot be
combined with the difficulty override. In the test Mod, `BattleAiBehavior=1`
requires actual special-victim recovery, moving fire and distant pursuit.
`BattleAiDeaths=1` records native death identities; adding `BattleAiTeleport=1`
requires a real local AI teleport with changed position, preserved HP and no
destruction request. These opt-in witnesses observe native gameplay.
Grid (`6`) and Tag (`7`) use the native VS channel business flow and require a
complete battleship deck on the account. Missing decks and server rejections
fail the case.
Results are under `receipts/report.json` and `receipts/junit.xml` in the run directory.

Account preparation can use `--bootstrap-only`, `--battleship-only`,
`--gift-requests-only` or `--deck-only`; these do not claim PvP acceptance.

`--friends-only` opens a coordinated native messenger session after normal
login and onboarding. `tools/game_test_friends.py` sends scoped commands to
that run's GameTest Mod; the native game thread executes the original friend
actions and records the resulting cache, actual transport and consumed replies.
Commands require the run UUID and a consecutive sequence. The private account
profile, quiet desktop and loopback network protections still apply. This scope
cannot be combined with the autonomous battle suite, shop or other account-preparation scopes.
The multiplayer coordinator below uses this same bounded command session.

The `campaign` command requires a stopped, passed `--asset-baseline` and the
verified unique loopback `--target-host` of an independently prepared backend.
Use existing test accounts in an isolated database copy. Put their persistent
profiles under `<work-root>/profiles`; missing profiles fail preparation rather
than registering another account. The coordinator covers requests,
accept/reject, conversation open/close, bidirectional and Chinese whispers,
offline presence and native send rejection, relogin, duplicate/self/unknown
validation, deletion, re-addition and multiple list pages. Its receipt records
each check independently. Profile acceptance requires the actual managed
popup clone, its original initialization, and a consumed `0x084B` reply with
the selected target ID and copied data; enqueue alone is insufficient.
This campaign currently does not prove room invitations. A failed or
interrupted bootstrap is never an accepted archive baseline. Developer test
files, accounts, addresses and database copies are not gameplay dependencies
of the distributed friend repair Mods.

```powershell
python tools/game_test_friends.py campaign --client-template D:\Games\SDGO --stage build/stage/Release --work-root build/friends/campaign --asset-baseline build/friends/passed-bootstrap --target-host <verified-loopback-address>
```

Ship acquisition uses the native tutorial. Gifts use an existing sender's normal
mail business and require the server's ordinary gift service plus independent
processing evidence. Deck setup reads four genuinely owned unit UIDs from
`[deck] unit_uid0` through `unit_uid3` in the private account profile and waits
for the original Save and GoOut replies. Log in again with that same account for
battle tests; no units or gameplay database records are supplemented.

Use `--pages-only` to test the authenticated unit-management page, inventory
readback, both exchange previews and cancellation. It compares unit UIDs, slots
and the active unit before and after; it never confirms an exchange with candidates.
Page cases do not count as battle acceptance.
It also opens native storage, waits for its real message loop and server list
reply, and confirms one claim batch. An empty batch does not prove delivery.
Capsule cases read all five native catalog tables and enter the actual page to
check machine 9301's UI/confirmation currency source and the collection price;
they do not purchase anything.
The suite requires the hangar, storage, operator/AI catalog, capsule catalog and
currency-fix Mods plus the server's 1200-slot capacity Mod. It reads every configured operator and NPC from the
native grid and compares the configured names, category references, NPC bytes
and source unit hashes. These catalog checks do not claim shop interaction or
mission completion.

For stock shop checks, pass both `--shop-assertions` and `--shop-display` with
`--pages-only`. These developer inputs describe existing stock and are copied
into the isolated client with fixed paths, SHA-256 and revision binding. The
case checks native names and permanent/rental options, opens and cancels buy
and gift previews, and compares the complete inventory and wallet. It does
not install products, purchase or send gifts.
The adapter opens the native search view, clicks its original `FilterBtn`, and
waits for native query completion. Selecting `AllSerchRadio` alone opens an
empty view and is not evidence that a stock query succeeded.

For a bounded visual shop regression, use `Scope=StockVisualAssertions` in the same sealed assertion
file and add each product's `DisplayName`. Optional `Query` fields select existing stock through the
native search edit. This scope registers login, native shop readiness and shop cases only; it does not
claim onboarding or run storage claims. It compares every inventory brief and wallet, checks genuine
buy/gift controls and their formatted prices, and captures the real D3D9 backbuffer before cancellation.
For tooltip text, seal all three optional fields `TooltipUnit` (0/1), `TooltipDescriptionHex` and
`TooltipUsageHex` (bounded UTF-16LE hex). Actual tooltip service/control identities and text must match;
only literal `\n` versus LF and CR line-ending representation are normalized. Native game records,
server replies and rendered pixels are never synthesized.
The tooltip scenario supplies virtual mouse input only inside the isolated test process and lets the
original input polling and hit testing open the native tooltip. It does not move the system cursor or
claim hardware-mouse acceptance; captured pixels still come from the genuine game renderer.

`--play-unit-uid` selects a genuinely owned unit through the native hangar
before page, PvP or mission cases. The adapter waits for the full native unit
record, invokes the original equip business, and requires the genuine reply
plus matching active UID. `--random-loadout` exercises the native `!rand` PvP
room command.

`--mission-mode 5` or `8` registers bounded mission smoke cases: native channel
and mission admission, room creation, start, three level-2 wingmen, movement,
target acquisition and native combat observations. These cases do not claim
mission clear, settlement or active exit. Unavailable native missions or
channels fail without fabricated admission data.

For `--gift-requests-only`, the private account profile may specify existing
owner mail commands in `[gift] body0` through `body3`, or use `page0/position0`
for unit gifts. The recipient must be an actual native pilot nickname. Native
authentication and the server authorize/process the mail; GameTest still waits
for an independent processing receipt and never grants currency or inventory
directly. Keep credentials and command configuration local.

### Coordinated multiplayer

Prepare one quiet `--friends-only` run for each distinct existing account, using
the same isolated native backend, a single `--mode-sequence`, and the same
`--expected-ai` setting. Each client needs its own mutable files; a stopped,
passed archive baseline can supply the shared read-only game archives. Install
`match_ai` on every client and `pvp_ai` plus the passive observer on the backend.
Bind the host run to that observer session as described above. Existing equipped
units and any mode-required battleship decks must already be usable.

Declare every human pilot before starting. Humans and AI share the eight native
room slots; the coordinator requires at least two humans and one AI. For example,
three existing pilots with `--expected-ai 5 --ai-allies 0 --ai-teams coop
--random-teams` exercise three humans against five AI while retaining the room's
random-team option. The `coop` rule groups the humans when the server starts.
Replace the example IDs and paths with the prepared test identities and actual
enabled Battle observer streams:

```powershell
python tools/game_test_multiplayer.py --host-run build/cohort/host --peer-run build/cohort/peer1 --peer-run build/cohort/peer2 --human-pilots 101,102,103 --server-events D:\SDGO-Test\server\Server\BattleServer4\sdgo\gametest\server-observer-events.jsonl --output build/cohort-result.json
```

The host creates a real room. Each guest selects the host's actual channel,
resolves the exact advertised title and enters through the original room-entry
business. Ready uses the original 3D Main action and player state machine: its
sender must emit the real request, then the original reply consumer must update
the live room model. The coordinator starts only after every guest completes
these steps. It never sets a Ready flag or inserts room participants.

Battle checks require local AI authority on the host, synchronized remote copies
on guests, real movement, host combat evidence, and original settlement back in
the same human-owned room. Guests then leave before the host through native
business methods, with the observed channel and room IDs supplied explicitly.
The independent server report verifies the declared human roster, AI teams and
complete battle lifecycle. A launch, battle or exit failure fails the overall
campaign and aborts only its owned clients. Accounts, credentials, endpoints and
reports remain local; this command is a developer test tool, not a player launcher.

For a passive server check, run `tools/game_test_server_report.py --run-dir
build/gametest/pvp --events <server-observer-events.jsonl> --host-pilot <pilot-id>`.
Repeat `--events` for each Battle host used by the matrix. The report requires a
matching run, room title, pilot, teams and complete four-event native lifecycle
for every requested mode. It writes `receipts/server-report.json`; a single
server scenario summary does not prove a whole client matrix.

Both components register with Mod Manager as development Mods. The Python
runner is a developer test command for a bounded job; it is not a player launcher
or a persistent service. Keep account profiles and run receipts out of distributed
packages and source control.

For explicitly visible developer footage, add `--visible --sfx-only` to `prepare`, build the
`game_test_audio` target, then use `python tools/game_test_record.py --run-dir
build/gametest/pvp --output D:\Videos\SDGO-GameTest`. The copied client's BGM
is muted and effects remain enabled. Recording is bounded to 25 seconds per
passed battle, using only the owned game window and its process audio. Failed,
silent or black clips are deleted. Retained clips still need visual review for
combat activity and framing before editing; a passed AI assertion alone does
not guarantee a useful shot.

## Release status after acceptance

When the declared basic workflow is implemented and shown usable by focused tests, actual use or user confirmation, publish it as Stable after deployment and main-branch delivery. Exhaustive mode/edge coverage and zero bugs are not prerequisites; later bug fixes use new versions. Remove obsolete
Beta, testing or pending-release labels from the bilingual feature index and guides, and align the
Mod channel (`stable`), Manager labels, version suffixes and package templates. Verify the installed
and published metadata agree. A build alone does not establish a working workflow, but valid focused verification remains valid without exhaustive coverage. Keep Beta for basic workflows still being built or not yet shown usable; unit import is currently in that category.

Evaluate the accepted version and its declared workflow. Later features or optimizations have their
own acceptance scope and do not undo a released version's completed tests. Historical runtime
receipts and user confirmations remain evidence after temporary test output has been cleaned up.
