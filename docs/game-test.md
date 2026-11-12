# Native GameTest

[简体中文](game-test.zh-CN.md) · [Back to the feature index](../README.md)

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

Tests use an existing local native test environment: a separate client copy, native servers and a native SQL
database, fully isolated from production, without Docker. Concurrent jobs that would conflict copy the
environment with their own ports and native SQL, and remove the whole job copy afterwards. Ordinary tests bind
existing accounts and units; test runtimes and database copies stay local.

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

### Whole-battle timing and AI query diagnostics

`LogNativePacketIngress=1` enables packet-entry diagnostics for random-loadout
runs. It defaults to `0`; leave it off when login recovery owns that native
entry. Normal battle assertions and performance observations do not require it.

Quiet PvP runs retain successful native D3D9 Present intervals in
`sdgo/gametest/present-timings.bin`. After a complete battle, run
`python tools/game_test_frame_times.py --run-dir <prepared-run>` to validate
PID, sequence, QPC intervals and native battle boundaries. The report includes
p50/p95/p99, stall counts and the no-Present tail at the closing boundary.
Login-page cadence and window-average FPS are separate measurements.

Developer setting `BattleAiPerformance=1` records passive per-unit activity;
`LogDecisions=1` includes AI query costs. `GeometryBoundsAudit=1` executes
original predicates for would-be shape culls and logs the first discrepancy
immediately. Audit runs check correctness only; exclude them from timing
comparisons. Compare identical rosters, AI counts, native caps, backend
topology and comparable host load; review activity so stopped bots cannot
appear as a performance improvement.

The optional `pvp_ai_performance_fixture` and `room_random_performance_fixture`
targets use a fixed initial seed only in an explicit GameTest server session.
Their DLLs are written to `performance-fixtures`, never the ordinary stage;
do not install them in player/server delivery or Release packages.

### Reuse a local native backend

`tools/game_test_backend.py serve` runs an already configured local test server
with its independent native SQL instance. It does not use Docker or recopy and
restore databases on every retry. The backend's `backend.json`, `private.json`
and actual SQL files must identify the same test copy; startup checks the live
SQL process and data-file paths.

The copied SQL launcher also confines database-file operations to
the marked test root before resuming the engine. It checks native NT file
operations, resolved directory links and hard-link counts; registry redirection
alone does not relocate a copied master's model, msdb or tempdb paths. Relocate
every catalog path before normal startup. Ordinary startup redirects only the
two Resource database files to the copy's own engine directory. Initial copy
relocation can additionally use `start --master-only --relocate-from <marked-source-test-root>`.
This maps declared old test DATA paths to the same filenames inside the new
copy's DATA directory; it never opens the old database files. The source must
be a different marked test copy, and the alias option is rejected for normal
startup. Relocate the copied catalog, stop that owned SQL process, then start
normally and verify every physical catalog path belongs to the new copy.
Copy SQL data only while its source engine is stopped; reuse the resulting
instance and existing accounts across retries. An invalid root or rejected
file operation must be diagnosed rather than bypassed. The launcher rejects
registry-only redirect helpers. Windows cryptography providers retain their normal
certificate-cache access. This is a database-file boundary for the SQL process,
not a general filesystem sandbox or a boundary for its child programs: keep the task copy exclusive, do not change its directory links
while it is running, and do not run shell commands through SQL. Database read
checks cover the native `.mdf`, `.ndf` and `.ldf` filenames; startup still verifies
the actual catalog and open-file paths.

Build the x64 helper separately with `cmake -S tools/native_sql -B build/native-sql -G "Visual Studio 17 2022" -A x64`, then build Release and run
`ctest --test-dir build/native-sql -C Release --output-on-failure` to run the real
file-I/O isolation fixture. The Python launcher supplies the root, Resource
directory and task-local temporary directory automatically.

Build `winmm_shim`, `sdgo_core`, `sdgo_data`, `pvp_ai` and `game_test_server` for
the server stage. This example assumes the test copy already uses `127.93.0.1`
and its private database configuration is ready:

```powershell
python tools/game_test.py prepare --client-template D:\SDGO-Test\client --stage build/stage/Release --run-dir build/fps-probe --target-host 127.93.0.1 --login-port 5001 --fps-cap 160
python tools/game_test_backend.py serve --work D:\SDGO-Test\server --client-run build/fps-probe --stage build/stage/Release --startup-xml D:\SDGO-Test\server\ServerStartUp\ServerStartUp.xml --lifetime 1800
```

Launch selection reads both the native startup XML and channel table. Selected
Store/Chat/Agent dependencies start before Battle hosts, with World last;
ordering does not activate any unselected channel. Wait for
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
SQL, existing accounts and common assets. After a passing run, move successful temporary
records and owned concurrent copies to the Recycle Bin; retain failure diagnostics until resolved. Test runtimes, databases
and private configuration stay local and are not runtime publication sources.

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
zero and checks the effective runtime flags; test visual quality and postprocessing
separately in a visible client. Local direct tests bind only loopback and do
not change firewall profiles or notifications. Nonlocal quiet runs require
permission to prepare their scoped application rule; setup failure never opens
a UAC prompt or falls back to an interactive client.

Same-machine direct PvP also resolves the local hostname to loopback inside the test process, so
native peer-address advertisements match its loopback-only UDP sockets. Other names and addresses
use the original resolver. Native adjacent-port retries remain unchanged.

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
For generated default units, pass `--unit-presets <paired-publication>` during preparation or reuse. The normal
data-only installer updates the separate preset scope after programme staging and before runtime hashes are sealed,
while preserving primary owner units. Changing a preset publication requires a new preparation; never replace its
catalog or sidecars inside a sealed or running client.
On Windows, reuse also checks the native executable name when the previous PID
has no readable image path. A PID recycled into a different executable does not
block reuse; an unknown identity or a matching game name still blocks all changes.
To switch a stopped client to another existing test identity, combine
`--reuse-client --rebind-existing-account --account-profile <existing-profile>`.
The target must declare `registered` or `existing_account`, without registration
retries. Both profiles are locked and preserved while the same client files are
reused. Bind the server observer to the target account's actual pilot ID too.

In a plain co-op mission, the opt-in `BattlePlayerCombat` probe also uses the mapped native
movement, firing and lock inputs. It binds to the actual mission generation, room, authenticated
pilot and released local player control. A native result or a return to the waiting room before
the required observation completes fails the case immediately. It never writes gameplay state.

For a gun-fire regression, enable `BattlePlayerCombat` in the copied test observer and set
`BattlePlayerWeaponSlot` to an ordinary ranged slot (`0`–`2`). The probe submits the native
weapon-change request, waits for the actual binding, and requires real ammunition consumption
in addition to mapped input, movement and an attack state. The default `-1` keeps the current
weapon. No weapon data, ammunition, cooldown or FSM state is forced by the probe.
The same opt-in works for each actor in coordinated native SDK sessions. It applies
only after that actor's bound room, pilot, battle sequence and original control
release are observed, and releases input when settlement completes. It remains off
by default.

`BattlePlayerSpecial=1`, together with `BattlePlayerCombat=1` and the default
`BattlePlayerWeaponSlot=-1`, also fires the special attack. Charging the SP gauge in combat
takes about 27,300 damage dealt, more than a short test battle provides, so this mode is a
declared test fixture. The probe waits until a living enemy AI is within the special weapon's
own range (at least 300; after a minute without a target, twice that), then fills the local
SP gauge (`CUnit+0xBC8`) to 30000. This is the only gameplay value it writes. It gives the
mapped special-slot action to the original weapon-selection consumer, which selects that
slot only with a full gauge, and waits until the native attack FSM binds the slot's fatal
weapon. Primary input, plus the lock action for melee specials, then fires it until the OC
client Mod records the local special's damage computation. An attempt that is not bound
within 6 seconds, or not computed within 20 seconds, waits for a target again; after ten
attempts, or at the end of the battle, the special fails. HP, FSM state and weapon data are
only read.

The Battle host checks the gauge again when the special request arrives (`0x04BE`, at
`0x4C3AEA`). The development-only Battle fixture `oc_special_server_probe`
(`tests/oc_special_server_probe.cpp`) fills the authenticated host pilot's server gauge at
that check, just before the native comparison, and records each fill in
`oc-special-server-<run>.ini` next to the client's `run.ini`. AI pilots and other humans
keep the native check; admission, the SP cost and damage stay native. The fixture is never
part of a release. It loads only in a copied test tree marked `TEST-ONLY.txt`, for the run
named in `SDGO_GAME_TEST_RUN`. `game_test_backend.py serve` installs it, with its symbol,
in each Battle host only for an `--oc-special` client run, which requires
`--oc-special-probe <build>/Release/oc_special_server_probe.dll`.

`prepare --oc-special` seals this into an `--oc-allocate` battle whose allocation includes
special points (stat `5`), with an existing `--play-unit-uid`. It sets the probe to
continuous primary fire with the lock action and requires GameTest client 0.1.33. The
battle plays against seven enemy AI and no allied AI (`--expected-ai 7 --ai-allies 0`, the
default for this flag), so enemies come within reach of the only human.
The `oc.special` case waits inside the battle, before `pvp.return`, and keeps the
whole-battle observations running meanwhile. It also reports the Battle fixture's record. In an OC battle (`--oc-room`, or any room on a
server in OC mode) it requires the special to carry the saved special allocation, the gain
to equal `min(carried, 7950 - DamageSize)` and the computed value to follow its site:
`trunc((DamageSize + gain) × r)` in the native x87 arithmetic for melee,
`(115 - 15c) × (DamageSize + gain) / 100` for ranged, and the native additive bonus plus
the gain for map specials. Without `--oc-room`, a server in room mode plays the current
system, and the case is the negative control: the special must stay native. `oc.stats`
runs in the same battle and in every single `--oc-allocate` battle with attack (`2`) or
defense (`3`) points. In an OC battle an allocated stat's carried delta must equal the
saved allocation, and the local battle stat block must hold the unit data value plus that
delta, capped at 169; outside OC no delta may be carried. `--oc-special` cannot be
combined with `--room-switch`, `--random-loadout` or the transform probes.

For collection quota rejection, use the pages-only collection-draw scenario with
`CollectionExpectedStatus=6006` in the copied observer. Declare an already-exhausted
quota in the isolated test database before launch. The same cases then require a
real native purchase rejection and no prize; they do not claim a successful draw
or delivery. Compare the native wallet, quota and prize-box rows before and after
to verify the refund. Keep the default `0` for a separate successful draw/claim.

`prepare --same-room-rematch` checks that the human retains room ownership after
settlement, starts a second full battle in that same room, and leaves only after
the rematch. It can combine with `--random-loadout`; pages and account preparation remain separate.
Each settlement must remove every AI slot while keeping the declared humans and original human host.
The passive server report requires two complete battle lifecycles per mode.

For asymmetric population regression, `prepare` accepts
`--expected-ai 7 --ai-allies 0 --random-teams`: request native human team
randomization and verify seven opponents with no allied AI relative to the host.
Without `--ai-allies`, the existing balanced test layout remains the default.
These are full PvP scenario options; page/mission smoke plans reject them.
Combine with `--random-loadout` to exercise owned units and team placement in the
same sealed native scenario.

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
`BattleAiActivity=1` separately checks every expected bot for target acquisition and real movement or
attack activity, and rejects ten seconds of free neutral idling against a visible opponent. It is off
by default and available through the test Mod's developer settings in Manager. Its passive diagnostics
include per-bot activity totals, weapon/controller state and sampling gaps; they never move a bot or
manufacture combat results.
`BattleAiDeaths=1` records native death identities; adding `BattleAiTeleport=1`
requires a real local AI teleport with changed position, preserved HP and no
destruction request. These opt-in witnesses observe native gameplay.
Grid (`6`) and Tag (`7`) use the native VS channel business flow and require a
complete battleship deck on the account. Missing decks and server rejections
fail the case.
Results are under `receipts/report.json` and `receipts/junit.xml` in the run directory.

Account preparation can use `--bootstrap-only`, `--battleship-only`,
`--gift-requests-only` or `--deck-only`; these are account preparation, not PvP battle tests.

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
each check independently. A profile check requires the actual managed
popup clone, its original initialization, and a consumed `0x084B` reply with
the selected target ID and copied data; enqueue alone is insufficient.
This campaign does not cover room invitations. A failed or interrupted
bootstrap cannot serve as an archive baseline. Developer test
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
Page cases are not battle tests.

For an imported unit, the Python `prepare` / `reuse_prepared` API accepts
`unit_scenario={"schema": 1, "capture_frames": True}` together with
`pages_only=True` and `play_unit_uid=<existing owned UID>`. This bounded scope
uses the original encyclopedia list, detail, alternate/base-form and close
callbacks. It does not repeat storage claims or start a battle. Add
`"portraits": True` to capture the model's actual wait/selection animations.
For acquisition-text acceptance, GameTest client `0.1.34` also accepts
`unit_scenario={"schema": 1, "presentation_target": <unit ID>, "detail_only": True}`
with `pages_only=True` and an existing `play_unit_uid`. This sealed scope follows
the normal login, onboarding, active-unit, encyclopedia-list, detail and close
flow while excluding unrelated inventory and form transitions. Acceptance must
come from that normal entry path; manually loaded or forced diagnostic layouts
can localize a failure but cannot prove the current player-facing UI.
An optional `"acquisition": {"kind": 1, "machine_id": ..., "unit_id": ...}`
checks a fixed capsule candidate; kind `3` checks a custom candidate. These
callbacks open the real model preview without buying or changing a selection.
`"collection_preview": True` instead opens the existing collection confirmation
without purchasing. Capsule previews and portrait captures do not cover draws,
delivery or gameplay.
An optional `"activity_target": {"machine_id": ..., "price": ..., "currency": ...}`
selects another existing native activity confirmation with exact matching. It
requires `collection_preview`; changed targets cannot submit a draw in this scope.
To test one purchase separately, explicitly add `"activity_draw": True` and
`"reward_pairs": [[<item category>, <amount>], ...]` copied from the reviewed host
pool. This scope invokes the original Buy callback at most once and requires its
real result and exact typed acknowledgement. It stops after result closure; an
inbox claim and inventory/wallet readback are separate checks. Rejected or uncertain
purchases are never automatically retried, and the test does not change pool odds.

The runner copies canonical scenario inputs, binds their SHA-256 and validates
the native observer settings against that input before launch. Install an authored
unit publication with the API's `unit_package` argument before sealing; do not edit
an already prepared client or its receipt hashes. Unit content stays in the server
owner's client package or update, outside the public test fixtures.
The API also accepts `capsule_package=<Manager deployment root>` to install a
manifest-verified capsule publication before sealing. Its framework and provider
programs must match the selected stage. The runner uses its trusted installer and
reads back the installed resource hashes; this path never applies the server SQL.
`--pages-only --page-group hangar` seals only the
hangar read, preview and cancel cases, together with login and optional active-unit
selection. This read-only group excludes storage claims, purchases and other page
features; it rejects shop or collection scope and preserves the full suite's assertions.
Adding `--hangar-sort` presses the original sort button and requires the live refresh: the server's
answer arrives, every unit is still present in slots 1..N and the active unit stays active, without a
re-login. `--hangar-confirm-gb` (task-owned or restored test data only: it really dismantles units)
confirms one GB batch and requires that exactly the previewed duplicates leave the refreshed page.
The account needs a duplicate candidate: more than `KeepCopies` (default 2) unlocked, level-1 copies
of a unit type that has a GB rule. After `--hangar-sort`, the GB case starts from the sorted order.
`--pages-only --page-group storage_gift` claims the storage tab with one confirmation and then checks
gift origins on the unit-information page. The claim must settle every reward collected from all pages
(granted, refused or left for manual selection) and leave only refused items in the item tab, so seed a
multi-page tab first. The account profile names two existing units in a `[gift_origin]` section:
`gifted_uid` with an accepted friend-gift record and an ordinary `plain_uid`; the gifted unit must show
its sender and date strip, the ordinary one nothing. Requires GameTest client 0.1.31. `--capture-native-frames`
saves two native frames after each passing page case under the run's `visual` folder for visual review.
It also opens native storage, waits for its real message loop and server list
reply, and confirms one claim batch. An empty batch only covers the empty-storage path.
Capsule cases read all five native catalog tables and enter the actual page to
check machine 9301's UI/confirmation currency source and the collection price;
they do not purchase anything.
The suite requires the hangar, storage, operator/AI catalog, capsule catalog and
currency-fix Mods plus the server's 1200-slot capacity Mod. It reads every configured operator and NPC from the
native grid and compares the configured names, category references, NPC bytes
and source unit hashes. These catalog checks do not cover shop interaction or
mission completion.

For stock shop checks, pass both `--shop-assertions` and `--shop-display` with
`--pages-only`. These developer inputs describe existing stock and are copied
into the isolated client with fixed paths, SHA-256 and revision binding. The
case checks native names and permanent/rental options, opens and cancels buy
and gift previews, and compares the complete inventory and wallet. It does
not install products, purchase or send gifts.
For an explicitly authorized one-shot purchase, use the same two input paths with
`[shop] Schema=1` and `Scope=OwnerPurchaseAssertions` in the assertions file. Provide exactly one enabled
product, its exact current native option ID/price/currency/validity in the display file, and matching revisions.
Owner SKU identities are allowed in this separate scope. Native GameTest client 0.1.37 or newer, an existing
registered account, a direct IPv4 loopback endpoint and one login attempt are required. The sealed case becomes
`pages.shop.purchase`; ordinary stock scopes remain read-only. It invokes the original option Buy button and
confirmation OK once, observes real `0931/074D/0758/0574` transport and original reply consumers, then checks
the exact native wallet debit. It never automatically retries a purchase. Full acceptance additionally needs
independent inventory and currency SQL before/after receipts plus a new native login proving persistence.
The case's transport/wallet success alone is not an inventory or purchase-delivery acceptance result.
For an ETC extension dialog, it also validates the exact original preflight product/option, the known native
service and its enabled Yes/No controls, captures the real frame, then invokes Yes once. The original dialog
constructs the commit packet; the adapter never manufactures it or confirms an unrelated modal.
The adapter opens the native search view, clicks its original `FilterBtn`, and
waits for native query completion. Selecting `AllSerchRadio` alone opens an
empty view and does not count as a successful stock query.

`--play-unit-uid` selects a genuinely owned unit through the native hangar
before page, PvP or mission cases. The adapter waits for the full native unit
record, invokes the original equip business, and requires the genuine reply
plus matching active UID. `--random-loadout` exercises the native `!rand` PvP
room command.

`--mission-mode 5` or `8` registers bounded mission smoke cases: native channel
and mission admission, room creation, start, three level-2 wingmen, movement,
target acquisition and native combat observations. These cases do not cover
mission clear, settlement or active exit. Unavailable native missions or
channels fail without fabricated admission data.

An explicitly selected imported mission can use sealed `--mission-ai-count 0`
to observe native enemy NPCs against the human without friendly wingmen. The
default remains three, and zero is not supported for revival/profile suites.
Zero omits the AI-summoning command; it does not use `!ai0`.
When mission data changes, pass its matching `--client-package` explicitly and
verify the installed payload and server-plan revision before starting. Enemy
damage acceptance requires bound native HP-loss/death receipts for the human;
a natural mission failure or empty observer output does not prove a clear or
a damage result.

For `--gift-requests-only`, the private account profile may specify existing
owner mail commands in `[gift] body0` through `body3`, or use `page0/position0`
for unit gifts. The recipient must be an actual native pilot nickname. Native
authentication and the server authorize/process the mail; GameTest still waits
for an independent processing receipt and never grants currency or inventory
directly. Keep credentials and command configuration local.

### Early page and room navigation

Use `unit_scenario={"schema": 1, "early_navigation": "catalog"}` with
`pages_only=True` and an existing `play_unit_uid` to request the encyclopedia
from the first available enabled native control after login/onboarding.
The other targets are `capsule` and `room`. The setting defaults to `off` and
is bound to the sealed scenario; an inactive default cannot arm this case.

The observer records whether the target was ready and whether inventory was
complete at the actual click. It does not delay inventory or manufacture an
incomplete state. The `catalog` and `capsule` targets require the real target to
finish loading, followed by a genuine return to the room list with an enabled
room control and no loading modal. A cached widget's visible bit alone does
not count as recovery. The room target creates and enters an actual 3D wait room,
checks its owner, manager, main state and model, then leaves through the native
path; opening the room list is not enough. Normal unit-page checks follow.

For the distinct initial-inventory window, use `early_navigation_before_inventory`
instead of `early_navigation`, with the same `catalog`, `capsule` or `room` target.
This sealed case clicks during onboarding only while the genuine `CUnitInfo+0x20`
ready flag is unset. It fails if the timing window was missed, records native
packet/error observations, then requires real inventory completion, target
readiness and native recovery before normal onboarding can pass. It neither
creates an incomplete inventory nor substitutes a fixed delay.

### Blueprint synthesis

The Python `prepare` / `reuse_prepared` API accepts a sealed `unit_scenario`
with `schema: 1` and `blueprint: {recipe_id, book_uid}` for the original book
and recipe pages. Use `pages_only=True` and an existing `play_unit_uid`.
Optional `materials` contains three to five ordered `{unit_id, uid}` objects
for genuinely owned instances. The original selection callbacks populate the
material slots; the test never supplies a full-unit record or writes selection
state.

The material picker uses the original page arrows when an owned instance is on
another hangar page. It waits for the corresponding native cache update before
selecting that exact UID.

Material selection does not consume anything. To exercise synthesis, explicitly
add `synthesize: true` and `output_unit_id` to the blueprint scope. This test
consumes its isolated account's existing book and materials through the native
confirmation dialog. It requires the original `0x09AD` request, `0x07C9` reply
consumer and matching output brief inventory record. Keep an independent SQL
before/after receipt and verify persistence with a new native login; a success
reply alone does not show that the result survives a reload or that the acquisition route is complete. All identities
and consumption settings are hash-bound before launch.

### Native catalog occupancy

A separate sealed unit-page scope accepts
`native_id_probe: {ids: [...], witness_unit_id: ..., require_absent: true}`.
It permits up to 3072 unique IDs in the native range 1–9999999. After a real
login and equip, the observer verifies an existing typed unit witness, then
uses the original global-map lookup and exact end sentinel for every ID. It
writes `native-id-occupancy.json` under the run's native diagnostics directory.
This is a read-only probe: it does not register records or reserve IDs. Absence
applies only to that exact prepared client at that time; installation still
needs its own conflict check. Keep this scope separate from blueprint,
portrait, acquisition and capsule scenarios.

### Native record ownership and shutdown

A separate sealed `native_record_lifecycle: {ids: [...]}` scope observes one to
sixteen exact records in the owned client's catalog. It requires a quiet
`pages_only` run and an existing equipped instance. After the ordinary
bootstrap, login and equip assertions, this scope requests `WM_CLOSE` on the
owned game window and returns to the original message loop. Other suites keep
their existing immediate-exit behavior.

The runner holds the process handle and waits for normal exit before cleanup.
The original host's two final-release branches return exit code 0 or 1; the
native destruction receipt is required for either branch.
Its additional report layer checks the native final-release receipt against
the sealed catalog, including complete record bytes, five distinct owning
allocation addresses and exactly one release of each address. Missing,
modified or duplicate evidence fails even when the login cases passed. This
tests raw-record registration and native grid destruction; it does not cover skill/funnel
actor activation, battle behavior or transform/death/respawn cleanup. The probe does not create server rows or player inventory.

### Directed native transformation paths

The Python `prepare` / `reuse_prepared` API accepts `unit_transform_steps=1..16`
for a complete ordinary battle with an existing owned `play_unit_uid`. It is
mutually exclusive with the Apsalus and generic two-form roundtrip options.
Preparation seals `UnitDirectedTransforms` and `UnitTransformSteps` in the copied
observer manifest; ordinary runs explicitly disable this scenario.

The observer derives every next identity from the actual native unit record's
`+316` field, including one-way entries into later cycles. Each step uses the
original transform request and waits for the expected actual actor definition and
released movement mode, then verifies mapped movement and primary attack. Missing
records, self loops, exhausted paths and unexpected identities fail the check.
This test does not create form records, alter eligibility or supply reverse edges.
The default two-form scenario still requires the original reciprocal pair.

Local admission is distinct from a successful transform. The observer waits for
normal reload/cooldown eligibility after a denied local request; it never fills
ammunition or overrides the condition. Directed runs also observe the original
reply and notification packets through the read-only `match_ai` event fanout,
recording their error words and target IDs without installing a second handler
hook or modifying a packet. This scope requires the matching `match_ai` provider.
Actor and registry targets are reported separately
so a native rejection is distinguishable from stale actor bindings.

When the loader recognizes the matching `[run] id` and `SDGO_GAME_TEST_RUN`,
it preserves the sealed candidate runtime instead of invoking the ordinary
Manager update. Startup-gate tests explicitly set `SDGO_GAME_TEST_LAUNCH_GATE`
to that same ID to exercise the real Manager path. A different or absent run ID
does not enable this candidate bypass.

### Coordinated multiplayer

For imported missions, prepare two separate existing-account runs with
`--friends-only --mission-mode 5 --mission-game-mode-id <listed-id> --mission-ai-count 0`.
Launch both normally, then run `tools/game_test_mission_coop.py --host-run <host-run>
--peer-run <peer-run> --output <report.json> --external-confirmation`. The caller's
isolated input driver confirms the original mission invitation popup. The coordinator uses real mission room creation,
friend invitations, guest Ready and host Start. The optional `MapGateProbe` observer
reads native geometry and HP; it does not alter the gate or its result. Client gate
receipts and a separately observed server lifecycle are reported independently.

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

Add `--rounds 2` to repeat the complete battle in the same room. Both settlements must leave only
the declared humans across all twelve slots. The second Ready requires a newly queued native
request and its original acknowledgement; the previous round's successful receipt cannot satisfy it.
Every actor must observe a fresh start and end sequence for each round, and the independent server
report must contain the same number of complete lifecycles. The default is one round.

For delayed-client processing, add `--slow-peer-pilot <existing-guest-id> --slow-peer-seconds 12`.
The coordinator declares this fault before launch, pauses only that authenticated capsule's exact
native process immediately before the first Start, and resumes it after the bounded 1–25 second
interval, including when the host action fails. It verifies the process identity again before
resuming. This delays a guest's processing; it is not a packet-loss or Internet-latency simulation.
All ordinary roster, battle, settlement and server-report checks remain required.

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
server scenario summary does not replace the whole client matrix.

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

Quiet runs can also keep review footage without showing the window. Set
`SDGO_GAME_TEST_RECORD_DIR` to a local folder before launching the quiet clients.
Nothing records until a `record.on` file appears in that folder: its first line
names the clip and an optional second line limits recording to clients whose
executable path contains that text. Changing the file switches clips and
deleting it stops recording. Each client copies its real backbuffer just before
Present, up to 60 frames per second (`SDGO_GAME_TEST_RECORD_FPS` lowers the
bound), through a small ring of render targets that is read back two frames
later, so the copy never waits for the frame being drawn. A background thread
writes `<clip>_<pid>.raw` plus a timestamp index; a busy disk drops frames
instead of slowing the game. Convert a pair to a constant-rate video with
`python tools/game_test_recording.py convert <clip>_<pid>.idx --out clip.mp4 --fps 60`
(`--size 1920:1080` rescales). When `SDGO_GAME_TEST_RECORD_FFMPEG` names an
`ffmpeg` executable, the thread feeds that encoder directly instead and the clip
lands as `<clip>_<pid>.mkv` (`SDGO_GAME_TEST_RECORD_ENCODER` replaces the default
`-c:v libx264 -preset veryfast -crf 16 -pix_fmt yuv420p` output options); each
frame carries its wall-clock time, so the clip stays true to real time at the
game's own rate, and is re-encoded to a constant rate before editing. Quiet
clients stay muted, so the clips have no sound. Raw frames are large; delete
them once converted.

Two options shape such footage. `prepare --review-footage` keeps the template's
post-processing settings (bloom, heat haze, depth of field) and plays battles
at 1920x1080 instead of the logic tests' no-postprocessing profile; the run
stays hidden and silent, and the runner records the choice in the sealed task.
The observer setting `BattleReviewCamera=1` (with `BattleReviewCameraSeconds`,
default 6) points the native tracking camera at one AI unit at a time during a
battle, so the recording follows the AI fighting rather than the idle test
pilot. In such runs the native TAB roster is opened only for its spawn check and
released afterwards, and the roster-render part of the return check is skipped.
Both are development review aids and assert nothing.

### Short page demonstrations

Build the current client observer and prepare a quiet page scenario. With its
isolated backend ready, run:

```powershell
python -m tools.game_test_release_demo --run-dir build/gametest/hangar --output build/review/hangar
```

The tool follows the SDK's real `case_begin`/`case_end` events and records the
genuine backbuffer at up to 12 FPS directly into MKV. For review, completed
read/preview pages remain visible for three seconds before the next test action;
native page operations and assertions remain intact. Recording is capped at
20 seconds, and only a passed native run becomes a visual-review candidate.
Review the clip and crop out account names or deployment details before export:

```powershell
ffmpeg -i capture.mkv -t 12 -an -vf "fps=12,scale=900:-2" -c:v libwebp_anim -quality 76 -loop 0 demonstration.webp
```

Embed publishable clips in the corresponding Mod's Chinese and English guides,
following the PvP AI and mission wingmen pages, and register them in
`docs/feature-demos.json`. Do not create a separate demonstration gallery page. Release notes select
only demonstrations applicable to the programme package, retain Chinese and
English descriptions, and publish media separately from programme ZIPs.
