# Automatic native hangar capacity (Beta)

[简体中文](hangar-capacity.zh-CN.md)

`hangar_capacity` is a WorldServer Mod that ensures at least **1200 total hangar
slots** before the native inventory-capacity query completes. New and existing
accounts use the same policy. The ordinary client initializes its own native
pages; no client Mod, launcher, new port or FRP configuration is needed.

Install a build containing `hangar_capacity` with the normal server installer:

```powershell
powershell -ExecutionPolicy Bypass -File install_server.ps1 -ServerRoot "D:\Server\WorldServer" -Role World
```

Install into a stopped instance. The feature defaults to **off**: turn on
**自动机库容量** for that WorldServer in SDGO Mod Manager, then start it normally.
The two timeouts are developer settings shown after **显示开发信息**.
The host's own `<exe-name>.ini` must contain `[DB Game] DBName`; the Mod borrows
the already authenticated native SQL session and does not copy credentials.
SQL table read/update and slot-pool insert rights are required.

The first inventory load for each account performs a bounded, transactional
policy: validate the database/schema/getter signature, fill missing slot numbers
1..1200, and raise that account's `ExpandSlotCount` to 1176 if lower. Native total
capacity is **24 + ExpandSlotCount**. Larger valid saved values are retained;
this feature creates no capacity above 1200. It creates no owned-unit rows.
Native SQL procedures, inventory, wallets and active-unit selections are not rewritten.

Failures roll back the policy and use the native database-failure reply rather
than publishing a partially prepared inventory. The same account can retry.
Transaction/implicit-transaction sessions and incompatible database layouts are
rejected. Error logs omit account identities, SQL text and credentials.
It also rejects enabled DML triggers on the two capacity tables, and
identity/computed slot or expansion columns, so an unknown database addon cannot
silently change unrelated data through this policy.

| Setting | Default | Meaning |
|---|---:|---|
| `LockTimeoutMs` | 1000 | SQL policy lock wait, 0..5000 ms |
| `QueryTimeoutSeconds` | 3 | Per-command timeout, 1..10 seconds; must exceed lock wait |

Capacity is applied before the client's initial inventory allocation. An
already initialized client sees the new policy on its next normal login;
a live inventory is not resized. Disabling/uninstalling stops
future policy calls. Added slots and acquired units remain saved, with no
automatic shrinking or destructive rollback.

The unique native count adapter is separate from the rank and starter-profile
hook sites. Hangar Manager's native 1200-slot preference remains applicable.
Custom capacities above 1200 and online growth are not supported.

Developers can run the opt-in SQL suite against a locally cached SQL Server
Docker image. It creates a new ownership-labeled container with no published
ports, uses temporary random credentials, and removes only that container:

```powershell
python tests/run_hangar_capacity_sql_test.py --build-dir "D:/Source/build" --config Release --work-root "D:/Source/buildwork/hangar-capacity-sql" --image "<local-sql-image>"
```

Build `hangar_capacity_sql_emit`, `hangar_capacity_sql_test`, `hangar_capacity`
and `sdgo_core` first. This fixture never reads a real WorldServer INI or connects
to an existing game database.

The [native contract](investigations/hangar-capacity-native.md) records the
1200-slot boundary, packet/adapter ABI and symbol verification command.
