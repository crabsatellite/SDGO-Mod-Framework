# Battle contribution rewards (Beta)

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
