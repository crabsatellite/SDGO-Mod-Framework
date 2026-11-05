# AI room loading timeouts

[简体中文](loading-sync-fix.zh-CN.md) · [Back to the feature index](../README.md)

`loading_sync_fix` prevents the battle server from treating framework AI slots as human clients
that failed to finish loading. It is an automatic server fix, enabled by default and listed in
the SDGO Mod Manager fix archive.

## Install

Install the server part of the framework package on every BattleServer that hosts these rooms,
then restart the affected channels. No additional client files, room command or setting is required.
The supported native host is `BattleServer-548A891D-0046C000`; unsupported signatures refuse to load.

## Behavior

The original server uses human counts to decide when loading is complete. Its timeout scans also
visit AI slots, which do not send human loading acknowledgements. Passing one of those slots to
the normal player-removal function incorrectly reduces the human count and can break later
loading checks.

The fix excludes framework AI slots from the initial loading scan, progress retries, timeout
selection, resource loading and final start confirmation. If a human host leaves, the native
replacement scan selects another human. Human acknowledgements, native retries and the removal of
an actual timed-out human continue through the original code. The server's existing host rules,
loading deadlines and start notifications are retained.

This covers the AI-slot timeout defect in PvP and mission rooms using the framework AI identity
convention. Ordinary rooms keep their native behavior. Network disconnections, missing resources
and unrelated client work-queue failures can also prevent loading and need separate diagnosis.

## Troubleshooting

Check the affected BattleServer's `sdgo/logs/` to confirm that `loading_sync_fix` loaded. Every
channel accepting these rooms needs the fix; updating only some channels can produce intermittent
results. The fix does not require a change to the original server executable or player data.
