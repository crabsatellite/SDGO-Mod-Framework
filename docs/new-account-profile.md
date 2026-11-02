# New-account starter profile

[简体中文](new-account-profile.zh-CN.md)

Server-side fix for `WorldServer-548A86E3-0010C000`, on by default. The stock server gives a new
account 9,999,999 GP and cash and writes rank 15/1 into a new profile. With this mod, new players
start with **5,000 GP, 0 cash and Corporal (rank 5, step 1)**, and an automatic unit gift at first
profile creation keeps at most **one** unit. Players need no client mod.

## Install

Stop the World server, install the server package with the World role, then start it again:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File install_server.ps1 -ServerRoot D:\SDGO\WorldServer -Role World
```

Install it on every World instance that creates new players. Battle, Agent, Store servers and player
clients do not need it. Check that `sdgo/logs/sdgo.log` shows `new-account defaults active: ...`.

## Settings

Edit these under **新账号默认配置** in SDGO Mod Manager, then restart World. They are stored in
`sdgo/mods/new_account_profile/config.ini`.

| Manager label | key | default | meaning |
|---|---|---|---|
| 初始 GB | `InitialGP` | 5000 | starting GP |
| 初始 MB | `InitialCash` | 0 | starting cash |
| 初始军衔 | `RankNumber` | 5 | starting rank, 1..15 in the Manager |
| 初始军衔子等级 | `RankStep` | 1 | starting step, 1..5 |
| 初始机体数量上限 | `InitialUnitLimit` | 1 | automatic units kept at first profile creation |

Starting experience is read from the server's `tbl_Levelname` row for that rank and step; the row
must exist.

`InitialUnitLimit` is **not** a limit on how many units a player can own. If the server grants
100 units automatically when a pilot is first created, `1` keeps one and `0` keeps none. Units the
player already owned, normal starter selection and later purchases, capsules and rewards are not
affected. If the server grants no units at that step, the mod does not add one.

## What it changes

- New accounts get the configured wallet when they are registered. The stock first-login retry
  behaviour stays as it was.
- A pilot created for the first time gets the configured rank, matching experience and wallet.
  This also covers accounts registered earlier that never created a pilot.
- Existing pilots are never reset: their money, experience, rank, units and rewards stay as they are.
- If creation fails, it is rolled back as a whole; a half-initialized profile is never kept.
- If the server's database session is unavailable, creation fails with a database error instead of
  falling back to the stock 9,999,999 defaults.

Native starter items, skills and operators are kept. The stored procedures themselves are not
modified; the mod applies its values around the verified World calls in a database transaction.

The mod reads only `[DB StoreBilling] DBName` from the World INI and uses the server's existing
database session; it never reads passwords. The Common, Game and Billing databases must be on that
SQL instance, as in the stock server.

## Turn off

Clear its switch in the Manager and restart World. New accounts then use the stock rules again.
Existing pilots keep their current data. `install_server.ps1 -Uninstall` removes the whole
framework, not just this mod.

## Developer tests

`ctest` runs `new_account_policy_test`. The SQL test needs a disposable SQL Server container:

```powershell
python tests/run_new_account_sql_test.py --container <isolated-sql-container> --credential-file <local-private.json> --out build/starter-sql-check
```
