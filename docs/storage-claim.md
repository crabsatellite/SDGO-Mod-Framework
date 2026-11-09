# Storage claim all

Client Mod `storage_claim` adds **一键领取** beside the two native storage tabs.
[简体中文](storage-claim.zh-CN.md)


Select the items or gifts tab, click the new button, then **确认领取** once for
the entire batch. Timed rewards may begin their validity period when claimed.
The whole tab is claimed at once: the Mod first reads every page, then submits
every reward to the server back to back and reloads the list a single time.
Progress and the result appear at the bottom. Click **停止领取** to stop
subsequent requests. Already submitted claims remain subject to the server's
final result.

Each reward still uses the original authenticated preflight and claim protocol,
but all preflights are sent together (by default up to 8 awaiting an answer)
and the allowed rewards are then claimed back to back, with no list reload in
between, so dozens of rewards usually finish within seconds. Stable IDs distinguish separate copies of the same item; gift
letters already claimed are skipped. A reward the server refuses (for example a
full hangar) is skipped and counted while the rest is claimed; the summary shows
the count and server code instead of one native failure dialog per reward.
Choice-required gifts are left for individual native handling. Disconnect, page
closure and timeout stop the batch. Unconfirmed claims are never retried
automatically.

Players receive it with the server owner's client package or update. Otherwise install the
client portion of a Full package, or close the target client and
import the dedicated overlay using SDGO Mod Manager. Start the game normally.
No server Mod, account settings, database access, additional skins, background
Manager or alternate launcher is required. The supported host is
`GOnline-54BE1568-00C97000`. Unknown code/layout/skin combinations fail closed,
preserving the original controls. Developer settings are `PipelineDepth=8` (requests
awaiting an answer, 1–32), `ReplyTimeoutMs=10000` and `MaxClaims=5000` in
`mod.ini`; normal users need not change them. Native page numbers are bounded at
255: items beyond that range are claimed in a further round, gift letters beyond
it stop with a notice.

The 450×540 native `1396.uir` layout places the new 124×23 button at (291,64)
and the status line at y=520, clear of existing rows, pagination, expiration
warning and close controls. The Mod uses host-allocated skins and controls.
Each preflight and claim is a self-contained stored-procedure call that does not
depend on a preceding preflight, so requests can be submitted back to back. The
World answers every request once, but concurrent requests may be answered out of
send order: success replies are matched by reward ID; error replies carry no ID,
are counted, and once every other outstanding request of the kind has answered,
the unanswered ones are the refused rewards. A grant takes the account's shop lock,
so claims are sent one after another; the timeout measures how long no answer
arrived. Mod list requests mark the page as loading
like the stock sender; the final reload waits while the stock grant handler is
reloading the list itself. Delayed cancelled preflight replies are consumed to
prevent native auto-claim. Successful claims still pass through the game's
grant/inventory handlers. Page
attachment and received clicks are logged separately from loading.

Automated fixtures use synthetic inventories, packet replies and UI boundaries,
covering whole-tab collection, the pipelining window, counted refusals, stops
before and after claims, timeouts and page ranges. The native GameTest page group
`storage_gift` claims a multi-page tab with one confirmation on a real client and
server and checks the collected, granted, skipped and remaining counts.
