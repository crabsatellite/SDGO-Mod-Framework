# Owner programme packages

[简体中文](owner-programmes.zh-CN.md) · [Manager](mod-manager.md) · [Content boundaries](content-boundaries.md)

An owner programme package locks an official framework baseline and adds explicitly distributable client Mods. Its source stays in the owner's private development repository. Players receive compiled programmes and metadata through the owner's distribution endpoint; units, audiovisual resources and shared settings remain a separate content layer.

The owner chooses the combination. Players import one owner configuration address and then start `GOnline.exe`: Manager downloads and verifies the locked programmes and content, commits them together, rechecks the remote configuration and starts the original game. A failed download or digest check preserves the installation and blocks launch. Matching mirrors can resume the same release; they cannot substitute another publisher or default content.

## Publish from Manager

1. Use a maintainer client and review the official baseline in **Updates and management**. Development clients keep their previews and do not follow player updates.
2. Import the separately built owner Mods. Their `mod.ini` and `manager.json` must both declare `distribution=owner`; the DLL carries the framework's owner-distribution marker. They install to `sdgo/owner-mods/<id>/`.
3. In **Publish owner programmes**, choose a dedicated output directory, matching distribution addresses, the formal or test channel, and the named Mods to distribute. Git publication can use an existing authenticated SSH or HTTPS remote; an offline directory is also supported.
4. Publish the configuration and content that lock this programme revision. Test and formal configurations use separate output locations and addresses. Advancing a programme channel alone does not upgrade players whose configuration still locks an earlier revision.

Programme packages accept a module DLL, its two manifests and an optional symbol fragment. They do not contain original game files, server DLLs, SQL, credentials or assets. Shared owner parameters are rendered from the content configuration rather than changing the programme's identity or native interfaces.

## Distribution and versions

| Scope | Installation | Export |
| --- | --- | --- |
| `public` | `sdgo/mods/` | Official/fork framework release |
| `owner` | `sdgo/owner-mods/` | Explicit owner programme package |
| `local_only` | `sdgo/local-mods/` | Local installation only |

Public guards reject owner programmes and source. Owner guards continue to reject local-only files, including renamed files and stripped manifests. Changing a module's intended audience requires a newly declared build and acceptance; modifying an old ZIP does not grant distribution rights.

The owner's Stable/Beta channel is independent of the official channel. A formal combination may pin a specifically tested official Beta when it needs those Mods. Official version strings remain unchanged; owner programme and content revisions are tracked separately. Both programme and content mirrors must match the selected owner channel.

## Contract and update transaction

`owner-programme.json` uses kind `sdgo-owner-programme`, schema 1. It records publisher identity, channel, revision, the exact official baseline, selected Mod versions and every file's size/SHA-256. Git bindings pin a commit. HTTPS and offline publication retain immutable revision directories, so a later publication cannot destroy an earlier lock.

Owner configuration schema 2 records the programme binding and content channel. Older Managers reject that schema instead of silently ignoring a patch. Its player source uses `manager_policy=owner`: the matching Manager is included in the approved programme update and automatically hands off a pending launch when it must replace itself. Ordinary profiles without this programme layer retain their existing Manager policy.

The installation receipt records the effective files and both programme/content revisions. Shared settings, dependency checks, staging, installation and rollback use the same transaction. Personal login/display settings, recovery state and other personal files remain local.

In managed player clients, undeclared files in Mod loading directories are saved to local recovery and removed from the loading path. Unknown ordinary files are preserved. This check covers the framework's Mod roots; it is not an inventory of every third-party DLL injection mechanism. Maintainer directories are not automatically quarantined.

## Troubleshooting

- A baseline without the owner-programme capability is rejected; select a compatible official package.
- Programme/content identity, channel, revision or digest disagreements stop installation. Fix the release or its matching mirror rather than replacing it with another package.
- Damaged installed metadata can be replaced or quarantined by the approved update; the original files remain in recovery.
- To roll back a programme, publish a new configuration revision that locks the previously accepted programme. Reusing a revision for different bytes or automatically decreasing its revision is rejected. Database changes still use the server maintenance workflow.
- Source repositories and player distribution endpoints have different audiences. A private Git repository readable by players must not contain server secrets on another branch.
