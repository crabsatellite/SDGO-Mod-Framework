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

## How it works

When a loading screen with a unit is shown, the game looks the unit up and uses the result without
checking that it was found. The mod checks first. If the unit is missing, it switches to the nearest
earlier loading set whose unit exists (or the nearest later one), as if the random pick had landed
there, so the screen and the login animation always have a valid unit.
