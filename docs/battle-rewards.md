# Battle contribution rewards

[完整中文安装教程](battle-rewards.zh-CN.md) · [Rule tables](battle-reward-design.md)

BattleServer mod wired to the native final settlement. It computes GB/MB from the outcome, kills,
effective damage, assists and, in missions, minion and Boss contribution; PvP MVP earns extra MB.
Owners adjust the multipliers in SDGO Mod Manager; players keep their normal client and launcher.

The optional [solo MB progression preset](solo-mb-progression.zh-CN.md) adds mission MB floors,
3,000 MB for the first qualifying match each day, another 27,000 for three daily matches and 15,000
for twelve weekly matches. Fixed bonuses do not multiply. It is off until reviewed in the Manager.

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

Keep the Manager running while the server is available. It remembers enabled services, restores them
when it starts and retries a failed wallet worker. A listening BattleServer port alone does not prove
reward readiness: check both the wallet service and the reward panel. If the wallet heartbeat expires,
the Mod retains the original game reward path instead of applying the contribution policy. A small
GB credit can therefore be a real native payment. Restoring the worker affects later settlements;
it does not reconstruct earlier combat statistics. For unattended operation, include the existing
Manager in the operating system's normal startup configuration with its existing workspace.

## Delivery and recovery

GB replaces the native final amount and continues through the normal World/wallet path. The complete
policy GB goes into the native base reward row and superseded legacy GB bonus rows are cleared, so the
animated and skipped result screens show the same total. Pilot/unit EXP and other systems' rewards
are unchanged.

MB uses durable jobs and a database receipt keyed by match and account, calling the native
`sp_PocketGC_IO`. A lost response can be retried without a second credit. The worker then reads the
current MB and sends the normal `0x064B` balance update through the Agent; offline players see the new
balance at their next login or shop cash query. Keep queued jobs and receipts during recovery.

To stop the worker, use **停止本 Manager 的钱包服务** in the reward panel; it only stops the worker this
Manager started.

## Historical GB compensation

When complete combat snapshots are unavailable, an administrator can compensate verified battle
payments using comparable healthy settlements under the same GB rules. Normalize each sample by
its event multiplier, take the exact average, then multiply by the affected battle's event factor and
round down once. Pay only `max(0, target - already_paid)`. Larger original payments are kept.
This is an administrative compensation, not an exact reconstruction of kills, damage or MVP.

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
