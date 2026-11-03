# Shared unit-preview title

The upper-left unit titles in shop blueprint and ordinary capsule previews are drawn through a shared preview-name
leaf that the localization dictionary was not hooked into; the strings themselves were already translated.

Supported artifact: `GOnline-54BE1568-00C97000`, SHA-256
`e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92`.

Static disassembly identifies `0x006EF390` as the shared preview-name leaf. It reads the unit-record name at `+0x69`,
assigns a native wide string at call site `0x006EF404` (bytes `E8 E7 32 D1 FF`, return `0x006EF409`), then passes
that string to the `UnitNameStatic` control. The four direct upstream preview callers are listed in
[the site inventory](shop-localization-sites.json). Factory-room and mixing previews share the same title, so the
Manager description names that scope explicitly.

`shop_localization` adds only this audited assign caller to its whitelist; no new hook or global text replacement is
introduced. The existing `TranslateNames` switch controls preview titles, unknown strings stay original, and source
unit records are unchanged.

The actual-DLL tests drive a generated preview caller with both native Build Strike spellings and Jegan through the
production dictionary, and check unknown-title passthrough, outside-caller passthrough and the name switch.
