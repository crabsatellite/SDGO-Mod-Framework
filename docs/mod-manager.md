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
