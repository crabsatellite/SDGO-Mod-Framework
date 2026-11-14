# Shop localization and Chinese search

[简体中文](shop-localization.zh-CN.md) · [Back to the feature index](../README.md)

Translates shop product names, descriptions and sale options, and the unit-preview titles on shop and capsule pages.
Hangar batch-exchange lists and fixed messages from other Mods use the same Chinese names. The shop can also be
searched by Chinese name, combined with rank and payment filters.

Paint-page unit previews also use Chinese names, including training units. Descriptions retain paragraph breaks;
battleship descriptions and usage restrictions appear in their separate fields. The buy/gift option dialogs and
the following confirmation pages show translated product titles; numeric purchase values stay native.

Product-detail and hover hooks preserve the native panel owner across translation. This prevents a crash when the shop opens a detail panel while custom product display records are enabled.

Prices, currencies, durations and billing keep their original definitions. Rental/permanent labels are handled by
the separate [shop rental and Chinese option fix](shop-labels.md).

Adds original-client unit-name aliases. Verified hangar, unit-information and preview callers use the unit ID and source text. Existing Chinese is retained; an ambiguous foreign label can use the same original record's Chinese primary name. Custom unit, weapon and skill names and descriptions are authored and distributed with custom-unit content packages, outside this Mod's stock dictionary. Content-owned records and matching text bypass translation; unresolved ownership preserves the source.

Original Thai controls and longer system messages use [Chinese native interface text](ui-localization.md).

## Use

`shop_localization` is a client Mod that players receive with the server owner's client package or update;
restart the game after updating. **商城中文汉化** in SDGO Mod Manager has five switches, all on by default; restart
the game after changing them:

| switch | effect |
|---|---|
| 汉化商品与机体名称 (names) | product names, unit-preview titles and unit names in hangar exchange lists |
| 汉化商品说明 (descriptions) | product descriptions |
| 汉化商城选项 (options) | sale options in the buy and gift windows |
| 汉化本地 Mod 界面提示 (Mod interface) | fixed messages and captions from hangar manager, extended room titles and other Mods |
| 中文关键词搜索 (Chinese search, needs the server update) | Chinese input in the shop search box, see below |

A disabled switch, or a client without this Mod, shows the original text; text not yet in the dictionary also stays
original. Hangar exchange lists only change displayed names; slots, ranks, counts, rewards and confirmations are
unchanged, and real model codes such as FAZZ and GN-XIII are kept. Player-written room titles, drafts and chat are
never translated.

## Chinese search and filters

### Owners: update the server search

Chinese search needs a matching server search. In Manager's **商城商品** (Shop) panel, configure the workspace,
WorldServer and client, then click **更新中文检索** (Update Chinese search):

- It is a separate action: it does not publish catalog drafts or change products, prices, wallets, inventories or delivery.
- It updates the product-list and sale-option-list search procedures plus the Chinese aliases, backing up the originals
  first. If either procedure is not the known original or was changed by another tool, the update refuses.
- WorldServer does not need a restart; players need the matching client Mod.
- Run it again after adding or renaming products.

Until the server is updated, turn off **中文关键词搜索** in Manager to keep the original English search.

### Players: search the shop

1. Choose the category, rank and payment type.
2. Type part of a Chinese name (for example 高达 or 强袭), an English name or a model code.
3. Click the **magnifier** on the right. Changing a dropdown alone does not search.

**All** means no restriction for that field. Name, rank and payment can be combined, and paging keeps the conditions.
Chinese or mixed keywords can be up to 36 UTF-16 units; a longer keyword shows a notice and stays in the box instead
of being cut off.

## Chinese names for new products

When adding products in Manager, names and sale options must contain Chinese; model codes may stay. Editing, import
and publishing all check this. Existing workspaces are not reset: fill in missing Chinese names before publishing.
Chinese product names the owner already entered take precedence.

## Logs

| log line | meaning |
|---|---|
| `Chinese shop localization ready: <n> dictionary entries, <m> verified shop callers ...` | localization loaded |
| `Chinese shop search bridge ready ...` | Chinese search loaded |
| `shop search rejected overlong query ...` | keyword over 36 UTF-16 units was not sent (the keyword is not logged) |
| `localization caller is not verified: <name>` / `shop localization refused: ...` | unsupported client; the Mod did not load and the shop shows original text |

## Developer notes

The native search entry converts the keyword with code page 874, which destroys Chinese before sending. At the two
verified shop keyword call sites only, the Mod encodes Chinese or mixed keywords as the reserved ASCII prefix `~CN:`
plus UTF-16LE Base64 (at most 36 UTF-16 units / 100 bytes). English and Thai keywords and other pages keep their
native encoding.

The native search procedure ignores rank in the global category and every optional filter in the hot category. The
procedure Manager installs decodes Chinese keywords, matches Unicode substrings against published names and
product-bound aliases, applies rank, subfilter and payment consistently in every category, and computes the page and
total from the same set. Native category and hot membership, event exclusion, the 10 output columns and sort order are
kept. The product list and its sale-option list share that ordered result, so old products keep icons, prices and
buy/gift previews in global and Chinese searches. Both procedures are inspected, backed up and updated in one
transaction.

Billing names can differ from client display names through rental prefixes, whitespace, legacy spelling or model-code
formatting. Dictionary entries of the form `@search:ProductID:Original` bind them exactly by product ID and original
name without replacing native display records or product identities; English and model queries also match these
aliases.

Other Mods query unit names, item names and fixed UI text through the optional `SdgoLocalizationLookupV1` C ABI and
keep their source text when this Mod is missing or disabled. Existing English, Thai and Korean descriptions are
translated entry by entry, preserving numbers, percentages, durations and model codes; corrupt, truncated or uncertain
source names are marked as such.

Addresses, packets and interfaces: [search and localization reference](investigations/shop-search-localization.md),
[unit-preview title notes](investigations/shop-preview-title-fix.md).
