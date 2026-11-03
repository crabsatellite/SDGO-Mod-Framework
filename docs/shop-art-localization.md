# Chinese shop art labels

[简体中文](shop-art-localization.zh-CN.md) · [Back to the feature index](../README.md)

`shop_art_localization` translates text that is baked into verified shop
artwork and therefore cannot be handled by the normal shop text dictionary.

## Use

Install it through the server owner's client package or update. **商城图片文字汉化** in SDGO Mod Manager
can be enabled or disabled independently; it is on by default. Restart the client after changing it.
Ordinary names and descriptions use [Chinese shop localization](shop-localization.md).

## Coverage

The supported artwork is the verified grey paint can. Its original can, chevron and
black label strip remain in place; the Thai rank text is covered and the native
shop row displays **上士及以上** ("Staff Sergeant or above"). Prices, currencies,
purchase requirements, product records and server checks are unchanged.
Unknown artwork is left untouched.

If the label is absent, confirm that the Mod is enabled, restart the client and
check that the product still uses the supported artwork. A client build with a
different native signature disables the Mod instead of applying an unverified
overlay.

## Developer notes

The exact mapping is `sprs/17825.spr`. The overlay uses children owned by the original shop dialog. They accept no
input and follow the visibility of the corresponding `Goods3DRender` row, so
page changes, category changes, searches, empty rows and reopening the shop use
the normal native lifecycle. No replacement SPR, TXR or other game asset is distributed.

Rows `0` through `7` use
`ModShopArtBackground{row}` and `ModShopArtText{row}`. Both children have input
disabled. The text child contains exactly `上士及以上`; both are visible only
while the matching verified artwork row is visible.
