# Configured FPS cap (Beta)

The client uses the FPS cap in its own `config.xml` instead of overwriting it
with 60 during initialization. A value of 160 requests a cap of 160 FPS.

| mod | host | part |
|---|---|---|
| `fps_limit` | client | Use `Framework/Timer@updatefps` for logic updates and the rendering ceiling |

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
not a promise that the computer will reach that rate. The same value sets the
native logic-update frequency and a separate ceiling at the native Present
wrapper. Vertical sync and rendering load can limit presentation further. The mod does
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
`interval = 1.0f / configured_fps`. Version 1.0.2 checks that interval and the
native independent-render mode at the original app-idle boundary. It writes only
when a native state transition has reset a value; steady idle calls do not
rewrite it. Accumulated time and the clock source remain native.

The verified `fps.device.present` wrapper is paced immediately before its
original Present call using QueryPerformanceCounter and an unnamed waitable
timer (high resolution where available). Each deadline starts from the actual
presentation start, preventing catch-up bursts after stalls. The original
graphics object, window, HRESULT and LastError behavior are preserved.
The passive `SdgoFpsLimitGetSnapshotV1` observer reports the configured update
and render targets, actual pacing period and wait count for tests.
A different signature disables the mod.

The account-free [native GameTest matrix](game-test.md) runs the real login page
with the mod disabled at configured 160, then enabled at 30, 60, 120 and 160.
It retains all sample-window averages and checks actual update cadence,
successful D3D9 presentations, their minimum spacing and the production pacing
period. The 30 FPS case must exercise real waits. A lower measured render rate
does not invalidate a ceiling; it is retained in the report.
Full combat acceptance at higher rates remains a
separate check; Beta does not claim that acceptance.

The eleven real-core/production-DLL fixture cases cover 160/120/60, range
boundaries, invalid-value fallback, disabled mode and rejected symbols, including
registers, flags, floating-point state, last error and unrelated memory guards.
They also check the native Present ABI, real waits and stall recovery.
Run `ctest --test-dir build -C Release -R "^fps_limit_runtime$" --output-on-failure`.
