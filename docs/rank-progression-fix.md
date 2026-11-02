# Battle rank progression fix

[简体中文](rank-progression-fix.zh-CN.md)

Core fix for `WorldServer-548A86E3-0010C000`: battle settlement no longer lowers a pilot's rank.
Players need no client update.

## The problem

The native procedures `dbo.sp_SetBattleExp_V4` and `dbo.sp_SetGridBattleExp_V2` decide whether to
change rank with a faulty comparison. When the EXP-derived rank has a higher sublevel, a higher
major rank is overwritten: for example rank **15-1** with 30,090 EXP gains 20 EXP and becomes **5-2**.

## Install

Stop the target WorldServer, install the server package with the World role, then start it normally:

```powershell
powershell -ExecutionPolicy Bypass -File .\install_server.ps1 -ServerRoot "D:\SDGO\Server\WorldServer" -Role World
```

The fix is on by default and SDGO Mod Manager lists it under **修复归档** (fix archive). Every
WorldServer that settles battles must load it; sharing a database is not enough.

The mod reads only the database name from WorldServer's `[DB Game]` section and uses the server's
existing database session. It never reads the password. That database account must be able to read
the two native procedures and create the mod's own schema, state table and procedures.

## What it changes

On the first matching battle settlement, the mod creates corrected copies of the two procedures in
its own schema `sdgo_rank_progression_fix`, changing only the rank comparison:

```sql
IF(@UserLevelNumber < @newlevel OR
   (@UserLevelNumber = @newlevel AND @UserLevelStep < @NewLevelStep))
```

Those two settlement calls are then routed to the copies. EXP, rewards, counters, normal promotions,
the native 15-5 and ranked-tier limits and leaderboard recalculation are unchanged. The original `dbo`
procedures are not modified. Ranks lost before installing the fix are not restored.

## Logs

| log line | meaning |
|---|---|
| `rank progression routing installed ...` | loaded; waiting for the first battle settlement |
| `rank policy active for normal battle` / `Grid/Tag` | settlements of that kind now use the corrected procedure |
| `rank policy unavailable; native command retained: ...` | the copy could not be prepared; that call used the original procedure |
| `host INI needs [DB Game] DBName ...` | the WorldServer INI has no database name; the mod stays inactive |

## Turn off

In the Manager click **显示开发信息** (show developer info), clear the switch in **修复归档** and
restart WorldServer. Settlement then uses the original procedures again; the mod's unused SQL objects
can stay in the database.

## Developer notes

The hook symbols are `rank.execute`, `rank.execute_rows`, `rank.connection_layout`,
`rank.command.battle` and `rank.command.grid`. Only the two exact native command strings are
redirected; native parameter binding, output processing, EXP awards, rewards and counters stay in
place, and the descriptor's command pointer is restored after the call.

The copies are derived from the installed definitions, replacing only the procedure identity and the
rank comparison. Both are prepared in one transaction under an application lock. Preparation checks
ownership, the supported comparison, definition hashes and SQL options, and refuses unknown
definitions, a foreign `sdgo_rank_progression_fix` schema or externally edited copies. No native SQL
is bundled. An existing transaction on the borrowed session is neither committed nor rolled back.
## Developer tests

`ctest` runs the offline C++ tests. The optional database test creates an isolated scratch database
with synthetic pilots and needs a backup of the two original procedure definitions:

```powershell
powershell -ExecutionPolicy Bypass -File tests\rank_progression_db_test.ps1 -ServerIni "D:\SDGO\Server\WorldServer\WorldServer.ini" -OriginalBackup "D:\Maintenance\original-rank-procedures.json" -BuildDir build
```
