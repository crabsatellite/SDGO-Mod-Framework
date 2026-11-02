# Sorting left a blank grid and misplaced preview

In version 0.2.3, sorting could receive all native slot updates while leaving
the unit grid blank and placing preview text behind the unit-detail panel.
Slot acknowledgements alone do not establish database persistence or refresh
other lobby views.

Two production-code errors explain the display:

1. The sort handler called `PageUi::Open`, which opens duplicate exchange and
   hides every original child. Completion kept that empty preview open. Native
   refresh was immediately followed by hiding the newly refreshed children.
2. Preview content used x=12 in a full-screen dialog, treating 1330.uir as if
   it were only the right-hand grid. Its actual dimensions are 1024x768;
   UnitLockBtn is (418,33,127,18), UnitDeleteBtn is (551,33,127,18),
   UnitIcon0 is (449,77,76,76). These scalar layout facts were read from the
   supported GPackData resource, SHA-256 of 1330.uir:
   `587c31bcda312a4c864bc7d5a8473e6f9c68186dfbace101b013d4b74ebf36bd`.
   No original resource bytes are committed or distributed.

The retained decompilation and current image signatures identify the original
page-change refresh at 00610120: SetViewList (00603FF0), SetUnitFrame_All
(00610040), then SetPageFrame_All (005EF150). The previous explicit refresh
omitted the final visibility pass. 0.2.4 calls all three in this order.
0.2.4 sorts without entering exchange, keeps native cells visible, changes the
entry to a stop action while busy, and puts status below the grid. Completion
or cancellation restores native children before refreshing. Exchange content
anchors to the native lock-button x coordinate and is bounded to the right-hand
region. New tests use the actual full-screen geometry, test the production sort
handler and acknowledgements through completion, assert list/icon/page ordering,
test zero-action and stopped plans, and preserve duplicate confirmation checks.

Other lobby/room views have independent lifecycles. Log in again to synchronize other views after a server-side sort. Automated tests never sort or exchange real units.
