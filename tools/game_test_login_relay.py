"""Bounded blind TCP fault relay for native login-recovery GameTests.

The relay accepts only numeric IPv4 loopback endpoints. It never parses,
copies to disk, hashes, or logs network payloads. Receipts contain lifecycle
events and byte counts only.
"""
from __future__ import annotations

import argparse
import ctypes
import dataclasses
import datetime as dt
import ipaddress
import json
import os
from pathlib import Path
import re
import select
import signal
import socket
import struct
import threading
import time
import uuid
from typing import TextIO


SCHEMA = 1
CHUNK_SIZE = 64 * 1024
RUN_ID_PATTERN = re.compile(
    r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-"
    r"[0-9a-f]{4}-[0-9a-f]{12}\Z"
)


class RelayError(RuntimeError):
    """Fail-closed relay configuration or lifecycle error."""


class FaultMode:
    PASS_THROUGH = "pass-through"
    FIRST_SERVER_BLACKHOLE = "first-generation-server-blackhole"
    RESET_AFTER_CLIENT_BYTES = "reset-after-first-client-bytes"
    ALL_SERVER_BLACKHOLE = "all-generations-server-blackhole"
    HOLD_UPSTREAM_ON_CLIENT_CLOSE = "hold-upstream-on-client-close"

    VALUES = (
        PASS_THROUGH,
        FIRST_SERVER_BLACKHOLE,
        RESET_AFTER_CLIENT_BYTES,
        ALL_SERVER_BLACKHOLE,
        HOLD_UPSTREAM_ON_CLIENT_CLOSE,
    )


@dataclasses.dataclass(frozen=True)
class Endpoint:
    host: str
    port: int

    def validate(self, *, allow_zero: bool) -> "Endpoint":
        try:
            address = ipaddress.IPv4Address(self.host)
        except ipaddress.AddressValueError as error:
            raise RelayError(
                "relay endpoints require a numeric IPv4 address"
            ) from error
        if not address.is_loopback:
            raise RelayError("relay endpoints must stay on IPv4 loopback")
        minimum = 0 if allow_zero else 1
        if not minimum <= self.port <= 65535:
            raise RelayError(
                f"endpoint port must be {minimum}..65535"
            )
        return Endpoint(str(address), self.port)


@dataclasses.dataclass(frozen=True)
class RelayConfig:
    bind: Endpoint
    upstream: Endpoint
    mode: str
    receipt: Path
    run_id: str
    controller_pid: int
    lifetime_seconds: float = 60.0
    hold_seconds: float = 30.0
    connect_timeout_seconds: float = 3.0
    max_generations: int = 8
    max_bytes_per_direction: int = 16 * 1024 * 1024
    arm_after_client_bytes: int = 0
    require_controller_arm: bool = False

    def validate(self) -> "RelayConfig":
        bind = self.bind.validate(allow_zero=True)
        upstream = self.upstream.validate(allow_zero=False)
        if bind.port and bind == upstream:
            raise RelayError("relay bind and upstream endpoints must differ")
        if self.mode not in FaultMode.VALUES:
            raise RelayError("unknown login relay fault mode")
        if (
            not RUN_ID_PATTERN.fullmatch(self.run_id)
            or str(uuid.UUID(self.run_id)) != self.run_id
        ):
            raise RelayError("run_id must be a canonical lowercase UUID")
        if self.controller_pid <= 0:
            raise RelayError("controller_pid must be a positive PID")
        if not 0.1 <= self.lifetime_seconds <= 7200:
            raise RelayError("lifetime_seconds must be 0.1..7200")
        if not 0.05 <= self.hold_seconds <= 600:
            raise RelayError("hold_seconds must be 0.05..600")
        if not 0.05 <= self.connect_timeout_seconds <= 30:
            raise RelayError(
                "connect_timeout_seconds must be 0.05..30"
            )
        if not 1 <= self.max_generations <= 64:
            raise RelayError("max_generations must be 1..64")
        if not 1024 <= self.max_bytes_per_direction <= 1024**3:
            raise RelayError(
                "max_bytes_per_direction must be 1024..1073741824"
            )
        if not 0 <= self.arm_after_client_bytes <= (
            self.max_bytes_per_direction
        ):
            raise RelayError(
                "arm_after_client_bytes must be nonnegative and no "
                "greater than max_bytes_per_direction"
            )
        if type(self.require_controller_arm) is not bool:
            raise RelayError("require_controller_arm must be a boolean")
        if self.require_controller_arm and self.mode not in (
            FaultMode.FIRST_SERVER_BLACKHOLE,
            FaultMode.ALL_SERVER_BLACKHOLE,
            FaultMode.RESET_AFTER_CLIENT_BYTES,
        ):
            raise RelayError(
                "controller arming is valid only for an injected fault mode"
            )
        receipt = self.receipt.resolve()
        if not receipt.parent.is_dir():
            raise RelayError("receipt parent directory does not exist")
        if receipt.exists():
            raise RelayError("refusing to overwrite an existing receipt")
        return dataclasses.replace(
            self, bind=bind, upstream=upstream, receipt=receipt
        )


def process_identity(pid: int) -> str | None:
    """Return a creation identity that detects dead or recycled PIDs."""
    if pid <= 0:
        return None
    if os.name != "nt":
        try:
            return (
                Path(f"/proc/{pid}/stat")
                .read_text(encoding="ascii")
                .rsplit(")", 1)[1]
                .split()[19]
            )
        except (OSError, IndexError):
            return None

    from ctypes import wintypes

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = (
        wintypes.DWORD,
        wintypes.BOOL,
        wintypes.DWORD,
    )
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.GetProcessTimes.argtypes = (
        wintypes.HANDLE,
        ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME),
    )
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    handle = kernel.OpenProcess(0x1000, False, pid)
    if not handle:
        return None
    try:
        created, exited, system, user = (
            wintypes.FILETIME() for _ in range(4)
        )
        if not kernel.GetProcessTimes(
            handle,
            ctypes.byref(created),
            ctypes.byref(exited),
            ctypes.byref(system),
            ctypes.byref(user),
        ):
            return None
        if exited.dwLowDateTime or exited.dwHighDateTime:
            return None
        return str(
            (created.dwHighDateTime << 32) | created.dwLowDateTime
        )
    finally:
        kernel.CloseHandle(handle)


def parse_endpoint(value: str, *, allow_zero: bool) -> Endpoint:
    host, separator, raw_port = value.rpartition(":")
    if not separator or not host or not raw_port.isdecimal():
        raise argparse.ArgumentTypeError(
            "endpoint must be numeric-loopback-ip:port"
        )
    try:
        endpoint = Endpoint(host, int(raw_port)).validate(
            allow_zero=allow_zero
        )
    except RelayError as error:
        raise argparse.ArgumentTypeError(str(error)) from error
    return endpoint


def _utc_now() -> str:
    return (
        dt.datetime.now(dt.timezone.utc)
        .isoformat(timespec="milliseconds")
        .replace("+00:00", "Z")
    )


def _socket_error(error: BaseException) -> dict:
    number = getattr(error, "errno", None)
    return {
        "error_type": type(error).__name__,
        "error_number": number if isinstance(number, int) else None,
    }


class ReceiptWriter:
    """Thread-safe exclusive JSONL writer with a fixed metadata envelope."""

    def __init__(
        self,
        path: Path,
        *,
        run_id: str,
        controller_pid: int,
        controller_identity: str,
    ):
        self.path = path
        self.run_id = run_id
        self.controller_pid = controller_pid
        self.controller_identity = controller_identity
        self._lock = threading.Lock()
        self._sequence = 0
        self._stream: TextIO = path.open(
            "x", encoding="utf-8", newline="\n"
        )
        try:
            os.chmod(path, 0o600)
        except OSError:
            pass

    def emit(self, event: str, **metadata: object) -> None:
        row = {
            "schema": SCHEMA,
            "sequence": 0,
            "time_utc": _utc_now(),
            "monotonic_ms": time.monotonic_ns() // 1_000_000,
            "event": event,
            "run_id": self.run_id,
            "controller_pid": self.controller_pid,
            "controller_identity": self.controller_identity,
            "relay_pid": os.getpid(),
        }
        row.update(metadata)
        with self._lock:
            self._sequence += 1
            row["sequence"] = self._sequence
            self._stream.write(
                json.dumps(
                    row,
                    ensure_ascii=True,
                    separators=(",", ":"),
                    sort_keys=True,
                )
                + "\n"
            )
            self._stream.flush()
            os.fsync(self._stream.fileno())

    def close(self) -> None:
        with self._lock:
            if not self._stream.closed:
                self._stream.close()


@dataclasses.dataclass
class GenerationCounters:
    client_bytes: int = 0
    server_bytes: int = 0
    forwarded_client_bytes: int = 0
    forwarded_server_bytes: int = 0
    dropped_server_bytes: int = 0

    def public(self) -> dict:
        return dataclasses.asdict(self)


class LoginFaultRelay:
    """Importable controller for one bounded relay run."""

    def __init__(self, config: RelayConfig):
        self.config = config.validate()
        identity = process_identity(self.config.controller_pid)
        if identity is None:
            raise RelayError("controller PID is not alive")
        self.controller_identity = identity
        self._writer: ReceiptWriter | None = None
        self._listener: socket.socket | None = None
        self._endpoint: Endpoint | None = None
        self._accept_thread: threading.Thread | None = None
        self._workers: set[threading.Thread] = set()
        self._workers_lock = threading.Lock()
        self._sockets: set[socket.socket] = set()
        self._sockets_lock = threading.Lock()
        self._stop_event = threading.Event()
        self._accept_done = threading.Event()
        self._lifecycle_lock = threading.Lock()
        self._fault_arm_lock = threading.Lock()
        self._fault_arm_event = threading.Event()
        self._fault_armed_by_controller = False
        self._stop_reason: str | None = None
        self._started = False
        self._closed = False
        self._deadline = 0.0
        self._generations = 0

    @property
    def endpoint(self) -> Endpoint:
        if self._endpoint is None:
            raise RelayError("relay has not started")
        return self._endpoint

    @property
    def stop_reason(self) -> str | None:
        with self._lifecycle_lock:
            return self._stop_reason

    def start(self) -> "LoginFaultRelay":
        with self._lifecycle_lock:
            if self._started:
                raise RelayError("relay already started")
            if self._closed:
                raise RelayError("relay already closed")
            self._started = True
        writer = ReceiptWriter(
            self.config.receipt,
            run_id=self.config.run_id,
            controller_pid=self.config.controller_pid,
            controller_identity=self.controller_identity,
        )
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
                listener.setsockopt(
                    socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1
                )
            listener.bind(
                (self.config.bind.host, self.config.bind.port)
            )
            listener.listen(min(self.config.max_generations, 16))
            listener.setblocking(False)
            bound_host, bound_port = listener.getsockname()[:2]
            self._writer = writer
            self._listener = listener
            self._endpoint = Endpoint(str(bound_host), int(bound_port))
            self._deadline = (
                time.monotonic() + self.config.lifetime_seconds
            )
            writer.emit(
                "relay_started",
                mode=self.config.mode,
                bind_host=self.endpoint.host,
                bind_port=self.endpoint.port,
                upstream_host=self.config.upstream.host,
                upstream_port=self.config.upstream.port,
                lifetime_ms=round(
                    self.config.lifetime_seconds * 1000
                ),
                hold_ms=round(self.config.hold_seconds * 1000),
                max_generations=self.config.max_generations,
                max_bytes_per_direction=(
                    self.config.max_bytes_per_direction
                ),
                arm_after_client_bytes=(
                    self.config.arm_after_client_bytes
                ),
                require_controller_arm=(
                    self.config.require_controller_arm
                ),
            )
            self._accept_thread = threading.Thread(
                target=self._accept_loop,
                name="login-fault-relay-accept",
                daemon=True,
            )
            self._accept_thread.start()
            return self
        except BaseException:
            listener.close()
            writer.close()
            self._closed = True
            raise

    def wait(self, timeout: float | None = None) -> bool:
        return self._accept_done.wait(timeout)

    def arm_fault_before_native_send(self) -> bool:
        """Arm after sealed UI readiness and before the one native login send."""
        if not self.config.require_controller_arm:
            raise RelayError(
                "relay was not configured for controller fault arming"
            )
        with self._fault_arm_lock:
            if self._fault_armed_by_controller:
                return False
            if not self._started or self._closed:
                raise RelayError(
                    "relay is not running while controller fault arming was requested"
                )
            self._require_writer().emit(
                "controller_fault_armed",
                trigger="sealed_ready_to_send_observation",
            )
            self._fault_armed_by_controller = True
            self._fault_arm_event.set()
            return True

    def serve_forever(self) -> None:
        self.start()
        try:
            self._accept_done.wait()
        finally:
            self.stop()

    def stop(self, reason: str = "controller_requested") -> None:
        if not self._started:
            return
        self._request_stop(reason)
        listener = self._listener
        if listener is not None:
            try:
                listener.close()
            except OSError:
                pass
        self._close_active_sockets()
        current = threading.current_thread()
        accept_thread = self._accept_thread
        if accept_thread is not None and accept_thread is not current:
            accept_thread.join(timeout=5)
        deadline = time.monotonic() + 5
        while True:
            with self._workers_lock:
                workers = [
                    worker
                    for worker in self._workers
                    if worker is not current
                ]
            if not workers or time.monotonic() >= deadline:
                break
            for worker in workers:
                worker.join(timeout=0.1)
        with self._lifecycle_lock:
            if self._closed:
                return
            self._closed = True
        writer = self._writer
        if writer is not None:
            writer.emit(
                "relay_stopped",
                reason=self.stop_reason or reason,
                generations=self._generations,
                workers_remaining=sum(
                    worker.is_alive() for worker in workers
                )
                if "workers" in locals()
                else 0,
            )
            writer.close()

    def __enter__(self) -> "LoginFaultRelay":
        return self.start()

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.stop(
            "context_error" if exc_type is not None else "context_complete"
        )

    def _request_stop(self, reason: str) -> None:
        with self._lifecycle_lock:
            if self._stop_reason is None:
                self._stop_reason = reason
        self._stop_event.set()

    def _accept_loop(self) -> None:
        writer = self._require_writer()
        listener = self._listener
        assert listener is not None
        try:
            while not self._stop_event.is_set():
                now = time.monotonic()
                if now >= self._deadline:
                    self._request_stop("lifetime_expired")
                    break
                if (
                    process_identity(self.config.controller_pid)
                    != self.controller_identity
                ):
                    self._request_stop("controller_exited")
                    break
                try:
                    readable, _, _ = select.select(
                        [listener], [], [], min(0.1, self._deadline - now)
                    )
                except (OSError, ValueError):
                    if not self._stop_event.is_set():
                        self._request_stop("listener_failed")
                    break
                if not readable:
                    continue
                try:
                    client, peer = listener.accept()
                except BlockingIOError:
                    continue
                except OSError:
                    if not self._stop_event.is_set():
                        self._request_stop("accept_failed")
                    break
                self._generations += 1
                generation = self._generations
                if generation > self.config.max_generations:
                    _abortive_close(client)
                    writer.emit(
                        "generation_rejected",
                        generation=generation,
                        reason="max_generations_reached",
                    )
                    self._request_stop("max_generations_reached")
                    break
                writer.emit(
                    "generation_accepted",
                    generation=generation,
                    peer_host=str(peer[0]),
                    peer_port=int(peer[1]),
                )
                self._track_socket(client)
                worker = threading.Thread(
                    target=self._run_generation,
                    args=(generation, client),
                    name=f"login-fault-relay-{generation}",
                    daemon=True,
                )
                with self._workers_lock:
                    self._workers.add(worker)
                worker.start()
        finally:
            try:
                listener.close()
            except OSError:
                pass
            self._accept_done.set()

    def _run_generation(
        self, generation: int, client: socket.socket
    ) -> None:
        writer = self._require_writer()
        counters = GenerationCounters()
        upstream: socket.socket | None = None
        started = time.monotonic()
        reason = "unknown"
        client_alive = True
        upstream_alive = False
        holding = False
        hold_deadline = 0.0
        hold_started_at = 0.0
        blackhole_scope = (
            self.config.mode == FaultMode.ALL_SERVER_BLACKHOLE
            or (
                self.config.mode == FaultMode.FIRST_SERVER_BLACKHOLE
                and generation == 1
            )
        )
        blackhole_armed = (
            blackhole_scope
            and self.config.arm_after_client_bytes == 0
            and (
                not self.config.require_controller_arm
                or self._fault_arm_event.is_set()
            )
        )
        pending_reset = False
        try:
            upstream = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self._track_socket(upstream)
            upstream.settimeout(
                min(
                    self.config.connect_timeout_seconds,
                    max(0.05, self._deadline - time.monotonic()),
                )
            )
            try:
                upstream.connect(
                    (
                        self.config.upstream.host,
                        self.config.upstream.port,
                    )
                )
            except OSError as error:
                reason = "upstream_connect_failed"
                writer.emit(
                    "upstream_connect_failed",
                    generation=generation,
                    **_socket_error(error),
                )
                return
            upstream_alive = True
            client.setblocking(False)
            upstream.setblocking(False)
            writer.emit(
                "upstream_connected",
                generation=generation,
                upstream_host=self.config.upstream.host,
                upstream_port=self.config.upstream.port,
            )
            if blackhole_armed:
                writer.emit(
                    "server_blackhole_armed",
                    generation=generation,
                    arm_after_client_bytes=0,
                    client_bytes=0,
                )

            while not self._stop_event.is_set():
                now = time.monotonic()
                if now >= self._deadline:
                    reason = "lifetime_expired"
                    break
                if holding and now >= hold_deadline:
                    reason = "upstream_hold_completed"
                    writer.emit(
                        "upstream_hold_completed",
                        generation=generation,
                        hold_ms=round((now - hold_started_at) * 1000),
                    )
                    break
                controller_ready = (
                    not self.config.require_controller_arm
                    or self._fault_arm_event.is_set()
                )
                if (
                    blackhole_scope
                    and not blackhole_armed
                    and controller_ready
                    and counters.client_bytes
                    >= self.config.arm_after_client_bytes
                ):
                    blackhole_armed = True
                    writer.emit(
                        "server_blackhole_armed",
                        generation=generation,
                        arm_after_client_bytes=(
                            self.config.arm_after_client_bytes
                        ),
                        client_bytes=counters.client_bytes,
                    )
                if pending_reset and controller_ready:
                    writer.emit(
                        "fault_reset_after_client_bytes",
                        generation=generation,
                        client_bytes=counters.client_bytes,
                    )
                    _abortive_close(client)
                    client_alive = False
                    _abortive_close(upstream)
                    upstream_alive = False
                    reason = "fault_reset_after_client_bytes"
                    break
                waiting_for_controller = (
                    self.config.require_controller_arm
                    and not controller_ready
                    and (
                        pending_reset
                        or (
                            blackhole_scope
                            and counters.client_bytes
                            >= self.config.arm_after_client_bytes
                        )
                    )
                )
                readers = []
                if client_alive and not holding and not pending_reset:
                    readers.append(client)
                if upstream_alive and not waiting_for_controller:
                    readers.append(upstream)
                if not readers:
                    if waiting_for_controller:
                        self._fault_arm_event.wait(
                            min(0.1, max(0.0, self._deadline - now))
                        )
                        continue
                    reason = "both_sides_closed"
                    break
                timeout = min(0.1, self._deadline - now)
                if holding:
                    timeout = min(timeout, hold_deadline - now)
                try:
                    readable, _, _ = select.select(
                        readers, [], [], max(0.0, timeout)
                    )
                except (OSError, ValueError) as error:
                    reason = "select_failed"
                    writer.emit(
                        "generation_socket_error",
                        generation=generation,
                        operation="select",
                        **_socket_error(error),
                    )
                    break
                if (
                    blackhole_scope
                    and not blackhole_armed
                    and upstream in readable
                    and not (
                        self.config.require_controller_arm
                        and client in readable
                    )
                ):
                    readable = [upstream] + [
                        source for source in readable if source is not upstream
                    ]
                for source in readable:
                    if source is client:
                        data = _receive(client)
                        if data is None:
                            client_alive = False
                            _close_socket(client)
                            writer.emit(
                                "client_closed",
                                generation=generation,
                                client_bytes=counters.client_bytes,
                            )
                            if (
                                self.config.mode
                                == FaultMode.HOLD_UPSTREAM_ON_CLIENT_CLOSE
                                and counters.client_bytes > 0
                                and upstream_alive
                            ):
                                holding = True
                                hold_started_at = time.monotonic()
                                hold_deadline = min(
                                    self._deadline,
                                    hold_started_at
                                    + self.config.hold_seconds,
                                )
                                writer.emit(
                                    "upstream_hold_started",
                                    generation=generation,
                                    requested_hold_ms=round(
                                        self.config.hold_seconds * 1000
                                    ),
                                )
                            else:
                                reason = "client_closed"
                                break
                            continue

                        counters.client_bytes += len(data)
                        if (
                            blackhole_scope
                            and not blackhole_armed
                            and counters.client_bytes
                            >= self.config.arm_after_client_bytes
                            and (
                                not self.config.require_controller_arm
                                or self._fault_arm_event.is_set()
                            )
                        ):
                            blackhole_armed = True
                            writer.emit(
                                "server_blackhole_armed",
                                generation=generation,
                                arm_after_client_bytes=(
                                    self.config.arm_after_client_bytes
                                ),
                                client_bytes=counters.client_bytes,
                            )
                        if (
                            counters.client_bytes
                            > self.config.max_bytes_per_direction
                        ):
                            reason = "client_byte_limit"
                            writer.emit(
                                "byte_limit_reached",
                                generation=generation,
                                direction="client_to_server",
                                byte_count=counters.client_bytes,
                            )
                            break
                        if (
                            self.config.mode
                            == FaultMode.RESET_AFTER_CLIENT_BYTES
                            and generation == 1
                        ):
                            if (
                                counters.client_bytes
                                < self.config.arm_after_client_bytes
                            ):
                                continue
                            if (
                                self.config.require_controller_arm
                                and not self._fault_arm_event.is_set()
                            ):
                                pending_reset = True
                                writer.emit(
                                    "fault_waiting_for_controller_arm",
                                    generation=generation,
                                    client_bytes=counters.client_bytes,
                                )
                                continue
                            writer.emit(
                                "fault_reset_after_client_bytes",
                                generation=generation,
                                client_bytes=counters.client_bytes,
                            )
                            _abortive_close(client)
                            client_alive = False
                            _abortive_close(upstream)
                            upstream_alive = False
                            reason = "fault_reset_after_client_bytes"
                            break
                        try:
                            _send_all(
                                upstream,
                                data,
                                self._stop_event,
                                self._deadline,
                            )
                        except OSError as error:
                            reason = "upstream_send_failed"
                            writer.emit(
                                "generation_socket_error",
                                generation=generation,
                                operation="upstream_send",
                                **_socket_error(error),
                            )
                            break
                        counters.forwarded_client_bytes += len(data)

                    elif source is upstream:
                        if (
                            self.config.require_controller_arm
                            and not self._fault_arm_event.is_set()
                            and (
                                pending_reset
                                or (
                                    blackhole_scope
                                    and counters.client_bytes
                                    >= self.config.arm_after_client_bytes
                                )
                            )
                        ):
                            continue
                        data = _receive(upstream)
                        if data is None:
                            upstream_alive = False
                            _close_socket(upstream)
                            writer.emit(
                                "upstream_closed",
                                generation=generation,
                                server_bytes=counters.server_bytes,
                                during_hold=holding,
                            )
                            reason = (
                                "upstream_closed_during_hold"
                                if holding
                                else "upstream_closed"
                            )
                            break

                        counters.server_bytes += len(data)
                        if (
                            counters.server_bytes
                            > self.config.max_bytes_per_direction
                        ):
                            reason = "server_byte_limit"
                            writer.emit(
                                "byte_limit_reached",
                                generation=generation,
                                direction="server_to_client",
                                byte_count=counters.server_bytes,
                            )
                            break
                        blackhole = (
                            holding
                            or (blackhole_scope and blackhole_armed)
                        )
                        if blackhole:
                            counters.dropped_server_bytes += len(data)
                            writer.emit(
                                "server_bytes_dropped",
                                generation=generation,
                                byte_count=len(data),
                                total_dropped=(
                                    counters.dropped_server_bytes
                                ),
                                reason=(
                                    "client_closed_hold"
                                    if holding
                                    else "configured_blackhole"
                                ),
                            )
                            continue
                        try:
                            _send_all(
                                client,
                                data,
                                self._stop_event,
                                self._deadline,
                            )
                        except OSError as error:
                            client_alive = False
                            _close_socket(client)
                            writer.emit(
                                "generation_socket_error",
                                generation=generation,
                                operation="client_send",
                                **_socket_error(error),
                            )
                            if (
                                self.config.mode
                                == FaultMode.HOLD_UPSTREAM_ON_CLIENT_CLOSE
                                and counters.client_bytes > 0
                                and upstream_alive
                            ):
                                holding = True
                                hold_started_at = time.monotonic()
                                hold_deadline = min(
                                    self._deadline,
                                    hold_started_at
                                    + self.config.hold_seconds,
                                )
                                writer.emit(
                                    "upstream_hold_started",
                                    generation=generation,
                                    requested_hold_ms=round(
                                        self.config.hold_seconds * 1000
                                    ),
                                )
                                continue
                            reason = "client_send_failed"
                            break
                        counters.forwarded_server_bytes += len(data)
                if reason != "unknown":
                    break
            if self._stop_event.is_set() and reason == "unknown":
                reason = self.stop_reason or "relay_stopped"
        finally:
            _close_socket(client)
            self._untrack_socket(client)
            if upstream is not None:
                _close_socket(upstream)
                self._untrack_socket(upstream)
            writer.emit(
                "generation_closed",
                generation=generation,
                reason=reason,
                duration_ms=round((time.monotonic() - started) * 1000),
                **counters.public(),
            )
            with self._workers_lock:
                self._workers.discard(threading.current_thread())

    def _require_writer(self) -> ReceiptWriter:
        writer = self._writer
        if writer is None:
            raise RelayError("relay receipt writer is unavailable")
        return writer

    def _track_socket(self, channel: socket.socket) -> None:
        with self._sockets_lock:
            self._sockets.add(channel)

    def _untrack_socket(self, channel: socket.socket) -> None:
        with self._sockets_lock:
            self._sockets.discard(channel)

    def _close_active_sockets(self) -> None:
        with self._sockets_lock:
            channels = list(self._sockets)
        for channel in channels:
            _close_socket(channel)


def _receive(channel: socket.socket) -> bytes | None:
    try:
        data = channel.recv(CHUNK_SIZE)
    except BlockingIOError:
        return b""
    except (ConnectionError, OSError):
        return None
    return data if data else None


def _send_all(
    channel: socket.socket,
    data: bytes,
    stop_event: threading.Event,
    deadline: float,
) -> None:
    view = memoryview(data)
    while view:
        if stop_event.is_set() or time.monotonic() >= deadline:
            raise TimeoutError("relay send was stopped")
        try:
            sent = channel.send(view)
        except BlockingIOError:
            sent = 0
        if sent:
            view = view[sent:]
            continue
        _, writable, _ = select.select(
            [], [channel], [], min(0.1, max(0.0, deadline - time.monotonic()))
        )
        if not writable:
            continue


def _close_socket(channel: socket.socket) -> None:
    try:
        channel.close()
    except OSError:
        pass


def _abortive_close(channel: socket.socket) -> None:
    try:
        channel.setsockopt(
            socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("hh", 1, 0)
        )
    except OSError:
        pass
    _close_socket(channel)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Bounded metadata-only loopback TCP fault relay for native "
            "login-recovery GameTests."
        )
    )
    parser.add_argument(
        "--bind",
        required=True,
        type=lambda value: parse_endpoint(value, allow_zero=True),
        help="numeric IPv4 loopback listener, for example 127.90.1.1:5001",
    )
    parser.add_argument(
        "--upstream",
        required=True,
        type=lambda value: parse_endpoint(value, allow_zero=False),
        help="numeric IPv4 loopback native login endpoint",
    )
    parser.add_argument(
        "--mode", required=True, choices=FaultMode.VALUES
    )
    parser.add_argument("--receipt", required=True, type=Path)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--controller-pid", required=True, type=int)
    parser.add_argument("--lifetime", type=float, default=60.0)
    parser.add_argument("--hold-seconds", type=float, default=30.0)
    parser.add_argument("--connect-timeout", type=float, default=3.0)
    parser.add_argument("--max-generations", type=int, default=8)
    parser.add_argument(
        "--max-bytes-per-direction",
        type=int,
        default=16 * 1024 * 1024,
    )
    parser.add_argument(
        "--arm-after-client-bytes",
        type=int,
        default=0,
        help=(
            "for server-blackhole modes, forward server bytes until each "
            "affected generation has received this many client bytes"
        ),
    )
    return parser


def config_from_args(args: argparse.Namespace) -> RelayConfig:
    return RelayConfig(
        bind=args.bind,
        upstream=args.upstream,
        mode=args.mode,
        receipt=args.receipt,
        run_id=args.run_id,
        controller_pid=args.controller_pid,
        lifetime_seconds=args.lifetime,
        hold_seconds=args.hold_seconds,
        connect_timeout_seconds=args.connect_timeout,
        max_generations=args.max_generations,
        max_bytes_per_direction=args.max_bytes_per_direction,
        arm_after_client_bytes=args.arm_after_client_bytes,
    ).validate()


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        relay = LoginFaultRelay(config_from_args(args))
    except RelayError as error:
        raise SystemExit(f"login relay refused configuration: {error}")

    def request_stop(signum, frame) -> None:
        del signum, frame
        relay.stop("signal")

    for name in ("SIGINT", "SIGTERM"):
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), request_stop)

    try:
        relay.start()
        print(
            json.dumps(
                {
                    "event": "relay_ready",
                    "run_id": relay.config.run_id,
                    "bind_host": relay.endpoint.host,
                    "bind_port": relay.endpoint.port,
                    "mode": relay.config.mode,
                    "relay_pid": os.getpid(),
                },
                ensure_ascii=True,
                separators=(",", ":"),
                sort_keys=True,
            ),
            flush=True,
        )
        relay.wait()
    finally:
        relay.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
