# Owner configuration and content updates

[中文](owner-updates.zh-CN.md) · [Manager](mod-manager.md) · [Content and SQL ownership](content-boundaries.md)

## Two independent sources

Start with a compatible original client and install the official Manager. The Mod source supplies Manager, core, Mod programs and generic defaults. The owner content source supplies Mod selection, exact versions, shared settings, custom units, assets and generated catalogs. Original programs and archives are obtained separately.

The default Mod source uses official GitHub Releases. Compatible Git SSH/HTTPS framework mirrors can also be imported. Owner content supports Git SSH/HTTPS, HTTPS manifests and files, offline directories and ZIP payloads. A complete-client repository and a particular branch layout are optional owner choices.

## Player installation

1. Place Manager from the official release in the compatible client folder and launch it, or add that folder in Manager.
2. Under owner configuration and content updates, enter the owner's URL or the local path to server-profile.json / profile-source.json.
3. Select the one-click install/follow action. Manager imports both bindings, fetches the locked programs and owner content separately, verifies them and installs them in one transaction.
4. Start the game through its original entry. While Manager runs, automatic following checks for updates and waits for the game to close. If the running Manager needs replacement, use the displayed restart action.

The binding is sdgo/client-source.json schema_version=3; the content manifest is server-profile.json. Git reuses the reader's existing credentials, SSH keys and agent; bindings never store passwords or tokens.

## Owner publication

Choose owner DIY for your editing client. Review and adopt the intended upstream programs, set shared parameters and generate custom content. Check the program upstream before using the owner profile export form.

Choose an absolute output directory outside the client, player-readable feeds and any extra asset paths relative to the client. Registered generator outputs and declared Mod configuration files are included automatically.

Export the profile. With no Git publication address, the directory is ready for an existing HTTPS host or offline transfer. With a Git SSH/HTTPS remote and content branch, Manager commits, pushes and reads back the remote commit using your existing identity. Hosting and mirror services remain the owner's responsibility; Manager does not start a public file server.

The export contains server-profile.json, profile-source.json and content/. Framework DLLs, Mod identity and interfaces remain in the independent program source. Generic AI catalog providers come from that source; generated catalog.bin comes only from owner content. An unchanged export reuses its revision; configuration or asset changes increment content revisions independently of framework versions.

Validate the resulting client/server combination before publishing it. Players install the owner's exact program and Mod versions rather than independently selecting the latest incompatible release.

## State and installation safety

Shared gameplay parameters and custom resources follow owner updates. Personal login, display/FPS and explicitly declared personal fields stay local. Accounts, inventory, currency, passwords, birthday rosters, databases, logs and recovery data never enter player content. Player installation runs no SQL or database migration.

Every transport uses the same staging, hash verification, journal and installation transaction. A failed source, incompatible version, corrupted file or publisher change preserves the installed client. An existing owner cannot be silently replaced, and player synchronization cannot overwrite an owner editing folder. First import does not retire original base files.

Content revisions cannot automatically move backwards or be reused with different bytes. Retirement uses the existing quarantine/recovery flow. Private repository access is not permission to distribute private rosters or databases.

## Troubleshooting

Check base compatibility, locked program version, content revision and actual installed status. Git failures require checking the reader's existing access; HTTPS failures require checking the complete hosted manifest and payload. Mirrors must serve the same identity and bytes.

Content updates do not require rebuilding the original client. If the base is incompatible, obtain the owner's supported original package. Close the running game and retry a waiting update; use recovery for an interrupted transaction.
