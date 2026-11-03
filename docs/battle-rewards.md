# Battle contribution rewards

[完整中文安装教程](battle-rewards.zh-CN.md) · [Rule tables](battle-reward-design.md)

BattleServer mod wired to the native final settlement. It computes GB/MB from the outcome, kills,
effective damage, assists and, in missions, minion and Boss contribution; PvP MVP earns extra MB.
Owners adjust the multipliers in SDGO Mod Manager; players keep their normal client and launcher.

The optional [MB progression preset](solo-mb-progression.md) adds mission MB floors
and configurable daily/weekly milestones. It is off by default. Its public initial
values are 100 MB for the daily first match, 400 extra for three daily matches and
1,000 extra for twelve weekly matches. Existing owner-authored settings are preserved.

## Human multipliers and gathering rewards

In **Battle rewards → Human multipliers and gathering rewards**, optionally enable
a twelve-entry multiplier table, one entry for each human count from 1 through 12.
Without it the existing `1.1^(human count - 1)` rule remains in effect. AI and
spectators do not count. The table multiplies ordinary contribution rewards along
with the base/event multipliers and participation factor; fixed bonuses are separate.

Gathering rewards are disabled by default and have no public payout preset. Add a
rule, choose its human threshold, PvP/PvE scope, per-match/daily/weekly period and
fixed GB/MB amounts, then enable it. Set the minimum active time, contribution
requirement and UTC offset for the group. Each counted human and the recipient
must meet those participation conditions. The server supplies this data; browser
previews cannot submit a match for payment.

Eligibility is per account. Friends who have claimed this week's reward do not
prevent a later friend from claiming their own share. All BattleServers sharing
the same wallet deduplicate the rule/account/period. Days start at local midnight
and weeks on Monday at the selected offset, using the original match-end time.
Retries do not duplicate payment. Rule IDs identify claims; keep an ID when editing
amounts to retain that period's claimed state. Creating a new ID creates a separate reward.

Fixed gathering bonuses do not multiply. GB and MB use the native wallet procedures
in one transaction, so a failed payment rolls back both currencies and the claim.
The separate GB payment is not constrained by the ordinary 65,535 settlement field.
Online balances use the native notification paths; disconnected players refresh
their balance through normal login or shop queries.

Install the matching Battle Mod and Manager/wallet worker before enabling these
rules. Gathering requires native v3 participant snapshots; older pending v1/v2
jobs retain their original behavior and never invent gathering eligibility.

## Install and manage in the Manager

Open **SDGO Mod Manager.exe** and choose **战斗奖励** (battle rewards). Select the existing reward
workspace, native server INI, the stopped BattleServer target and the extracted framework/reward kit.
Click **检查安装 / 更新** (check install/update), resolve any blockers, then apply the reviewed plan.
The installer keeps configuration, symbols and recovery receipts and adds only this feature's
versioned wallet objects; native wallet procedures and balances are not modified.

Start the Manager-owned wallet service in the same panel, then start the game server normally.
Multipliers, rules and previews are edited there and apply to later settlements without restarting
BattleServer. The panel shows a connection only when the policy, native settlement and wallet
heartbeat are all confirmed. See the [Manager guide](mod-manager.md).

Keep the Manager running to deliver queued cash awards. It restores enabled services and reconnects
broken database sessions. With `battle_rewards 0.2.2`, the worker first verifies and persists the native
ledger identity separately from its short-lived heartbeat. Later Manager pauses, updates or restarts
do not cancel contribution calculation or durable jobs: Battle uses the frozen match policy and the
verified binding. An uninitialized or mismatched wallet, invalid policy or failed durable write still
rejects custom settlement and reports the cause. Update Manager and start its wallet service before
updating each stopped, idle BattleServer.

A listening BattleServer port alone does not prove the wallet has recovered. Saved jobs remain queued
while the panel reports a disconnected worker; restoring the same workspace processes them automatically
without waiting for a nightly batch. For unattended operation, add the existing Manager and workspace
to the operating system's normal startup configuration.

The default time factor reaches its full value at 180 active seconds. A 45-second round receives 25%
of that factor, so a small short-round GB amount alone does not demonstrate an outage.

## Delivery and recovery

GB replaces the native final amount and continues through the normal World/wallet path. The complete
policy GB goes into the native base reward row and superseded legacy GB bonus rows are cleared, so the
animated and skipped result screens show the same total. Pilot/unit EXP and other systems' rewards
are unchanged.

MB uses durable jobs and a database receipt keyed by match and account, calling the native
`sp_PocketGC_IO`. A lost response can be retried without a second credit. The worker then reads the
current MB and sends the normal `0x064B` balance update through the Agent; offline players see the new
balance at their next login or shop cash query. Keep queued jobs and receipts during recovery.
Brief Windows sharing conflicts during job reads or archival are retried with a fixed limit. Persistent
failures remain visible and are never treated as successful payment. Reconnection cannot silently
replace the verified ledger identity or weaken account, policy-hash and receipt deduplication.

To stop the worker, use **停止本 Manager 的钱包服务** in the reward panel; it only stops the worker this
Manager started.

## Historical GB compensation

When complete combat snapshots are unavailable, an administrator can compensate verified battle
payments using comparable healthy settlements under the same GB rules. Normalize each sample by
its event multiplier, take the exact average, then multiply by the affected battle's event factor and
round down once. Pay only `max(0, target - already_paid)`. Larger original payments are kept.
This is an administrative compensation, not an exact reconstruction of kills, damage or MVP.
Older fallback settlements without a captured intent cannot be reconstructed exactly. Legitimate
short-round rewards must not be classified as missing payments merely because their amounts are low.

The source checkout and reward owner toolkit include a maintenance command (Python 3.10+, `pyodbc`
and a SQL Server ODBC driver). Prepare and review a private plan using the
[recovery plan format](battle-reward-design.md#gb-compensation-plan), then preview it:

```powershell
python tools/recover_battle_reward_gb.py --plan "D:\SDGO-Owner\Recovery\gb-plan.json" --server-ini "D:\SDGO\Server\WorldServer\WorldServer.ini"
```

The preview validates the arithmetic without database writes. Add `--apply` to execute the reviewed
plan. Execution checks the selected database's file scope, wallet identity, healthy sample receipts
and original native credits, then uses the unchanged `sp_PocketGP_IO` to credit the difference.
The batch is transactional. Each original payment has an immutable compensation receipt; retrying
the same plan pays nothing again, while changing already-applied terms is rejected. Keep the private
plan and database receipts, and refresh the GB balance through a normal game login after compensation.

## Disable or remove

Close the BattleServer, switch the feature off in the Manager and start the server again. To remove
it, plan a scoped uninstall and then add `--apply`:

```powershell
python tools/install_battle_rewards.py --battle-dir "D:\SDGO\Server\BattleServer1" --workspace "D:\SDGO-Owner\Rewards" --uninstall
```

The framework, other mods, queue, configuration and wallet history are kept. Rewards already paid are
not rolled back.

## Developer tests

```powershell
cmake -S . -B build -A Win32
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Default checks use the real core/mod DLLs, synthetic native frames and an isolated ledger. Optional
native SQL checks need a local server INI and permission to create and drop scratch databases.
