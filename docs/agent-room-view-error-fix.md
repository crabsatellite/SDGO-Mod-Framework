# Agent room-view error forwarding fix

The original AgentServer routes native room-view error reply `0x0880` to the
correct authenticated client, but forwards only the first four bytes of the
eight-byte BattleServer reply. The missing error field makes the client read
unrelated memory and can produce a misleading error dialog during room
operations.

This automatic server fix preserves the complete eight-byte reply at that one
verified Agent forwarding call. It does not change the destination player,
native error code, send flags, room logic, or any other packet. The Mod is
enabled by default and has no settings.

Install the Mod on every AgentServer that serves players, then restart those
AgentServer processes. Clients do not need a matching client-side component.
If a native room operation really fails, the client still receives and shows
the original native error.
