# Native login recovery

[简体中文](login-recovery.zh-CN.md) · [Back to the feature index](../README.md)

## What it does

After a dropped connection, the original server can reject the player's next login as already
online. `login_recovery` completes the server's own recovery in that case. After the original authenticated login has
cleared the previous session, it notifies the old Agent connection and repeats
the native authentication once in the same request.

Normal logins, incorrect passwords, bans and other authentication failures keep
their original handling. The Mod does not create accounts or change inventory,
currencies or credentials. It does not repair unrelated network interruptions.

## Use

Install the matching framework server files on WorldServer and restart that
host. In Mod Manager, find **异常断线登录恢复** under **修复归档** (fix archive).
The fix is active by default and has no gameplay settings. Disabling it also
requires restarting the affected host. Players use the server owner's client
package or update; this fix does not require a separate player application.

## Logs

`native authenticated old-session recovery active` confirms that the fix loaded.
`retrying authentication once in the same request` records the recovery branch;
it is not a substitute for the subsequent native login result. No password or
login packet is logged.

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
