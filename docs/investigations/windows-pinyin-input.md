# Windows Pinyin: candidate request and stale preedit

2026-11-02 status: the user visually confirmed that 1.1.0 restored the candidate
window, but supplied `自由'yo` as a continuing residual. END cleanup alone was
insufficient. Version 1.1.1 fixes the separate native result-replacement argument
described below; complete game acceptance of that change remains pending.

Supported artifact: `GOnline-54BE1568-00C97000`, SHA-256
`e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92`.
REA `open_binary` verified that identity. Its configured Ghidra/Java installation
was missing, so no new REA decompilation or runtime trace was produced. The native
observations below use retained local Ghidra output (`chunk_049.c`) corroborated
with Capstone disassembly of this current file. These are static observations.

| Native path | Observation |
| --- | --- |
| `0x00C49620` | Creates the hidden EDIT and subclasses it with `0x00C4CA10`; original EDIT procedure at handler `+0x144`, hidden HWND at `+0x134`. |
| `0x00C473F0` | Gives that hidden EDIT input focus. |
| `0x00C4CA10` | Calls the shared filter with pointers to HWND/message/WPARAM/LPARAM; if not consumed, calls the original EDIT procedure with the resulting parameters. |
| `0x00C492F0` | Shared filter, five stack arguments, stdcall `ret 0x14`, boolean return consumed as AL. |
| `0x00C4942C..0x00C49445` | For WM_IME_SETCONTEXT when handler `+0x24` is false, applies `lParam &= 0x3FFFFFF0`, clearing all four candidate request bits. |
| `0x00C47640` | WM_IME_ENDCOMPOSITION handler clears state flags `+0x29/2A`, but leaves preedit at `+0xA8`, length at `+0xB8`. |
| `0x00C48EBB..0x00C48ED8` | Assigns the new result into `+0xA8`, then reads its new length at `+0xB8` as the erase count for replacing the old preedit in `+0xC4`. This uses the wrong range length. |
| `0x00C48F0E..0x00C48F87` | Copies the resulting display into `+0x70` and writes it back to the hidden EDIT. Any old preedit tail has now become committed text. |
| `0x00C48220`, `0x00C4837F..0x00C483B7` | Refreshes text from the hidden EDIT into `+0xC4` and inserts cached preedit; modern layouts can use the default branch. A stale preedit can therefore appear after committed Chinese. |
| `0x00C48FD8..0x00C48FF4` | GCS_COMPSTR already shifts its byte length right once and assigns from a bounded begin/end range. It must not receive the commit-call-only character-count adjustment. |
| `0x00C4C880` | Top-level default handler suppresses IME messages, but normal active text input uses the focused hidden EDIT. No evidence justifies changing this additional path in this patch. |

The old `ime_fix` 1.0.0 was present and loaded in the reported primary-client log;
missing installation was not the explanation for this report. Its result-length
fix alone does not address candidate visibility or the END lifecycle.

The latest 1.1.0 runtime log also confirmed candidate restoration, with no nonempty
END cache-cleanup receipt. That is consistent with RESULT clearing `+0xA8` only
after the bad tail has been copied into committed text. An attempted read-only
OpenProcess/ReadProcessMemory inspection was denied by Windows (error 5); no live
text or event sequence was read. The result-replacement conclusion is established
by the current instructions, independent review, screenshot and controlled replay.

## Result replacement in 1.1.1

The current EXE's block `0x00C48EC0..0x00C48EDC` is 29 bytes. It calls the native
`wstring::replace` at `0x00C47710` with `(this=C4, pos=handler+48, erase=B8,
source=A8, sourcePos=0, sourceCount=UINT_MAX)`. At this point A8/B8 already contain
the **new** result. For `zi'yo` -> `自由`, erase=2 leaves `'yo` in the old display.

The filter now snapshots the old A8 before RESULT or a zero-flag cancellation.
It first requires exact UTF-16 equality with C4 at the current insertion position.
A thread-local scoped snapshot restores the previous scope across nested dispatch.
The replace hook admits only return address `0x00C48EDD`, the same C4/A8 objects,
unchanged position, native source range and result length, and a second exact match
against the saved old preedit. It substitutes only the old preedit erase count;
the original replace implementation and native commit/update path still run.
Mismatches retain native arguments, with a bounded length-only diagnostic receipt.

The controlled test extracts the original block read-only from the local EXE,
verifies host identity and all 29 bytes, relocates only its CALL target into a
fixture, and executes the actual Mod DLL. Original argument instructions reproduce
`自由'yo`; the patched call produces `自由`. Tests cover shorter/longer/equal results,
UTF-16 surrogate units, legitimate Latin/numeric prefix/suffix, zero-flag cancel,
RESULT+COMPSTR in one message, nested scopes, and bypasses for mismatched content,
caller, object, source, position, HWND and non-Chinese layout. Other native callbacks
and containers remain fixtures, so this is not a full-game or actual IME trace.

The candidate/END logic inherited from 1.1.0 runs the original first, and applies only when the recorded
keyboard layout's primary language is Chinese and the HWND matches `+0x134`.
It restores exactly the candidate bits originally requested, without forcing
unrequested UI or changing the original filter return. At END it validates native
SSO/heap shape, clears `+0xA8` with the game's own assign API and resets `+0x54/5C`.
It does not assign committed strings at `+0x70/0xC4`, change EDIT contents, intercept
WM_CHAR, synthesize input, alter Windows settings or re-register the IME.

Microsoft contracts corroborating the repair:

- [WM_IME_SETCONTEXT](https://learn.microsoft.com/en-us/windows/win32/intl/wm-ime-setcontext): UI flags are passed to the default procedure; composition and candidate visibility are separate choices.
- [WM_IME_ENDCOMPOSITION](https://learn.microsoft.com/en-us/windows/win32/intl/wm-ime-endcomposition): applications drawing their own composition process its end.
- [WM_IME_COMPOSITION](https://learn.microsoft.com/en-us/windows/win32/intl/wm-ime-composition): canceled composition must be removed by the drawing application.
- [ImmGetCompositionStringW](https://learn.microsoft.com/en-us/windows/win32/api/imm/nf-imm-immgetcompositionstringw): lengths are bytes, including for Unicode.

Verification for 1.1.0: three actual-DLL ABI fixtures pass, covering the existing scoped
commit fix and foreign byte-count contract, candidate flags, native dispatcher
order/return, END without RESULT, SSO/heap cache cleanup, repeated END, committed
canaries, foreign HWND/non-Chinese bypass, invalid shape rejection and text-free
logs. A script-free Manager overlay import passes against a temporary legacy
installation while preserving other Mods and framework files. All three symbols
match the current executable.

The fixtures mock the original filter/string API according to the independently
checked instructions; they do not execute the game's full input subsystem or a
real Windows Pinyin session. The screenshot's exact IME event sequence is unknown.
The user subsequently confirmed candidate rendering in 1.1.0. Correct result
replacement and absence of residual text in 1.1.1 remain manual acceptance after
normal client restart.

Local delivery readback (2026-11-01): Manager standard import updated the stopped
primary client to 1.1.0, enabled and registered in the Fix archive. Installed Mod
file hashes match `SDGO-IMEFix-1.1.0.zip` (SHA-256
`9cdefafe1a291368e197a60c8021886b75ddf461560c5d5f6002af76d15a8285`).
All 194 previous symbol sections retain their values; two new IME definitions are
added. The loader, core, global settings and 1284 checked unrelated files retain
their hashes. No game was launched, no live input session is claimed, and the
existing Manager retains its built-in older title “输入法提交修复”.

Local delivery readback (2026-11-02): Manager imported 1.1.1 into the stopped
primary client, enabled and registered. The installed DLL SHA-256 is
`25bf23d39f316c684ee6fcb463e110ebe6001575ac02f44c4a738a6a968592e7`;
the standalone package SHA-256 is
`0c12c24d865d62bc1e2a9dc29dbe8834df6c459dbdb94e3acbd9827a9dfebfa4`.
All 383 prior symbol sections retain their values; only the native replace and
exact result caller definitions are added. All 73 checked unrelated files retain
their hashes. Four native/DLL CTest cases, the original-file-backed replay, the
Manager import test and all five exact symbol checks pass. No new game or live
IME session was started by the agent; the residual repair's gameplay result is
still pending user acceptance.
