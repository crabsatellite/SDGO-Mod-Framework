# First login lobby music

[简体中文](lobby-music-fix.zh-CN.md) · [Back to the feature index](../README.md)

`lobby_music_fix` restores the first-login lobby track. Before entering the first battle in a
login session, native lobby requests for `BGM/0042.mp3` use `BGM/0043.mp3` instead. After entering
a battle, room return and subsequent lobby navigation retain the original 0042 selection.
Returning to the login gate starts a new session. Cancelling a loading screen before a battle
actually starts does not count as having played a battle.

## Install and controls

This is a default-enabled client fix in the SDGO Mod Manager fix archive. Install the client
framework package or the server owner's client package or update, then restart the client.
The supported host is `GOnline-54BE1568-00C97000`; unsupported signatures refuse to load.
No server installation or room command is needed.

The existing native music switch and volume continue to apply. The Mod passes the selected
path to the original Miles player and leaves its looping, fade and output settings intact.
Battle music, result music and custom music are unchanged. Disabling music prevents native
playback; the Mod does not force it to start.

Both tracks must already be supplied by the client package. The public Mod contains no music
assets. If `BGM/0043.mp3` is missing, the Mod retains the original track and logs a warning.

## Troubleshooting

Confirm that `lobby_music_fix` loaded in `sdgo/logs/`. Its native BGM entries show the requested
and selected track IDs, service state and battle-session flag. `stream_open=1` and a Miles
playing status show that the original player opened and started the stream; they do not prove
that the user's speakers are audible. Check the in-game music switch and volume separately.

The behavior is per login session, independent of an account's historical battle count.
