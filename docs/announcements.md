# Scheduled announcements

Open **SDGO Mod Manager → Owner tools → 公告与展示规则**, select the client directory,
then add a rule and save all rules. The client loads saved rules automatically;
Manager can be closed afterwards. Install the server owner's client package or
update on each player's machine to distribute the shared rules.
The panel can also export the saved announcement package. Other clients can
install it through Manager's standard package import after closing the game.
The package includes the Mod, rules and login pages; local counts stay local.

The login location embeds Windows' web control in the client's original announcement drawer,
with its native open and close buttons. The
in-game location uses the original left-side GM text panel, including its native
animation and pagination. Rules support titles and plain text; text is escaped
for the login page. Authored rules start empty. Dynamic public news is enabled
by default and can be disabled independently of authored announcements.

## Dynamic news

The same panel provides daily news, technology headlines, active Epic game
giveaways and an occasional historical item. It previews the current cache,
edits sources, categories, weekdays, daily windows, triggers and display limits,
and offers a manual refresh. Manual notices win ties at the same priority.

The login digest defaults to three news and two technology headlines, once per
day and process, displayed for 25 seconds. In-game notices are disabled by default
and no in-game triggers are selected. New authored rules also select only the login
location, with a limit of one per day and process. Enable in-game notices and choose
triggers explicitly to use them. Their optional limits are five cards per day, three
per process, with at least ten minutes between cards and ten seconds per native page.
The lobby-only restriction remains available and can
be changed in the panel; otherwise a room/battle trigger waits for the lobby.
Intervals and read state belong to the client installation, not a server account.

Clients perform bounded background fetches when their cache needs refreshing;
the render/login thread does not wait for the network. The default refresh
interval is six hours and content lifetime is at most 48 hours. Source failure
retains recent cache; expired news, mismatched history dates and inactive/future
giveaways are excluded. Giveaway validity uses the explicit start/end timestamps,
not an upstream `is_free_now` flag. The actual offer deadline is distinct from
the cache expiration time. Dates and source names are retained; a previous-day
digest is never relabeled as today's news.

Two keyless community instances of the open-source [60s API](https://github.com/vikiboss/60s)
are configured for ordered fallback. Public instances have no availability
guarantee. Choose compatible instances from the project's
[public list](https://docs.60s-api.viki.moe/7306811m0), or a self-hosted endpoint.
The upstream main demo domain is intended for development and is not a default
production source. Public endpoints must use HTTPS without URL credentials or
query tokens; literal loopback HTTP is also available for local endpoints.
Responses have size, structure and timeout limits, and fetched text is escaped
before it enters the login page. No account, inventory or battle data is sent.

The optional **公告资讯更新** service uses the normal Manager background-services
controls. It keeps the selected installation's cache warm while Manager is open;
the client's own finite fetches remain available after Manager closes. Stop this
service before changing the selected client directory. Source/cache status and
article preview stay in the announcement panel.

Each rule combines an optional date range, selected weekdays and a daily time
window with one or more events: entering the location, periodic evaluation,
successful login, entering a lobby or room, battle start and battle end. Events
are alternatives; date, weekday, time, interval and count limits must all match.
Login rules use entering the login location and periodic evaluation. The other
events belong to the in-game location. A window includes its start and excludes
its end. Equal times mean all day; a window crossing midnight belongs to the
date and weekday on which it starts. The configured UTC offset defines dates.

Higher priority runs first, with editor order breaking ties. Native notices
already being shown finish first. Event notices wait while the panel is busy,
and expire if their time window closes before they can be shown. The interval
sets the minimum time between displays of the same rule. In-game duration is
per native text page; login duration is for the whole notice. An active native
page finishes its display when the triggering window ends.

Per-start, per-day and lifetime limits are optional; **0 means unlimited**.
Counts belong to this client installation, persist locally for daily/lifetime
limits, and are shared between both locations for the same rule. They are not
account-level server statistics. Editing text keeps the rule's counts. Copy a
rule to create a new identity with independent counts. Login counts require the
native browser to expose the actual visible document; game counts require the
original GM renderer to consume the queued notice.
The limits count automatic delivery. The native login buttons also allow a
player to reread the current notice. Server-originated immediate GM broadcasts
continue through the original broadcast queue.

Shared authoring inputs are `rules.json`, `runtime.ini`, `news-settings.json`,
`news.ini` and generated manual `pages`
under the Mod directory. Framework updates preserve authored rule files.
Counters and fetched cache/pages remain under `sdgo/local-state` and are not
included in the exported configuration package. Saving
uses a revision check to prevent overwriting another editor's changes. Changes
apply to subsequent display decisions; an already displayed notice completes
its native animation. The supported client build is checked through verified
symbols. Login notices use the Windows ATL/WebBrowser components; in-game text
uses a separate native path. Installing or updating the Mod requires restarting
the client; saved rules then reload automatically.
