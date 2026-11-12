"""Coordinate one already-running two-human native imported mission.

The actors must already be separately prepared and launched. This tool only
publishes run-scoped FriendSession commands, reads native receipts/events, and
optionally reads the server observer stream. It never launches or stops a
process and never edits SQL or game state.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import time

import game_test
from game_test_friends import FriendActor, assert_receipt, find_friend

GATE_PREFIX = "native-map-gate "


def _integer(mapping: dict, key: str, *, minimum: int = 0) -> int:
    try:
        value = int(mapping[key])
    except (KeyError, TypeError, ValueError) as error:
        raise game_test.GameTestError(f"native receipt lacks integer {key}") from error
    if value < minimum:
        raise game_test.GameTestError(f"native receipt has invalid {key}")
    return value


def validate_pair(host: FriendActor, peer: FriendActor) -> int:
    if host.metadata["run_id"] == peer.metadata["run_id"]:
        raise game_test.GameTestError("mission actors must have distinct run IDs")
    if Path(host.metadata["client"]["root"]).resolve() == Path(peer.metadata["client"]["root"]).resolve():
        raise game_test.GameTestError("mission actors must have distinct client roots")
    plans = (host.metadata["plan"], peer.metadata["plan"])
    for plan in plans:
        if (
            plan.get("friends_only") is not True
            or plan.get("mission_mode") != 5
            or type(plan.get("mission_game_mode_id")) is not int
            or plan["mission_game_mode_id"] <= 0
            or plan.get("mission_ai_count") != 0
            or plan.get("mission_behavior_profile") is not None
            or plan.get("mission_respawns") is not None
            or plan.get("play_unit_uid") is not None
        ):
            raise game_test.GameTestError(
                "mission actors require the sealed friends-only Mode5 zero-wingmen imported mission scope"
            )
    keys = ("target_host", "login_port", "target_server", "target_channel", "mission_game_mode_id")
    if any(plans[0].get(key) != plans[1].get(key) for key in keys):
        raise game_test.GameTestError("mission actors do not target the same sealed server, channel, and mission")
    return plans[0]["mission_game_mode_id"]


def parse_gate_samples(rows: list[dict], mission_id: int) -> list[dict]:
    samples = []
    for ordinal, row in enumerate(rows):
        if row.get("role") != "client" or row.get("event") != "observation" or row.get("case") != "friends.session":
            continue
        message = row.get("message", "")
        if not isinstance(message, str) or not message.startswith(GATE_PREFIX):
            continue
        try:
            sample = json.loads(message[len(GATE_PREFIX):])
        except json.JSONDecodeError as error:
            raise game_test.GameTestError("native map-gate observation is malformed") from error
        required = (
            "elapsed_ms", "game_mode", "block", "query_valid", "scenes", "found", "ray_hits",
            "hp", "obbs", "spheres", "phase_observer", "hide_count", "last_action", "last_object",
        )
        if any(type(sample.get(key)) is not int for key in required):
            raise game_test.GameTestError("native map-gate observation has a non-integer field")
        if sample["game_mode"] == mission_id:
            sample = dict(sample)
            sample["ordinal"] = ordinal
            samples.append(sample)
    return samples


def gate_transition(rows: list[dict], mission_id: int, expected_block: int | None = None) -> dict | None:
    closed = None
    for sample in parse_gate_samples(rows, mission_id):
        if expected_block is not None and sample["block"] != expected_block:
            continue
        if closed is None:
            if (
                sample["block"] > 0
                and sample["query_valid"] == 1
                and sample["found"] >= 1
                and sample["ray_hits"] > 0
                and sample["obbs"] + sample["spheres"] > 0
                and sample["hp"] > 0
                and sample["phase_observer"] == 1
            ):
                closed = sample
            continue
        if sample["block"] != closed["block"] or sample["elapsed_ms"] <= closed["elapsed_ms"]:
            continue
        if (
            sample["query_valid"] == 1
            and sample["scenes"] > 0
            and sample["found"] == 0
            and sample["ray_hits"] == 0
            and sample["obbs"] == 0
            and sample["spheres"] == 0
            and sample["phase_observer"] == 1
            and sample["hide_count"] > closed["hide_count"]
            and sample["last_action"] == 1
        ):
            return {"block": closed["block"], "closed": closed, "open": sample}
    return None


def wait_gate(actor: FriendActor, mission_id: int, baseline: int, timeout: float, expected_block: int | None = None) -> dict:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        transition = gate_transition(actor.native_events()[baseline:], mission_id, expected_block)
        if transition is not None:
            return transition
        time.sleep(0.25)
    raise game_test.GameTestError("native client did not observe the sealed gate closed then genuinely hidden")


def read_server_rows(path: Path) -> list[dict]:
    if not path.exists():
        return []
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines(keepends=True):
        if not line.endswith("\n"):
            break
        try:
            row = json.loads(line)
        except json.JSONDecodeError as error:
            raise game_test.GameTestError("server observer stream is malformed") from error
        if not isinstance(row, dict) or row.get("role") != "server":
            raise game_test.GameTestError("server observer stream contains an invalid record")
        rows.append(row)
    return rows


def server_battle_active(rows: list[dict], host_pilot: int, peer_pilot: int) -> dict | None:
    expected = {host_pilot, peer_pilot}
    for row in rows:
        if row.get("event") != "battle_active":
            continue
        try:
            humans = int(row.get("humans_team_1", -1)) + int(row.get("humans_team_2", -1))
            ai = int(row.get("ai_team_1", -1)) + int(row.get("ai_team_2", -1))
            slots = {
                int(slot["pilot_id"])
                for slot in row.get("slots", [])
                if not slot.get("is_ai") and int(slot.get("pilot_id", 0)) > 0
            }
            valid = (
                int(row.get("host_pilot_id", 0)) == host_pilot
                and int(row.get("mode", 0)) == 5
                and humans == 2
                and ai == 0
                and int(row.get("active_slots", 0)) == 2
                and slots == expected
            )
        except (TypeError, ValueError, KeyError):
            valid = False
        if valid:
            return row
    return None


def wait_server_phase(path: Path, host_pilot: int, peer_pilot: int, timeout: float) -> dict:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        observed = server_battle_active(read_server_rows(path), host_pilot, peer_pilot)
        if observed is not None:
            return observed
        time.sleep(0.25)
    raise game_test.GameTestError("server observer did not report the exact two-human Mode5 battle-active phase")


def abort(actors: tuple[FriendActor, FriendActor]) -> None:
    for actor in actors:
        try:
            config = game_test.ini_parser()
            config["run"] = {"id": actor.metadata["run_id"]}
            game_test.write_ini(actor.directory / "friend-abort.ini", config)
        except OSError:
            pass


def _room_matches(state: dict, room_id: int, *, host: bool) -> bool:
    room = state.get("room", {})
    try:
        return (
            int(room.get("actual_room_id", 0)) == room_id
            and int(room.get("player_count", 0)) == 2
            and int(room.get("room_mode", 0)) == 5
            and room.get("model_present") == "1"
            and room.get("local_is_host") == ("1" if host else "0")
        )
    except ValueError:
        return False


def _mission_receipt(state: dict, operation: int, room_id: int, host_pilot: int, *, local_host: bool) -> dict:
    receipt = state.get("mission_coop", {})
    if (
        _integer(receipt, "operation") != operation
        or _integer(receipt, "result") not in (4, 5, 6, 7)
        or _integer(receipt, "expected_host") != host_pilot
        or _integer(receipt, "host") != host_pilot
        or _integer(receipt, "room") != room_id
        or _integer(receipt, "humans") != (1 if operation == 43 else 2)
        or _integer(receipt, "occupied") != (1 if operation == 43 else 2)
        or _integer(receipt, "ai") != 0
        or _integer(receipt, "mode") != 5
        or receipt.get("local_host") != ("1" if local_host else "0")
        or receipt.get("roster_verified") != "1"
    ):
        raise game_test.GameTestError("native mission command receipt does not match the exact human roster")
    return receipt


def coordinate(host: FriendActor, peer: FriendActor, *, timeout: float, server_events: Path | None = None, capture: bool = False, external_confirmation: bool = False) -> dict:
    if not external_confirmation:
        raise game_test.GameTestError("mission invitation confirmation requires an explicit isolated native input driver")
    mission_id = validate_pair(host, peer)
    actors = (host, peer)
    for actor in actors:
        state = actor.state()
        actor.sequence = int(state.get("command", {}).get("sequence", "0")) if state else 0
    host_state = host.wait(lambda state: state.get("run", {}).get("ready") == "1", timeout)
    peer_state = peer.wait(lambda state: state.get("run", {}).get("ready") == "1", timeout)
    host_pilot = _integer(host_state.get("state", {}), "pilot", minimum=1)
    peer_pilot = _integer(peer_state.get("state", {}), "pilot", minimum=1)
    host_name = host_state.get("state", {}).get("own_name", "")
    peer_name = peer_state.get("state", {}).get("own_name", "")
    if host_pilot == peer_pilot or not host_name or not peer_name:
        raise game_test.GameTestError("native actors did not expose two distinct authenticated identities")
    if not find_friend(host_state, peer_pilot) or not find_friend(peer_state, host_pilot):
        raise game_test.GameTestError("mission actors are not genuine existing friends")

    for actor, other in ((host, peer_pilot), (peer, host_pilot)):
        assert_receipt(actor.command("refresh", target_id=other, timeout=timeout), sent=True)
        actor.wait(lambda state, pilot=other: bool(find_friend(state, pilot)), timeout)

    peer_lobby = assert_receipt(peer.command(
        "mission_lobby", target_id=peer_pilot, body=peer.metadata["plan"]["room_title"],
        human_count=1, room_id=0, timeout=timeout,
    ))
    peer_channel = _integer(peer_lobby.get("room", {}), "actual_channel_id", minimum=1)
    assert_receipt(host.command("refresh", target_id=peer_pilot, timeout=timeout), sent=True)
    # Match the existing native invitation campaign: select the recipient in
    # the lobby before the room scene takes ownership of the messenger.
    assert_receipt(host.command("open", target_id=peer_pilot, target_name=peer_name, timeout=timeout))
    created = assert_receipt(host.command(
        "mission_room", target_id=host_pilot, body=host.metadata["plan"]["room_title"],
        human_count=1, room_id=0, timeout=timeout,
    ))
    created_receipt = created.get("mission_coop", {})
    room_id = _integer(created_receipt, "room", minimum=1)
    _mission_receipt(created, 43, room_id, host_pilot, local_host=True)
    room = created.get("room", {})
    channel_id = _integer(room, "actual_channel_id", minimum=1)
    if channel_id != peer_channel:
        raise game_test.GameTestError("mission actors did not enter the same genuine mission channel")
    if _integer(room, "actual_room_id", minimum=1) != room_id:
        raise game_test.GameTestError("mission creation receipt disagrees with the native room model")

    sent = assert_receipt(host.command(
        "invite_send", target_id=peer_pilot, target_name=peer_name,
        channel_id=channel_id, room_id=room_id, timeout=timeout,
    ), sent=True)
    sender = sent.get("invite_sender", {})
    if _integer(sender, "request_opcode") != 0x067B or sender.get("recipient_correlated") != "1":
        raise game_test.GameTestError("mission invitation did not use the genuine correlated native transport")
    # Mission invitations use their own native question popup. Confirmation
    # belongs to the caller's isolated input driver; never claim the normal
    # invitation adapter's unrelated packet/prompt receipts for this path.
    print(json.dumps({"phase": "awaiting_native_invitation_confirmation", "room_id": room_id}), flush=True)
    host.wait(lambda state: _room_matches(state, room_id, host=True), timeout)
    peer.wait(lambda state: _room_matches(state, room_id, host=False), timeout)

    if capture:
        assert_receipt(peer.command("capture", label="mission-room-before-ready", timeout=timeout))
    ready = assert_receipt(peer.command(
        "mission_ready", target_id=host_pilot, human_count=2, room_id=room_id, timeout=timeout,
    ))
    ready_receipt = _mission_receipt(ready, 44, room_id, host_pilot, local_host=False)
    if ready_receipt.get("guest_ready") != "1" or _integer(ready_receipt, "ready_result") == 0:
        raise game_test.GameTestError("guest Ready lacks a native ready-model receipt in the exact mission room")

    host_gate_baseline = len(host.native_events())
    peer_gate_baseline = len(peer.native_events())
    started = assert_receipt(host.command(
        "mission_start", target_id=host_pilot, human_count=2, room_id=room_id, timeout=timeout,
    ))
    start_receipt = _mission_receipt(started, 45, room_id, host_pilot, local_host=True)
    if _integer(start_receipt, "mission_result") == 0:
        raise game_test.GameTestError("host Start lacks the original mission admission/start receipt")

    host.wait(lambda state: state.get("state", {}).get("app") == "22", timeout)
    peer.wait(lambda state: state.get("state", {}).get("app") == "22", timeout)
    server = wait_server_phase(server_events, host_pilot, peer_pilot, timeout) if server_events else None
    host_gate = wait_gate(host, mission_id, host_gate_baseline, timeout)
    peer_gate = wait_gate(peer, mission_id, peer_gate_baseline, timeout, host_gate["block"])

    captures = {}
    if capture:
        captures["host"] = assert_receipt(host.command("capture", label="mission-gate-open-host", timeout=timeout)).get("capture", {})
        captures["peer"] = assert_receipt(peer.command("capture", label="mission-gate-open-peer", timeout=timeout)).get("capture", {})

    result = {
        "schema": 1,
        "status": "pass" if server is not None else "client_gate_pass",
        "full_acceptance": server is not None,
        "mission_game_mode_id": mission_id,
        "room_id": room_id,
        "channel_id": channel_id,
        "host": {"run_id": host.metadata["run_id"], "pilot": host_pilot, "name": host_name, "gate": host_gate},
        "peer": {"run_id": peer.metadata["run_id"], "pilot": peer_pilot, "name": peer_name, "gate": peer_gate},
        "server_battle_active": server,
        "captures": captures,
    }
    if server is not None:
        host.command("finish", timeout=30)
        peer.command("finish", timeout=30)
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host-run", type=Path, required=True)
    parser.add_argument("--peer-run", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--server-events", type=Path)
    parser.add_argument("--timeout", type=float, default=420)
    parser.add_argument("--capture", action="store_true")
    parser.add_argument("--external-confirmation", action="store_true", help="an isolated input driver confirms the real mission invitation popup")
    args = parser.parse_args()
    if not args.external_confirmation:
        parser.error("--external-confirmation requires the caller's isolated native invitation input driver")
    host = FriendActor(args.host_run)
    peer = FriendActor(args.peer_run)
    try:
        result = coordinate(host, peer, timeout=args.timeout, server_events=args.server_events, capture=args.capture, external_confirmation=True)
        game_test.atomic_write(args.output, json.dumps(result, ensure_ascii=True, indent=2))
        print(json.dumps(result, ensure_ascii=True, indent=2))
    except BaseException as error:
        if not isinstance(error, SystemExit):
            abort((host, peer))
            failure = {"schema": 1, "status": "failed", "error_type": type(error).__name__, "error": str(error)}
            game_test.atomic_write(args.output, json.dumps(failure, ensure_ascii=True, indent=2))
        raise


if __name__ == "__main__":
    main()
