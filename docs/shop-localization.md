# Shop localization and Chinese search (Beta)

`shop_localization` translates native shop text, shared unit-preview titles, unit names in hangar exchange
previews and fixed messages from cooperating Mods. All five scopes default to enabled. Manage them in SDGO Mod Manager and restart
the client after changing them. Missing or disabled localization preserves original consumer text.
Player-authored room titles, drafts and chat are never translated.

Clients update through the existing `update.bat` Git runtime. Owners configure the Shop panel's existing
workspace/World/client paths and click **Update Chinese search**. This independent action publishes only
owned search aliases and a backed-up, guarded search procedure; it does not publish a catalog draft,
change products/prices, or touch inventories and wallets. Unknown or externally edited procedures refuse.
World does not need a restart; the corresponding client Mod must also be updated.

Choose category, rank and payment, enter part of a Chinese name or an English/model name, then click the
native magnifier. Native dropdown selection alone does not submit. Pagination retains conditions.
The paired server procedure applies rank/payment in global and hot searches as well as ordinary categories.
Chinese/mixed queries support 36 UTF-16 units without truncation; ordinary English/Thai queries retain their
native encoding. Until a server is updated, disable Chinese search and use its original English query.
Refresh the index after adding or renaming offers. Exact product/name aliases handle legacy billing-name
differences, including rental prefixes and model spelling; English/model queries also match displayed aliases.

New offers require Chinese names and sales labels. Existing owner state remains readable and intact.
Known descriptions are translated while preserving numeric terms and model codes; unknown future text
stays original. See the [Chinese guide](shop-localization.zh-CN.md) and
[engineering reference](investigations/shop-search-localization.md).
