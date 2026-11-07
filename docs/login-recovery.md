# Native login recovery

[简体中文](login-recovery.zh-CN.md) · [Back to the feature index](../README.md)

## What it does

After a dropped connection, the original server can reject the player's next login as already
online. `login_recovery` completes the server's own recovery in that case. After the original authenticated login has
cleared the previous session, it notifies the old Agent connection and repeats
the native authentication once in the same request.

Normal logins, incorrect passwords, bans and other authentication failures keep
their original handling. The Mod does not create accounts or change inventory,
currencies or credentials. The server and matching client components are Stable.

`login_recovery_client` adds bounded recovery for a pending login that stops
progressing or loses its connection. It uses the native logout and reconnect
business flow, with at most three automatic retries. Each phase waits up to
15 seconds, backoff is 1/2/4 seconds, and the total budget is 90 seconds.
Nickname and other interactive gates pause the clock. Incorrect passwords,
bans and other explicit rejections stop retries. Escape cancels recovery.
When recovery ends, the original login inputs are restored for another attempt.

Old queued replies are discarded before a new connection starts. Duplicate
submissions cannot replace an identity that is still waiting. The client keeps
only the current username and password digest in process memory, clears them
on success, cancellation or termination, and never writes them to files or logs.

## Use

Install the matching framework server files on WorldServer and restart that
host. In Mod Manager, find **异常断线登录恢复** under **修复归档** (fix archive).
The fix is active by default and has no gameplay settings. Disabling it also
requires restarting the affected host. Players use the server owner's client
package or update; this fix does not require a separate player application.
Restart the game after installing the client component. It appears as
**登录等待自动恢复** in the fix archive and needs no configuration.

## Logs

`native authenticated old-session recovery active` confirms that the fix loaded.
`retrying authentication once in the same request` records the recovery branch;
it is not a substitute for the subsequent native login result. No password or
login packet is logged.
Client `native reconnect` and `native retry request` messages report recovery
actions. `interactive native login page restored` confirms actual input widgets;
`native login completed into usable lobby/hangar` confirms that the login flow
has left the waiting stage.

If repeated login failures continue, check the original login result and the
Agent connection. An incompatible host or symbol mismatch prevents this Mod
from activating.

## Developer notes

The supported host is `WorldServer-548A86E3-0010C000`. The hook runs only after
the original duplicate-session branch has authenticated the request and called
`ForceDisconnetUserDB`. It sends the original eight-byte `0x1F4B` Agent logout
notification before repeating authentication, avoiding a later account-based
logout notification after a new session is admitted. A request-local guard
allows one retry; a second conflict follows the original failure path.

The native request uses x86 `thiscall` with no stack arguments. The instruction
hook preserves general registers, the stack and FP/SIMD state. The retry returns
to the original authentication call, so the native SQL procedure and any starter
profile policy retain ownership of authentication and account initialization.

The client invokes the original logout/reconnect business command and rebuilds
the network and login UI. Native Gate release clears pending requests, but the
reconnect command does not guarantee that complete release. Before reopening
login input, the fix therefore explicitly removes `REQ_LOCAL_LOGIN_USER`
through the native request API. The original timeout only marks a request's
timestamp; it does not remove its lock,
so sending another packet alone cannot restore input. Return readiness must
belong to the freshly initialized Gate. Old controls still visible during the
transition cannot acknowledge completion, and a null initialization context is
valid. An authentication reply alone is also insufficient: success requires
actual lobby or hangar entry.

The channel predicate inspects the current owner and real loading, visibility
and pending-request fields. Incomplete channel data and pending joins keep the
watchdog active; ready VS, mission and clan choices pause it. Gate and channel
scenes have different object lifetimes, so the previous Gate is never retained
after that transition.
