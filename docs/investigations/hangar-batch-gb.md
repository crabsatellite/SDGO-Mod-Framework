# GB batch exchange and account-specific login overflow (0.4.0)

0.3.0 still requested fresh details and waited for each native dismantle reply.
Per-unit round trips make GB previews and batch execution slow for large selections.
0.4.0 previews from the native summary, retaining identity, type, slot, protection
and KeepCopies rules. Two confirmations precede any network submission. A small
bounded set of native requests transfers eight selected records per fragment;
four messages are submitted per UI pump. There is no per-unit detail request or
per-unit network acknowledgement wait.

The same authenticated server adapter stages the header and exact selection.
Fragments, header and explicit commit may arrive out of order. Nothing executes
until the complete indexed selection and commit are both present. Conflicting
fragments reject the batch. The server locks the owner, validates the current
records and reward, calls its original sp_DeleteUserUnit for each selected unit,
checks unselected records, credits the native GB wallet and optionally sorts.
All business writes share one transaction; retries are idempotent. The native
mail transport still has no proven completion acknowledgement, so the UI says
submitted and requires relogging. Server job state is independent commit evidence.

The isolated SQL fixture covers 498 selected units, incomplete/cancelled staging,
duplicate and out-of-order requests, ownership, stale slots/rewards, protections,
KeepCopies, wallet overflow and a native failure after an earlier deletion. It
checks complete rollback of inventory, recovery logging and GB. Native producer
replay checks the full 345-byte messages against the shipped client constructor.
No real inventory is exchanged by these tests.

## Native slot capacity

The supported image at `006178DE` reads the expanded slot count and adds `0x18`
before calling `CreateGroup`. `InsertOrUpdateSlot` does not bound the returned
slot, so imported inventories that exceed the allocated capacity can crash at login.
Capacity must cover every stored slot even when unit IDs and references are valid.

The server sort procedure now keeps capacity sufficient for its dense result,
using the minimum native six-slot blocks and preserving higher existing capacity.
The fixture also reproduces an imported 952-unit inventory and verifies 954-slot
capacity after sorting.
