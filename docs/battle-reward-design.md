# Battle rewards: rules and native settlement

[中文完整规则](battle-reward-design.zh-CN.md)

See [deployment and recovery](battle-rewards.md) for installation.

## Policy

```text
final GB/MB = floor(
    one-times reward * base multiplier * event multiplier
    * human multiplier * participation duration factor
)
```

Standard PvP with AI teammates/opponents uses the same reward tier as human-only
PvP. AI and observers do not increase the human multiplier or receive wallets.
An owner-configured table can set a separate multiplier for each human count.
Without that table, the default compounds: 1, 1.1, 1.21, 1.331 for one through four humans.
A base multiplier of 10, double time and four humans therefore produce 26.62.

Daily/weekly milestones and gathering bonuses are separate fixed awards; base,
event and human multipliers do not multiply them. Gathering rules may pay per
match, day or week, with per-account/rule/period deduplication across servers.
Public defaults disable gathering awards and contain no payout rules.

The one-times amount combines outcome/clear participation rewards with kills,
effective damage, assists and, for PvP, an additional MVP award.
Missions reward minions and proportional Boss contribution, rather than only the
last hit. Currency is rounded once after all factors using exact rational math.

The initial PvP defaults include 500 GB/2 MB for a win, 300 GB/1 MB for a loss,
60 GB/0.2 MB per kill, 15 GB/0.1 MB per 1,000 effective damage and an additional
200 GB/6 MB for MVP. Contribution caps and participation-time scaling apply.
Full amounts and limits are in `owner/rewards/policy.example.json` and the Chinese
rule guide; they remain configurable starting values.

For the same three-minute, four-human, base-ten and double-time conditions:

| Example | GB | MB |
|---|---:|---:|
| Normal mission: 15 minions, one Boss credit, 8,000 damage | 4,871 | 13 |
| PvP loss: two kills, 6,000 damage | 13,576 | 53 |
| PvP win: three kills, 8,000 damage | 21,296 | 90 |
| Same PvP win with MVP | 26,620 | 250 |

## Unified control surface

The reward preview, policy editor and worker controls are registered providers of
[SDGO Mod Manager](mod-manager.md). They share its local API, session and configuration
checks. The retired standalone reward HTTP server is not part of the package.

## Settlement integration contract

The trusted native server collector supplies outcomes, combat stats, MVP and
unique participating accounts. All recipients use one policy/event snapshot;
`preview_match` derives qualifying human count, rejects duplicate accounts and
checks aggregate Boss credit. The preview accepts owner-authored scenarios only;
HTTP input must never become an authoritative payout source.

The double-time component supplies a factor of one or two, consumed once. New
one-times policy values must replace, not additionally multiply, existing boosted
currency rewards. The native adapter and owner worker coordinate GB/MB delivery receipts,
duplicate settlement prevention, client display and verified packet field widths.
Experience, login gifts, compensation and purchases remain outside this currency
policy.
The reserved objective-event coefficients are zero because the current native adapter
does not supply that statistic; unverified Stars are never treated as objective counts.

## GB compensation plan

`tools/recover_battle_reward_gb.py` consumes a private JSON object with these fields:

| Field | Meaning |
| --- | --- |
| `schema_version`, `method` | `1` and `healthy_average_gb` |
| `expected_scope` | Lowercase `ScopeId` from the selected wallet's `SdgoBattleRewardWalletMeta` |
| `expected_data_root` | Absolute directory containing all files of the selected StoreBilling database |
| `samples` | Healthy records: `source`, `event_multiplier`, `match_id` and `plan_hash` |
| `sources` | Affected records: `source` and `event_multiplier` |
| `entries` | Output of `owner.rewards.gb_recovery.build_entries(samples, sources)` |

Each `source` contains exactly `account`, `gb`, `after`, `local_time` and `table`: the account,
native credit, resulting balance, database-local ISO timestamp with milliseconds and native GB log
table name. Only battle credits (`f_from=2`, `f_etc=1`) qualify. Each sample must also match an
acknowledged `SdgoBattleRewardMbReceipt` by match, account and plan hash. Before selecting samples,
independently match that receipt's time and reward policy to the native GB credit. Use an event
multiplier of `1` or `2` from the rules active at each settlement, rather than guessing from the amount.
Keep account records, proofs and plans outside version control.

The command recalculates `entries` and rejects edited amounts, duplicate evidence, foreign wallet
scopes and ambiguous original credits. Original LiteSQL relative Data paths are resolved using the
instance's verified data directory. Successful execution writes `SdgoBattleRewardGbRecovery` receipts
and native credit reason `31603`; it does not change original wallet procedures. A receipt's source
key identifies the original native transaction independently of the requested compensation.

Run calculation checks with `python -B -m unittest discover -s tests -p test_reward_gb_recovery.py -v`.
`tests/run_reward_gb_recovery_sql.py --test-root <marked-test-copy> --server-ini <test-ini> --account <existing-unused-test-account>`
checks native credit, replay and rollback in the existing isolated test database. It always rolls back
the test transaction and verifies that the account balance and native log count are restored.
