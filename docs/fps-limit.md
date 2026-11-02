# FPS cap (Beta)

Set one value in **SDGO Mod Manager** to control the client's logic-update
frequency and rendering ceiling. For example, 160 selects 160 logic updates
per second and a maximum rendering rate of 160 FPS.

The shared logic/render setting requires `fps_limit` 1.0.2 or newer.

[简体中文](fps-limit.zh-CN.md)

| Mod | Install on | Purpose |
|---|---|---|
| `fps_limit` | Client | Use the configured rate instead of the native forced 60 Hz |

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

This setting changes logic updates as well as the rendering ceiling. Set it
to 60 to use 60 Hz logic updates and a 60 FPS rendering cap. Resolution, aspect
ratio and the native clock source are unchanged.

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
| Rendering stays below the limit | Check rendering load, vertical sync and display settings |

Developers can reproduce the parameter and pacing checks with the
[native GameTest FPS suite](game-test.md#fps-cap-without-account-login).
