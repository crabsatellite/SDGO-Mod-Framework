# Owner configuration and content updates

[中文](owner-updates.zh-CN.md) · [Manager](mod-manager.md) · [Content and SQL ownership](content-boundaries.md)

## Two independent sources

Start with a compatible original client and install the official Manager. The Mod source supplies Manager, core, Mod programs and generic defaults. The owner content source supplies Mod selection, exact versions, shared settings, custom units, assets and generated catalogs. Original programs and archives are obtained separately.

The default Mod source uses official GitHub Releases. Compatible Git SSH/HTTPS framework mirrors can also be imported. Owner content supports Git SSH/HTTPS, HTTPS manifests and files, offline directories and ZIP payloads. A complete-client repository and a particular branch layout are optional owner choices.

## Player installation

1. Place Manager from the official release in the compatible client folder and launch it, or add that folder in Manager.
2. Under owner configuration and content updates, enter the owner's URL or the local path to server-profile.json / profile-source.json.
3. Select the one-click install/follow action. Manager imports both bindings, fetches the locked programs and owner content separately, verifies them and installs them in one transaction.
4. Double-click `GOnline.exe`. Manager shows startup progress, synchronizes the locked programs and content, verifies the installation and then opens the game.

The binding is sdgo/client-source.json schema_version=3; the content manifest is server-profile.json. Git reuses the reader's existing credentials, SSH keys and agent; bindings never store passwords or tokens.

The page reports the current file, received bytes, verified file counts, staging and installation. Slow transfers have no fixed total duration. Cancel and continue preserve verified files; later content revisions also reuse identical installed files. HTTPS resumes partial files when the server supports Range; Git preserves completed objects and files.

A backup source must supply the same locked manifest and file digests. Download failure cannot switch the attempt to another revision, content set or official defaults. Installation begins only after all required files verify. Missing, corrupted, cancelled or interrupted content is never reported as success; installation failure restores the original files.

An imported binding can select program mirrors for the same publisher and transport. The owner still controls the locked program version and channel. If the owner migrates the transport type, Manager uses that declared transport with the same integrity checks.

## Updates before launch

Bound player clients use the framework loader and the same Manager update worker. Install a matching Manager and loader from the current release; no separate gameplay Mod, launcher or replacement original executable is needed. Unbound clients and directories explicitly maintained as owner DIY retain their ordinary startup behavior.

Each launch checks the selected owner's current profile, downloads required program and content files, commits the verified installation and checks the remote revision again. The game opens after the effective managed files match. The official program source does not override the owner's version lock or shared settings. Background automatic following and this startup check are separate: stopping periodic following does not bypass startup verification.

Progress includes checking, received bytes or file counts, installation and launch. Slow downloads have no total-time or minimum-speed limit. An error leaves the game closed and offers Retry or Cancel; verified downloads remain available. Repeated clicks during one pending launch share one update and launch. If an update requires a running game or Manager to close, the progress window waits for normal exit or the existing Manager restart transaction; it never terminates a player's game.

The loader exits the initial process before replacement and accepts a single-use, short-lived launch permit bound to this client, its source and its verified receipt. Manager preserves the original arguments. A missing or outdated Manager shows an actionable error; incomplete content cannot be used as an offline success path. Explicitly imported offline profiles verify their declared local source.
Concurrent clicks share a verified helper copy. Its executable is written by one process at a time, so another click cannot replace it while Windows is starting it.

## Owner publication

Choose owner DIY for your editing client. Review and adopt the intended upstream programs, set shared parameters and generate custom content. Check the program upstream before the first profile export or after adopting a different program version.

Choose an absolute output directory outside the client, player-readable feeds and any extra asset paths relative to the client. Registered generator outputs and declared Mod configuration files are included automatically.

Previously published content and content recorded by an installed profile remain part of that inventory while their files exist. Taking over an installed client does not require its former private generator workspace. These files still pass content and privacy validation; deleted files retire through the normal update transaction.

Export the profile. With no Git publication address, the directory is ready for an existing HTTPS host or offline transfer. With a Git SSH/HTTPS remote and content branch, Manager commits, pushes and reads back the remote commit using your existing identity. Hosting and mirror services remain the owner's responsibility; Manager does not start a public file server.

The export contains server-profile.json, profile-source.json and content/. Framework DLLs, Mod identity and interfaces remain in the independent program source. Generic AI catalog providers come from that source; generated catalog.bin comes only from owner content. An unchanged export reuses its revision; configuration or asset changes increment content revisions independently of framework versions.

Later content exports retain the published program lock while that program version remains installed. A Manager restart or a newer upstream release does not require a program upgrade. The installed loader, core and selected Mod binaries must still match the locked package; modified or unpublished programs are refused. Review and adopt a different program version explicitly before moving the profile to that version. Git publication transfers have no fixed total duration; closing Manager cancels its owned transfer processes.

Validate the resulting client/server combination before publishing it. Players install the owner's exact program and Mod versions rather than independently selecting the latest incompatible release.

## State and installation safety

Shared gameplay parameters and custom resources follow owner updates. Personal login, display/FPS and explicitly declared personal fields stay local. Accounts, inventory, currency, passwords, birthday rosters, databases, logs and recovery data never enter player content. Player installation runs no SQL or database migration.

Every transport uses the same staging, hash verification, journal and installation transaction. A failed source, incompatible version, corrupted file or publisher change preserves the installed client. An existing owner cannot be silently replaced, and player synchronization cannot overwrite an owner editing folder. First import does not retire original base files.

Content revisions cannot automatically move backwards or be reused with different bytes. Retirement uses the existing quarantine/recovery flow. Private repository access is not permission to distribute private rosters or databases.

## Troubleshooting

Check base compatibility, locked program version, content revision and actual installed status. Git failures require checking the reader's existing access; HTTPS failures require checking the complete hosted manifest and payload. Mirrors must serve the same identity and bytes.

On Windows, Manager's Git transfers support deep install and workspace paths. Existing SSH identities and credential helpers are retained; long-path support applies only to Manager-owned Git commands.

Content updates do not require rebuilding the original client. If the base is incompatible, obtain the owner's supported original package. Close the running game and retry a waiting update; use recovery for an interrupted transaction.
