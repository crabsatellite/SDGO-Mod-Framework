# Friend unit gifting server

This Agent + World Mod provides transactional gifting of an exact unit already
owned by the sender. The Agent role validates the native authenticated account
before forwarding an extension frame. The World role owns all policy and
database decisions. The public default is disabled. Server owners can enable it
in `mod.ini`.

World accepts the extension only from a route that the original `sip.txt`
handshake has registered as an `AGENT` peer. The first authenticated capability
probe validates the borrowed native Member/Game sessions with read-only
queries. Opening or installing the Mod-owned policy is deferred until the
following expanded probe.

An offer requires mutual accepted friendship and the configured sender
progress thresholds. Active, assist, locked, saved-ship, temporary, expired,
run-out, and forbidden units cannot be offered. These ownership and safety
rules are fixed and cannot be relaxed by configuration.

The Mod moves the full native 50-column unit row into its own SQL escrow. An
accepted or returned unit keeps its original UID, acquisition date, combat
statistics, levels, colors, decals, skills, parts, class, and composition
fields. Only the owner account and inventory slot change. Sender name, offer
time, and the optional note remain available as provenance.

The recipient may accept or return a pending offer even if the friendship later
ends. If the sender has no free slot during a return, the exact payload remains
in escrow in `ReturningWaitingCapacity`; the sender can claim it after making
space. Disabling the feature blocks new offers only and leaves these resolution
operations available.

The Mod owns `sdgo_friend_unit_gift` and does not replace original procedures,
triggers, or executable files. A disabled fresh installation does not create
the schema. Installation refuses an unexpected native table shape, foreign
objects in the owned schema, or edited stored procedure definitions.

Mutation request IDs are idempotency keys. If execution may have committed but
the result row or reply cannot be read, the server reports an uncertain outcome
and supports a read-only receipt lookup with the original request ID. Clients
must reconcile that receipt and never retry the mutation with a new ID.
