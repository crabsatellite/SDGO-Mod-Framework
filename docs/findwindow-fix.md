# General input method fix (FindWindowW)

Stops input methods from freezing or crashing the game, whichever input method it is. The
repack's `SDPlus.dll` breaks window lookup for every DLL in the game process; this fix
restores it for everything except the game itself. 中文：[findwindow-fix.zh-CN.md](findwindow-fix.zh-CN.md)

| mod | host | part |
|---|---|---|
| `findwindow_fix` | client | the whole feature |

Independent of [`ime_fix`](ime-fix.md), which fixes how the game reads committed text.

![Using an input method in the lobby](findwindow-fix-ime.png)

## Use

Nothing to do: it works automatically once the framework is installed on the client. The mod
needs the `sdgo\core.dll` of the same release (it uses `HookSystemExport`); installing or updating
the client framework updates both. SDGO Mod Manager lists it under **修复归档** (fix archive). To
switch it off while troubleshooting, click **显示开发信息** (show developer info), clear its
switch and restart the game; this writes `findwindow_fix=0` under `[mods]` in `sdgo\sdgo.ini`.

## Settings

Developer setting, shown in the Manager after **显示开发信息**. Stored in
`sdgo\mods\findwindow_fix\mod.ini` and read when the game starts:

| key | default | meaning |
|---|---|---|
| `Trace` | 0 | 1: log the handle returned for every external window lookup (no window names); 0: log only the first successful one |

## Logs and troubleshooting

```
findwindow_fix: hooked system export user32.dll!FindWindowW
findwindow_fix: ready: normal FindWindowW for every caller except GOnline.exe and SDPlus.dll
findwindow_fix: loaded FindWindowW fix 1.0.0
findwindow_fix: window lookup for an external caller: 000105DC
```

| log line | meaning |
|---|---|
| `window lookup for an external caller: ...` | a DLL other than the game looked up a window and got a real handle |
| `core lacks HookSystemExport` | `sdgo\core.dll` is older than the mod; reinstall the whole client package |
| `SDPlus.dll is not loaded; nothing to fix` | this game folder has no `SDPlus.dll`, so window lookup is not broken; the mod stays off |
| `hook system export user32.dll!FindWindowW failed: ...` | another mod already owns that hook |

The fix covers window lookup only. An input method that still freezes the game has a
different cause.

## How it works

The repack's localization plugin `SDPlus.dll` replaces `user32!FindWindowW` with a routine that
never really looks for a window. It only answers the game's own single-instance check (which is
what lets several clients run at once); any other caller gets a fake handle, or crashes when the
class name is NULL or an atom.

Input methods run inside the game process. One that looks up a window, such as iFlytek looking
for its background `iFlyPlatform.exe`, then sends its start-up request to the fake handle and waits
forever on the game's UI thread. Input methods that never look up a window, such as Microsoft
Pinyin, were not affected.

`findwindow_fix` hooks the same function through the core and checks who is calling. Calls from
`GOnline.exe` and `SDPlus.dll` keep SDPlus's behaviour, so multi-client still works. Every other
caller gets a normal top-level window lookup. It does not matter which input method or DLL is
calling.

## Developer notes

`SDPlus.dll` is imported by the repack's `GOnline.exe` and patches `user32!FindWindowW` with the
routine at `SDPlus.dll+0x3020`:

* class `_GONLINE_` with title `Gundam Online` returns NULL ("not running");
* any other query returns -1 or +1 (the sign of a string comparison), which is not a window;
* a NULL or atom class name is dereferenced as a string and crashes.

`GOnline.exe` calls `FindWindowW` once, at `0x004914F6`, with exactly that class and title: its
single-instance check.

`findwindow_fix` detours the export with the core's `HookSystemExport` (MinHook), so the trampoline
keeps SDPlus's patch as the previous hook. At load it records the address ranges of `GOnline.exe`
and `SDPlus.dll` and dispatches on each call's return address:

* calls from those two modules go to SDPlus's routine;
* every other call, including DLLs loaded later and generated code, gets
  `FindWindowExW(GetDesktopWindow(), NULL, class, title)`: top-level windows only (a NULL parent
  would also return message-only windows), atoms and NULL arguments with their usual meaning, and
  the last error left unchanged.

SDPlus also patches `MultiByteToWideChar` and `WideCharToMultiByte`, but only to remap code page 874
(Thai); other code pages pass through, so they do not affect input methods.

Test: `tests/findwindow_fix_test.ps1` (after a build) runs the real core and mod against a copy of
SDPlus's routine on a private desktop, with caller fixtures named `SDPlus.dll` and
`generic_input_method.dll`.
