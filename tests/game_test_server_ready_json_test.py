import json
from pathlib import Path
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 2:
        return 2
    executable = Path(sys.argv[1])
    for source, enabled in (
        ("battle_rewards_bridge", True),
        ("direct_native_hook", True),
        ("disabled", False),
    ):
        completed = subprocess.run(
            [str(executable), "--emit-ready-json", source],
            check=True,
            capture_output=True,
            text=True,
            encoding="utf-8",
        )
        lines = completed.stdout.splitlines()
        if len(lines) != 1:
            raise AssertionError(
                f"{source}: expected one JSONL record, got {lines!r}"
            )
        ready = json.loads(lines[0])
        expected = {
            "run_id": "01234567-89ab-cdef-0123-456789abcdef",
            "role": "server",
            "suite": "pvp-ai",
            "event": "observer_ready",
            "status": "running",
            "normal_server_main": True,
            "pid": 12345,
            "host_key": "BattleServer-548A891D-0046C000",
            "nonce": "T7x",
            "expected_host_pilot_id": 9229,
            "mode": 3,
            "minimum_ai": 2,
            "require_end": True,
            "battle_ai_deaths": enabled,
            "death_source": source,
        }
        if ready != expected:
            raise AssertionError(
                f"{source}: observer_ready mismatch\n"
                f"actual={ready!r}\nexpected={expected!r}"
            )
    print("PASS observer_ready JSONL parses and binds all fields")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
