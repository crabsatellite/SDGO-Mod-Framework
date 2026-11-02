# Activity capsule currency fix

Corrects activity capsule machine 9301 from **99 GP** to **99 coins** in the machine list and
in its purchase confirmation. The server already charges coins (`tbl_ItemMachinePrice`
`Type=2, Price=99`); only the client record said GP.
中文：[capsule-currency-fix.zh-CN.md](capsule-currency-fix.zh-CN.md)

| mod | host | part |
|---|---|---|
| `capsule_currency_fix` | client | the whole feature |

## Use

Nothing to do: it is one of the core fixes installed with the client framework, and friend-server
players receive it through `update.bat`. A client that only needs this fix can import
`SDGO-CapsuleCurrencyFix-<version>.zip` with Mod Manager's **导入客户端配置包** while that game is
closed. The Manager lists it under **修复归档** (fix archive). Restart the client after installing;
a running game does not reload it.

It is independent of the shop labels and of the optional capsule catalog, and works alongside both.

## Settings

None.

## Check

1. Open the activity capsule page. Machine 93 (ID 9301) shows **99 coins**; other machines keep
   their currency.
2. Open its purchase confirmation and cancel. Currency, price and the current/after balance use coins.
3. `sdgo\logs\sdgo.log` contains both `capsule_currency_fix: loaded ...` and a result line.

| log line | meaning |
|---|---|
| `activity machine 9301 display currency corrected from type 0 ...` | list copy and client registry record corrected |
| `... is already type 2 (token); no change made` | nothing needed changing |
| `activity machine 9301 display fix refused (<reason>)` | a check failed; nothing was written, investigate |
| `required symbol capsule.activity.load_items ...` | unsupported client build; the mod stays off |

A `loaded` line alone only proves the DLL loaded, not that the page was corrected.

## How it works

The mod hooks `CapsuleMachineUI::LoadCapsuleItemData`. After the original function fills the
activity list it checks vector bounds, the 179-byte record size, a page shape of `pages × 8` slots
(at most 4096), exactly one 9301 row, price 99 and a currency of 0 (wrong) or 2 (already right).

The confirmation dialog does not read that list copy: it looks up GridData type `0x3C` again and
takes currency from `+9` and price from `+15` of the registry record. So the row's Grid ID must
be `145093`, the native `grid.find` must return that type `0x3C` record, both records must be
writable and all 179 bytes must match. Only then is one currency byte changed in each.
Any other input is left untouched.

Disk GDT files, catalog overlays, server tables, prices, prize pools, wallet values, charging and
refunds are not changed.

## Verification scope

Unit tests cover the single-byte change, idempotence and rejection of duplicate, missing,
wrongly priced or unknown records. A DLL test runs the real hook against the current 94-machine /
96-slot list with a simulated registry, including the 100000 GP collection machine. The standalone
package is checked through the Manager import and archive. The confirmation-dialog read path comes
from static analysis; the actual screen and a real draw remain manual checks by the player.
Implementation notes: [mods/capsule_currency_fix/README.md](../mods/capsule_currency_fix/README.md).
