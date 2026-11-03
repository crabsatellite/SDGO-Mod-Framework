# Shop catalog manager (Beta)

[简体中文（完整说明）](shop-catalog.zh-CN.md)

Owners manage their own native shop offers in the **商城商品** (shop) panel of SDGO Mod Manager:
add, search, edit, list/unlist, retire and restore. Players keep buying in the native in-game shop.
Chinese option names and rental labels come from the separate [shop labels fix](shop-labels.md).

Set the existing shop workspace, the `WorldServer` folder and optionally a local client folder (for
display labels). Pick a native item and click **新增商品** (add); edit name, option label, price, GB/MB
currency, quantity or rental days and listing, then **保存商品** (save). Changes are drafts until
**应用到游戏** (apply to game). Retired offers keep their IDs and purchase history. JSON import/export
contains no credentials or local paths.

The WorldServer module (`548A86E3-0010C000`) starts disabled until configured and is installed once with
the standalone `Install.ps1`. It updates only its own single-item offers in a transaction and keeps native
listing, billing and delivery; accounts, balances, inventory and history are untouched. Prices must be
positive; GB is native currency 1 (GP), MB native currency 2 (GC). Only the Manager publishes the
configuration; the WorldServer component applies nothing at startup.

The apply tool preserves native computed columns and their UDFs. It writes the
underlying subitems and checks the effective parent quantity, validity, price
and currency in the same transaction, without changing the database layout.

New product names and sale-option labels require Chinese text. Existing owner data remains intact;
fill missing names before editing or publishing. The separate [shop localization Mod](shop-localization.md)
provides translated descriptions and shared preview names. Its **Update Chinese search** action refreshes
only the guarded search procedure and aliases; it does not apply catalog drafts or prices.
