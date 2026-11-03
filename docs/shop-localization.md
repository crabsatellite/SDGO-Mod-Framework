# Shop localization and Chinese search

`shop_localization` translates native shop text, shared unit-preview titles, unit names in hangar exchange
previews and fixed messages from cooperating Mods. All five scopes default to enabled. Manage them in SDGO Mod Manager and restart
the client after changing them. Missing or disabled localization preserves original consumer text.
Player-authored room titles, drafts and chat are never translated.
The initial buy/gift dialogs and later confirmation pages have separate title paths. Operator, battleship,
paint and unit tooltips translate both description and usage text, including literal `\n` and actual newlines.

Players install the server owner's client package or update. Owners configure the Shop panel's existing
workspace/World/client paths and click **Update Chinese search**. This independent action publishes only
owned search aliases and both backed-up, guarded native list procedures; it does not publish a catalog draft,
change products/prices, or touch inventories and wallets. Unknown or externally edited procedures refuse.
World does not need a restart; the corresponding client Mod must also be updated.

Choose category, rank and payment, enter part of a Chinese name or an English/model name, then click the
native magnifier. Native dropdown selection alone does not submit. Pagination retains conditions.
The product list and its sales-option list share the same ordered search result, including pagination,
rank and payment filters. This keeps icons, prices and buy/gift previews available for old products in global
and Chinese searches. Both procedures are inspected, backed up and updated in one transaction; an external
edit to either procedure refuses the update.
Chinese/mixed queries support 36 UTF-16 units without truncation; ordinary English/Thai queries retain their
native encoding. Until a server is updated, disable Chinese search and use its original English query.
Refresh the index after adding or renaming offers. Exact product/name aliases handle legacy billing-name
differences, including rental prefixes and model spelling; English/model queries also match displayed aliases.

New offers require Chinese names and sales labels. Existing owner state remains readable and intact.
Known descriptions are translated while preserving numeric terms and model codes; unknown future text
stays original. See the [Chinese guide](shop-localization.zh-CN.md) and
[engineering reference](investigations/shop-search-localization.md).
