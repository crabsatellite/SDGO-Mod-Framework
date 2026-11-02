# Friend list fix

[简体中文](friend-list-fix.zh-CN.md)

An independent automatic fix for the native friend list. Friendship and messaging
actions stay in the game's existing friend page. The Mod Manager lists this fix
in its repair archive; no additional launcher or background service is needed.

The distributed fix consists of `friend_list_client_fix` on the client and
`friend_list_fix` on WorldServer, with their matching manifests, Manager
contracts and verified host symbol files. The World Mod creates and upgrades
its own SQL objects automatically; there is no separate hand-applied database
patch. Clients connecting to an updated WorldServer receive its repaired list
responses. Owners of a separate server must install the server component too.
Native executable files and native `dbo` procedure definitions are retained.

The development test backend, database snapshots, temporary IP addresses and
test-account files are verification infrastructure and are not player runtime
dependencies. Publication verifies the complete owner runtime against the
client Git payload and its mirror, together with the matching server files.

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

Version 0.1.2 also derives the canonical pilot-name column's collation and
applies it to both fallback operands before ANSI conversion. This handles
native member databases with a Thai default and a Chinese Unicode name column,
without changing either column or losing canonical Chinese names.

It borrows the authenticated member database session. `[DB Member] DBName` in
the host INI selects the expected database; credentials are not copied. On the
first request it creates an ownership-marked `sdgo_friend_list_fix` schema and
query copy. The native `dbo.sp_FriendsList` and player data are retained. An
unknown source shape, foreign schema, edited owned procedure, or SQL failure
produces the native failure reply; initialization can retry. Schema/procedure
creation and metadata read permissions are required. A changed native query
is rechecked on the next WorldServer restart. Disabling the Mod restores native
routing on restart; it leaves its owned query objects available for inspection.

`friend_list_client_fix` 1.1.1 restores the native messenger service and
conversation panels to the friend layout already shipped in the supported
client archive. The executable still asks for `uirs\1364.uir`, which that
archive now uses for the parts-exchange popup; the orphaned
`uirs\0553.uir` contains the messenger's required `Edit`, `Slider` and
`Static0` through `Static19` controls. The Mod redirects only the two verified
friend calls. It leaves the shared string-assignment entry available to shop
localization and other Mods. The parts-exchange caller and other native string
assignments remain unchanged. It does not patch the executable on disk or rewrite
`GPackData`.

The same client Mod selectively allows a complete, accepted offline friend to
open the restored conversation UI. It retains actual offline presence,
selection guards, normal online behavior and native message errors.

**Offline whisper delivery is not added.** WorldServer returns native error
`0x1407` for an offline recipient and `0x1406` for an unknown name. It has no
offline queue on this path. Use the existing in-game mail facility for a message
that needs to be read later.

The [client evidence](friend-list-investigation-client.md) and
[server evidence](friend-list-investigation-server.md) describe the supported
builds, packet/cache behavior and exact boundaries. Developer checks load the
production client DLL, install both real MinHook detours, exercise the two
accepted messenger resource callers plus a forwarded exchange caller, and
verify the exact host signatures. SQL contract checks cover offline rows and
pagination in a dedicated test database. Use two updated native clients against
the isolated local test backend to check login/logout, friend-list display and
message exchange; see the [native testing guide](../skills/sdgo-native-testing/SKILL.md).


```powershell
cmake --build build --config Release --target friend_list_fix friend_list_client_fix friend_list_fix_test friend_list_client_fix_test friend_list_db_test
ctest --test-dir build -C Release -R friend_list --output-on-failure
```
