# MB progression

[简体中文](solo-mb-progression.zh-CN.md) · [Battle rewards](battle-rewards.md)

In **SDGO Mod Manager → Battle rewards**, preview the progression preset or edit
the daily/weekly milestones separately. The preset preserves GB and multipliers,
changes ordinary battle MB floors and enables fixed milestone bonuses. New public
configurations start with milestones disabled; updates preserve existing settings
and never issue an immediate grant.

| Milestone | Public initial fixed MB |
|---|---:|
| First qualifying match each day | 100 |
| Three qualifying daily matches, additional | 400 |
| Twelve qualifying weekly matches, additional | 1,000 |

Amounts, target match counts, minimum duration and enablement are configurable.
Set them from your server's prices and intended play frequency. Daily and weekly
targets share qualifying-match progress. Solo missions and AI matches count;
defeats can count without MVP or consecutive login days. A match must pass native
eligibility, meet the configured active time and contain actual combat contribution.
Damage without a kill can qualify. Spectating, AI accounts and idle/short matches
do not qualify. Browser previews never submit a payable match.

The preset's one-times mission clear/fail MB floors are 2/0.5 with a total cap of
5. PvP win/loss/draw floors are 5/3/4, with 10 extra for MVP. Fixed milestone bonuses
do not multiply. Days reset at midnight UTC+8 and weeks on Monday; the same wallet
account shares progress across BattleServers. Gathering rules have their own
configurable period/offset and are described in the battle-rewards guide.

Budget against your own shop prices. A weekly fixed-income estimate is login days
times (daily first + daily target), plus the weekly target and any gathering bonus.
Include ordinary battle and holiday income separately. The Manager's four-day
budget example does not change shop prices or promise a particular purchase time.

Install the matching Battle Mod and Manager after ending the affected battles.
Installation adds the owned progress objects and preserves native wallet procedures.
Immutable policy snapshots and original match-end timestamps determine eligibility
and period attribution. Older v1 jobs can still pay ordinary MB but cannot invent
missing milestone metadata.

Progress, fixed MB, native wallet credit and milestone receipts commit together.
Ordinary battle MB keeps its own transaction. Retries safely finish between those
transactions without counting or crediting twice. Editing amounts does not repay
an already claimed milestone. Disabling affects future settlements; queued jobs
retain their original snapshot. Preserve pending jobs, policies and wallet receipts.
Players use the existing native balance notifications, login or shop refresh.
