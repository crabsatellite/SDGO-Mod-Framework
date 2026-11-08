# Mission behavior profiles

[简体中文](mission-behavior.zh-CN.md) · [Back to the feature index](../README.md)

Mission behavior profiles let a server owner choose one paired mission behavior for a client and every
Battle channel in a server group. **OC is the default.** **GCO (current G改 behavior)** is available as
the unchanged current-data option. The Manager applies one selection and, when needed, one matching
`profile.bin` to both components:

| Mod | Runs in | Responsibility |
|---|---|---|
| `mission_behavior` | Each participating client | Applies the verified M1 client AI bindings for the selected profile |
| `mission_behavior_server` | Every Battle channel in the selected group | Applies the verified mission NPC-row differences for the selected profile |

OC restores the verified historical bindings and spawn differences listed below. Other mission data
stays on the current version.

## Profile scope

### OC (default)

The current OC profile restores only the differences supported by the verified reference:

- 12 M1 client Boss AI bindings, selecting the existing HP-driven map-special AI outside Very Easy;
- 1,198 older mission NPC-row differences across 18 difficulty modes;
- those modes belong to five mission groups: G2, G5, G7, G15 and G17.

The original Boss scripts keep their own HP conditions and special limits. Very Easy keeps its current
binding. The profile does not add an unlimited-SP rule.

Mission data without a verified older row continues to use the current G改 data. The complete current
set of 39 missions remains installed. The profile does not replace rewards, reward multipliers or unit
stats.

### GCO / G改 (current)

GCO keeps the current native mission data and current M1 bindings. Selecting it does not rewrite the
game's mission database or archives. It is the rollback and compatibility profile when the owner does
not want the verified OC differences.

## Configure the server group in Mod Manager

Routine configuration uses the single **Mission behavior** page in SDGO Mod Manager. Do not edit each
client or Battle channel separately.

1. Install the matching client and server components from the server owner's client package or update.
2. Open **Mission behavior**. Select one client and the server group that contains every participating
   Battle channel.
3. If the OC status says that no owner profile is ready, import the matching `profile.bin` supplied or
   generated for that client/server data set.
4. Select **OC (default)** or **GCO (current)**.
5. Close the selected client and all Battle channels in the group, then apply once. The Manager refuses
   a partial group, a disabled component, a stale edit, or a running target.
6. Restart the client and all Battle channels. Profile changes are loaded at process startup.

The operation writes the same selection to both components. When an owner profile is present, it also
writes the same validated bytes to the client and every Battle channel. A failed write restores files
already touched by that operation.

## Distribute the client configuration

The generic public framework release contains the Mod code and Manager integration, but it does not
contain an owner's generated `profile.bin`.

After applying and verifying one profile across the whole group, use **Export player package** on the
same Manager page. Export is refused if the client and any Battle channel disagree about the selected
profile or bundle. The resulting client package contains the selected `config.ini`, the validated
client `profile.bin` when one is required, and the public client component.

Players close the game, import that ZIP with Mod Manager's normal **Import configuration package**
entry, then start the game normally. They do not generate mission data and do not configure the server
side. A server owner may instead deliver the same files through their normal client package or update.

`config.ini` and `profile.bin` are declared configuration files. Framework updates and uninstall keep
their target-side bytes; importing a newer owner package intentionally installs that package's matching
configuration.

## If OC is unavailable or refused

- **Owner profile missing:** import a matching `profile.bin` in the Mission behavior page. OC is not
  applied without a valid bundle; GCO remains usable.
- **Baseline mismatch:** generate a new owner profile from the currently paired client and server
  mission data, import it in the Manager, and apply again. Do not reuse another server's bundle or a
  bundle made for a different data set. A mismatched row is never blindly overwritten.
- **Targets disagree:** select the complete Battle group and apply once. Player export stays disabled
  until the client and all channels report the same profile and bundle.
- **Change appears inactive:** confirm that the player received the latest owner package and restart
  both the client and every affected Battle channel.

The bundle contains bounded plain mission-row differences and integrity metadata. It contains no
account data, database credentials, or local paths.

## Boundaries

- Both components operate on process-local native mission records. They do not write SQL or replace
  game archives.
- OC restores the verified scope listed above. Missions, difficulties, scripts, rewards, and opcodes
  outside that scope keep the current behavior.
- GCO makes no native mission-data changes.
- Switching is not supported while the client or a selected Battle channel is running.

## Developer notes

- The 12 M1 client bindings are ten Boss001 bindings and two Boss002 bindings.
- Unverified historical `Clear` opcodes keep their current values and are never blindly overwritten.
