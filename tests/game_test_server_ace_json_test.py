import json
from pathlib import Path
import subprocess
import sys


PREFIX = "ace-v1 "
CLIENT_UNIT_KEYS = {
    "pilot_id",
    "native_pilot_id",
    "object_id",
    "npc_id",
    "machine_id",
    "generation",
    "current_hp",
    "max_hp",
    "slot",
    "form",
    "base_difficulty",
    "effective_difficulty",
    "ace",
    "team",
    "colour",
    "active",
    "name_matches",
    "slot_name_valid",
    "expected_name",
    "slot_name",
}
SERVER_UNIT_KEYS = {
    "pilot_id",
    "native_pilot_id",
    "npc_id",
    "machine_id",
    "generation",
    "current_hp",
    "max_hp",
    "original_max_hp",
    "hp_percent",
    "attack_percent",
    "slot",
    "form",
    "base_difficulty",
    "effective_difficulty",
    "ace",
    "team",
    "colour",
    "active",
}
DAMAGE_KEYS = {
    "sequence",
    "room_cookie",
    "generation",
    "attacker_pilot_id",
    "native_pilot_id",
    "attack_type",
    "proposed_damage",
    "adjusted_damage",
    "native_result",
    "attack_percent",
    "source",
    "ace",
    "policy_matched",
}


def parse_message(line: str) -> dict:
    if not line.startswith(PREFIX):
        raise AssertionError(f"missing exact {PREFIX!r} prefix: {line!r}")
    value = json.loads(line[len(PREFIX) :])
    if not isinstance(value, dict):
        raise AssertionError(f"receipt is not an object: {value!r}")
    return value


def assert_client(value: dict, scope: str, sequence_name: str, sequence: int) -> None:
    expected_keys = {"kind", "scope", sequence_name, "unit_count", "units"}
    if set(value) != expected_keys:
        raise AssertionError(f"{scope} client keys: {set(value)!r}")
    if value["kind"] != "client" or value["scope"] != scope:
        raise AssertionError(f"{scope} client identity: {value!r}")
    if value[sequence_name] != sequence or value["unit_count"] != 1:
        raise AssertionError(f"{scope} client sequence/count: {value!r}")
    if len(value["units"]) != 1 or set(value["units"][0]) != CLIENT_UNIT_KEYS:
        raise AssertionError(f"{scope} client unit schema: {value['units']!r}")
    unit = value["units"][0]
    if unit["expected_name"] != '红队 "Ace"\\\n':
        raise AssertionError(f"expected_name escape/readback: {unit['expected_name']!r}")
    if unit["slot_name"] != "蓝队\t机体":
        raise AssertionError(f"slot_name escape/readback: {unit['slot_name']!r}")


def main() -> int:
    if len(sys.argv) != 2:
        return 2
    executable = Path(sys.argv[1])
    completed = subprocess.run(
        [str(executable), "--emit-ace-json"],
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    lines = completed.stdout.splitlines()
    if len(lines) != 5:
        raise AssertionError(f"expected five JSONL records, got {lines!r}")

    pvp = parse_message(lines[0])
    mission = parse_message(lines[1])
    announcement = parse_message(lines[2])
    playback = parse_message(lines[3])
    outer = json.loads(lines[4])

    assert_client(pvp, "pvp", "battle_sequence", 8)
    assert_client(mission, "mission", "battle_generation", 19)

    if set(announcement) != {
        "kind",
        "scope",
        "sequence",
        "source",
        "colour",
        "feed_count_before",
        "feed_count_after",
        "successful_count",
        "text",
    }:
        raise AssertionError(f"announcement schema: {announcement!r}")
    if announcement != {
        "kind": "announcement",
        "scope": "pvp",
        "sequence": 5,
        "source": 1,
        "colour": 2,
        "feed_count_before": 10,
        "feed_count_after": 11,
        "successful_count": 2,
        "text": "蓝队出现了王牌驾驶员！\n",
    }:
        raise AssertionError(f"announcement readback: {announcement!r}")

    if set(playback) != {
        "kind",
        "scope",
        "playback_sequence",
        "playback_object_id",
        "playback_generation",
        "playback_pilot_id",
        "playback_machine_id",
        "playback_slot",
        "playback_colour",
        "playback_native_result",
        "playback_name_refreshed",
    }:
        raise AssertionError(f"playback schema: {playback!r}")
    if playback["kind"] != "playback" or playback["scope"] != "mission":
        raise AssertionError(f"playback identity: {playback!r}")
    if playback["playback_sequence"] != 4 or playback["playback_name_refreshed"] != 1:
        raise AssertionError(f"playback values: {playback!r}")

    expected_outer_keys = {
        "run_id",
        "role",
        "suite",
        "event",
        "pid",
        "nonce",
        "room_cookie",
        "message",
    }
    if set(outer) != expected_outer_keys:
        raise AssertionError(f"server outer schema: {outer!r}")
    if outer["run_id"] != "01234567-89ab-cdef-0123-456789abcdef":
        raise AssertionError(f"server run binding: {outer!r}")
    if outer["role"] != "server" or outer["suite"] != "pvp-ai":
        raise AssertionError(f"server role/suite binding: {outer!r}")
    if outer["event"] != "observation" or outer["nonce"] != "T7x":
        raise AssertionError(f"server event/nonce binding: {outer!r}")
    if outer["pid"] <= 0 or outer["room_cookie"] != 0x10203040:
        raise AssertionError(f"server pid/room binding: {outer!r}")

    server = parse_message(outer["message"])
    if set(server) != {
        "kind",
        "scope",
        "room_cookie",
        "mode",
        "generation",
        "unit_count",
        "units",
        "damage",
    }:
        raise AssertionError(f"server message schema: {server!r}")
    if server["kind"] != "server" or server["scope"] != "pvp":
        raise AssertionError(f"server message identity: {server!r}")
    if server["unit_count"] != 1 or len(server["units"]) != 1:
        raise AssertionError(f"server unit count: {server!r}")
    if set(server["units"][0]) != SERVER_UNIT_KEYS:
        raise AssertionError(f"server unit schema: {server['units'][0]!r}")
    if server["units"][0]["original_max_hp"] != 15000:
        raise AssertionError(f"server raw HP seed: {server['units'][0]!r}")
    if set(server["damage"]) != DAMAGE_KEYS:
        raise AssertionError(f"server damage schema: {server['damage']!r}")
    if server["damage"]["adjusted_damage"] != 1500:
        raise AssertionError(f"server damage readback: {server['damage']!r}")

    print("PASS ace-v1 JSONL parses and preserves every receipt schema")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
