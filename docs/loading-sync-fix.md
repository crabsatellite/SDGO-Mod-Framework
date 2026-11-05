# Multiplayer loading synchronization

[简体中文](loading-sync-fix.zh-CN.md) · [Back to the feature index](../README.md)

`loading_sync_fix` preserves completed loading when another player is late, and prevents framework
AI slots from being treated as timed-out human clients. It is an automatic server fix, enabled by
default and listed in the SDGO Mod Manager fix archive.

## Install

Install the server part of the framework package on every BattleServer that hosts these rooms,
then restart the affected channels. No additional client files, room command or setting is required.
The supported native host is `BattleServer-548A891D-0046C000`; unsupported signatures refuse to load.

## Behavior

The server may already have begun resource loading when a slower player's earlier readiness
acknowledgement arrives. Reinitializing the phase at that point clears completion flags already
acknowledged to faster players; those clients do not send the completion again. The fix initializes
this phase only once. Late readiness still receives the original notification, while existing
completion flags, the current phase and its deadlines remain intact. This applies without AI too.

The original server uses human counts to decide when loading is complete. Its timeout scans also
visit AI slots, which do not send human loading acknowledgements. Passing one of those slots to
the normal player-removal function incorrectly reduces the human count and can break later
loading checks.

The fix excludes framework AI slots from the initial loading scan, progress retries, timeout
selection, resource loading and final start confirmation. If a human host leaves, the native
replacement scan selects another human. Human acknowledgements, native retries and the removal of
an actual timed-out human continue through the original code. The server's existing host rules,
loading deadlines and start notifications are retained.

The AI-specific protection covers PvP and mission rooms using the framework AI identity
convention. On-time human acknowledgements keep their native behavior. Network disconnections, missing resources
and unrelated client work-queue failures can also prevent loading and need separate diagnosis.

## Troubleshooting

Check the affected BattleServer's `sdgo/logs/` to confirm that `loading_sync_fix` loaded. Every
channel accepting these rooms needs the fix; updating only some channels can produce intermittent
results. The fix does not require a change to the original server executable or player data.
