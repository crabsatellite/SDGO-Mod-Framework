# Unified Mod Manager

[简体中文](mod-manager.zh-CN.md) · [Back to the feature index](../README.md)

**For server owners and maintainers.** Ordinary players keep using the game launcher and follow their server
owner's installation and update instructions. Client Mods that do not match the server configuration can prevent
joining, cause disconnects or crash the game. A notice at the top of every Manager page says the same.

Open Manager from **0_MOD管理（服主专用）** at the top of the client folder, or from **SDGO Mod Manager.exe** in the
framework package. The first normal launch creates the folder entry; **创建目录入口** (create folder entry) restores a
missing one, and existing entries keep their workspace. No desktop shortcut is created.

Client/server selection, mod configuration, shop, operators, unit import, capsule machines, rewards, FRP and AI
catalogs share one local interface. Users do not install Python or launch per-feature programs.

**添加目录** (add folder) and **浏览…** (browse) in settings open the Windows folder or file picker. If the picker
cannot open, Manager shows a box for typing the path instead. Manager installs the framework into a newly added
folder, and the page lists its features once the installation finishes.

Each feature card shows only its name, a one-line description and its switch, with
**设置** (settings) when it has adjustable values. Automatic bug fixes appear in the
archive as always on. With only a client added, the sidebar shows just the game
features and the archive; adding a server directory (or clicking **我是服主**) reveals the
owner tools: shop, operators, unit import, capsules, rewards, FRP, AI catalog, collection capsule,
hangar server, [holiday and birthday gifts](calendar-gifts.md), and background tasks. The developer view expands internal identifiers,
versions, low-level settings and diagnostics; it is not a server-owner permission gate.

Choose existing workspaces to preserve authored data. Configuration writes retain
unknown keys and comments, validate registered values and reject stale revisions.
Games continue using their normal launchers. A saved setting takes effect after the
reported restart of the game or server.

How saving works is stated on every page:

- A feature card's switch saves as soon as it is clicked; it takes effect after the game or server restarts.
- Settings pages and tool pages with manual saves keep a save bar in view at the bottom: it lists unsaved changes
  and offers save and discard. Each setting is tagged **重启后生效** (after restart) or **即时生效** (immediately).
  Leaving a page with unsaved changes asks first.
- Holiday and birthday gifts save in two parts, the common rules and the server's private roster; each section
  names its part, and **保存全部并重启后台任务** saves both and restarts the background task.
- Battle rewards saves each part with the button in its own section; each heading says what it saves, and the
  preview sections say they save nothing. The bottom bar lists unsaved parts and jumps to them. Saving one part
  reloads the page from the saved configuration, so Manager asks first when another part has unsaved changes.
- FRP keeps **保存配置** (save) and **导出玩家配置包（先保存）** (export, saving first) in the bottom bar; the
  collection capsule page numbers its generate, check, install and export steps.
- Save results appear at the bottom right of the window, wherever the page is scrolled; errors stay until closed.
- Long pages list their sections under the title for one-click jumps, with a back-to-top button.
- The capsule machine list and editor scroll separately; the editor's save button and the footer's export and
  **导入到游戏** (apply to game) stay in view.

## Updates

Open **更新** (Update) in the sidebar. New installations default to **Stable**: only published stable features are installed. **Beta** includes Stable and Beta features and follows the newest available release; development diagnostics are excluded from both channels. Switching back to Stable leaves existing Beta and custom Mods in place and updates only the features in the Stable package.

Automatic checking is enabled while Manager runs, at startup and hourly afterward. Use **检查更新** to check now or **立即更新** to install. The program upstream defaults to this project's GitHub Releases and can be changed to a compatible GitHub `owner/repository` in this page. Windows checks the HTTPS certificate, as the browser does. Release identity, SHA-256 and the complete file manifest are checked before installation. Network errors leave the current installation usable and can be retried. Automatic updates can be disabled without disabling manual checks.

Each registered directory has a maintenance mode: **starter** accepts upstream runtime updates, **owner DIY** protects its runtime and adopts only reviewed components, and **player** follows its pinned owner's pack. Existing Mod installations without a declared mode are protected as DIY during registration or migration. Program updates do not automatically replace DIY or player runtime files. The official pack supplies a starting collection; DIY configuration, scripts, resources, compatibility and published versions are the owner's responsibility. See [Owner-managed updates](owner-updates.md) for publication, player binding and private Git access.

Running games/servers are never stopped: runtime updates wait until they close. Mod switches, settings, separately declared configuration files, custom Mods, game resources and private data are preserved by upstream updates. Selected DIY program/script components use upstream files; preview their differences first. Failed writes roll back the affected host; recovery copies stay in the local Manager workspace.

Locally installed Mods with a higher version than the release are kept together with their required native symbols instead of being downgraded.

When Manager itself has an update, choose **重启 Manager 完成更新**. The downloaded Manager waits for the old process to close, replaces the verified executable and reopens the same workspace. Ordinary Mod updates do not require restarting Manager.

Updates also check unit-content installation receipts: only program entries change, while unit definitions, models, textures, acquisition data and installation identity are kept. Modified content is rejected and a failed check rolls the update back.

Configurable timezones default to the server's local timezone. The Updates page's display-timezone field follows server-local time when empty, or takes a UTC offset in minutes for this Manager only; it does not change the Windows system timezone.

## AI handoff and development

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

## Building releases

For publishers, `python tools/build_release.py --build --channel stable --version X.Y.Z` creates a Stable asset, and `--channel beta --version X.Y.Z-beta.N` includes Beta features. Both contain only Manager and declared framework payloads. The release workflow tests update/install contracts, builds selected code and checks the standalone Manager before publishing. Pushes to `master` create Beta releases; stable version tags create Stable releases. Never include copied clients, native servers, databases or owner settings.

### Local-only Mods

Install these addons under `sdgo/local-mods/<id>/`. Both the core and Manager discover this root;
Manager imports marked local-only packages there automatically. Ignore the whole `sdgo/local-mods/`
directory in version control. Public package, export and source guards reject its contents, so additional
local addons do not need individual exclusion rules.
These guards compare reserved directory and manifest names without regard to case,
matching Windows paths; changing letter case does not make the contents exportable.

An owner-only Mod stays in a separate private repository. Its `[mod]` manifest uses
`distribution=local_only`, its `manager.json` uses `"distribution": "local_only"`,
the repository `distribution.json` uses `{"distribution":"local_only",
"redistributable":false}`, every private `.cpp`/`.h` starts with the framework's
local-source marker, and its runtime binary carries the framework non-redistributable
marker. Manager can install and configure it on the owner's selected client, labels it
as local-only, and refuses to treat it as exportable.

Every Mod bundled by this public repository explicitly declares `public` in both
manifests. `tools/distribution_guard.py` checks source commits, staged directories and
ZIP archives. It also scans every distinct blob in an outgoing Git range, including a
private file added and deleted before the final commit. The catalog builder, frozen
Manager build, Manager download responses, package script, release builder and update
reader repeat the fail-closed checks, so renaming a DLL or deleting one manifest does
not make a local-only payload publishable.

```powershell
python tools/distribution_guard.py --git-index
python tools/distribution_guard.py --git-tree HEAD
python tools/distribution_guard.py --git-range origin/master..HEAD
python tools/distribution_guard.py --source-path .
python tools/distribution_guard.py --path build\public-package
python tools/distribution_guard.py --archive dist\SDGO-ModFramework-X.Y.Z.zip
```

For an older client repository whose public Mods predate the distribution fields,
Git hooks may add `--allow-legacy-public`. This accepts only a fully undeclared legacy
pair and still rejects local-only declarations, mismatches, repository/source markers
and marked binary bytes. Public framework source, Manager builds and packages use the
strict commands above.
