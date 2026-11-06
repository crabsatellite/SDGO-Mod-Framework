# Owner-managed pack updates

[简体中文](owner-updates.zh-CN.md) · [Back to the feature index](../README.md) · [Mod Manager](mod-manager.md)

## Purpose and responsibility

The official pack is a curated starting collection. A server owner who starts DIY takes responsibility for
their configuration, scripts, resources, compatibility, access permissions and published versions. Manager
provides review, packaging and verified delivery; it cannot determine whether arbitrary custom combinations
work together. Owners should check their client/server combination before publishing it to players.

The chain is: upstream release → owner review → confirmed owner revision → player update.
Changing the Manager program upstream does not change a player's owner binding.

## Choose a maintenance mode

Open **更新** (Update), then **整合包维护与服主发布**. Choose the registered directory and save its mode.

| Mode | Runtime update behavior |
| --- | --- |
| 基础整合包 — starter | Accept compatible upstream runtime updates using the selected Stable/Beta channel. |
| 服主 DIY — owner | Keep authored runtime files. Review and explicitly select any upstream components to adopt. |
| 玩家 — player | Check and install only the pinned owner's confirmed pack. Available for client directories. |

Unclassified directories start in starter mode, including existing Mod installations. Earlier automatic DIY
classifications without a maintainer declaration, explicit choice or publication settings migrate to this default.
To take over maintenance, explicitly save **服主 DIY**. Existing explicit choices and pinned player sources stay
in place. The local DIY marker survives switching Manager workspaces. Explicitly switching
back to starter clears the owner binding and permits upstream runtime updates; saving the mode itself does
not immediately replace files.

Use an owner-aware Manager before managing a DIY installation. When upgrading from an older Manager that
does not understand these modes, replace only the Manager executable first. Protected installations accept
program updates only from compatible releases declaring the owner-update capability.

## Review upstream components

The **程序上游** field accepts a compatible GitHub `owner/repository`. Its release identity and asset manifest
must match that repository. This setting controls Manager/program releases, separately from owner packs.

In DIY mode, check for an upstream release, then choose **预览上游差异**. Expand each component to inspect its
changed files, select the components you want, and choose **采用所选组件**. Nothing is selected by default.
Confirmation applies only to that preview; changes to the inputs require a fresh review. Unselected components
and declared independent configuration files remain in place. Selected program and script files use the
upstream versions. A newer Manager can review older upstream components without downgrading itself.

## Publish an owner pack

1. Select the owner's client directory and save **服主 DIY** mode.
2. Enter the pack name, a dedicated publication directory outside the game directory, and player read addresses.
   Use either an HTTPS `owner-feed.json` URL or a Git HTTPS repository address prefixed with
   `git+` and suffixed with `#` followed by the branch name.
   Multiple addresses must belong to the same publisher and serve matching content.
3. For Git publication, enter an HTTPS Git remote and branch. Manager uses existing local Git credentials;
   grant the publisher write access. The dedicated checkout must be clean and updates must fast-forward.
   For static hosting, leave the Git remote empty and serve the publication directory through your existing
   HTTPS site. Manager does not start a public hosting service.
4. Save the settings, choose **预览发布内容**, and inspect the actual file list. Add authored shared resource
   paths only when needed, relative to the client directory. You are responsible for their contents and rights.
5. Choose **确认发布并核验公开下载**. Manager creates an immutable pack revision, updates the feed atomically,
   and reads the metadata and asset bytes back from a configured player source. Only successful readback is
   reported as published. **重试推送／核对已有发布** retries the existing revision without creating another one.

Owner revision numbers are independent of the Manager version. A failed publication check leaves the existing
player release in use. Do not overwrite an immutable ZIP or part, or reuse its revision for different content.

The pack includes framework files, public Mods, shared configuration, hash-verified generated resources and
explicitly declared authored resources. It fingerprints the required original game base; it does not ship
the original game executable or complete native archives. Players need the compatible base supplied through
the server owner's client package or update.

Login and display settings, databases, credentials, private birthday/account lists, local-only Mods,
developer diagnostics, logs and local installation receipts are excluded. The shared `sdgo/sdgo.ini`
projection carries public Mod/feature/category settings and preserves unrelated local settings. Enabled
addons need valid distribution declarations. Limits are 10,000 files, 512 MiB compressed, 1 GiB expanded
and 64 MiB per file.

Packs larger than 48 MiB are published as immutable parts of at most 48 MiB each. The feed records their
order, sizes and SHA-256 values. Manager verifies every part and the complete reconstructed ZIP before
installation; matching backup sources must provide the same bytes. Existing single-ZIP feeds remain
supported. Update Manager before following a pack that uses parts. These content-pack limits are
separate from the generic framework release limits.

## Bind and update players

In **跟随负责人**, enter the owner's addresses, choose **查看发布者**, verify the pack name/publisher, then
confirm the binding. A published client package can also carry the credential-free `sdgo/owner-source.json`
binding. Private Git readers need their own existing Git HTTPS access; an owner's credentials are never
embedded in the binding or pack. The owner manages repository permissions and availability.

Use **检查负责人更新** or **同步负责人版本**, or enable automatic following while Manager runs. The updater
pins the publisher and verifies base identity, revision, SHA-256 and file manifest before installation.
Unavailable sources keep the current version. Matching mirrors may be tried; a valid feed from a different
publisher is rejected. The updater never silently switches to the official pack.

Runtime installation waits for the game to close. Writes are backed up and rolled back on failure or
cancellation. Retirement only removes unchanged files previously managed by that owner; personal files and
local-only Mods stay local. An owner cannot use the player-sync command on their DIY maintenance directory.

## Headless use and troubleshooting

The same Manager executable supports integration into an owner's delivery process:

```text
"SDGO Mod Manager.exe" --check-owner-pack <game-directory>
"SDGO Mod Manager.exe" --sync-owner-pack <game-directory>
```

The check is read-only. Both commands write their result to
`sdgo/local-state/owner-updater/result.json`. Exit codes are `0` for success/current, `2` for waiting or invalid
arguments, `3` for an audit difference, and `1` for an error. A one-shot sync returns waiting while the game runs.

For source failures, check HTTPS availability and the reader's Git credentials/access. For publisher mismatch,
verify the owner's address and binding before changing it. For base mismatch, obtain the owner's compatible
client base. If preview confirmation becomes stale, generate a fresh preview. For interrupted publication,
retry verification of the existing revision. Manager shutdown cancels owned Git process trees; it does not
stop the game or unrelated Git work.
