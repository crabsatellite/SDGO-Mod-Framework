# Shared unit-preview title omission, 0.1.1

The user's screenshots showed English upper-left titles in shop blueprint and
ordinary capsule previews. Both strings already existed in the shipped 12757-entry
dictionary; this was a missing display caller, not a missing translation.

Supported artifact: `GOnline-54BE1568-00C97000`, SHA-256
`e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92`.

Static disassembly identifies `0x006EF390` as the shared preview-name leaf. It
reads the unit-record name at `+0x69`, assigns a native wide string at call site
`0x006EF404` (bytes `E8 E7 32 D1 FF`, return `0x006EF409`), then passes that string
to the `UnitNameStatic` control. The four direct upstream preview callers are
documented in [the site inventory](shop-localization-sites.json). The title is also
shared by factory-room and mixing previews, so that scope is explicit in Manager.

Only this audited assign caller is added. No new hook or global text replacement
is introduced. Existing `TranslateNames` controls the preview titles, unknown
strings remain original, and source unit records remain unchanged.

Automatic verification:

- Seven actual-DLL cases pass. A separate generated preview caller proves the
  new whitelist entry is needed, exercises both native Build Strike spellings and
  Jegan with the production dictionary, and checks unknown-title passthrough,
  outside-caller passthrough and the existing name switch.
- Dictionary, Manager overlay and player-package tests pass (16 Python tests).
- All 69 symbol definitions match the current executable, including the new site.
- Manager standard import into the stopped primary client reports 0.1.1 enabled
  and registered; every installed Mod file hash matches the standalone package.
- Existing 193 symbol sections preserve their values; only the new title caller
  is added. Owner switches, dictionary, custom display data, other Mods, framework
  and capsule resources retain their pre-import values/hashes. Manager reads back
  the updated scope and name-switch label.

These tests execute the real Mod DLL using native-ABI fixture objects. They do not
execute the game's preview control or prove rendered Chinese glyphs. Local
installation awaits a normal client start; screenshot acceptance remains manual.
