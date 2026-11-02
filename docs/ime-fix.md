# IME candidates and text commit

`ime_fix` 1.1.1 restores Chinese IME candidate requests and fixes residual preedit letters after word selection.
The original single-call-site UTF-16 commit-length correction remains for Chinese, Japanese and Korean input.

Run `update.bat` in an existing client, then start the client normally. The automatic fix appears in
Mod Manager's Fix archive; older Managers may retain its IME commit fix label. Developer toggles and
length-only diagnostics are available under Show developer information and take effect after restart.

The Mod restores only the candidate UI bits requested for the game's own Chinese EDIT. Before a result
message it snapshots the old preedit and corrects the erase count at the verified result caller only
when the display range, objects, source, position and UTF-16 contents match exactly. Other native string
operations and legitimate English/numeric prefixes and suffixes are preserved. Composition end clears
the native preedit cache. Windows settings, keyboard layouts and the server remain unchanged.

Logs contain events and lengths, not typed text. A preedit/display guard mismatch keeps the native path
and requires investigation. See the [Chinese guide](ime-fix.zh-CN.md) and
[implementation record](investigations/windows-pinyin-input.md).
