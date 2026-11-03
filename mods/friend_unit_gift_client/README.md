# Friend owned-unit gift client

This client Mod owns every player-visible part of friend unit gifts:

- a capability-gated entry in the native friend context menu;
- native unit management and its double-click event for owned-instance selection;
- the native `1238` unit-acquisition card for review, an optional Unicode note
  and explicit confirmation;
- the original Storage unit-entry widgets, with notes shown in unit information;
- received/returned pagination with accept, return, take-back and provenance
  actions;
- packet correlation, timeout handling and same-request receipt lookup when a
  mutating result is uncertain.

The Mod sends only through the authenticated native session. It does not patch
`GOnline.exe`, write the player's database, fabricate native 402-byte gift
rows, or depend on GameTest files. The server remains authoritative for
friendship, ownership, eligibility, capacity, escrow and commit.

`storage_claim` is optional. When it is active, this Mod registers with its
single storage hook broker. Otherwise this Mod installs the same guarded
storage listener itself. Enabling this feature therefore never enables the
unrelated Claim All action.

The public server may keep new offers disabled. Existing gifts can still be
accepted, returned or taken back, and provenance remains readable when the
server advertises those capabilities.

All shipped behavior is implemented by the client and server Mods. GameTest
can invoke the same native controls through a run-scoped observer, but the
observer adds no alternate transfer, database or UI path.
