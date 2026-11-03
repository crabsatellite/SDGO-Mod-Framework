# Holiday and birthday gifts

[简体中文](calendar-gifts.zh-CN.md) · [Mod Manager](mod-manager.md) · [Friend unit gifts](friend-unit-gift.md)

Calendar gifts is an independent Manager module. Server owners configure holidays, birthdays and particular dates; rewards enter the player's original Storage page for normal claiming. It creates new rewards rather than transferring another player's inventory, and requires no game executable or archive patch.

## Defaults

Automatic delivery starts disabled. The bundled editable rules provide:

- One currently unowned unit on New Year's Day, Spring Festival, Qingming, Labour Day, Dragon Boat Festival, Mid-Autumn Festival and National Day, once per festival per year.
- One unowned unit in each of C, CS, CR, B, BS, BR, A, AS, AR, S, SS and SR on a registered birthday.
- UTC+08:00 and up to seven days of downtime recovery. First activation starts on the server's current date and does not backfill earlier events.

An exhausted birthday tier searches higher rarity, then lower rarity within the same base rank. Choices within one bundle remain distinct. AU, BU and CU can be enabled separately. An exhausted tier and fallback range are skipped rather than filled with duplicates.

Ownership checks include the hangar, permanent units awaiting collection in native Storage, and relevant units held by the friend unit gift module. Eligible units need an existing valid permanent-unit reward definition on the server. Fixed-unit rewards also skip owned or pending types.

## Configuration

1. Open the owner-only SDGO Mod Manager and select **Holiday and birthday gifts**.
2. Select a separate private configuration directory and this server's `WorldServer.ini`. Private data cannot be placed inside a distributable Mod directory. Existing INI credentials remain local and are not returned by the API or included in rule exports.
3. Configure each event's date, audience and rewards. Choose one missing unit, one per selected tier, fixed units, or native items found by name in the server catalog.
4. Register birthday month/day values by player name, or import existing birthday registrations. Import preserves existing entries and does not read email addresses.
5. Save the rules, review a player/date with the read-only preview, then start the supervised calendar task.

Private player groups support anniversaries and one-time events. February 29 birthdays can use February 28, March 1, or leap years only.

Each event identity, year and account has one final delivery receipt. Editing the date or rewards of an already-delivered event does not issue another bundle. Creating a new event identity creates a separate event; do not use that to retry an uncertain result.

Native item rewards retain their category and quantity. Unit rewards use the unit selector and cannot bypass ownership checks through the ordinary item list. This module does not directly change GB, MB or capsule-coin balances; available items and reward packs depend on the server's existing native definitions.

## Public rules and private rosters

Generic rules and server-private rosters are separate files. Birthdays store an account identity and month/day, without a birth year. Private groups, activation dates, diagnostics and delivery receipts are not framework payloads.

**Export generic rules** exports only events targeting all players and disables automatic delivery in the export. Birthdays, private groups and dates targeting private groups are excluded. Export does not change the running server's policy.

Stop the old birthday scheduler before enabling this module, so two independent systems do not each issue rewards. Select the old script's `processor_state.json` and import its completed birthday receipts. They remain private and are checked by account and year when the task starts, preventing a second birthday bundle in the migration year. Pending or invalid entries are never treated as completed deliveries.

## Operation and troubleshooting

Manager owns task start, stop, health checks and recovery. Closing Manager stops scheduling; reopening it restores enabled tasks. Players can claim already-issued rewards without Manager remaining online.

When a hangar is full, free space before claiming the unit. Display, eligibility and retention follow the native Storage rules. Use the matching complete client runtime so unit definitions, item categories and names agree with the server.

The receipt and all native Storage inserts share one database transaction. A later reward failure rolls back the bundle. A lost commit acknowledgement is recovered by the existing receipt rather than issuing another bundle. Disabling rules stops new deliveries without withdrawing existing rewards.

The module maintains its own `sdgo_calendar_gifts` data. Existing native triggers and their permissions remain authoritative. Servers using collection-catalog triggers, for example, need the existing game login's transaction-observation permissions. Missing permissions stop activation; the module never grants server permissions automatically.

The offline 2025–2040 festival calendar uses the [Hong Kong Observatory's Gregorian–Lunar conversion data](https://www.hko.gov.hk/tc/gts/time/conversion.htm). It describes festival dates, rather than every adjusted public-holiday day. Update the bundled calendar or select an explicit date outside that range.

For failures, inspect Manager's task status and local diagnostics. Share generic rule exports when troubleshooting, keeping private rosters, database INIs and full receipts on the server.
