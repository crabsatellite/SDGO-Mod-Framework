# Hangar manager

Sorting and the native per-unit item exchange use the established workflows below.
The newer server GB batch flow remains in Beta and is assessed separately.

[简体中文](hangar-manager.zh-CN.md)

The client mod `hangar_manager` adds **Sort** and **Exchange duplicates** buttons next to the lock and
dismantle buttons on the game's unit-management page. Everything happens in that native page; there is
no separate window, overlay or hotkey, and the Manager does not need to stay open. Sorting and GB batch
exchange run on the server, so the owner installs the server adapter first.

## Sort

The server orders all units by grade (S/A/B/C), sub-level, unit number, acquisition time and instance
ID, changing only slot numbers. Locks, sortie units, decks, training and other unit data are kept.
Sorting also raises the registered capacity in six-slot blocks to fit the inventory, up to 1200
(an imported inventory larger than `24 + ExpandSlotCount` can crash the native login slot writer).
After submitting, **log in again** to see the result.

## Exchange duplicates

- Copies are counted by unit number; at least two of each are kept by default (`KeepCopies`, 1–20),
  and protected copies count toward that. Paint variants count as the same unit; lock any copy you
  want to keep.
- Locked, sortie, deck, rental and trained units are never exchanged; unknown units are skipped.
- Choose native GB or the native item reward (usually experience capsules). The server's
  `tbl_UnitGradeInfo` and native dismantling decide the actual reward.
- Preview and the first confirmation delete nothing; two confirmations are required.

**GB batch exchange (Beta)** builds the list from the native inventory summary. After confirmation the exact
instances, unit numbers and slots go to the server, which rechecks ownership, protection, KeepCopies and
rewards, dismantles natively and credits GB in one transaction. Incomplete or stale lists and any failure
change nothing; repeated submissions do not pay twice. Log in again afterwards.

**Item exchange** keeps the native per-unit flow and stops, without retrying, when a reply cannot be
confirmed, the connection changes or the hangar is modified elsewhere. Confirmed steps are not undone.

## Install

**Server:** in SDGO Mod Manager open **机库整理与批量回收服务端** (hangar server), choose the server INI
containing `DB Game` and `DB Member` (same SQL instance) and install or update. Normal native mail keeps
working; the panel can check the installed procedure hashes and restore the original procedure. No game
server restart is needed.

**Client:** friend-server players update with `update.bat`. The Full framework package includes this mod
(Base does not). Alternatively import the mod's overlay with **导入客户端配置包** in the Manager, or close
the client and run `install_hangar.ps1 -GameDir "D:\Games\SDGO"` from the standalone package
(`-RefreshRules` refreshes the reward preview rules). Supported client: `GOnline-54BE1568-00C97000`.

## Settings

Under **机库整理与回收** in the Manager: **至少保留机体份数** (`KeepCopies`, default 2) and **回收后整理机库**
(`SortAfterExchange`, default 1). `RequestIntervalMs`, `ReplyTimeoutMs`, `MaxSlots` and `RulesFile` are
developer settings. The rules file only affects the preview; it cannot raise server rewards. Owners can
export it read-only with `python export_hangar_rules.py --server-ini "D:\Server\StoreServer.ini" --output units.tsv`
(needs `pyodbc`; `--snapshot` reads table JSON instead).

## Developer notes

- Sort and GB batch requests use the native mail send `0x0891` (345 bytes); the server derives the owner
  from its own session. Native mail has no completion acknowledgement, so the client only reports
  "submitted"; the server writes a separate receipt that the Manager reads back.
- The sort transaction changes only `tbl_UserUnits.UserUnitSlotNumber` and verifies all other columns.
- Item exchange uses native dismantle `0x06D2` (19 bytes; reward 1 = GB, 2 = item) and detail `0x078B`
  (14 bytes), always addressed by the stable `UserUnitNumber`.

Build and test:

```powershell
cmake -S . -B build/hangar -G "Visual Studio 17 2022" -A Win32
cmake --build build/hangar --config Release
ctest --test-dir build/hangar -C Release --output-on-failure
```

Native mouse replay needs `-DSDGO_HANGAR_NATIVE_HOST="D:/Games/SDGO/GOnline.exe"` and the
`hangar_native_input_replay` test.
