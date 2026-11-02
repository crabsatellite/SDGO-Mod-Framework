# Configured FPS cap (Beta)

The client uses the FPS cap in its own `config.xml` instead of overwriting it
with 60 during initialization. A value of 160 requests a cap of 160 FPS.

| mod | host | part |
|---|---|---|
| `fps_limit` | client | Preserve the native `Framework/Timer@updatefps` value |

## Use

Enable **FPS cap** in SDGO Mod Manager and set the limit there. The Manager reads
and updates the existing client `config.xml`; there is no second FPS setting in
the mod manifest. Close and restart that client after changing the value.
Existing `updatefps="160"` remains 160 when this mod is installed.
Use the matching updated Manager; close an older running Manager before updating
the client. The client Git update also includes the shared Manager executable.

## Settings

| key | default | meaning |
|---|---|---|
| `config.xml`, `Framework/Timer@updatefps` | 60 | Integer FPS cap, 1–1000; takes effect at client startup |

Missing/invalid numeric values use the native 60 FPS fallback. This is a cap,
not a promise that the computer will reach that rate. Existing render/vertical
sync settings are preserved and can limit presentation further. The mod does
not change resolution or the game's time source.

## Logs and troubleshooting

`fps_limit: config.xml updatefps=160; effective FPS cap=160`

| symptom or log | cause |
|---|---|
| Mod skipped because a symbol did not verify | Unsupported or modified client; no FPS hook is installed |
| `invalid value; using 60` | Parsed cap is outside 1–1000 |
| Setting saved, running game unchanged | Restart the selected client |
| Effective cap is 160, measured FPS is lower | Check load, presentation sync and the actual render rate |

## How it works

On the supported client, the native XML reader fills `CApplication+0x80`, then
the instruction at `fps.config.force60` overwrites it with 60. A verified site
hook skips exactly that ten-byte instruction and preserves native CPU, flags
and floating-point state. The original startup code creates the timer with
`interval = 1.0f / configured_fps`. A different signature disables the mod.

Validation distinguishes native core/mod fixtures from actual game rendering
and multiplayer timing. Full combat acceptance at higher rates remains a
separate check; Beta does not claim that acceptance.

The eleven real-core/production-DLL fixture cases cover 160/120/60, range
boundaries, invalid-value fallback, disabled mode and rejected symbols, including
registers, flags, floating-point state, last error and unrelated memory guards.
Run `ctest --test-dir build -C Release -R "^fps_limit_runtime$" --output-on-failure`.
