from __future__ import annotations

import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import uuid

from tools.game_test_login_relay import (
    Endpoint,
    FaultMode,
    LoginFaultRelay,
    RelayConfig,
    RelayError,
    parse_endpoint,
)


PAYLOAD_MARKERS = (
    b"private-client-request",
    b"private-server-reply",
    b"private-server-welcome",
)


class SyntheticServer:
    def __init__(
        self,
        reply: bytes = PAYLOAD_MARKERS[1],
        welcome: bytes = b"",
    ):
        self.reply = reply
        self.welcome = welcome
        self.received: list[bytes] = []
        self.accepted = threading.Event()
        self.closed = threading.Event()
        self.eof_times: list[float] = []
        self._stop = threading.Event()
        self._workers: list[threading.Thread] = []
        self._listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listener.bind(("127.0.0.1", 0))
        self.endpoint = Endpoint(*self._listener.getsockname()[:2])
        self._listener.listen(8)
        self._listener.settimeout(0.1)
        self._thread = threading.Thread(
            target=self._accept, name="synthetic-login-server", daemon=True
        )
        self._thread.start()

    def _accept(self) -> None:
        while not self._stop.is_set():
            try:
                channel, _ = self._listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            self.accepted.set()
            worker = threading.Thread(
                target=self._serve,
                args=(channel,),
                name="synthetic-login-connection",
                daemon=True,
            )
            self._workers.append(worker)
            worker.start()

    def _serve(self, channel: socket.socket) -> None:
        channel.settimeout(0.1)
        try:
            if self.welcome:
                channel.sendall(self.welcome)
            while not self._stop.is_set():
                try:
                    data = channel.recv(65536)
                except socket.timeout:
                    continue
                except ConnectionError:
                    self.eof_times.append(time.monotonic())
                    break
                if not data:
                    self.eof_times.append(time.monotonic())
                    break
                self.received.append(data)
                if self.reply:
                    try:
                        channel.sendall(self.reply)
                    except ConnectionError:
                        break
        finally:
            channel.close()
            self.closed.set()

    def stop(self) -> None:
        self._stop.set()
        self._listener.close()
        self._thread.join(timeout=2)
        for worker in self._workers:
            worker.join(timeout=2)

    def __enter__(self) -> "SyntheticServer":
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.stop()


class RelayCase(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def config(
        self,
        upstream: Endpoint,
        mode: str,
        *,
        lifetime: float = 3.0,
        hold: float = 0.25,
        max_generations: int = 8,
        receipt_name: str = "relay.jsonl",
        controller_pid: int | None = None,
        arm_after_client_bytes: int = 0,
        require_controller_arm: bool = False,
    ) -> RelayConfig:
        return RelayConfig(
            bind=Endpoint("127.91.2.1", 0),
            upstream=upstream,
            mode=mode,
            receipt=self.root / receipt_name,
            run_id=str(uuid.uuid4()),
            controller_pid=controller_pid or os.getpid(),
            lifetime_seconds=lifetime,
            hold_seconds=hold,
            connect_timeout_seconds=0.5,
            max_generations=max_generations,
            max_bytes_per_direction=1024 * 1024,
            arm_after_client_bytes=arm_after_client_bytes,
            require_controller_arm=require_controller_arm,
        )

    def connect(self, relay: LoginFaultRelay) -> socket.socket:
        channel = socket.create_connection(
            (relay.endpoint.host, relay.endpoint.port), timeout=1
        )
        channel.settimeout(1.0)
        return channel

    def events(self, receipt_name: str = "relay.jsonl") -> list[dict]:
        path = self.root / receipt_name
        return [
            json.loads(line)
            for line in path.read_text(encoding="utf-8").splitlines()
        ]

    def wait_for_events(
        self,
        event: str,
        count: int,
        *,
        receipt_name: str = "relay.jsonl",
        timeout: float = 1.0,
    ) -> list[dict]:
        deadline = time.monotonic() + timeout
        rows: list[dict] = []
        while time.monotonic() < deadline:
            rows = [
                row
                for row in self.events(receipt_name)
                if row["event"] == event
            ]
            if len(rows) >= count:
                return rows
            time.sleep(0.01)
        self.fail(
            f"receipt did not record {count} {event} event(s): {rows}"
        )

    def assert_metadata_only(
        self, receipt_name: str = "relay.jsonl"
    ) -> None:
        content = (self.root / receipt_name).read_bytes()
        for marker in PAYLOAD_MARKERS:
            self.assertNotIn(marker, content)
        forbidden = {
            "payload",
            "data",
            "bytes_content",
            "username",
            "password",
            "credential",
        }
        for event in self.events(receipt_name):
            self.assertTrue(forbidden.isdisjoint(event))

    def exchange(
        self, relay: LoginFaultRelay, *, expect_reply: bool
    ) -> bytes | None:
        with self.connect(relay) as client:
            if not expect_reply:
                client.settimeout(1.0)
            client.sendall(PAYLOAD_MARKERS[0])
            try:
                value = client.recv(65536)
            except (ConnectionError, socket.timeout):
                value = None
        if expect_reply:
            self.assertEqual(value, PAYLOAD_MARKERS[1])
        else:
            self.assertNotEqual(value, PAYLOAD_MARKERS[1])
        return value

    def test_configuration_is_loopback_sealed_and_exclusive(self) -> None:
        with self.assertRaises(RelayError):
            self.config(
                Endpoint("192.0.2.1", 5001), FaultMode.PASS_THROUGH
            ).validate()
        with self.assertRaises(RelayError):
            RelayConfig(
                **{
                    **self.config(
                        Endpoint("127.0.0.1", 5001),
                        FaultMode.PASS_THROUGH,
                    ).__dict__,
                    "bind": Endpoint("0.0.0.0", 0),
                }
            ).validate()
        with self.assertRaises(RelayError):
            self.config(
                Endpoint("127.0.0.1", 5001),
                FaultMode.FIRST_SERVER_BLACKHOLE,
                arm_after_client_bytes=-1,
            ).validate()
        with self.assertRaises(RelayError):
            self.config(
                Endpoint("127.0.0.1", 5001),
                FaultMode.PASS_THROUGH,
                require_controller_arm=True,
            ).validate()
        with self.assertRaises(RelayError):
            RelayConfig(
                **{
                    **self.config(
                        Endpoint("127.0.0.1", 5001),
                        FaultMode.PASS_THROUGH,
                    ).__dict__,
                    "run_id": "not-a-run",
                }
            ).validate()
        existing = self.root / "existing.jsonl"
        existing.write_text("preserve", encoding="utf-8")
        with self.assertRaises(RelayError):
            self.config(
                Endpoint("127.0.0.1", 5001),
                FaultMode.PASS_THROUGH,
                receipt_name=existing.name,
            ).validate()
        self.assertEqual(
            parse_endpoint("127.8.9.10:0", allow_zero=True),
            Endpoint("127.8.9.10", 0),
        )

    def test_pass_through_and_receipt_has_only_metadata(self) -> None:
        with SyntheticServer() as server:
            relay = LoginFaultRelay(
                self.config(server.endpoint, FaultMode.PASS_THROUGH)
            )
            with relay:
                self.exchange(relay, expect_reply=True)
            events = self.events()
            closed = [
                row
                for row in events
                if row["event"] == "generation_closed"
            ][0]
            self.assertEqual(
                closed["client_bytes"], len(PAYLOAD_MARKERS[0])
            )
            self.assertEqual(
                closed["server_bytes"], len(PAYLOAD_MARKERS[1])
            )
            self.assertEqual(closed["dropped_server_bytes"], 0)
            self.assertEqual(events[-1]["workers_remaining"], 0)
            self.assert_metadata_only()

    def test_first_generation_blackholes_only_server_reply(self) -> None:
        with SyntheticServer() as server:
            relay = LoginFaultRelay(
                self.config(
                    server.endpoint,
                    FaultMode.FIRST_SERVER_BLACKHOLE,
                )
            )
            with relay:
                self.exchange(relay, expect_reply=False)
                self.exchange(relay, expect_reply=True)
            closed = [
                row
                for row in self.events()
                if row["event"] == "generation_closed"
            ]
            self.assertEqual(len(closed), 2)
            self.assertGreater(closed[0]["dropped_server_bytes"], 0)
            self.assertEqual(closed[1]["dropped_server_bytes"], 0)
            self.assert_metadata_only()

    def test_reset_after_first_client_bytes_then_passes(self) -> None:
        with SyntheticServer() as server:
            relay = LoginFaultRelay(
                self.config(
                    server.endpoint,
                    FaultMode.RESET_AFTER_CLIENT_BYTES,
                )
            )
            with relay:
                self.exchange(relay, expect_reply=False)
                self.exchange(relay, expect_reply=True)
            events = self.events()
            resets = [
                row
                for row in events
                if row["event"] == "fault_reset_after_client_bytes"
            ]
            self.assertEqual(len(resets), 1)
            self.assertEqual(resets[0]["generation"], 1)
            self.assertGreater(resets[0]["client_bytes"], 0)
            self.assert_metadata_only()

    def test_all_generation_blackhole_never_forwards_reply(self) -> None:
        with SyntheticServer() as server:
            relay = LoginFaultRelay(
                self.config(
                    server.endpoint,
                    FaultMode.ALL_SERVER_BLACKHOLE,
                )
            )
            with relay:
                self.exchange(relay, expect_reply=False)
                self.wait_for_events("server_bytes_dropped", 1)
                self.exchange(relay, expect_reply=False)
                self.wait_for_events("server_bytes_dropped", 2)
            closed = [
                row
                for row in self.events()
                if row["event"] == "generation_closed"
            ]
            self.assertEqual(len(closed), 2)
            self.assertTrue(
                all(row["dropped_server_bytes"] > 0 for row in closed)
            )
            self.assertTrue(
                all(row["forwarded_server_bytes"] == 0 for row in closed)
            )
            self.assert_metadata_only()

    def test_first_blackhole_forwards_welcome_until_client_bytes_arm_it(
        self,
    ) -> None:
        welcome = PAYLOAD_MARKERS[2]
        request = PAYLOAD_MARKERS[0]
        first_chunk = request[:1]
        remaining_request = request[1:]
        with SyntheticServer(welcome=welcome) as server:
            relay = LoginFaultRelay(
                self.config(
                    server.endpoint,
                    FaultMode.FIRST_SERVER_BLACKHOLE,
                    arm_after_client_bytes=len(request),
                )
            )
            with relay:
                with self.connect(relay) as first:
                    self.assertEqual(first.recv(65536), welcome)
                    first.sendall(first_chunk)
                    self.assertEqual(
                        first.recv(65536), PAYLOAD_MARKERS[1]
                    )
                    first.sendall(remaining_request)
                    first.settimeout(1)
                    with self.assertRaises(socket.timeout):
                        first.recv(65536)
                armed = self.wait_for_events(
                    "server_blackhole_armed", 1
                )
                self.assertEqual(armed[0]["generation"], 1)
                self.assertEqual(
                    armed[0]["arm_after_client_bytes"], len(request)
                )
                self.assertEqual(armed[0]["client_bytes"], len(request))
                self.wait_for_events("server_bytes_dropped", 1)

                with self.connect(relay) as second:
                    self.assertEqual(second.recv(65536), welcome)
                    second.sendall(request)
                    self.assertEqual(
                        second.recv(65536), PAYLOAD_MARKERS[1]
                    )
            self.assert_metadata_only()

    def test_all_blackhole_rearms_after_welcome_for_each_generation(
        self,
    ) -> None:
        welcome = PAYLOAD_MARKERS[2]
        with SyntheticServer(welcome=welcome) as server:
            relay = LoginFaultRelay(
                self.config(
                    server.endpoint,
                    FaultMode.ALL_SERVER_BLACKHOLE,
                    arm_after_client_bytes=1,
                )
            )
            with relay:
                for generation in (1, 2):
                    with self.connect(relay) as client:
                        self.assertEqual(client.recv(65536), welcome)
                        client.sendall(PAYLOAD_MARKERS[0])
                        client.settimeout(1)
                        with self.assertRaises(socket.timeout):
                            client.recv(65536)
                    self.wait_for_events(
                        "server_blackhole_armed", generation
                    )
                    self.wait_for_events(
                        "server_bytes_dropped", generation
                    )
            armed = [
                row
                for row in self.events()
                if row["event"] == "server_blackhole_armed"
            ]
            self.assertEqual(
                [row["generation"] for row in armed], [1, 2]
            )
            self.assert_metadata_only()

    def test_controller_arm_precedes_framed_chunked_wire_traffic(
        self,
    ) -> None:
        welcome = PAYLOAD_MARKERS[2]
        wire_request = b"W" * 93
        with SyntheticServer(welcome=welcome) as server:
            relay = LoginFaultRelay(
                self.config(
                    server.endpoint,
                    FaultMode.FIRST_SERVER_BLACKHOLE,
                    arm_after_client_bytes=1,
                    require_controller_arm=True,
                )
            )
            with relay:
                with self.connect(relay) as client:
                    self.assertEqual(client.recv(65536), welcome)
                    self.assertFalse([
                        row
                        for row in self.events()
                        if row["event"] == "server_blackhole_armed"
                    ])
                    self.assertTrue(
                        relay.arm_fault_before_native_send()
                    )
                    self.assertFalse(
                        relay.arm_fault_before_native_send()
                    )
                    client.sendall(wire_request[:17])
                    client.sendall(wire_request[17:61])
                    client.sendall(wire_request[61:])
                    client.settimeout(1)
                    with self.assertRaises(socket.timeout):
                        client.recv(65536)
                self.wait_for_events(
                    "server_blackhole_armed", 1
                )
                self.wait_for_events(
                    "server_bytes_dropped", 1
                )
            events = self.events()
            controller = next(
                row
                for row in events
                if row["event"] == "controller_fault_armed"
            )
            injected = next(
                row
                for row in events
                if row["event"] == "server_blackhole_armed"
            )
            self.assertLess(
                controller["sequence"], injected["sequence"]
            )
            self.assertEqual(
                controller["trigger"],
                "sealed_ready_to_send_observation",
            )
            self.assertGreater(injected["client_bytes"], 0)
            closed = next(
                row
                for row in events
                if row["event"] == "generation_closed"
            )
            self.assertEqual(
                closed["client_bytes"], len(wire_request)
            )
            self.assert_metadata_only()

    def test_controller_armed_reset_waits_for_complete_declared_request(
        self,
    ) -> None:
        welcome = PAYLOAD_MARKERS[2]
        request = b"R" * 93
        with SyntheticServer(welcome=welcome) as server:
            relay = LoginFaultRelay(
                self.config(
                    server.endpoint,
                    FaultMode.RESET_AFTER_CLIENT_BYTES,
                    arm_after_client_bytes=1,
                    require_controller_arm=True,
                )
            )
            with relay:
                with self.connect(relay) as client:
                    self.assertEqual(client.recv(65536), welcome)
                    self.assertTrue(
                        relay.arm_fault_before_native_send()
                    )
                    client.sendall(request)
                    client.settimeout(1)
                    with self.assertRaises(
                        (ConnectionError, socket.timeout)
                    ):
                        value = client.recv(65536)
                        if value == b"":
                            raise ConnectionResetError()
                reset = self.wait_for_events(
                    "fault_reset_after_client_bytes", 1
                )[0]
                self.assertGreater(reset["client_bytes"], 0)
            self.assert_metadata_only()

    def test_client_close_holds_real_upstream_socket(self) -> None:
        hold = 0.3
        with SyntheticServer(reply=b"") as server:
            relay = LoginFaultRelay(
                self.config(
                    server.endpoint,
                    FaultMode.HOLD_UPSTREAM_ON_CLIENT_CLOSE,
                    hold=hold,
                )
            )
            with relay:
                client = self.connect(relay)
                client.sendall(PAYLOAD_MARKERS[0])
                self.assertTrue(server.accepted.wait(1))
                deadline = time.monotonic() + 1
                while (
                    not server.received and time.monotonic() < deadline
                ):
                    time.sleep(0.01)
                self.assertTrue(server.received)
                closed_at = time.monotonic()
                client.close()
                time.sleep(hold / 2)
                self.assertFalse(
                    server.eof_times,
                    "upstream closed before the configured stale hold",
                )
                self.assertTrue(server.closed.wait(hold + 1))
                self.assertGreaterEqual(
                    server.eof_times[0] - closed_at, hold * 0.75
                )
            event_names = [row["event"] for row in self.events()]
            self.assertIn("upstream_hold_started", event_names)
            self.assertIn("upstream_hold_completed", event_names)
            self.assert_metadata_only()

    def test_lifetime_and_controller_identity_bound_the_run(self) -> None:
        with SyntheticServer() as server:
            lifetime_relay = LoginFaultRelay(
                self.config(
                    server.endpoint,
                    FaultMode.PASS_THROUGH,
                    lifetime=0.15,
                    receipt_name="lifetime.jsonl",
                )
            )
            lifetime_relay.start()
            self.assertTrue(lifetime_relay.wait(1))
            lifetime_relay.stop()
            self.assertEqual(
                self.events("lifetime.jsonl")[-1]["reason"],
                "lifetime_expired",
            )

            controller = subprocess.Popen(
                [
                    sys.executable,
                    "-c",
                    "import time; time.sleep(1)",
                ]
            )
            controller_relay: LoginFaultRelay | None = None
            try:
                controller_relay = LoginFaultRelay(
                    self.config(
                        server.endpoint,
                        FaultMode.PASS_THROUGH,
                        lifetime=3,
                        receipt_name="controller.jsonl",
                        controller_pid=controller.pid,
                    )
                )
                controller_relay.start()
                controller.wait(timeout=3)
                self.assertTrue(controller_relay.wait(3))
                controller_relay.stop()
                self.assertEqual(
                    self.events("controller.jsonl")[-1]["reason"],
                    "controller_exited",
                )
            finally:
                if controller.poll() is None:
                    controller.kill()
                    controller.wait(timeout=5)
                if controller_relay is not None:
                    controller_relay.stop()


if __name__ == "__main__":
    unittest.main()
