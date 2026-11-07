"""Run sealed negative native login-recovery acceptance scenarios.

Each scenario must finish as an ordinary failing GameTest only after the
production Mod restores the real credential-free login UI. The wrapper then
independently validates that expected failure, terminal snapshot, blind relay
receipt, copied-runtime seals and source account preservation.
"""
from __future__ import annotations

import argparse
import configparser
import ctypes
import dataclasses
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import sys
from typing import Iterable

if str(Path(__file__).resolve().parents[1]) not in sys.path:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools import game_test
from tools import game_test_login_recovery as recovery
from tools.game_test_login_relay import (
    Endpoint,
    FaultMode,
    LoginFaultRelay,
    RelayConfig,
    parse_endpoint,
    process_identity,
)


TERMINAL_PATTERN = re.compile(
    r"login_recovery_terminal "
    r"generation=(?P<generation>[1-9][0-9]*) "
    r"state=(?P<state>[0-9]+) "
    r"outcome=(?P<outcome>[0-9]+) "
    r"retries=(?P<retries>[0-9]+) "
    r"retry_sends_delta=(?P<retry_sends>[0-9]+) "
    r"rebuilds_delta=(?P<rebuilds>[0-9]+) "
    r"connection_delta=(?P<connection>[0-9]+) "
    r"auth_replies_delta=(?P<auth_replies>[0-9]+) "
    r"reject=(?P<reject>[0-9]+) "
    r"app=(?P<app>[0-9]+) "
    r"gate=(?P<gate>[0-9]+) "
    r"ready=(?P<ready>[0-9]+) "
    r"credentials=(?P<credentials>[0-9]+) "
    r"active=(?P<active>[0-9]+) "
    r"interaction=(?P<interaction>[0-9]+)\Z"
)
SNAPSHOT_PATTERN = re.compile(
    r"login_recovery_snapshot "
    r"generation=(?P<generation>[0-9]+) "
    r"state=(?P<state>[0-9]+) "
    r"outcome=(?P<outcome>[0-9]+) "
    r"retries=(?P<retries>[0-9]+) "
    r"action=(?P<action>[0-9]+) "
    r"connection=(?P<connection>[0-9]+) "
    r"rebuilds=(?P<rebuilds>[0-9]+) "
    r"retry_sends=(?P<retry_sends>[0-9]+) "
    r"auth_replies=(?P<auth_replies>[0-9]+) "
    r"reject=(?P<reject>[0-9]+) "
    r"app=(?P<app>[0-9]+) "
    r"gate=(?P<gate>[0-9]+) "
    r"purged=(?P<purged>[0-9]+) "
    r"dropped=(?P<dropped>[0-9]+) "
    r"credentials=(?P<credentials>[0-9]+) "
    r"ready=(?P<ready>[0-9]+) "
    r"active=(?P<active>[0-9]+) "
    r"interaction=(?P<interaction>[0-9]+)\Z"
)
EXPECTED_TERMINAL_ASSERT = (
    "production login recovery restored the native login page "
    "after a terminal outcome"
)


class LoginTerminalGameTestError(RuntimeError):
    """A terminal scenario or its independent evidence is incomplete."""


@dataclasses.dataclass(frozen=True)
class TerminalScenario:
    name: str
    relay_mode: str
    minimum_retries: int
    expected_state: int
    expected_outcome: int
    expected_retries: int
    expected_reject: int
    wrong_password: bool = False
    cancel: bool = False

    @property
    def faulted(self) -> bool:
        return self.relay_mode != FaultMode.PASS_THROUGH


SCENARIOS = {
    scenario.name: scenario
    for scenario in (
        TerminalScenario(
            "exhaustion",
            FaultMode.ALL_SERVER_BLACKHOLE,
            minimum_retries=1,
            expected_state=8,
            expected_outcome=7,
            expected_retries=3,
            expected_reject=0,
        ),
        TerminalScenario(
            "wrong-password",
            FaultMode.PASS_THROUGH,
            minimum_retries=0,
            expected_state=8,
            expected_outcome=3,
            expected_retries=0,
            expected_reject=1001,
            wrong_password=True,
        ),
        TerminalScenario(
            "cancel",
            FaultMode.ALL_SERVER_BLACKHOLE,
            minimum_retries=1,
            expected_state=7,
            expected_outcome=2,
            expected_retries=0,
            expected_reject=0,
            cancel=True,
        ),
    )
}


def _event_messages(events: Iterable[dict], case: str) -> list[str]:
    return recovery._event_messages(events, case)


def _fault_handshake(login_messages: list[str], faulted: bool) -> None:
    ready = recovery.READY_TO_SEND_MESSAGE
    ack = recovery.CONTROLLER_ACK_MESSAGE
    initial = recovery.INITIAL_SEND_MESSAGE
    transports = [
        message
        for message in login_messages
        if message.startswith(
            "login_recovery_native_transport "
        )
    ]
    if login_messages.count(initial) != 1:
        raise LoginTerminalGameTestError(
            "terminal evidence does not contain exactly one native initial login send"
        )
    if (
        len(transports) != 1
        or recovery.TRANSPORT_PATTERN.fullmatch(
            transports[0]
        ) is None
    ):
        raise LoginTerminalGameTestError(
            "terminal evidence does not contain one successful in-process native 0x2329/0x4D login transport"
        )
    transport = transports[0]
    if faulted:
        if (
            login_messages.count(ready) != 1
            or login_messages.count(ack) != 1
            or login_messages.index(ready) >= login_messages.index(ack)
            or login_messages.index(ack) >= login_messages.index(transport)
            or login_messages.index(transport)
            >= login_messages.index(initial)
        ):
            raise LoginTerminalGameTestError(
                "terminal fault did not use the sealed ready/ack/native-transport/send ordering"
            )
    elif ready in login_messages or ack in login_messages:
        raise LoginTerminalGameTestError(
            "pass-through terminal scenario unexpectedly waited for a fault arm"
        )
    elif (
        login_messages.index(transport)
        >= login_messages.index(initial)
    ):
        raise LoginTerminalGameTestError(
            "pass-through terminal transport observation did not precede its send receipt"
        )


def validate_terminal_native_evidence(
    events_path: Path,
    *,
    run_id: str,
    scenario: TerminalScenario,
) -> dict:
    events = recovery._read_jsonl(events_path)
    if any(event.get("run_id") != run_id for event in events):
        raise LoginTerminalGameTestError(
            "terminal native evidence contains another run identity"
        )
    login_messages = _event_messages(events, "flow.login")
    _fault_handshake(login_messages, scenario.faulted)

    captures = [
        message
        for message in login_messages
        if message.startswith("login_recovery_native_capture ")
    ]
    if len(captures) != 1:
        raise LoginTerminalGameTestError(
            "terminal evidence lacks one production initial-request capture"
        )
    captured = recovery.CAPTURE_PATTERN.fullmatch(captures[0])
    if captured is None:
        raise LoginTerminalGameTestError(
            "terminal native request capture has an incompatible schema"
        )

    terminal_messages = [
        message
        for message in login_messages
        if message.startswith("login_recovery_terminal ")
    ]
    if len(terminal_messages) != 1:
        raise LoginTerminalGameTestError(
            "terminal evidence does not contain exactly one restored-login proof"
        )
    matched = TERMINAL_PATTERN.fullmatch(terminal_messages[0])
    if matched is None:
        raise LoginTerminalGameTestError(
            "terminal production observation has an incompatible schema"
        )
    proof = {
        key: int(value)
        for key, value in matched.groupdict().items()
    }
    if proof["generation"] != int(captured.group("generation")):
        raise LoginTerminalGameTestError(
            "terminal generation differs from the captured native request"
        )
    if (
        proof["state"] != scenario.expected_state
        or proof["outcome"] != scenario.expected_outcome
        or proof["retries"] != scenario.expected_retries
        or proof["retry_sends"] != scenario.expected_retries
        or proof["reject"] != scenario.expected_reject
    ):
        raise LoginTerminalGameTestError(
            "production terminal policy result differs from the sealed scenario"
        )
    if (
        proof["app"] != 4
        or proof["gate"] not in (1, 3)
        or proof["ready"] != 1
        or proof["credentials"] != 0
        or proof["active"] != 0
        or proof["interaction"] != 0
    ):
        raise LoginTerminalGameTestError(
            "terminal recovery did not restore the real credential-free login UI"
        )
    minimum_rebuilds = (
        scenario.expected_retries
        if scenario.name == "exhaustion"
        else (1 if scenario.cancel else 0)
    )
    if proof["rebuilds"] < minimum_rebuilds:
        raise LoginTerminalGameTestError(
            "terminal recovery lacks the required native connection rebuilds"
        )
    if (
        scenario.name == "exhaustion"
        and (
            proof["connection"] < 3
            or proof["auth_replies"] != 0
        )
    ):
        raise LoginTerminalGameTestError(
            "retry exhaustion did not remain a three-retry transport failure"
        )
    if (
        scenario.wrong_password
        and proof["auth_replies"] < 1
    ):
        raise LoginTerminalGameTestError(
            "wrong-password terminal proof lacks a genuine native auth rejection"
        )
    if scenario.cancel and proof["auth_replies"] != 0:
        raise LoginTerminalGameTestError(
            "cancel scenario received an auth reply before the owned Escape"
        )

    fail_asserts = [
        event
        for event in events
        if event.get("event") == "assert"
        and event.get("case") == "flow.login"
        and event.get("status") == "fail"
        and event.get("message") == EXPECTED_TERMINAL_ASSERT
    ]
    case_ends = [
        event
        for event in events
        if event.get("event") == "case_end"
        and event.get("case") == "flow.login"
        and event.get("status") == "fail"
    ]
    run_ends = [
        event
        for event in events
        if event.get("event") == "run_end"
        and event.get("status") == "fail"
    ]
    if (
        len(fail_asserts) != 1
        or len(case_ends) != 1
        or len(run_ends) != 1
    ):
        raise LoginTerminalGameTestError(
            "restored terminal outcome was not returned as one ordinary GameTest failure"
        )
    terminal_index = next(
        index
        for index, event in enumerate(events)
        if event.get("event") == "observation"
        and event.get("message") == terminal_messages[0]
    )
    assert_index = events.index(fail_asserts[0])
    case_index = events.index(case_ends[0])
    run_index = events.index(run_ends[0])
    if not terminal_index < assert_index < case_index < run_index:
        raise LoginTerminalGameTestError(
            "terminal proof was not emitted before the expected ordinary-suite failure"
        )
    return proof


def validate_terminal_relay_evidence(
    receipt_path: Path,
    *,
    run_id: str,
    scenario: TerminalScenario,
) -> dict:
    events = recovery._read_jsonl(receipt_path)
    if (
        not events
        or events[0].get("event") != "relay_started"
        or events[-1].get("event") != "relay_stopped"
    ):
        raise LoginTerminalGameTestError(
            "terminal relay receipt lacks its complete bounded lifecycle"
        )
    if any(event.get("run_id") != run_id for event in events):
        raise LoginTerminalGameTestError(
            "terminal relay receipt contains another run identity"
        )
    if events[0].get("mode") != scenario.relay_mode:
        raise LoginTerminalGameTestError(
            "terminal relay mode differs from the sealed scenario"
        )
    forbidden = {
        "payload",
        "data",
        "bytes_content",
        "username",
        "password",
        "credential",
    }
    if any(not forbidden.isdisjoint(event) for event in events):
        raise LoginTerminalGameTestError(
            "terminal relay receipt contains payload or credential fields"
        )
    generations = [
        event
        for event in events
        if event.get("event") == "generation_accepted"
    ]
    upstream = [
        event
        for event in events
        if event.get("event") == "upstream_connected"
    ]
    if not generations or not upstream:
        raise LoginTerminalGameTestError(
            "terminal relay did not carry a genuine native connection"
        )

    controller = [
        event
        for event in events
        if event.get("event") == "controller_fault_armed"
    ]
    blackholes = [
        event
        for event in events
        if event.get("event") == "server_blackhole_armed"
    ]
    dropped = [
        event
        for event in events
        if event.get("event") == "server_bytes_dropped"
    ]
    if scenario.faulted:
        if (
            len(controller) != 1
            or controller[0].get("trigger")
            != "sealed_ready_to_send_observation"
            or not blackholes
            or not dropped
            or controller[0].get("sequence", 0)
            >= min(event.get("sequence", 0) for event in blackholes)
        ):
            raise LoginTerminalGameTestError(
                "terminal transport fault was not armed after sealed UI readiness and applied to native traffic"
            )
        minimum_generations = (
            4 if scenario.name == "exhaustion" else 1
        )
        if (
            len(generations) < minimum_generations
            or len(blackholes) < minimum_generations
        ):
            raise LoginTerminalGameTestError(
                "terminal relay did not observe every required native retry generation"
            )
        required_blackholes = sorted(
            blackholes,
            key=lambda event: event.get("generation", 0),
        )[:minimum_generations]
        if any(
            type(event.get("client_bytes")) is not int
            or event.get("client_bytes", 0) <= 0
            for event in required_blackholes
        ):
            raise LoginTerminalGameTestError(
                "a required retry generation did not expose positive client wire traffic before blackholing its reply"
            )
    elif controller or blackholes or dropped:
        raise LoginTerminalGameTestError(
            "wrong-password pass-through unexpectedly injected a transport fault"
        )
    return {
        "mode": scenario.relay_mode,
        "generation_count": len(generations),
        "upstream_connection_count": len(upstream),
        "controller_arm_count": len(controller),
        "blackhole_count": len(blackholes),
        "dropped_server_event_count": len(dropped),
    }


def validate_expected_report_failure(
    report: dict,
    *,
    run_dir: Path,
) -> None:
    if report.get("status") != "fail":
        raise LoginTerminalGameTestError(
            "terminal scenario did not return the expected ordinary GameTest failure"
        )
    layers = report.get("layers", {})
    source = layers.get("source", {})
    launch = layers.get("launch", {})
    native = layers.get("native", {})
    if (
        source.get("status") != "bound"
        or launch.get("status") != "pass"
        or native.get("status") != "fail"
        or native.get("cases", {})
        .get("flow.login", {})
        .get("status") != "fail"
    ):
        raise LoginTerminalGameTestError(
            "terminal result was mixed with a source, launch or infrastructure failure"
        )
    metadata = game_test.validate_metadata(run_dir)
    game_test.verify_hashes(metadata)
    execution = metadata.get("execution", {})
    if execution.get("status") != "completed":
        raise LoginTerminalGameTestError(
            "terminal GameTest did not complete before owned runtime cleanup"
        )


def archive_negative_evidence(
    *,
    run_dir: Path,
    run_id: str,
    report: dict,
    summary: dict,
    evidence_paths: Iterable[Path],
) -> Path:
    archive = (
        run_dir.resolve().parent
        / "login-terminal-evidence"
        / run_id
    )
    if archive.exists():
        raise LoginTerminalGameTestError(
            f"refusing to overwrite terminal evidence archive: {archive}"
        )
    archive.mkdir(parents=True)
    files = [
        run_dir.resolve() / "run.json",
        Path(summary["native_events"]),
        *evidence_paths,
    ]
    copied: list[Path] = []
    try:
        for source in files:
            source = source.resolve()
            if not source.is_file():
                raise LoginTerminalGameTestError(
                    f"terminal evidence file is missing: {source}"
                )
            destination = archive / source.name
            if destination.exists():
                raise LoginTerminalGameTestError(
                    f"terminal evidence basename collides: {source.name}"
                )
            shutil.copy2(source, destination)
            copied.append(destination)
        report_path = archive / "ordinary-report.json"
        report_path.write_text(
            json.dumps(report, indent=2, sort_keys=True)
            + "\n",
            encoding="utf-8",
        )
        copied.append(report_path)
        summary_path = archive / "negative-summary.json"
        archived_summary = {
            key: value
            for key, value in summary.items()
            if key != "native_events"
        }
        summary_path.write_text(
            json.dumps(
                archived_summary,
                indent=2,
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )
        copied.append(summary_path)
        manifest = {
            "schema": 1,
            "run_id": run_id,
            "files": {
                path.name: game_test.sha256(path)
                for path in copied
            },
        }
        manifest_path = archive / "manifest.json"
        manifest_path.write_text(
            json.dumps(manifest, indent=2, sort_keys=True)
            + "\n",
            encoding="utf-8",
        )
        copied.append(manifest_path)
    except BaseException:
        for path in reversed(copied):
            path.unlink(missing_ok=True)
        try:
            archive.rmdir()
        except OSError:
            pass
        raise
    return archive


def _wrong_digest(profile: configparser.ConfigParser) -> str:
    password = profile["account"]["password"]
    password_format = profile.get(
        "account", "password_format", fallback="plain"
    )
    if password_format == "md5":
        valid = password.casefold()
    else:
        valid = hashlib.md5(
            password.encode("ascii"),
            usedforsecurity=False,
        ).hexdigest()
    for suffix in range(1, 100):
        candidate = hashlib.md5(
            (
                "SDGO GameTest intentionally wrong login recovery "
                + str(suffix)
            ).encode("ascii"),
            usedforsecurity=False,
        ).hexdigest()
        if candidate != valid:
            return candidate
    raise LoginTerminalGameTestError(
        "could not derive a known-wrong native MD5 credential"
    )


def _make_wrong_password_profile(
    source: Path,
    target: Path,
) -> bytes:
    source = source.resolve()
    target = target.resolve()
    if target.exists():
        raise LoginTerminalGameTestError(
            f"refusing to overwrite task profile: {target}"
        )
    original = source.read_bytes()
    profile = game_test.load_account_profile(source)
    username = profile["account"]["username"]
    profile["account"]["password"] = _wrong_digest(profile)
    profile["account"]["password_format"] = "md5"
    if not profile.has_section("registration"):
        profile.add_section("registration")
    profile["registration"]["state"] = "existing_account"
    game_test.validate_account_credentials(
        profile,
        username,
        profile["account"]["password"],
    )
    target.parent.mkdir(parents=True, exist_ok=True)
    game_test.write_ini(target, profile)
    copied = game_test.load_account_profile(target)
    if (
        copied["account"]["username"] != username
        or copied.get("registration", "state")
        != "existing_account"
    ):
        target.unlink(missing_ok=True)
        raise LoginTerminalGameTestError(
            "private wrong-password profile did not preserve the existing account identity"
        )
    return original


def _read_quiet_desktop(receipt: Path) -> str | None:
    try:
        value = json.loads(receipt.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    proof = value.get("proof")
    if not isinstance(proof, dict) or proof.get("verified") is not True:
        return None
    name = proof.get("desktop_name")
    return name if isinstance(name, str) and name else None


NATIVE_GAME_SHA256 = (
    "e2dba73c10302b50cb0e6f2e58dd0a14"
    "effa8d224989ae570a7f8e05af79ff92"
)
NATIVE_GAME_HOST_KEY = "GOnline-54BE1568-00C97000"
NATIVE_APP_SINGLETON_RVA = 0xC05604
NATIVE_APP_WINDOW_OFFSET = 0x3C
NATIVE_IMAGE_SIZE = 0x00C97000


def _select_owned_cancel_window(
    windows: list[dict],
    *,
    pid: int,
    authoritative_hwnd: int,
) -> dict:
    matches = [
        window
        for window in windows
        if window.get("pid") == pid
        and window.get("hwnd") == authoritative_hwnd
        and window.get("owner_hwnd") == 0
    ]
    if len(matches) != 1:
        raise LoginTerminalGameTestError(
            "the authoritative native application HWND is not one unique top-level window for the owned PID on the sealed quiet desktop"
        )
    return matches[0]


def _read_owned_native_app_hwnd(
    *,
    pid: int,
    expected_path: Path,
) -> dict:
    if os.name != "nt":
        raise LoginTerminalGameTestError(
            "native application HWND readback requires Windows"
        )
    expected_path = expected_path.resolve()
    if (
        game_test.HOST_KEY != NATIVE_GAME_HOST_KEY
        or game_test.sha256(expected_path)
        != NATIVE_GAME_SHA256
    ):
        raise LoginTerminalGameTestError(
            "cancel HWND readback supports only the pinned GOnline client fingerprint"
        )
    current = game_test.process_image_path(pid)
    identity_before = process_identity(pid)
    if (
        not current
        or Path(current).resolve() != expected_path
        or identity_before is None
    ):
        raise LoginTerminalGameTestError(
            "cancel HWND readback target is not the verified owned game process"
        )

    from ctypes import wintypes

    class ModuleEntry32W(ctypes.Structure):
        _fields_ = (
            ("dwSize", wintypes.DWORD),
            ("th32ModuleID", wintypes.DWORD),
            ("th32ProcessID", wintypes.DWORD),
            ("GlblcntUsage", wintypes.DWORD),
            ("ProccntUsage", wintypes.DWORD),
            ("modBaseAddr", ctypes.POINTER(wintypes.BYTE)),
            ("modBaseSize", wintypes.DWORD),
            ("hModule", wintypes.HMODULE),
            ("szModule", wintypes.WCHAR * 256),
            ("szExePath", wintypes.WCHAR * 260),
        )

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = (
        wintypes.DWORD,
        wintypes.BOOL,
        wintypes.DWORD,
    )
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel.CloseHandle.restype = wintypes.BOOL
    kernel.CreateToolhelp32Snapshot.argtypes = (
        wintypes.DWORD,
        wintypes.DWORD,
    )
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.Module32FirstW.argtypes = (
        wintypes.HANDLE,
        ctypes.POINTER(ModuleEntry32W),
    )
    kernel.Module32FirstW.restype = wintypes.BOOL
    kernel.Module32NextW.argtypes = (
        wintypes.HANDLE,
        ctypes.POINTER(ModuleEntry32W),
    )
    kernel.Module32NextW.restype = wintypes.BOOL
    kernel.ReadProcessMemory.argtypes = (
        wintypes.HANDLE,
        wintypes.LPCVOID,
        wintypes.LPVOID,
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_size_t),
    )
    kernel.ReadProcessMemory.restype = wintypes.BOOL

    process = kernel.OpenProcess(
        0x0010 | 0x1000,
        False,
        pid,
    )
    if not process:
        raise LoginTerminalGameTestError(
            "verified owned game process could not be opened read-only"
        )
    snapshot = kernel.CreateToolhelp32Snapshot(
        0x00000008 | 0x00000010,
        pid,
    )
    invalid_handle = ctypes.c_void_p(-1).value
    snapshot_value = (
        snapshot
        if isinstance(snapshot, int)
        else int(getattr(snapshot, "value", 0) or 0)
    )
    if not snapshot_value or snapshot_value == invalid_handle:
        kernel.CloseHandle(process)
        raise LoginTerminalGameTestError(
            "owned game module snapshot could not be opened read-only"
        )
    try:
        entry = ModuleEntry32W()
        entry.dwSize = ctypes.sizeof(entry)
        found_base = 0
        if kernel.Module32FirstW(
            snapshot,
            ctypes.byref(entry),
        ):
            while True:
                if entry.szModule.casefold() == "gonline.exe":
                    found_base = int(
                        ctypes.cast(
                            entry.modBaseAddr,
                            ctypes.c_void_p,
                        ).value
                        or 0
                    )
                    break
                if not kernel.Module32NextW(
                    snapshot,
                    ctypes.byref(entry),
                ):
                    break
        if not found_base:
            raise LoginTerminalGameTestError(
                "pinned GOnline module base is absent from the owned process"
            )

        def read_u32(address: int) -> int:
            value = ctypes.c_uint32()
            read = ctypes.c_size_t()
            if (
                not kernel.ReadProcessMemory(
                    process,
                    ctypes.c_void_p(address),
                    ctypes.byref(value),
                    ctypes.sizeof(value),
                    ctypes.byref(read),
                )
                or read.value != ctypes.sizeof(value)
            ):
                raise LoginTerminalGameTestError(
                    "authoritative native application HWND chain could not be read"
                )
            return int(value.value)

        app = read_u32(
            found_base + NATIVE_APP_SINGLETON_RVA
        )
        if not 0x10000 <= app <= 0x7FFFFFFF:
            raise LoginTerminalGameTestError(
                "native application singleton pointer is outside the 32-bit user address range"
            )
        vtable = read_u32(app)
        if not (
            found_base
            <= vtable
            < found_base + NATIVE_IMAGE_SIZE
        ):
            raise LoginTerminalGameTestError(
                "native application singleton vtable is outside the pinned GOnline image"
            )
        hwnd = read_u32(
            app + NATIVE_APP_WINDOW_OFFSET
        )
        if not hwnd:
            raise LoginTerminalGameTestError(
                "native application singleton has no main HWND"
            )
    finally:
        kernel.CloseHandle(snapshot)
        kernel.CloseHandle(process)

    identity_after = process_identity(pid)
    current = game_test.process_image_path(pid)
    if (
        identity_after != identity_before
        or not current
        or Path(current).resolve() != expected_path
    ):
        raise LoginTerminalGameTestError(
            "owned game process identity changed during native HWND readback"
        )
    return {
        "hwnd": hwnd,
        "process_identity": identity_before,
        "module_base": found_base,
        "app": app,
        "vtable": vtable,
    }


def send_escape_to_owned_game(
    *,
    pid: int,
    expected_path: Path,
    desktop_name: str,
    receipt_path: Path,
    run_id: str,
    trigger: dict,
) -> None:
    if os.name != "nt":
        raise LoginTerminalGameTestError(
            "owned native Escape cancellation requires Windows"
        )
    native_app = _read_owned_native_app_hwnd(
        pid=pid,
        expected_path=expected_path,
    )
    from ctypes import wintypes

    user = ctypes.WinDLL("user32", use_last_error=True)
    user.OpenDesktopW.argtypes = (
        wintypes.LPCWSTR,
        wintypes.DWORD,
        wintypes.BOOL,
        wintypes.DWORD,
    )
    user.OpenDesktopW.restype = wintypes.HANDLE
    user.CloseDesktop.argtypes = (wintypes.HANDLE,)
    user.CloseDesktop.restype = wintypes.BOOL
    user.EnumDesktopWindows.argtypes = (
        wintypes.HANDLE,
        ctypes.c_void_p,
        wintypes.LPARAM,
    )
    user.EnumDesktopWindows.restype = wintypes.BOOL
    user.GetWindowThreadProcessId.argtypes = (
        wintypes.HWND,
        ctypes.POINTER(wintypes.DWORD),
    )
    user.GetWindowThreadProcessId.restype = wintypes.DWORD
    user.GetClassNameW.argtypes = (
        wintypes.HWND,
        wintypes.LPWSTR,
        ctypes.c_int,
    )
    user.GetClassNameW.restype = ctypes.c_int
    user.GetWindowTextW.argtypes = (
        wintypes.HWND,
        wintypes.LPWSTR,
        ctypes.c_int,
    )
    user.GetWindowTextW.restype = ctypes.c_int
    user.IsWindowVisible.argtypes = (wintypes.HWND,)
    user.IsWindowVisible.restype = wintypes.BOOL
    user.GetWindow.argtypes = (wintypes.HWND, wintypes.UINT)
    user.GetWindow.restype = wintypes.HWND
    user.PostMessageW.argtypes = (
        wintypes.HWND,
        wintypes.UINT,
        wintypes.WPARAM,
        wintypes.LPARAM,
    )
    user.PostMessageW.restype = wintypes.BOOL
    user.GetForegroundWindow.restype = wintypes.HWND

    desktop = user.OpenDesktopW(
        desktop_name,
        0,
        False,
        0x0001,
    )
    if not desktop:
        raise LoginTerminalGameTestError(
            "verified quiet GameTest desktop could not be opened read-only"
        )
    windows: list[dict] = []
    callback_type = ctypes.WINFUNCTYPE(
        wintypes.BOOL,
        wintypes.HWND,
        wintypes.LPARAM,
    )

    @callback_type
    def visit(hwnd, _parameter):
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(
            hwnd, ctypes.byref(owner)
        )
        if int(owner.value) == pid:
            class_name = ctypes.create_unicode_buffer(64)
            title = ctypes.create_unicode_buffer(128)
            user.GetClassNameW(
                hwnd, class_name, len(class_name)
            )
            user.GetWindowTextW(
                hwnd, title, len(title)
            )
            windows.append({
                "hwnd": int(hwnd),
                "pid": int(owner.value),
                "owner_hwnd": int(
                    user.GetWindow(hwnd, 4) or 0
                ),
                "class_name": class_name.value,
                "title": title.value,
                "visible": bool(
                    user.IsWindowVisible(hwnd)
                ),
            })
        return True

    try:
        if not user.EnumDesktopWindows(desktop, visit, 0):
            raise LoginTerminalGameTestError(
                "owned game windows could not be enumerated on the quiet desktop"
            )
    finally:
        user.CloseDesktop(desktop)
    target = _select_owned_cancel_window(
        windows,
        pid=pid,
        authoritative_hwnd=native_app["hwnd"],
    )
    hwnd = target["hwnd"]
    repeated_native_app = _read_owned_native_app_hwnd(
        pid=pid,
        expected_path=expected_path,
    )
    owner = wintypes.DWORD()
    user.GetWindowThreadProcessId(
        wintypes.HWND(hwnd),
        ctypes.byref(owner),
    )
    if (
        repeated_native_app != native_app
        or int(owner.value) != pid
        or user.GetWindow(wintypes.HWND(hwnd), 4)
    ):
        raise LoginTerminalGameTestError(
            "cancel target identity changed after sealed quiet-desktop enumeration"
        )
    foreground_before = int(user.GetForegroundWindow() or 0)
    if not user.PostMessageW(hwnd, 0x0100, 0x1B, 1):
        raise LoginTerminalGameTestError(
            "WM_KEYDOWN/VK_ESCAPE could not be posted to the owned game window"
        )
    if not user.PostMessageW(
        hwnd, 0x0101, 0x1B, 0xC0000001
    ):
        raise LoginTerminalGameTestError(
            "WM_KEYUP/VK_ESCAPE could not be posted to the owned game window"
        )
    foreground_after = int(user.GetForegroundWindow() or 0)
    if foreground_after != foreground_before:
        raise LoginTerminalGameTestError(
            "owned cancel changed the user's foreground window"
        )
    receipt = {
        "schema": 1,
        "run_id": run_id,
        "event": "owned_escape_posted",
        "pid": pid,
        "hwnd": hwnd,
        "desktop_name": desktop_name,
        "window_class": target["class_name"],
        "window_title": target["title"],
        "window_visible": target["visible"],
        "native_module_base": native_app["module_base"],
        "native_app": native_app["app"],
        "native_vtable": native_app["vtable"],
        "process_identity": native_app[
            "process_identity"
        ],
        "foreground_unchanged": True,
        "trigger_generation": trigger["generation"],
        "trigger_state": trigger["state"],
        "trigger_active": trigger["active"],
        "created_utc": game_test.utc_now(),
    }
    with receipt_path.open(
        "x", encoding="utf-8", newline="\n"
    ) as stream:
        json.dump(receipt, stream, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())


def prepare_and_run(
    *,
    client_template: Path,
    stage: Path,
    run_dir: Path,
    account_profile: Path,
    upstream: Endpoint,
    scenario: TerminalScenario,
    relay_host: str = "127.91.3.2",
    target_server: str = "1",
    target_channel: str = "1",
    timeout: float = 300.0,
    asset_baseline: Path | None = None,
) -> dict:
    upstream = upstream.validate(allow_zero=False)
    recovery.preflight_upstream(upstream)
    run_dir = run_dir.expanduser().resolve()
    relay_host = Endpoint(relay_host, 0).validate(
        allow_zero=True
    ).host
    relay_port = recovery._reserve_loopback_port(relay_host)

    original_profile = account_profile.expanduser().resolve()
    original_bytes: bytes | None = None
    source_lock: tuple[Path, str] | None = None
    task_profile: Path | None = None
    baseline_attempted = False
    baseline_rebound = False
    initial_wrong_profile_rebound = False
    source_lock_race = False
    selected_profile = original_profile
    if scenario.wrong_password:
        source_lock = game_test.acquire_exclusive_lock(
            game_test.account_profile_lock_path(original_profile),
            "source persistent account",
        )
        task_profile = (
            run_dir.parent
            / f".{run_dir.name}.wrong-password-account.ini"
        )
        try:
            original_bytes = _make_wrong_password_profile(
                original_profile,
                task_profile,
            )
        except BaseException:
            task_profile.unlink(missing_ok=True)
            game_test.release_owned_lock(*source_lock)
            raise
        selected_profile = task_profile

    try:
        preparation_options = dict(
            target_host=relay_host,
            login_port=relay_port,
            target_server=target_server,
            target_channel=target_channel,
            retry_count=1,
            expect_first_fail=False,
            account_profile=selected_profile,
            pages_only=True,
            page_group="hangar",
            network_policy="direct",
            quiet=True,
            asset_baseline=asset_baseline,
            extra_mods=("login_recovery_client",),
            login_recovery_min_retries=(
                scenario.minimum_retries
            ),
            login_recovery_hold_after_completion=False,
        )
        if (run_dir / "run.json").is_file():
            if scenario.wrong_password:
                if source_lock is None:
                    raise LoginTerminalGameTestError(
                        "wrong-password reuse lost its source-account guard before rebinding"
                    )
                game_test.release_owned_lock(*source_lock)
                source_lock = None
                reuse_error: BaseException | None = None
                try:
                    metadata = game_test.reuse_prepared(
                        client_template,
                        stage,
                        run_dir,
                        **preparation_options,
                        rebind_existing_account=True,
                    )
                    initial_wrong_profile_rebound = True
                except BaseException as error:
                    reuse_error = error
                try:
                    source_lock = game_test.acquire_exclusive_lock(
                        game_test.account_profile_lock_path(
                            original_profile
                        ),
                        "source persistent account",
                    )
                except BaseException as error:
                    source_lock_race = True
                    raise LoginTerminalGameTestError(
                        "another owner acquired the source persistent account during the bounded wrong-password rebind transaction"
                    ) from error
                if reuse_error is not None:
                    raise reuse_error
            else:
                metadata = game_test.reuse_prepared(
                    client_template,
                    stage,
                    run_dir,
                    **preparation_options,
                )
        else:
            metadata = game_test.prepare(
                client_template,
                stage,
                run_dir,
                **preparation_options,
            )
        run_id = metadata["run_id"]
        receipt_dir = run_dir / "receipts"
        receipt_dir.mkdir(exist_ok=True)
        relay_receipt = receipt_dir / "login-terminal-relay.jsonl"
        controller_ack = (
            receipt_dir / "login-recovery-arm.json"
        )
        cancel_receipt = receipt_dir / "login-cancel.json"
        relay = LoginFaultRelay(
            RelayConfig(
                bind=Endpoint(relay_host, relay_port),
                upstream=upstream,
                mode=scenario.relay_mode,
                receipt=relay_receipt,
                run_id=run_id,
                controller_pid=os.getpid(),
                lifetime_seconds=min(
                    7200.0, max(timeout + 60.0, 180.0)
                ),
                hold_seconds=30.0,
                connect_timeout_seconds=3.0,
                max_generations=8,
                max_bytes_per_direction=16 * 1024 * 1024,
                arm_after_client_bytes=(
                    recovery.MINIMUM_FAULT_WIRE_BYTES
                    if scenario.faulted else 0
                ),
                require_controller_arm=scenario.faulted,
            )
        )

        game_process: dict[str, object] = {}
        fault_armed = False
        cancel_sent = False

        def remember_process(pid: int, path: Path) -> None:
            if path.name.casefold() == game_test.GAME_EXE.casefold():
                game_process.update(
                    pid=pid,
                    path=path.resolve(),
                )

        def relay_health_and_input() -> None:
            nonlocal fault_armed, cancel_sent
            if relay.wait(0):
                raise LoginTerminalGameTestError(
                    "terminal relay stopped before native GameTest completed"
                )
            events_path = Path(metadata["client"]["events"])
            if not events_path.is_file():
                return
            try:
                events = recovery._read_jsonl(events_path)
            except recovery.LoginRecoveryGameTestError:
                return
            login_messages = _event_messages(
                events, "flow.login"
            )
            if (
                scenario.faulted
                and not fault_armed
                and recovery.READY_TO_SEND_MESSAGE
                in login_messages
            ):
                if not relay.arm_fault_before_native_send():
                    raise LoginTerminalGameTestError(
                        "terminal relay rejected its first sealed pre-send arm"
                    )
                recovery.signal_controller_ack(
                    run_id=run_id,
                    receipt_path=controller_ack,
                )
                fault_armed = True
            if (
                not scenario.cancel
                or cancel_sent
                or not fault_armed
            ):
                return
            snapshots = []
            for message in login_messages:
                matched = SNAPSHOT_PATTERN.fullmatch(message)
                if matched is not None:
                    snapshots.append({
                        key: int(value)
                        for key, value
                        in matched.groupdict().items()
                    })
            if not snapshots:
                return
            trigger = snapshots[-1]
            if (
                trigger["generation"] <= 0
                or trigger["active"] != 1
                or trigger["state"] not in (1, 2, 3, 4)
            ):
                return
            pid = game_process.get("pid")
            path = game_process.get("path")
            desktop_name = _read_quiet_desktop(
                receipt_dir / "quiet-runtime.json"
            )
            if (
                type(pid) is not int
                or not isinstance(path, Path)
                or desktop_name is None
            ):
                return
            send_escape_to_owned_game(
                pid=pid,
                expected_path=path,
                desktop_name=desktop_name,
                receipt_path=cancel_receipt,
                run_id=run_id,
                trigger=trigger,
            )
            cancel_sent = True

        with relay:
            if relay.endpoint != Endpoint(
                relay_host, relay_port
            ):
                raise LoginTerminalGameTestError(
                    "terminal relay did not bind the sealed endpoint"
                )
            report = game_test.run_prepared(
                run_dir,
                timeout=timeout,
                on_process=remember_process,
                on_poll=relay_health_and_input,
            )

        native_proof = validate_terminal_native_evidence(
            Path(metadata["client"]["events"]),
            run_id=run_id,
            scenario=scenario,
        )
        relay_proof = validate_terminal_relay_evidence(
            relay_receipt,
            run_id=run_id,
            scenario=scenario,
        )
        validate_expected_report_failure(
            report,
            run_dir=run_dir,
        )
        if scenario.faulted:
            try:
                ack = json.loads(
                    controller_ack.read_text(encoding="utf-8")
                )
            except (OSError, json.JSONDecodeError) as error:
                raise LoginTerminalGameTestError(
                    "terminal controller acknowledgement receipt is missing or malformed"
                ) from error
            if (
                ack.get("run_id") != run_id
                or ack.get("trigger")
                != "sealed_ready_to_send_observation"
            ):
                raise LoginTerminalGameTestError(
                    "terminal controller acknowledgement belongs to another run"
                )
        if scenario.cancel:
            if not cancel_sent or not cancel_receipt.is_file():
                raise LoginTerminalGameTestError(
                    "cancel scenario did not post Escape to the verified owned game HWND"
                )
            cancel_proof = json.loads(
                cancel_receipt.read_text(encoding="utf-8")
            )
            if (
                cancel_proof.get("run_id") != run_id
                or cancel_proof.get("event")
                != "owned_escape_posted"
                or cancel_proof.get("foreground_unchanged")
                is not True
            ):
                raise LoginTerminalGameTestError(
                    "owned Escape receipt is incomplete"
                )
        else:
            cancel_proof = None

        negative_summary = {
            "schema": 1,
            "run_id": run_id,
            "scenario": scenario.name,
            "status": "expected-fail-verified",
            "standard_report_status": report.get("status"),
            "native_terminal": native_proof,
            "relay": relay_proof,
            "cancel": cancel_proof,
            "runtime_seals_verified": True,
            "source_account_preserved": True,
            "native_events": str(
                Path(metadata["client"]["events"]).resolve()
            ),
        }
        evidence_paths = [relay_receipt]
        if controller_ack.is_file():
            evidence_paths.append(controller_ack)
        if cancel_receipt.is_file():
            evidence_paths.append(cancel_receipt)
        evidence_archive = archive_negative_evidence(
            run_dir=run_dir,
            run_id=run_id,
            report=report,
            summary=negative_summary,
            evidence_paths=evidence_paths,
        )
        if (
            original_bytes is not None
            and original_profile.read_bytes() != original_bytes
        ):
            raise LoginTerminalGameTestError(
                "source account changed before the recovery baseline rebind"
            )
        if source_lock is not None:
            game_test.release_owned_lock(*source_lock)
            source_lock = None

        baseline_attempted = True
        baseline = recovery.prepare_and_run(
            client_template=client_template,
            stage=stage,
            run_dir=run_dir,
            account_profile=original_profile,
            upstream=upstream,
            scenario=recovery.SCENARIOS["baseline"],
            relay_host=relay_host,
            target_server=target_server,
            target_channel=target_channel,
            timeout=timeout,
            asset_baseline=asset_baseline,
            reuse_client=True,
            rebind_existing_account=scenario.wrong_password,
        )
        baseline_rebound = True
        if task_profile is not None:
            task_profile.unlink(missing_ok=True)
            task_profile = None
        if (
            original_bytes is not None
            and original_profile.read_bytes() != original_bytes
        ):
            raise LoginTerminalGameTestError(
                "source account changed during the fresh manual-login baseline"
            )

        summary = {
            "schema": 1,
            "run_id": baseline["run_id"],
            "scenario": scenario.name,
            "status": (
                "expected-terminal-fail-and-fresh-login-pass"
            ),
            "negative_run_id": run_id,
            "negative_evidence_archive": str(
                evidence_archive.resolve()
            ),
            "negative": {
                key: value
                for key, value in negative_summary.items()
                if key != "native_events"
            },
            "fresh_manual_login": baseline,
            "source_account_preserved": True,
        }
        summary_path = (
            run_dir.resolve()
            / "receipts/login-terminal-campaign.json"
        )
        summary_path.write_text(
            json.dumps(summary, indent=2, sort_keys=True)
            + "\n",
            encoding="utf-8",
        )
        return summary
    finally:
        profile_changed = False
        retain_safety_locks = False
        retain_task_profile = False
        run_metadata = run_dir / "run.json"
        if source_lock is not None and run_metadata.is_file():
            try:
                execution = json.loads(
                    run_metadata.read_text(encoding="utf-8")
                ).get("execution", {})
                retain_safety_locks = (
                    execution.get("status") == "cleanup_error"
                    and any(
                        "owned process exit is unconfirmed"
                        in str(error)
                        for error in execution.get(
                            "cleanup_errors", ()
                        )
                    )
                )
            except (OSError, json.JSONDecodeError):
                retain_safety_locks = True
        if (
            task_profile is not None
            and not baseline_rebound
            and (
                baseline_attempted
                or initial_wrong_profile_rebound
            )
        ):
            try:
                current = game_test.validate_metadata(
                    run_dir,
                    for_reuse=True,
                )
                active_lock = game_test.validate_private_account(
                    Path(current["client"]["root"]),
                    current["plan"],
                )
                baseline_rebound = (
                    active_lock
                    == game_test.account_profile_lock_path(
                        original_profile
                    )
                )
            except (game_test.GameTestError, OSError):
                baseline_rebound = False
            retain_task_profile = not baseline_rebound
        if original_bytes is not None:
            try:
                profile_changed = (
                    original_profile.read_bytes()
                    != original_bytes
                )
            except OSError:
                profile_changed = True
        if (
            task_profile is not None
            and not retain_safety_locks
            and not retain_task_profile
        ):
            task_profile.unlink(missing_ok=True)
        if source_lock is not None and not retain_safety_locks:
            game_test.release_owned_lock(*source_lock)
        if profile_changed:
            raise LoginTerminalGameTestError(
                "source persistent account profile changed during the private wrong-password scenario"
            )
        if retain_safety_locks:
            raise LoginTerminalGameTestError(
                "owned process exit is unconfirmed; private profile and source-account safety locks were preserved"
            )
        if retain_task_profile:
            if not source_lock_race:
                raise LoginTerminalGameTestError(
                    "fresh correct-account rebind did not complete; the private wrong-password profile was preserved for safe owned-client reuse"
                )


def _scenario(value: str) -> TerminalScenario:
    try:
        return SCENARIOS[value]
    except KeyError as error:
        raise argparse.ArgumentTypeError(
            "scenario must be one of "
            + ", ".join(sorted(SCENARIOS))
        ) from error


def _upstream(value: str) -> Endpoint:
    return parse_endpoint(value, allow_zero=False)


def parse_args(
    argv: list[str] | None = None,
) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--client-template",
        required=True,
        type=Path,
    )
    parser.add_argument("--stage", required=True, type=Path)
    parser.add_argument("--run-dir", required=True, type=Path)
    parser.add_argument(
        "--account-profile",
        required=True,
        type=Path,
    )
    parser.add_argument(
        "--upstream",
        required=True,
        type=_upstream,
        help="isolated native login service as numeric loopback IPv4:port",
    )
    parser.add_argument(
        "--scenario",
        required=True,
        type=_scenario,
    )
    parser.add_argument(
        "--relay-host",
        default="127.91.3.2",
    )
    parser.add_argument("--target-server", default="1")
    parser.add_argument("--target-channel", default="1")
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--asset-baseline", type=Path)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        summary = prepare_and_run(
            client_template=args.client_template,
            stage=args.stage,
            run_dir=args.run_dir,
            account_profile=args.account_profile,
            upstream=args.upstream,
            scenario=args.scenario,
            relay_host=args.relay_host,
            target_server=args.target_server,
            target_channel=args.target_channel,
            timeout=args.timeout,
            asset_baseline=args.asset_baseline,
        )
    except (
        game_test.GameTestError,
        recovery.LoginRecoveryGameTestError,
        LoginTerminalGameTestError,
        OSError,
        ValueError,
    ) as error:
        print(f"login-terminal GameTest failed: {error}")
        return 1
    print(
        "login-terminal GameTest completed: "
        f"scenario={summary['scenario']} "
        f"status={summary['status']} "
        f"run_id={summary['run_id']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
