# Complete PvP All Map

Expand the room's **All Map / Random Map** choice to the complete PvP map
catalog. Mission-only maps remain available only in mission modes.

[简体中文](all-maps.zh-CN.md) · [Feature index](../README.md#gameplay-extensions)

| Mod | Install on | Purpose |
|---|---|---|
| `all_maps` | Every serving BattleServer | Select and announce the authoritative map from the complete PvP pool |

No client Mod is required. The room receives the selected map through the
game's existing native start and announcement flow.

## Use

1. Stop the BattleServer instances being updated.
2. In **SDGO Mod Manager**, enable **Complete PvP random map pool (server)** for
   every BattleServer that serves these rooms.
3. Save the changes and restart the affected BattleServer instances.
4. Create a PvP room and choose **All Map / Random Map** in the native map
   selector.

There are no user settings. Disabling the Mod and restarting restores the
native pool behavior on that BattleServer.

## Pool rules

The Mod reads the map catalog already loaded by the game. A map is included
once when it is marked for at least one native PvP family:

- Normal
- Death Match
- Grid
- Tag
- Leader

The union is shared by supported PvP room modes. The room's battle mode and
rules remain unchanged; only its All Map selection pool is expanded.

The following choices keep their native behavior:

- a specific fixed map
- Hot Map
- Fast Map
- a custom random set
- mission and special AI modes

Catalog row zero is the native random-map marker and is never treated as a
playable map. Mission-only, training-only and archive-only terrain resources
are not added to PvP.

## Compatibility and troubleshooting

The Mod supports `BattleServer-548A891D-0046C000`. A different executable or
an unverified required symbol keeps it disabled. The server writes
`All Map mode=` when it performs an authoritative draw.

| Symptom | Check |
|---|---|
| All Map still uses the old small pool | Enable `all_maps` on the serving BattleServer, restart it, and check its log |
| Different rooms use different pool behavior | Confirm `all_maps` is enabled on every BattleServer that can serve those rooms |
| Room start is rejected | Check the server log for a catalog or random-source error; the Mod refuses the draw instead of silently selecting catalog row zero |
| Fixed, Hot, Fast or custom random behavior did not change | These selections intentionally keep their native rules |

## Developer notes

The server reads its native linked MapFile table and uses rejection-sampled
system randomness for an unbiased draw. It hooks the authoritative room start
choice; the client continues to receive the selected map through the native
flow. The Mod does not rewrite catalog flags or map resources.

The focused policy test is:

```powershell
ctest --test-dir build/native -C Release -R all_maps_offline --output-on-failure
```
