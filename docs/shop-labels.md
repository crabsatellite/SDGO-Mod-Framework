# Shop rental and Unicode labels

[简体中文](shop-labels.zh-CN.md)

Client fix for `GOnline-54BE1568-00C97000`. Shop sale options such as the Thai `7วัน` turn into
question marks in this client. The fix shows clear labels such as "7-day rental" and "permanent" in
the hover details, purchase window and gift window, and can show Chinese option names exported by
the server's shop catalog. Prices, currency, durations and purchases are unchanged.

| mod | host | part |
|---|---|---|
| `shop_labels` | client | the whole feature |

## Install

It is installed with the core fixes of the client framework, so players receive it with their server
owner's client package or update. A standalone `SDGO-ShopLabels-Patch` package with its own `Install.ps1` is also
available; see the [Chinese guide](shop-labels.zh-CN.md).

## Settings

Under **商城物品显示** in SDGO Mod Manager; restart the game after saving.

| Manager label | key | default | meaning |
|---|---|---|---|
| 显示租赁物品标签 | `ShowRental` | 1 | show the actual rental duration |
| 显示永久物品标签 | `ShowPermanent` | 1 | mark permanent units as permanent |
| 使用自定义中文名称 | `UseUnicodeOverrides` | 1 | read option names from `display.ini` |

`display.ini` is the UTF-16LE file exported by the [shop catalog](shop-catalog.md). When the rental or
permanent label is enabled for an option, that label is shown. Otherwise a custom name is used, and only
while the option's ID, category, duration, price and currency still match.

This is not a global font or chat fix, and it cannot recover product names already stored as question
marks in the database. The activity capsule currency label is handled by the separate
[capsule currency fix](capsule-currency-fix.md).

Log line when active: `rental/permanent labels enabled; <n> Unicode option overrides; prices and currency untouched`.

## Developer notes

The native grid lookup returns a single byte in `AL` and can leave pointer residue in the upper `EAX`
bits, so callers must use a byte return type.

The client converts sale option names with code page 874 (Thai), which turns them into question marks.
The mod hooks `shop.buy_options.populate`, `shop.gift_options.populate` and `shop.tooltip.option` to
record which option rows are being drawn, and `text.ansi_to_wide` to replace the CP874 conversion of
those rows only. The rental flag comes from the option category's client metadata (GridData type `0x22`, byte `+0x6D`, looked up
through `shop_labels.grid_data` / `shop_labels.grid_find`); the duration comes from the option's numeric validity.
Other text conversions are passed to the original function.
