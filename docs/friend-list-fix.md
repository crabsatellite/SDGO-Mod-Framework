# Friend list fix

[简体中文](friend-list-fix.zh-CN.md)

An independent automatic fix for the native friend list. Friendship and messaging
actions stay in the game's existing friend page. The Mod Manager lists this fix
in its repair archive; no additional launcher or background service is needed.

Install both components on the corresponding hosts, then restart WorldServer
and log in again with the updated client. The native messenger fetches membership
once per login; reopening its tab does not reload a broken cached list.

`friend_list_fix` runs in WorldServer. The stock query left-joins online status,
clan and other optional data but sends SQL NULLs to a native OLE DB row binding
that has no status buffers. The row reader stops on warnings, so a single
offline or incomplete row can truncate the list. The Mod returns non-null
defaults of the original types, reads the current pilot name with the saved
friendship name as a fallback, and preserves relationships, sorting, paging,
and the five-parameter/17-column native contract.

It borrows the authenticated member database session. `[DB Member] DBName` in
the host INI selects the expected database; credentials are not copied. On the
first request it creates an ownership-marked `sdgo_friend_list_fix` schema and
query copy. The native `dbo.sp_FriendsList` and player data are retained. An
unknown source shape, foreign schema, edited owned procedure, or SQL failure
produces the native failure reply; initialization can retry. Schema/procedure
creation and metadata read permissions are required. A changed native query
is rechecked on the next WorldServer restart. Disabling the Mod restores native
routing on restart; it leaves its owned query objects available for inspection.

`friend_list_client_fix` selectively allows a complete, accepted offline friend
to open the existing conversation UI. It retains actual offline presence,
selection guards, normal online behavior and native message errors.

**Offline whisper delivery is not added.** WorldServer returns native error
`0x1407` for an offline recipient and `0x1406` for an unknown name. It has no
offline queue on this path. Use the existing in-game mail facility for a message
that needs to be read later.

The [client evidence](friend-list-investigation-client.md) and
[server evidence](friend-list-investigation-server.md) describe the supported
builds, packet/cache behavior and exact boundaries. Developer checks include
the actual client detour, exact host signatures, and an opt-in disposable SQL
fixture with synthetic accounts. Real two-client login/logout, UI display and
message exchange remain separate acceptance checks.

```powershell
cmake --build build --config Release --target friend_list_fix friend_list_client_fix friend_list_fix_test friend_list_client_fix_test friend_list_db_test
ctest --test-dir build -C Release -R friend_list --output-on-failure
python tests/run_friend_list_sql_test.py --build-dir build --work-root build/sql-fixtures --native-source "<private UTF-8 procedure export>" --image "<locally cached SQL Server image>"
```
