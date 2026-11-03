# Native room-style preview development fixture

This directory preserves the source and offline checks for an isolated native
2D room screenshot preview. It is a development fixture, not a published Mod
or a room-style feature.

The code can redirect one strictly verified native APP18 transition to an
existing 2D client scene:

- ordinary Battle-domain room: APP7 with source tag 6;
- cooperative Mission room: APP16 with source tag 7.

The adapter preserves the original 24-byte transition payload except for the
source tag, requires the exact shipped caller and factory, checks the current
APP and native profile context, and requires equality with the authoritative
server-acknowledged room global used by that caller. Unknown or mismatched
transitions continue through the original APP18 path.

This fixture does **not** define or synchronize an authoritative `RoomStyle`.
It has no style packet, room-key binding, host-transfer rule, joiner update,
server persistence, or player-preference persistence. A screenshot from this
fixture cannot establish multiplayer style agreement.

## Acceptance boundary

The cooperative Mission preview produced a native screenshot with the
six-player resource loaded.

The ordinary PvP preview reached the native APP7 room and produced a native
frame, but the top title/settings/exit information bar was absent and only
eight slot outlines were unambiguously visible. Twelve-slot visibility and a
complete playable PvP room layout remain unaccepted.

These findings are intentionally stated without run identifiers or machine
paths so the fixture remains reusable.

## Source-only boundary

No original or generated `uir`, `zdx`, `zpk`, control dump, screenshot, client
binary, or game archive is stored here. The generators require a separately
available sealed client archive with the pinned source hashes. Generated files
must remain local test artifacts and must not be added to this fixture.

## Resource compatibility mechanism

The shipped ordinary-room resource has eight authored player-control families,
while the matching executable addresses twelve. `generate_room_ui.py` preserves
every original record, clones complete controls for slots 8 through 11, and
rebases every absolute record-end offset.

`Multi3DRender4` contains a serialized array whose count is stored at offset
`0xA4`; full entries begin at `0xA8` and are 48 bytes each. The source contains
16 identical entries, while the native twelve-slot consumer reaches entries
12 through 23. The generator therefore:

1. requires the exact 16-entry shape and verifies that all original entries
   are identical;
2. preserves all 16 original entries byte for byte;
3. changes the serialized count from 16 to 24;
4. appends eight complete 48-byte clones of entry 0.

The shipped Mission resource has four authored player families, while its
native consumer binds six. `generate_mission_ui.py` preserves the authored
records, clones complete controls for slots 4 and 5, and rebases absolute
record ends.

`Multi3DRender77` uses the same count and entry layout. The generator:

1. requires the exact four-entry serialized shape;
2. preserves all four original entries byte for byte;
3. changes the count from 4 to 6;
4. appends complete 48-byte clones of entries 1 and 2 for the two added slots.

Both repairs extend complete serialized arrays. They do not hide slots, alias
control pointers, or substitute partial records.

## Build

Configure a 32-bit Visual Studio build from this directory:

```powershell
cmake -S . -B out -G "Visual Studio 17 2022" -A Win32
cmake --build out --config Release
ctest --test-dir out -C Release --output-on-failure
```

The default framework include path resolves from this tracked fixture to the
repository `include` directory. For a standalone copy, provide it explicitly:

```powershell
cmake -S . -B out -G "Visual Studio 17 2022" -A Win32 `
  -DSDGO_FRAMEWORK_INCLUDE_DIR=<framework-include-directory>
```

The offline executable tests transition planning and payload preservation. It
does not start SDGO.

## Generate local overlays

Run either generator from this directory with a sealed client root:

```powershell
python generate_room_ui.py `
  --client <sealed-client-root> `
  --output generated-12slot

python generate_mission_ui.py `
  --client <sealed-client-root> `
  --output generated-mission
```

Each output must be a direct child of this fixture directory. The scripts fail
closed on source archive hashes, resource identity, serialized record shape,
duplicate controls, chain metadata, and absolute record boundaries.

For an isolated screenshot run, copy the built DLL plus `mod.ini`,
`manager.json`, and the locally generated overlay into an owned test client.
Merge `symbols.fragment.ini` only into that isolated client. `Preference=3d`
or `enabled=0` retains the native APP18 path and does not install the resource
hook.

The framework permits one owner per hook site. `capsule_catalog` uses the same
pack-open hook, so a preview run must explicitly disable it. The fixture never
chains or silently replaces that hook.
