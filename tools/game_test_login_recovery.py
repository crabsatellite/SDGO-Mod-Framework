"""Run sealed native login-recovery acceptance through the normal GameTest.

Each invocation prepares one isolated copied client, starts a metadata-only
loopback relay, and delegates launch, quiet-desktop ownership, account locking,
hash verification and cleanup to tools.game_test.run_prepared.
"""
from __future__ import annotations

import argparse
import ctypes
import dataclasses
import inspect
import json
import os
from pathlib import Path
import re
import socket
import sys
import time
from typing import Iterable

if str(Path(__file__).resolve().parents[1]) not in sys.path:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools import game_test
from tools.game_test_login_relay import (
    Endpoint,
    FaultMode,
    LoginFaultRelay,
    RelayConfig,
    parse_endpoint,
)


FINAL_PATTERN = re.compile(
    r"login_recovery_final "
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
    r"pilot=(?P<pilot>[0-9]+) "
    r"credentials=(?P<credentials>[0-9]+) "
    r"active=(?P<active>[0-9]+) "
    r"interaction=(?P<interaction>[0-9]+)\Z"
)
CAPTURE_PATTERN = re.compile(
    r"login_recovery_native_capture "
    r"generation=(?P<generation>[1-9][0-9]*) "
    r"state=(?P<state>[0-9]+) "
    r"active=(?P<active>[0-9]+) "
    r"credentials=(?P<credentials>[0-9]+)\Z"
)
TRANSPORT_PATTERN = re.compile(
    r"login_recovery_native_transport "
    r"sequence=(?P<sequence>[1-9][0-9]*) "
    r"opcode=2329 size=77 success=1 "
    r"result=(?P<result>[1-9][0-9]*)\Z"
)
INITIAL_SEND_MESSAGE = (
    "login_recovery_initial_send count=1 "
    "path=native_login_ctor retries_by_gametest=0"
)
NATIVE_LOGIN_PLAINTEXT_BYTES = 0x4D
MINIMUM_FAULT_WIRE_BYTES = 1
READY_TO_SEND_MESSAGE = (
    "login_recovery_ready_to_send "
    "controller_ack_required=1 native_ui_ready=1"
)
CONTROLLER_ACK_MESSAGE = (
    "login_recovery_controller_ack armed=1 same_run=1"
)


class LoginRecoveryGameTestError(RuntimeError):
    """A sealed scenario or its independent evidence is incomplete."""


@dataclasses.dataclass(frozen=True)
class Scenario:
    name: str
    relay_mode: str
    minimum_retries: int
    arm_after_client_bytes: int
    hold_after_completion: bool


SCENARIOS = {
    scenario.name: scenario
    for scenario in (
        Scenario(
            "baseline",
            FaultMode.PASS_THROUGH,
            minimum_retries=0,
            arm_after_client_bytes=0,
            hold_after_completion=False,
        ),
        Scenario(
            "transient-blackhole",
            FaultMode.FIRST_SERVER_BLACKHOLE,
            minimum_retries=1,
            # The original server welcome reaches the client. The fault becomes
            # active only after positive post-arm client wire traffic appears.
            # Wire length is deliberately not inferred from the 0x4D plaintext.
            arm_after_client_bytes=MINIMUM_FAULT_WIRE_BYTES,
            hold_after_completion=False,
        ),
        Scenario(
            "transient-reset",
            FaultMode.RESET_AFTER_CLIENT_BYTES,
            minimum_retries=1,
            arm_after_client_bytes=MINIMUM_FAULT_WIRE_BYTES,
            hold_after_completion=False,
        ),
        Scenario(
            "stale-session-hold",
            FaultMode.HOLD_UPSTREAM_ON_CLIENT_CLOSE,
            minimum_retries=0,
            arm_after_client_bytes=0,
            hold_after_completion=True,
        ),
    )
}


def _reserve_loopback_port(host: str) -> int:
    """Choose a relay port; the real relay bind still fails closed on a race."""
    endpoint = Endpoint(host, 0).validate(allow_zero=True)
    channel = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            channel.setsockopt(
                socket.SOL_SOCKET,
                socket.SO_EXCLUSIVEADDRUSE,
                1,
            )
        channel.bind((endpoint.host, 0))
        return int(channel.getsockname()[1])
    finally:
        channel.close()


def preflight_upstream(
    upstream: Endpoint,
    *,
    timeout: float = 3.0,
) -> None:
    """Fail before client preparation when the isolated login TCP port is down."""
    upstream = upstream.validate(allow_zero=False)
    try:
        channel = socket.create_connection(
            (upstream.host, upstream.port),
            timeout=timeout,
        )
    except OSError as error:
        raise LoginRecoveryGameTestError(
            "isolated native login upstream is unavailable; "
            "wait for its separate Ready/SQL guard before preparing or launching GameTest"
        ) from error
    channel.close()


def _arm_event_name(run_id: str) -> str:
    if not re.fullmatch(
        r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-"
        r"[0-9a-f]{4}-[0-9a-f]{12}",
        run_id,
    ):
        raise LoginRecoveryGameTestError(
            "controller acknowledgement requires a canonical run id"
        )
    return "Local\\SDGO.GameTest.LoginRecoveryArm." + run_id


def signal_controller_ack(
    *,
    run_id: str,
    receipt_path: Path,
) -> None:
    if os.name != "nt":
        raise LoginRecoveryGameTestError(
            "native login-recovery controller acknowledgement requires Windows"
        )
    from ctypes import wintypes

    event_name = _arm_event_name(run_id)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenEventW.argtypes = (
        wintypes.DWORD,
        wintypes.BOOL,
        wintypes.LPCWSTR,
    )
    kernel.OpenEventW.restype = wintypes.HANDLE
    kernel.SetEvent.argtypes = (wintypes.HANDLE,)
    kernel.SetEvent.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel.CloseHandle.restype = wintypes.BOOL
    handle = kernel.OpenEventW(
        0x0002 | 0x00100000,
        False,
        event_name,
    )
    if not handle:
        raise LoginRecoveryGameTestError(
            "sealed native ready-to-send event was not owned by the active GameTest"
        )
    try:
        receipt = {
            "schema": 1,
            "run_id": run_id,
            "event": "controller_ack",
            "trigger": "sealed_ready_to_send_observation",
            "named_event": event_name,
            "created_utc": game_test.utc_now(),
        }
        with receipt_path.open(
            "x", encoding="utf-8", newline="\n"
        ) as stream:
            json.dump(
                receipt,
                stream,
                indent=2,
                sort_keys=True,
            )
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        if not kernel.SetEvent(handle):
            receipt_path.unlink(missing_ok=True)
            raise LoginRecoveryGameTestError(
                "sealed native ready-to-send event could not be acknowledged"
            )
    finally:
        kernel.CloseHandle(handle)


def _read_jsonl(path: Path) -> list[dict]:
    rows: list[dict] = []
    for number, raw in enumerate(path.read_bytes().splitlines(), 1):
        try:
            row = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise LoginRecoveryGameTestError(
                f"{path.name} line {number} is malformed: {error}"
            ) from error
        if not isinstance(row, dict):
            raise LoginRecoveryGameTestError(
                f"{path.name} line {number} is not an object"
            )
        rows.append(row)
    return rows


def _event_messages(events: Iterable[dict], case: str) -> list[str]:
    return [
        event["message"]
        for event in events
        if event.get("event") == "observation"
        and event.get("case") == case
        and isinstance(event.get("message"), str)
    ]


def validate_native_evidence(
    events_path: Path,
    *,
    run_id: str,
    minimum_retries: int,
) -> dict:
    events = _read_jsonl(events_path)
    if any(event.get("run_id") != run_id for event in events):
        raise LoginRecoveryGameTestError(
            "native login-recovery evidence contains another run identity"
        )

    login_messages = _event_messages(events, "flow.login")
    if login_messages.count(INITIAL_SEND_MESSAGE) != 1:
        raise LoginRecoveryGameTestError(
            "native evidence does not contain exactly one GameTest initial login send"
        )
    transport_messages = [
        message
        for message in login_messages
        if message.startswith("login_recovery_native_transport ")
    ]
    if len(transport_messages) != 1:
        raise LoginRecoveryGameTestError(
            "native evidence does not contain exactly one in-process login transport observation"
        )
    transport = TRANSPORT_PATTERN.fullmatch(
        transport_messages[0]
    )
    if transport is None:
        raise LoginRecoveryGameTestError(
            "the in-process login transport was not a successful native 0x2329/0x4D request"
        )
    transport_message = transport_messages[0]
    if minimum_retries:
        if (
            login_messages.count(READY_TO_SEND_MESSAGE) != 1
            or login_messages.count(CONTROLLER_ACK_MESSAGE) != 1
            or login_messages.index(READY_TO_SEND_MESSAGE)
            >= login_messages.index(CONTROLLER_ACK_MESSAGE)
            or login_messages.index(CONTROLLER_ACK_MESSAGE)
            >= login_messages.index(transport_message)
            or login_messages.index(transport_message)
            >= login_messages.index(INITIAL_SEND_MESSAGE)
        ):
            raise LoginRecoveryGameTestError(
                "faulted native login did not use the sealed ready/ack/native-transport/send ordering"
            )
    elif (
        READY_TO_SEND_MESSAGE in login_messages
        or CONTROLLER_ACK_MESSAGE in login_messages
    ):
        raise LoginRecoveryGameTestError(
            "baseline login unexpectedly waited for a fault-controller acknowledgement"
        )
    elif (
        login_messages.index(transport_message)
        >= login_messages.index(INITIAL_SEND_MESSAGE)
    ):
        raise LoginRecoveryGameTestError(
            "baseline native transport observation did not precede the initial-send receipt"
        )
    capture_messages = [
        message
        for message in login_messages
        if message.startswith("login_recovery_native_capture ")
    ]
    if len(capture_messages) != 1:
        raise LoginRecoveryGameTestError(
            "native evidence does not contain exactly one production initial-request capture"
        )
    capture = CAPTURE_PATTERN.fullmatch(capture_messages[0])
    if capture is None:
        raise LoginRecoveryGameTestError(
            "production initial-request capture has an incompatible schema"
        )
    final_messages = [
        message
        for message in _event_messages(events, "flow.onboarding")
        if message.startswith("login_recovery_final ")
    ]
    if len(final_messages) != 1:
        raise LoginRecoveryGameTestError(
            "native evidence does not contain one final production recovery snapshot"
        )
    matched = FINAL_PATTERN.fullmatch(final_messages[0])
    if matched is None:
        raise LoginRecoveryGameTestError(
            "final production recovery observation has an incompatible schema"
        )
    proof = {
        key: int(value)
        for key, value in matched.groupdict().items()
    }
    proof["transport_sequence"] = int(
        transport.group("sequence")
    )
    proof["transport_result"] = int(
        transport.group("result")
    )
    if proof["generation"] != int(capture.group("generation")):
        raise LoginRecoveryGameTestError(
            "final recovery generation differs from the captured initial native request"
        )
    if (
        proof["state"] != 6
        or proof["outcome"] != 1
        or proof["app"] not in (5, 6)
        or proof["pilot"] <= 0
        or proof["credentials"] != 0
        or proof["active"] != 0
        or proof["interaction"] != 0
    ):
        raise LoginRecoveryGameTestError(
            "production recovery did not finish in a usable credential-free lobby or hangar"
        )
    if (
        proof["retries"] < minimum_retries
        or proof["retry_sends"] < minimum_retries
        or proof["rebuilds"] < minimum_retries
        or proof["connection"] < minimum_retries
        or proof["retries"] > 3
        or proof["auth_replies"] < 1
    ):
        raise LoginRecoveryGameTestError(
            "production retry/reconnect counters do not satisfy the sealed scenario"
        )
    if proof["reject"] not in (0, 1002):
        raise LoginRecoveryGameTestError(
            "a terminal native login rejection was incorrectly accepted"
        )
    if proof["reject"] == 1002 and (
        proof["retries"] < 1
        or proof["retry_sends"] < 1
        or proof["rebuilds"] < 1
        or proof["auth_replies"] < 2
    ):
        raise LoginRecoveryGameTestError(
            "genuine 0x232A/1002 was not followed by production-owned recovery"
        )
    return proof


def validate_relay_evidence(
    receipt_path: Path,
    *,
    scenario: Scenario,
    run_id: str,
) -> dict:
    events = _read_jsonl(receipt_path)
    if not events:
        raise LoginRecoveryGameTestError("login relay receipt is empty")
    if any(event.get("run_id") != run_id for event in events):
        raise LoginRecoveryGameTestError(
            "login relay receipt contains another run identity"
        )
    if events[0].get("event") != "relay_started":
        raise LoginRecoveryGameTestError(
            "login relay receipt does not start with relay_started"
        )
    if events[-1].get("event") != "relay_stopped":
        raise LoginRecoveryGameTestError(
            "login relay receipt does not end with relay_stopped"
        )
    if events[0].get("mode") != scenario.relay_mode:
        raise LoginRecoveryGameTestError(
            "login relay mode differs from the requested scenario"
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
        raise LoginRecoveryGameTestError(
            "login relay receipt contains forbidden payload or credential fields"
        )

    names = [event.get("event") for event in events]
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
        raise LoginRecoveryGameTestError(
            "login relay never carried a real native connection"
        )
    if scenario.arm_after_client_bytes:
        controller_arms = [
            event
            for event in events
            if event.get("event") == "controller_fault_armed"
        ]
        if (
            len(controller_arms) != 1
            or controller_arms[0].get("trigger")
            != "sealed_ready_to_send_observation"
        ):
            raise LoginRecoveryGameTestError(
                "fault relay was not armed exactly once from the sealed native-UI readiness observation"
            )
        injected = [
            event
            for event in events
            if event.get("event") in (
                "server_blackhole_armed",
                "fault_reset_after_client_bytes",
            )
        ]
        if (
            not injected
            or controller_arms[0].get("sequence", 0)
            >= min(event.get("sequence", 0) for event in injected)
        ):
            raise LoginRecoveryGameTestError(
                "transport fault was injected before sealed UI readiness was acknowledged"
            )
        first_injected = min(
            injected,
            key=lambda event: event.get("sequence", 0),
        )
        if (
            first_injected.get("generation") != 1
            or type(first_injected.get("client_bytes")) is not int
            or first_injected.get("client_bytes", 0) <= 0
        ):
            raise LoginRecoveryGameTestError(
                "the first fault was not triggered by positive post-arm client wire traffic"
            )

    if scenario.relay_mode == FaultMode.PASS_THROUGH:
        forbidden_faults = {
            "server_bytes_dropped",
            "fault_reset_after_client_bytes",
            "server_blackhole_armed",
        }
        if forbidden_faults.intersection(names):
            raise LoginRecoveryGameTestError(
                "baseline relay unexpectedly injected a transport fault"
            )
    elif scenario.relay_mode == FaultMode.FIRST_SERVER_BLACKHOLE:
        armed = [
            event
            for event in events
            if event.get("event") == "server_blackhole_armed"
            and event.get("generation") == 1
        ]
        dropped = [
            event
            for event in events
            if event.get("event") == "server_bytes_dropped"
            and event.get("generation") == 1
        ]
        if not armed or not dropped or len(generations) < 2:
            raise LoginRecoveryGameTestError(
                "transient blackhole was not armed after the first native request and recovered on a later connection"
            )
    elif scenario.relay_mode == FaultMode.RESET_AFTER_CLIENT_BYTES:
        resets = [
            event
            for event in events
            if event.get("event") ==
            "fault_reset_after_client_bytes"
            and event.get("generation") == 1
        ]
        if len(resets) != 1 or len(generations) < 2:
            raise LoginRecoveryGameTestError(
                "transient reset did not fault the first request and recover on a later connection"
            )
    elif scenario.relay_mode == FaultMode.HOLD_UPSTREAM_ON_CLIENT_CLOSE:
        if "upstream_hold_started" not in names:
            raise LoginRecoveryGameTestError(
                "stale-session scenario did not retain the native upstream after the owned client closed"
            )
    else:
        raise LoginRecoveryGameTestError(
            "unsupported login-recovery relay scenario"
        )

    return {
        "mode": scenario.relay_mode,
        "generation_count": len(generations),
        "upstream_connection_count": len(upstream),
        "upstream_hold_count": names.count(
            "upstream_hold_started"
        ),
        "fault_events": {
            name: names.count(name)
            for name in (
                "server_blackhole_armed",
                "server_bytes_dropped",
                "fault_reset_after_client_bytes",
            )
        },
    }


def _verified_hold_termination(
    *,
    report: dict,
    hold_terminated: bool,
    hold_termination: dict | None,
    native_proof: dict,
    relay_proof: dict,
) -> bool:
    layers = report.get("layers", {})
    execution = (
        layers.get("launch", {})
        .get("execution", {})
    )
    return bool(
        report.get("status") == "fail"
        and layers.get("source", {}).get("status")
        == "bound"
        and layers.get("launch", {}).get("status")
        == "fail"
        and execution.get("status") == "launch_error"
        and hold_terminated
        and isinstance(hold_termination, dict)
        and hold_termination.get("verified") is True
        and hold_termination.get("terminated") is True
        and type(hold_termination.get("pid")) is int
        and hold_termination.get("pid", 0) > 0
        and native_proof.get("state") == 6
        and native_proof.get("outcome") == 1
        and native_proof.get("pilot", 0) > 0
        and native_proof.get("credentials") == 0
        and native_proof.get("active") == 0
        and relay_proof.get("mode")
        == FaultMode.HOLD_UPSTREAM_ON_CLIENT_CLOSE
        and relay_proof.get("upstream_hold_count") == 1
    )


def prepare_and_run(
    *,
    client_template: Path,
    stage: Path,
    run_dir: Path,
    account_profile: Path,
    upstream: Endpoint,
    scenario: Scenario,
    relay_host: str = "127.91.3.1",
    target_server: str = "1",
    target_channel: str = "1",
    timeout: float = 600.0,
    asset_baseline: Path | None = None,
    reuse_client: bool = False,
    rebind_existing_account: bool = False,
    on_hold_ready=None,
) -> dict:
    if "login_recovery_min_retries" not in inspect.signature(
        game_test.prepare
    ).parameters:
        raise LoginRecoveryGameTestError(
            "tools.game_test.prepare lacks the sealed login_recovery_min_retries integration"
        )
    upstream = upstream.validate(allow_zero=False)
    preflight_upstream(upstream)
    if rebind_existing_account and not reuse_client:
        raise LoginRecoveryGameTestError(
            "existing-account rebind is valid only for owned-client reuse"
        )
    relay_host = Endpoint(relay_host, 0).validate(
        allow_zero=True
    ).host
    relay_port = _reserve_loopback_port(relay_host)

    preparation = (
        game_test.reuse_prepared
        if reuse_client
        else game_test.prepare
    )
    preparation_options = dict(
        target_host=relay_host,
        login_port=relay_port,
        target_server=target_server,
        target_channel=target_channel,
        retry_count=1,
        expect_first_fail=False,
        account_profile=account_profile,
        pages_only=True,
        page_group="hangar",
        network_policy="direct",
        quiet=True,
        asset_baseline=asset_baseline,
        extra_mods=("login_recovery_client",),
        login_recovery_min_retries=scenario.minimum_retries,
        login_recovery_hold_after_completion=
            scenario.hold_after_completion,
    )
    if reuse_client:
        preparation_options["rebind_existing_account"] = (
            rebind_existing_account
        )
    metadata = preparation(
        client_template,
        stage,
        run_dir,
        **preparation_options,
    )
    run_id = metadata["run_id"]
    receipt_dir = run_dir.resolve() / "receipts"
    receipt_dir.mkdir(exist_ok=True)
    relay_receipt = receipt_dir / "login-relay.jsonl"
    relay = LoginFaultRelay(
        RelayConfig(
            bind=Endpoint(relay_host, relay_port),
            upstream=upstream,
            mode=scenario.relay_mode,
            receipt=relay_receipt,
            run_id=run_id,
            controller_pid=os.getpid(),
            lifetime_seconds=min(7200.0, max(timeout + 60.0, 180.0)),
            hold_seconds=min(600.0, timeout + 60.0) if scenario.hold_after_completion else 30.0,
            connect_timeout_seconds=3.0,
            max_generations=8,
            max_bytes_per_direction=16 * 1024 * 1024,
            arm_after_client_bytes=scenario.arm_after_client_bytes,
            require_controller_arm=bool(
                scenario.arm_after_client_bytes
            ),
        )
    )

    game_process: dict[str, object] = {}
    hold_terminated = False
    hold_termination: dict | None = None
    fault_armed = False
    controller_ack = receipt_dir / "login-recovery-arm.json"

    def remember_process(pid: int, path: Path) -> None:
        if path.name.casefold() == game_test.GAME_EXE.casefold():
            game_process.update(pid=pid, path=path.resolve())

    def relay_health() -> None:
        nonlocal hold_terminated, hold_termination, fault_armed
        if relay.wait(0):
            raise LoginRecoveryGameTestError(
                "login relay stopped before the native GameTest completed"
            )
        if (
            scenario.arm_after_client_bytes
            and not fault_armed
            and Path(metadata["client"]["events"]).is_file()
        ):
            try:
                login_messages = _event_messages(
                    _read_jsonl(
                        Path(metadata["client"]["events"])
                    ),
                    "flow.login",
                )
            except LoginRecoveryGameTestError:
                login_messages = []
            if READY_TO_SEND_MESSAGE in login_messages:
                if not relay.arm_fault_before_native_send():
                    raise LoginRecoveryGameTestError(
                        "fault relay rejected its first sealed pre-send arm"
                    )
                signal_controller_ack(
                    run_id=run_id,
                    receipt_path=controller_ack,
                )
                fault_armed = True
        if (scenario.hold_after_completion and
                not hold_terminated and
                Path(metadata["client"]["events"]).is_file()):
            try:
                messages = _event_messages(
                    _read_jsonl(
                        Path(metadata["client"]["events"])
                    ),
                    "flow.onboarding",
                )
            except LoginRecoveryGameTestError:
                return
            if any(message.startswith("login_recovery_hold_ready ")
                   for message in messages):
                pid = game_process.get("pid")
                path = game_process.get("path")
                if (type(pid) is not int or
                        not isinstance(path, Path) or
                        not game_test.terminate_verified_process(pid, path)):
                    raise LoginRecoveryGameTestError(
                        "owned completed hold client could not be terminated by verified identity"
                    )
                hold_terminated = True
                hold_termination = {
                    "verified": True,
                    "terminated": True,
                    "pid": pid,
                    "image_name": path.name,
                }

    with relay:
        if relay.endpoint != Endpoint(relay_host, relay_port):
            raise LoginRecoveryGameTestError(
                "login relay did not bind the endpoint sealed into GameTest"
            )
        report = game_test.run_prepared(
            run_dir,
            timeout=timeout,
            on_process=remember_process,
            on_poll=relay_health,
        )
        if scenario.hold_after_completion:
            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline:
                if relay_receipt.is_file():
                    try:
                        held = any(
                            event.get("event") ==
                            "upstream_hold_started"
                            for event in _read_jsonl(
                                relay_receipt
                            )
                        )
                    except LoginRecoveryGameTestError:
                        held = False
                    if held:
                        break
                time.sleep(0.05)
            else:
                raise LoginRecoveryGameTestError(
                    "relay did not retain the stale upstream after the owned client closed"
                )
            if on_hold_ready is not None:
                on_hold_ready({
                    "run_id": run_id,
                    "metadata": metadata,
                    "relay_endpoint": relay.endpoint,
                })

    native_proof = validate_native_evidence(
        Path(metadata["client"]["events"]),
        run_id=run_id,
        minimum_retries=scenario.minimum_retries,
    )
    relay_proof = validate_relay_evidence(
        relay_receipt,
        scenario=scenario,
        run_id=run_id,
    )
    hold_integrity_verified = False
    if scenario.hold_after_completion:
        game_test.verify_hashes(
            game_test.validate_metadata(run_dir)
        )
        hold_integrity_verified = True
    if scenario.arm_after_client_bytes:
        try:
            ack = json.loads(
                controller_ack.read_text(encoding="utf-8")
            )
        except (OSError, json.JSONDecodeError) as error:
            raise LoginRecoveryGameTestError(
                "sealed pre-send controller acknowledgement receipt is missing or malformed"
            ) from error
        if (
            ack.get("run_id") != run_id
            or ack.get("event") != "controller_ack"
            or ack.get("trigger")
            != "sealed_ready_to_send_observation"
            or ack.get("named_event") != _arm_event_name(run_id)
        ):
            raise LoginRecoveryGameTestError(
                "sealed pre-send controller acknowledgement receipt differs from the active run"
            )
    if scenario.hold_after_completion:
        status = (
            "expected-held-client-terminated"
            if _verified_hold_termination(
                report=report,
                hold_terminated=hold_terminated,
                hold_termination=hold_termination,
                native_proof=native_proof,
                relay_proof=relay_proof,
            )
            else "fail"
        )
    else:
        status = (
            "pass"
            if report.get("status") == "pass"
            else "fail"
        )
    summary = {
        "schema": 1,
        "run_id": run_id,
        "scenario": scenario.name,
        "status": status,
        "standard_report_status": report.get("status"),
        "native_recovery": native_proof,
        "relay": relay_proof,
        "hold_termination": hold_termination,
        "hold_integrity_verified": hold_integrity_verified,
    }
    summary_path = receipt_dir / "login-recovery-report.json"
    summary_path.write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    if status not in (
        "pass",
        "expected-held-client-terminated",
    ):
        raise LoginRecoveryGameTestError(
            "standard native GameTest report did not pass"
        )
    return summary


def _scenario(value: str) -> Scenario:
    try:
        return SCENARIOS[value]
    except KeyError as error:
        raise argparse.ArgumentTypeError(
            "scenario must be one of " +
            ", ".join(sorted(SCENARIOS))
        ) from error


def _upstream(value: str) -> Endpoint:
    return parse_endpoint(value, allow_zero=False)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
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
        default="127.91.3.1",
    )
    parser.add_argument("--target-server", default="1")
    parser.add_argument("--target-channel", default="1")
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--asset-baseline", type=Path)
    parser.add_argument(
        "--reuse-client",
        action="store_true",
        help="reseal the stopped task-owned client in run-dir",
    )
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
            reuse_client=args.reuse_client,
        )
    except (
        game_test.GameTestError,
        LoginRecoveryGameTestError,
        OSError,
        ValueError,
    ) as error:
        print(f"login-recovery GameTest failed: {error}")
        return 1
    print(
        "login-recovery GameTest completed: "
        f"scenario={summary['scenario']} "
        f"status={summary['status']} "
        f"run_id={summary['run_id']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
