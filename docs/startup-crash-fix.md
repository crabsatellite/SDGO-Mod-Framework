# Random crash at game start

Stops the game from occasionally crashing on the loading screen right after it starts. The game
picks a loading screen at random, and a few of them show a unit that is missing from the client's
unit data. Such a pick is now replaced by the nearest loading screen whose unit exists.
中文：[startup-crash-fix.zh-CN.md](startup-crash-fix.zh-CN.md)

| mod | host | part |
|---|---|---|
| `startup_crash_fix` | client | the whole feature |

## Use

Nothing to do: it works automatically once the framework is installed on the client. SDGO Mod
Manager lists it under **修复归档** (fix archive). To switch it off while troubleshooting, click
**显示开发信息** (show developer info), clear its switch and restart the game.

## Settings

None.

The protection also stays active when optional login-showcase fitting is unavailable. In that case the expanded showcase is disabled and the original loading selection still receives the missing-unit check.

## Logs and troubleshooting

On the first loading screen the mod lists the loading sets whose unit is missing (numbers depend
on the client's data):

```
startup_crash_fix: loading set <set id>: unit <unit id> is not in this client's unit data
startup_crash_fix: <N> loading sets, <M> with a missing unit
startup_crash_fix: loading set <set id> picked: its unit <unit id> is missing, set <set id> (unit <unit id>) shown instead
```

| log line | meaning |
|---|---|
| `N loading sets, M with a missing unit` | M of the sets would have crashed the game when picked |
| `loading set ... picked: its unit ... is missing, set ... shown instead` | this start would have crashed; the named set is shown instead |
| `... no set has a known unit; shown as the game would` | no usable replacement exists in this client's data |
| crash when clicking login, `access violation at 008FEE6B` | mod version 1.0.0 is installed; update the framework |
| `required symbol gate.show_set_unit ...` | unsupported client build; the mod stays off |
| `login showcase fitting unavailable; original startup crash protection remains active` | update the framework to restore showcase support; the startup guard is already working |

## How it works

When a loading screen with a unit is shown, the game looks the unit up and uses the result without
checking that it was found. The mod checks first. If the unit is missing, it switches to the nearest
earlier loading set whose unit exists (or the nearest later one), as if the random pick had landed
there, so the screen and the login animation always have a valid unit.

## Developer notes

While the game starts (`CGameApp::SetState`, `SERVICE_LOGO`), the gate loading screen (`0x0090C320`)
picks a loading set at random (`0x00904180`: grid data type 5, one of the ids from 27102 on, as many
as there are sets with a unit; `srand(timeGetTime())`) and keeps its id at `+0x20E0`. It then walks
the loading sets from 27101 and shows each set without a unit and the picked set (`0x008FE380`,
symbol `gate.show_set_unit`). For a set with a unit, that function looks the unit up in the unit data
(grid data type 2, `0x0059D2B0`, symbol `grid.find`), creates the unit's model at `+0x207C`, and reads
the unit record's name at `+0x264` without checking that the unit was found (access violation at
`0x008FE610`, also present in the game's own `zoa_log\Exp_*.txt`).

When logging in, `0x008FEE30` plays an animation of the set whose id is at `+0x20E0` on the model at
`+0x207C`, without checking either. So the mod hooks `gate.show_set_unit` and writes the replacement
set's id to `+0x20E0`. A replacement before the picked set has already been passed by the walk and is
shown at once; one after it is shown when the walk reaches it. Either way exactly one unit is set up.

Version 1.0.0 showed no unit instead, which left `+0x207C` unset and crashed at login
(`0x008FEE6B`) whenever a set with a missing unit had been picked.

On the reference client, 2 of 9 loading sets have a missing unit: set 27106 (unit 25452) and set
27107 (unit 25454).
