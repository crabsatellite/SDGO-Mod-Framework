# FPS cap

Set the rendering ceiling in **SDGO Mod Manager** while keeping the game's
native speed. For example, 160 allows rendering up to 160 FPS; logic updates
remain at the original 60 Hz so movement and AI do not accelerate.

Independent rendering with native gameplay speed requires `fps_limit` 1.0.4 or newer.
Update older versions before selecting a higher cap.

[简体中文](fps-limit.zh-CN.md)

| Mod | Install on | Purpose |
|---|---|---|
| `fps_limit` | Client | Apply the configured rendering cap independently of 60 Hz gameplay |

## Use

1. Close the game and open **SDGO Mod Manager**.
2. Select the client, enable **FPS cap (FPS 上限)**, and save the desired limit.
3. Start the game with its usual launcher. Restart the game after changing the limit.

The Manager updates the existing `config.xml`. Installing the Mod preserves
your current value and other client settings. No server component is required.

## Settings

| Setting | Default | Allowed values | Takes effect |
|---|---|---|---|
| FPS cap | 60 | Integer, 1–1000 | After restarting the client |

The value is stored in `Framework/Timer@updatefps` in `config.xml`; the Mod
does not maintain a separate FPS value. Missing or invalid values use 60.

Only the rendering ceiling follows this setting. Logic updates remain at the
native 60 Hz for every cap, including 30, 60, 120 and 160. Resolution, aspect
ratio and the native clock source are unchanged.

Rendering continues while the simulation catches up after a slow update. This
prevents update debt from hiding otherwise continuing gameplay for several
seconds in a busy AI battle. The original update scheduling and accumulated
time are retained; the configured cap still limits actual Present calls.

The cap is a maximum. Rendering load, vertical sync and display refresh rate
can reduce the rate you see. Raising the cap does not guarantee higher performance.

## Compatibility and troubleshooting

The supported client is `GOnline-54BE1568-00C97000`. An unsupported executable
or changed hook signature causes the Mod to stay disabled. See [setup](setup.md)
for installation and version checks.

Logs are written to `sdgo/logs/sdgo.log` in the client folder. For a setting of 160,
the startup log includes `effective FPS cap=160`.

| Symptom | Check |
|---|---|
| Saved value has no effect | Restart the selected client and check that the Mod is enabled |
| `invalid value; using 60` | Enter an integer from 1 to 1000 |
| Required symbol did not verify | Use a supported client and matching framework files |
| Rendering stays below the limit | Check scene complexity, AI load, vertical sync and display settings |
| AI keeps moving while the picture freezes for seconds | Update and enable `fps_limit` 1.0.4 or newer, then restart the client |

For performance diagnosis, enable **帧率采样日志** under **Show developer information**
and restart the client. `render FPS sample` records a five-second average of
successful Present calls. Occluded or failed submissions are counted separately;
this measures rendering submissions rather than the monitor's physical refresh
rate. The diagnostic is off by default and can be disabled after testing.

Developers can reproduce the parameter and pacing checks with the
[native GameTest FPS suite](game-test.md#fps-cap-without-account-login).
