# Friend list fix

[简体中文](friend-list-fix.zh-CN.md)

Fixes three problems in the native friend list: offline friends disappearing, outdated pilot names, and
being unable to open a conversation with an offline friend. Adding friends, viewing the list and messaging
still use the game's own friend page.

| mod | host | part |
|---|---|---|
| `friend_list_fix` | WorldServer | returns the complete list with current pilot names |
| `friend_list_client_fix` | client | lets an accepted offline friend open the native conversation window |

## Use

Both parts are automatic fixes listed under **修复归档** (fix archive) in SDGO Mod Manager; there is nothing
to configure. After installing, restart WorldServer and log in again with the updated client. The game reads
the friend list once per login, so reopening the friend tab does not refresh an already broken list.

**Offline whisper delivery is not added.** Messages to an offline friend still get the native error `0x1407`
(`0x1406` for an unknown name); nothing pretends to be delivered. Use the in-game mail for messages that
should be read later.

The WorldServer database account needs permission to create a schema and procedures and to read metadata.

## Logs

| log line | meaning |
|---|---|
| `friend list fix registered; owned SQL query initializes on the first list request ...` | server part loaded; initializes on the first list request |
| `friend list policy unavailable; native database failure returned, retry is allowed` | initialization failed; the native failure reply was sent and it will retry |
| `accepted offline friends may open the native conversation UI ...` | client part loaded |

## Developer notes

`friend_list_client_fix` 1.1.1 also restores the native messenger service and
conversation panels to `uirs/0553.uir`, which already contains the required
chat controls. The client requests `uirs/1364.uir`, now a parts-exchange layout.
Only the two verified friend call sites are redirected; the shared string
assignment entry remains available to shop localization and other Mods.
The executable and native archive are retained.

Server 0.1.2 derives the canonical pilot-name column collation before converting
the current name and fallback to the native ANSI result, supporting mixed Thai
database defaults and Chinese Unicode name columns without changing columns.


The stock query left-joins online status, clan and other optional data and sends SQL NULLs to a native OLE DB
row binding without status buffers. The row reader stops on the warning, so one offline or incomplete row can
truncate the list. `friend_list_fix` returns non-null defaults of the original types, reads the current pilot
name with the saved friendship name as a fallback, and keeps relationships, sorting, paging and the
five-parameter / 17-column native contract.

It borrows the authenticated member database session; `[DB Member] DBName` in the host INI selects the expected
database and no credentials are copied. On the first request it creates an ownership-marked query copy in its own
`sdgo_friend_list_fix` schema; `dbo.sp_FriendsList` and player data are untouched. An unknown source shape, foreign
schema, edited owned procedure or SQL failure produces the native failure reply. A changed native query is rechecked
on the next restart; disabling the mod restores native routing and leaves its owned objects for inspection.

`friend_list_client_fix` only admits complete, accepted offline friends and keeps the real offline presence,
selection guards, online behavior and native message errors.

Packet, cache and address evidence: [client](investigations/friend-list-client.md),
[server](investigations/friend-list-server.md).

## Developer tests

```powershell
cmake --build build --config Release --target friend_list_fix friend_list_client_fix friend_list_fix_test friend_list_client_fix_test friend_list_db_test
ctest --test-dir build -C Release -R friend_list --output-on-failure
python tests/run_friend_list_sql_test.py --build-dir build --work-root build/sql-fixtures --native-source "<private UTF-8 procedure export>" --image "<locally cached SQL Server image>"
```
