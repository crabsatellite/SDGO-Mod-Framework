# Activity capsule currency fix

Corrects activity capsule machine 9301 from **99 GP** to **99 coins** in the machine list and
in its purchase confirmation. The server already charges coins (`tbl_ItemMachinePrice`
`Type=2, Price=99`); only the client record said GP.
中文：[capsule-currency-fix.zh-CN.md](capsule-currency-fix.zh-CN.md)

| mod | host | part |
|---|---|---|
| `capsule_currency_fix` | client | the whole feature |

## Use

Nothing to do: it is one of the core fixes installed with the client framework, so players receive
it with their server owner's client package or update. A client that only needs this fix can import
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

## How it works

After the activity list loads, the mod finds machine 9301, confirms it still costs 99 and that the
record has the expected layout, then sets the currency to coins in the client-memory record that both
the list and the purchase confirmation read. If anything does not match, nothing is changed.

Only the client's in-memory display changes; game files, server data, prices, prize pools and
charging are untouched. Developer notes:
[mods/capsule_currency_fix/README.md](../mods/capsule_currency_fix/README.md).
