# Unified Mod Manager

[简体中文](mod-manager.zh-CN.md)

Open **SDGO Mod Manager.exe** from the framework package. Client/server selection,
mod configuration, shop, operators, capsule machines, rewards, FRP and AI catalogs
share one local interface. Users do not install Python or launch per-feature UIs.

Each feature card shows only its name, a one-line description and its switch, with
**设置** (settings) when it has adjustable values. Automatic bug fixes appear in the
archive as always on. With only a client added, the sidebar shows just the game
features and the archive; adding a server directory (or clicking **我是服主**) reveals the
owner tools: shop, operators, capsules, rewards, FRP, AI catalog, collection capsule,
hangar server and background tasks. The developer view expands internal identifiers,
versions, low-level settings and diagnostics; it is not a server-owner permission gate.

Choose existing workspaces to preserve authored data. Configuration writes retain
unknown keys and comments, validate registered values and reject stale revisions.
Games continue using their normal launchers. Saving configuration does not prove
that a running game loaded it; follow the reported restart/runtime status.

Every feature can copy a credential-free AI configuration/debugging handoff or a
GitHub bug-report prompt. The repair path helps willing users set up a development
environment and submit a tested PR. PR text must disclose actual AI model names and
versions, their scope, human verification and remaining uncertainty. Use explicit
Unknown when the tool did not disclose a model, and None when no AI was used.

The interface is built on the SDGO Mecha UI library (`manager/static/ui.css`, `ui.js`;
gallery at `/ui-kit.html` in the developer view). Mobile-suit and hangar artwork is read
at run time from the selected client's own GPackData; the repository and package carry
no game art.

See the [full guide](mod-manager.zh-CN.md), [contribution guidance](contributing.zh-CN.md)
and [registration design](mod-manager-design.zh-CN.md).

## Updates

Open **更新** (Update) in the sidebar. New installations default to **Stable**: only published stable features are installed. **Beta** includes Stable and Beta features and follows the newest available release; development diagnostics are excluded from both channels. Switching back to Stable leaves existing Beta and custom Mods in place and updates only the features in the Stable package.

Automatic checking and installation are enabled while Manager runs. It checks at startup and hourly afterward. Use **检查更新** to check now or **立即更新** to install. GitHub Release assets are verified against their SHA-256 and the complete file manifest before installation. Network errors leave the current installation usable and can be retried. Automatic updates can be disabled without disabling manual checks.

Updates apply to registered client and server directories. Running games/servers are never stopped: their update waits until they close. Mod switches, settings, separately declared configuration files, custom Mods, game resources and private data are preserved. Failed writes roll back the affected host; recovery copies stay in the local Manager workspace.

When Manager itself has an update, choose **重启 Manager 完成更新**. The downloaded Manager waits for the old process to close, replaces the verified executable and reopens the same workspace. Ordinary Mod updates do not require restarting Manager.

For publishers, `python tools/build_release.py --build --channel stable --version X.Y.Z` creates a Stable asset, and `--channel beta --version X.Y.Z-beta.N` includes Beta features. Both contain only Manager and declared framework payloads. The release workflow tests update/install contracts, builds selected code and checks the standalone Manager before publishing. Pushes to `master` create Beta releases; stable version tags create Stable releases. Never include copied clients, native servers, databases or owner settings.
