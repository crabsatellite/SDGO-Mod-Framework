# Native GameTest

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

### Recreate a local backend

`tools/game_test_backend.py` owns copied native services and an isolated SQL
container. It requires Docker, a compatible local native server tree, the server
stage, and verified database backups (`backend.json` plus its `backups` folder).
Build `winmm_shim`, `sdgo_core`, `sdgo_data`, `pvp_ai` and `game_test_server` for
that server stage. Missing loader, observer or symbol files fail preflight
before SQL resources are created or databases are restored.
Use a fresh work directory and an unused IPv4 loopback address. For example:

```powershell
python tools/game_test_backend.py prepare --source-server D:\SDGO\Server --stage build/stage/Release --backup-source D:\Tests\verified-backend --work build/fps-backend --target-ip 127.93.0.1
python tools/game_test.py prepare --client-template D:\Games\SDGO --stage build/stage/Release --run-dir build/fps-probe --target-host 127.93.0.1 --login-port 5001 --fps-cap 160
python tools/game_test_backend.py serve --work build/fps-backend --client-run build/fps-probe --stage build/stage/Release --startup-xml D:\SDGO\ServerStartUp\ServerStartUp.xml --lifetime 1800
```

Run the matrix from another terminal after `backend_ready`, specifying that
same `--target-host`. Service startup retries and database readiness probes are
bounded; launch selection uses both the startup XML and native channel table.
The FPS suite never needs an account. Other suites reuse their private account
profile by default; authenticated backend observers also require
`serve --host-pilot-id <actual native pilot ID>` for that account. No account ID
is embedded in the backend tool. Live COPY_ONLY backup is an explicit opt-in
(`--allow-live-copy-only`) when a verified backup source is unavailable.

```powershell
python tools/game_test_backend.py stop --work build/fps-backend
python tools/game_test_backend.py cleanup --work build/fps-backend --remove-runtime
```

Cleanup verifies owned service exit and the SQL container/volume identity, then
removes private credentials and copied runtime while retaining public receipts.
The separately prepared `fps-probe` belongs only to that test: remove it after
its process has exited and evidence is exported. Shared backups and source
servers remain available for later environments.

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
on the private desktop. Fullscreen is rejected. The native Miles WaveOut
driver's zero master gain is read back independently. The copied logic-test
profile disables native postprocessing, sets all five native effect levels to
zero and checks the effective runtime flags. These checks do not claim visual or
postprocessing acceptance. Local direct tests bind only loopback and do
not change firewall profiles or notifications. Nonlocal quiet runs require
permission to prepare their scoped application rule; setup failure never opens
a UAC prompt or falls back to an interactive client.

Runs reuse `%LOCALAPPDATA%\SDGO-GameTest\account.ini`, or the file selected with
`--account-profile`. Preparing another run never generates a new account.
Only explicit `--create-test-account` creates the first persistent profile;
existing profiles are never overwritten.

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

For focused ordinary PvP checks, `--ai-difficulty Hard --expected-ai 5` seals the
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

After a feature passes its complete acceptance scenarios and the matching runtime is deployed and
published to the main branch, update its release status as part of the delivery. Remove obsolete
Beta, testing or pending-release labels from the bilingual feature index and guides, and align the
Mod channel (`stable`), Manager labels, version suffixes and package templates. Verify the installed
and published metadata agree. A build, fixture or partial scenario alone does not promote a feature;
other features retain their existing status until their own acceptance and publication are complete.
