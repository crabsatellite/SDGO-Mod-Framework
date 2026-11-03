# Storage claim all

Client Mod `storage_claim` adds **一键领取** beside the two native storage tabs.
[简体中文](storage-claim.zh-CN.md)


Select the items or gifts tab, click the new button, then **确认领取** once for
the entire batch. Timed rewards may begin their validity period when claimed.
The Mod processes every page in that tab, with progress at the bottom. Click
**停止领取** to stop subsequent requests. Already submitted claims remain subject
to the server's final result.

Each reward uses the original authenticated preflight and claim protocol, with
one outstanding request and a fresh list between claims. Item removal is handled
by reloading page one; gift pages skip already claimed notes and advance. Stable
IDs distinguish separate copies of the same item. Choice-required gifts pause
for individual native handling. Capacity errors, refusal, disconnect, page closure
and timeout stop the batch. Unconfirmed claims are never retried automatically.

Friend-server players receive it through `update.bat`. Otherwise install the
client portion of a Full package, or close the target client and
import the dedicated overlay using SDGO Mod Manager. Start the game normally.
No server Mod, account settings, database access, additional skins, background
Manager or alternate launcher is required. The supported host is
`GOnline-54BE1568-00C97000`. Unknown code/layout/skin combinations fail closed,
preserving the original controls. Developer settings are `RequestIntervalMs=200`,
`ReplyTimeoutMs=10000` and `MaxClaims=5000` in `mod.ini`; normal users need not
change them. Native gift page numbers are bounded at 255.

The 450×540 native `1396.uir` layout places the new 124×23 button at (291,64)
and the status line at y=520, clear of existing rows, pagination, expiration
warning and close controls. The Mod uses host-allocated skins and controls.
Delayed cancelled preflight replies are consumed to prevent native auto-claim.
Successful claims still pass through the game's grant/inventory handlers. Page
attachment and received clicks are logged separately from loading.

Automated fixtures use synthetic inventories, packet replies and UI boundaries.
