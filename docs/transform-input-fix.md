# Apsalus III transformation input recovery

`transform_input_fix` is an automatic client fix, listed in Mod Manager's
automatic-fix archive. Restart the client after installation. There are no
player settings or database changes.

When Apsalus III is owned as form `24788`, its first transformation swaps to
model `1957`, which has no `change_ma` action. The reciprocal request also
encounters its missing `change_ms`. The stock animation wrapper returns handle
zero but can retain the preceding animation's raw slots. Nonempty events for
that old handle prevent `TransformUpdate` from taking its empty-event fallback
or matching the requested handle, so movement and attack stay locked.

The Mod recognizes only the `24787`/`24788` pair, active transformation mode 3,
a null requested animation handle, a nonempty event list and a latched target
in that pair. It queues the same native WAIT signal and state selection used
by the existing empty-event path. Native `TransformRelease` retains ownership
of the pending form commit, mode reset, weapon changes and cleanup. Resolved
animations and other units keep their original update path.

The original missing animation remains absent; this repair restores the exit
from that state using the existing native fallback semantics.

## Source and validation

Supported host: `GOnline-54BE1568-00C97000`, executable SHA-256
`e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92`.

- `0x006DBB90`: selects target `change_ma` after the early swap, or current
  `change_ms` before the delayed swap.
- `0x00C571F0`: a missing action returns zero while previous raw playback can
  survive.
- `0x006D9AD0`: completion requires the matching handle and terminal event;
  the empty-event path signals WAIT through `0x00BE3920` and virtual `+0xD8`.
- `0x006D8E40`: native release commits an uncommitted target and clears mode.

The actual production DLL test exercises its Win32 ABI, recovery ordering,
both pair orientations, exclusion conditions and byte preservation. A bounded
native-function replay executes 740 observations with the shipping DLL: the
96 retained-slot null-handle consumers that previously failed to request WAIT
all recover, while the other observations retain their results. Ten original
bodies/tables remain unchanged. These checks are not whole-client gameplay.

**Delivery gate remains open:** complete background native combat acceptance
is still pending. Direct loopback testing has already logged in and read the
real inventory without firewall changes. The earlier access-denied error was
specific to remote firewall-rule setup. The managed test environment now uses
private SQL and native service copies, with five preinitialized fixture accounts
and mandatory cleanup. No deployment or master push is claimed yet.

```powershell
cmake --build build --config Release --target transform_input_fix_runtime_test
ctest --test-dir build -C Release --output-on-failure -R '^transform_input_fix_actual_dll$'
```
