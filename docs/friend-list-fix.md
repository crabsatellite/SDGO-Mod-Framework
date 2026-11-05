# Friend list fix

[简体中文](friend-list-fix.zh-CN.md)

Fixes problems in the native friend list and chat window: offline friends disappearing, outdated pilot names,
being unable to open a conversation with an offline friend, and the chat window loading the wrong layout without
its input box. Adding friends, viewing the list and messaging
still use the game's own friend page. The home screen's top-right **USER LIST** also uses Chinese context-menu
actions and displays a friend's rank when the corresponding game profile is available.

| mod | host | part |
|---|---|---|
| `friend_list_fix` | WorldServer | returns the complete list with current pilot names and a fallback for missing member-rank records |
| `friend_list_client_fix` | client | restores the messenger layout, preserves known names during lossy status updates, localizes the home context menu and opens accepted offline conversations |

## Use

Both parts are automatic fixes listed under **修复归档** (fix archive) in SDGO Mod Manager; there is nothing
to configure. After installing, restart WorldServer and log in again with the updated client. The game reads
the friend list once per login, so reopening the friend tab does not refresh an already broken list.

**Offline whisper delivery is not added.** Messages to an offline friend still get the native error `0x1407`
(`0x1406` for an unknown name); nothing pretends to be delivered. Use the in-game mail for messages that
should be read later.

The WorldServer database account needs permission to create a schema and procedures and to read metadata.
For missing member-rank records, it also needs read access to the game database selected by the native INI's
`[DB Game] DBName`. Existing member ranks take precedence. If game-rank lookup is unavailable, the complete
friend list still works using the member database.

If the server enables [friend unit gifts](friend-unit-gift.md), right-clicking an accepted offline friend in
the home USER LIST also offers unit gifting. Native actions keep their original enabled/disabled state.

## Logs

| log line | meaning |
|---|---|
| `friend list fix registered; owned SQL query initializes on the first list request ...` | server part loaded; initializes on the first list request |
| `friend list policy unavailable; native database failure returned, retry is allowed` | initialization failed; the native failure reply was sent and it will retry |
| `accepted offline friends may open the native conversation UI ...` | client part loaded |

## Developer notes

The client requests `uirs/1364.uir` for the messenger service and conversation panels, but that resource is a
parts-exchange layout without the chat input, so native initialization reads a null control.
`friend_list_client_fix` redirects only the two verified messenger call sites to the archive's existing
`uirs/0553.uir`, which has the required chat controls; the parts-exchange screen keeps using `1364`. The shared
string assignment entry stays available to shop localization and other Mods, and the executable and native
archive are unchanged.

`friend_list_fix` derives the pilot-name column's actual collation before converting the current name and
fallback to the native ANSI result, so mixed Thai database defaults with Chinese Unicode name columns display
correctly without changing columns.

`friend_list_fix` also reads damaged names through the authenticated native Unicode binding and encodes the
name field using that column's code page, handling native ANSI binding loss without changing the original
procedures or player data. It fills only missing rank number/step fields from the matching game profile; it
does not create member-profile records or overwrite saved ranks.

`friend_list_client_fix` preserves a complete cached name when a status reply for the same pilot contains
question marks. The original updater still applies presence, location and relationship changes and rebuilds
the display text; a complete new name follows the normal update path. It localizes both the home community
list's context menu and the messenger's, preserving their stock action meanings and button skins.

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
```
