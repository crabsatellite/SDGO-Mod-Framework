from __future__ import annotations

from datetime import datetime, timezone
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

from manager import storage
from manager.engines.operators import model, runtime, server, workspace


def write(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(data, str):
        path.write_text(data, encoding="utf-8", newline="\n")
    else:
        path.write_bytes(data)
    return path


def fake_pe(path, *, stamp=0x548A891D, image_size=0x0046C000):
    data = bytearray(0x300)
    data[:2] = b"MZ"
    nt = 0x80
    struct.pack_into("<I", data, 0x3C, nt)
    data[nt:nt + 4] = b"PE\0\0"
    struct.pack_into("<H", data, nt + 4, 0x14C)
    struct.pack_into("<I", data, nt + 8, stamp)
    struct.pack_into("<H", data, nt + 20, 0xE0)
    struct.pack_into("<H", data, nt + 24, 0x10B)
    struct.pack_into("<I", data, nt + 24 + 0x38, image_size)
    return write(path, bytes(data))


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def rewrite_manifest_representation(path):
    path = Path(path)
    text, _, _ = storage.decode_ini(path.read_bytes())
    represented = (
        "\ufeff; normalized by the native host\r\n"
        + text.replace("\r\n", "\n").replace("\n", "\r\n").rstrip("\r\n")
        + "\r\n\r\n"
    )
    path.write_bytes(represented.encode("utf-8"))


def abilities():
    return {
        "schema_version": 1,
        "mode": "custom",
        "general": {
            "gp_percent": 10,
            "unit_exp_percent": 20,
            "energy_save_percent": 30,
        },
        "victory": {
            "gp_percent": 11,
            "unit_exp_percent": 21,
            "energy_save_percent": 31,
        },
        "defeat": {
            "gp_percent": 12,
            "unit_exp_percent": 22,
            "energy_save_percent": 32,
        },
        "timed": [],
        "pilot_exp_percent": 25,
        "mission_parts": True,
        "extensions": {"bonus": {"scale": 2}},
    }


def catalog():
    operators = []
    for identity, managed, enabled in (
        (60001, True, True),
        (60002, False, True),
        (60003, True, False),
    ):
        operators.append({
            "id": identity,
            "category_id": model.native_category(identity),
            "template_id": identity,
            "name": f"Operator {identity}",
            "managed": managed,
            "enabled": enabled,
            "delivery": "product",
            "avatar": {"kind": "native", "operator_id": identity},
            "drops": [],
            "abilities": abilities(),
        })
    return {
        "schema_version": 1,
        "native_operator_ids": [60001, 60002, 60003],
        "items": [],
        "operators": operators,
        "registered_ability_extensions": ["bonus"],
    }


class FakeCursor:
    def __init__(self, rows):
        self.rows = rows
        self.statement = None

    def execute(self, statement):
        self.statement = statement
        if statement != (
            "SELECT AddrPT,ServerID FROM dbo.tbl_ChannelInfo ORDER BY AddrPT"
        ):
            raise AssertionError("unexpected SQL: " + statement)
        return self

    def fetchall(self):
        return list(self.rows)


class FakeConnection:
    def __init__(self, rows):
        self.cursor_value = FakeCursor(rows)
        self.rolled_back = False
        self.closed = False

    def cursor(self):
        return self.cursor_value

    def rollback(self):
        self.rolled_back = True

    def close(self):
        self.closed = True


class OperatorRuntimeFixture(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.server = self.root / "native server"
        self.package = self.root / "stage"
        self.server_ini = write(
            self.root / "server.ini",
            "[DB Game]\nDBServerName=fixture\nDBName=fixture\nID=x\nPWD=y\n",
        )
        self.hosts = {}
        for number in (1, 2, 3):
            host = self.server / "Server" / f"BattleServer{number}"
            executable = fake_pe(host / "BattleServer.exe")
            write(
                host / "sip.txt",
                f"LISTEN 127.0.0.1:{9300 + number} AGENT\n",
            )
            write(host / "winmm.dll", b"installed loader")
            write(host / "sdgo/core.dll", b"installed core")
            self.hosts[number] = {
                "root": host,
                "exe": executable,
                "sha": sha256(executable),
            }
        self.write_startup(enabled=(1, 3), disabled=(2,))
        self.make_package()
        self.connections = []

    def tearDown(self):
        self.temporary.cleanup()

    def write_startup(self, *, enabled=(), disabled=(), extra=""):
        rows = []
        for number in enabled:
            rows.append(
                f'<Process Run="True" '
                f'Path=".\\Server\\BattleServer{number}\\BattleServer.exe" />'
            )
        for number in disabled:
            rows.append(
                f'<Process Run="False" '
                f'Path=".\\Server\\BattleServer{number}\\BattleServer.exe" />'
            )
        write(
            self.server / "ServerStartUp/ServerStartUp.xml",
            "<Processes>\n" + "\n".join(rows) + extra + "\n</Processes>\n",
        )

    def connect(self, rows=((9301, 1), (9302, 2))):
        def value(_server_ini):
            connection = FakeConnection(rows)
            self.connections.append(connection)
            return connection
        return value

    def make_package(self):
        server = self.package / "server"
        operator = server / "sdgo/mods/operator_catalog_server"
        rewards = server / "sdgo/mods/battle_rewards"
        operator_manifest = """\
[mod]
distribution=public
id=operator_catalog_server
category=gameplay
feature=operator-assistant
channel=stable
activation=manual
name=Operator ability runtime
version=0.1.0
api=1
dll=operator_catalog_server.dll
enabled=1
order=150
hosts=BattleServer-548A891D-0046C000
requires=battle.rewards.final.mission
files=effects.example.ini,symbols.fragment.ini
config_files=effects.ini
"""
        rewards_manifest = """\
[mod]
distribution=public
id=battle_rewards
category=gameplay
feature=battle-rewards
channel=stable
activation=manual
name=Battle contribution rewards
version=0.4.0
api=1
dll=battle_rewards.dll
enabled=0
order=110
hosts=BattleServer-548A891D-0046C000
requires=battle.rewards.net_send
files=config.example.ini
config_files=config.ini

[settings]
RuntimeDirectory=
InstanceId=
GpPolicy=replace
Trace=0
"""
        write(operator / "mod.ini", operator_manifest)
        write(operator / "manager.json", "{}\n")
        fake_pe(operator / "operator_catalog_server.dll")
        write(operator / "effects.example.ini", "[effects]\nVersion=1\n")
        write(
            operator / "symbols.fragment.ini",
            "[host]\nimage=BattleServer.exe\n",
        )
        write(rewards / "mod.ini", rewards_manifest)
        write(rewards / "manager.json", "{}\n")
        fake_pe(rewards / "battle_rewards.dll")
        write(rewards / "config.example.ini", "[settings]\nTrace=0\n")
        write(
            server / f"sdgo/symbols/{runtime.HOST_KEY}.ini",
            """\
[host]
image=BattleServer.exe
timestamp=0x548A891D
size_of_image=0x0046C000

[battle.rewards.final.mission]
kind=site
rva=0x0009F26A

[battle.rewards.net_send]
kind=code
rva=0x00324020
""",
        )

    def supported(self):
        return mock.patch.dict(
            runtime.SUPPORTED_BATTLE_HOSTS,
            {runtime.HOST_KEY: self.hosts[1]["sha"]},
            clear=True,
        )

    def plan(self, *, rows=((9301, 1), (9302, 2)), processes=()):
        with self.supported():
            return runtime.plan_runtime_install(
                self.server,
                self.package,
                catalog(),
                self.server_ini,
                connect_fn=self.connect(rows),
                process_snapshot_fn=lambda: list(processes),
            )


class OperatorRuntimePlanningTests(OperatorRuntimeFixture):
    def test_native_windows_symbol_line_endings_are_supported(self):
        path=self.package / f"server/sdgo/symbols/{runtime.HOST_KEY}.ini"
        path.write_bytes(path.read_bytes().replace(b'\r\n',b'\n').replace(b'\n',b'\r\n'))
        plan=self.plan()
        self.assertTrue(plan['ready'],plan['blockers'])

    def test_selects_only_xml_and_database_intersection(self):
        plan = self.plan()
        self.assertTrue(plan["ready"], plan["blockers"])
        self.assertEqual(
            [row["folder"] for row in plan["selected_hosts"]],
            ["BattleServer1"],
        )
        self.assertEqual(
            [(row["folder"], row["port"]) for row in plan["skipped_hosts"]],
            [("BattleServer3", 9303)],
        )
        self.assertNotIn("BattleServer2", json.dumps(plan))
        self.assertTrue(plan["actions"])
        self.assertFalse(any(
            isinstance(value, (bytes, bytearray))
            for value in plan.values()
        ))
        self.assertTrue(all(
            connection.rolled_back and connection.closed
            for connection in self.connections
        ))

    def test_out_of_bounds_startup_entry_is_rejected(self):
        self.write_startup(
            enabled=(),
            extra=(
                '\n<Process Run="True" '
                'Path="..\\outside\\BattleServer1\\BattleServer.exe" />'
            ),
        )
        plan = self.plan()
        self.assertFalse(plan["ready"])
        self.assertIn("unsafe", " ".join(plan["blockers"]))
        self.assertFalse(plan["actions"])

    def test_reparse_host_is_rejected(self):
        original = runtime._is_reparse

        def marked(path):
            path = Path(path)
            return path.name == "BattleServer1" or original(path)

        with mock.patch.object(runtime, "_is_reparse", side_effect=marked):
            plan = self.plan()
        self.assertFalse(plan["ready"])
        self.assertIn("reparse point", " ".join(plan["blockers"]))

    def test_unsupported_executable_is_rejected(self):
        with mock.patch.dict(runtime.SUPPORTED_BATTLE_HOSTS, {}, clear=True):
            plan = runtime.plan_runtime_install(
                self.server,
                self.package,
                catalog(),
                self.server_ini,
                connect_fn=self.connect(),
                process_snapshot_fn=lambda: [],
            )
        self.assertFalse(plan["ready"])
        self.assertIn(
            "unsupported BattleServer executable",
            " ".join(plan["blockers"]),
        )

    def test_missing_framework_reports_first_install_prerequisite(self):
        (self.hosts[1]["root"] / "sdgo/core.dll").unlink()
        plan = self.plan()
        self.assertFalse(plan["ready"])
        self.assertIn(
            "install the framework first", " ".join(plan["blockers"])
        )

    def test_running_selected_host_refuses_plan(self):
        plan = self.plan(processes=[{
            "pid": 77,
            "path": str(self.hosts[1]["exe"]),
            "creation_time": datetime.now(timezone.utc).isoformat(),
        }])
        self.assertFalse(plan["ready"])
        self.assertIn("is running (PID 77)", " ".join(plan["blockers"]))

    def test_symbol_conflict_refuses_all_writes(self):
        write(
            self.hosts[1]["root"]
            / f"sdgo/symbols/{runtime.HOST_KEY}.ini",
            """\
[host]
image=BattleServer.exe
timestamp=0x548A891D
size_of_image=0x0046C000
[battle.rewards.net_send]
kind=code
rva=0xDEADBEEF
""",
        )
        plan = self.plan()
        self.assertFalse(plan["ready"])
        self.assertIn("conflicts", " ".join(plan["blockers"]))
        self.assertFalse(plan["actions"])


class OperatorEffectsTests(unittest.TestCase):
    def test_compiles_every_profile_and_only_enabled_managed_custom_effects(self):
        value = catalog()
        raw = runtime.compile_effects_ini(value)
        text = raw.decode("ascii")
        self.assertIn("Version=1", text)
        self.assertIn("Revision=" + model.revision(value), text)
        self.assertEqual(text.count("[operator:"), 3)
        expected = json.dumps(
            {"bonus": {"scale": 2}},
            sort_keys=True,
            separators=(",", ":"),
        ).encode().hex().upper()
        sections = storage.ini_values(raw)
        self.assertEqual(
            sections["operator:60001"]["PilotExpPercent"], "25"
        )
        self.assertEqual(
            sections["operator:60001"]["ExtensionsHex"], expected
        )
        for identity in (60002, 60003):
            self.assertEqual(
                sections[f"operator:{identity}"]["PilotExpPercent"], "0"
            )
            self.assertEqual(
                sections[f"operator:{identity}"]["ExtensionsHex"], "7B7D"
            )


class OperatorRuntimeInstallTests(OperatorRuntimeFixture):
    def install_existing_settings(self):
        host = self.hosts[1]["root"]
        write(
            host / "sdgo/mods/operator_catalog_server/mod.ini",
            """\
[mod]
id=operator_catalog_server
enabled=0
version=old
[settings]
AuthorChoice=keep-me
""",
        )
        write(
            host / "sdgo/mods/battle_rewards/mod.ini",
            """\
[mod]
id=battle_rewards
enabled=1
version=old
[settings]
RuntimeDirectory=C:\\owner\\runtime
InstanceId=owner-instance
GpPolicy=augment
Trace=1
""",
        )
        write(
            host / "sdgo/mods/battle_rewards/config.ini",
            "[settings]\nPrivateOwnerValue=preserve\n",
        )
        write(
            host / "sdgo/mods/unrelated/private.ini",
            "[settings]\nKeep=yes\n",
        )
        write(
            host / f"sdgo/symbols/{runtime.HOST_KEY}.ini",
            """\
; owner comment
[host]
image=BattleServer.exe
timestamp=0x548A891D
size_of_image=0x0046C000

[unrelated.owner.symbol]
kind=data
rva=0x1234
""",
        )

    def test_install_preserves_settings_unrelated_mods_and_originals(self):
        self.install_existing_settings()
        host = self.hosts[1]["root"]
        executable_hash = sha256(self.hosts[1]["exe"])
        config = (
            host / "sdgo/mods/battle_rewards/config.ini"
        ).read_bytes()
        unrelated = (
            host / "sdgo/mods/unrelated/private.ini"
        ).read_bytes()
        plan = self.plan()
        self.assertTrue(plan["ready"], plan["blockers"])
        result = runtime.install_runtime(
            plan, process_snapshot_fn=lambda: []
        )
        self.assertEqual(result["status"], "complete")
        self.assertTrue(result["changed"])
        self.assertTrue(Path(result["backup_directory"]).is_dir())
        operator = storage.ini_values(
            (host / "sdgo/mods/operator_catalog_server/mod.ini").read_bytes()
        )
        rewards = storage.ini_values(
            (host / "sdgo/mods/battle_rewards/mod.ini").read_bytes()
        )
        self.assertEqual(operator["mod"]["enabled"], "0")
        self.assertEqual(operator["settings"]["authorchoice"], "keep-me")
        self.assertEqual(rewards["mod"]["enabled"], "1")
        self.assertEqual(
            rewards["settings"]["runtimedirectory"],
            r"C:\owner\runtime",
        )
        self.assertEqual(rewards["settings"]["gppolicy"], "augment")
        self.assertEqual(
            (host / "sdgo/mods/battle_rewards/config.ini").read_bytes(),
            config,
        )
        self.assertEqual(
            (host / "sdgo/mods/unrelated/private.ini").read_bytes(),
            unrelated,
        )
        self.assertEqual(sha256(self.hosts[1]["exe"]), executable_hash)
        symbols = (
            host / f"sdgo/symbols/{runtime.HOST_KEY}.ini"
        ).read_text(encoding="utf-8")
        self.assertIn("[unrelated.owner.symbol]", symbols)
        self.assertIn("[battle.rewards.final.mission]", symbols)
        self.assertIn("[battle.rewards.net_send]", symbols)
        effects = (
            host / "sdgo/mods/operator_catalog_server/effects.ini"
        ).read_bytes()
        self.assertEqual(effects, runtime.compile_effects_ini(catalog()))
        self.assertFalse(result["original_executables_modified"])
        self.assertFalse(result["database_modified"])
        self.assertFalse(result["processes_controlled"])

    def test_manifest_representation_changes_are_semantically_installed(self):
        plan = self.plan()
        runtime.install_runtime(plan, process_snapshot_fn=lambda: [])
        host = self.hosts[1]["root"]
        manifests = [
            host / "sdgo/mods/operator_catalog_server/mod.ini",
            host / "sdgo/mods/battle_rewards/mod.ini",
        ]
        before = {
            path: storage.ini_values(path.read_bytes())
            for path in manifests
        }
        for path in manifests:
            rewrite_manifest_representation(path)
            self.assertEqual(storage.ini_values(path.read_bytes()), before[path])

        readback_plan = self.plan()
        self.assertFalse([
            action for action in readback_plan["actions"]
            if action["path"].endswith("/mod.ini")
        ])
        status = runtime.inspect_runtime_plan(readback_plan)
        self.assertTrue(status["files_installed"], status["reasons"])

    def test_manifest_critical_values_remain_exact_after_plan(self):
        plan = self.plan()
        runtime.install_runtime(plan, process_snapshot_fn=lambda: [])
        installed = self.plan()
        path = (
            self.hosts[1]["root"]
            / "sdgo/mods/battle_rewards/mod.ini"
        )
        original = path.read_bytes()
        text, _, _ = storage.decode_ini(original)
        changes = {
            "version": re.sub(
                r"(?m)^version=.*$", "version=9.9.9", text
            ),
            "hosts": re.sub(
                r"(?m)^hosts=.*$",
                "hosts=OtherHost-00000000-00000000",
                text,
            ),
            "requires": re.sub(
                r"(?m)^requires=.*$", "requires=untrusted.symbol", text
            ),
        }
        for field, changed in changes.items():
            with self.subTest(field=field):
                path.write_text(changed, encoding="utf-8", newline="\n")
                status = runtime.inspect_runtime_plan(installed)
                self.assertFalse(status["files_installed"])
                self.assertIn(
                    "manifest values do not match",
                    " ".join(status["reasons"]),
                )
                path.write_bytes(original)

    def test_running_host_after_plan_refuses_without_writes(self):
        plan = self.plan()
        before = {
            action["path"]: (
                self.hosts[1]["root"] / action["path"]
            ).read_bytes()
            if (self.hosts[1]["root"] / action["path"]).is_file()
            else None
            for action in plan["actions"]
        }
        with self.assertRaisesRegex(ValueError, "is running"):
            runtime.install_runtime(
                plan,
                process_snapshot_fn=lambda: [{
                    "pid": 88,
                    "path": str(self.hosts[1]["exe"]),
                    "creation_time": datetime.now(
                        timezone.utc
                    ).isoformat(),
                }],
            )
        for relative, raw in before.items():
            path = self.hosts[1]["root"] / relative
            self.assertEqual(path.read_bytes() if path.is_file() else None, raw)

    def test_partial_replace_failure_rolls_back_all_task_changes(self):
        self.install_existing_settings()
        host = self.hosts[1]["root"]
        plan = self.plan()
        before = {
            action["path"]: (
                host / action["path"]
            ).read_bytes()
            if (host / action["path"]).is_file()
            else None
            for action in plan["actions"]
        }
        calls = 0

        def fail_third(source, destination):
            nonlocal calls
            calls += 1
            if calls == 3:
                raise OSError("synthetic replacement failure")
            os.replace(source, destination)

        with self.assertRaisesRegex(
            RuntimeError, "all task changes were rolled back"
        ):
            runtime.install_runtime(
                plan,
                process_snapshot_fn=lambda: [],
                replace_fn=fail_third,
            )
        for relative, raw in before.items():
            path = host / relative
            self.assertEqual(path.read_bytes() if path.is_file() else None, raw)
        self.assertFalse(
            (self.server / ".sdgo-manager-backups").exists()
        )


class InstalledRuntimeEvidenceTests(OperatorRuntimeFixture):
    def install_and_evidence(self):
        plan = self.plan()
        runtime.install_runtime(plan, process_snapshot_fn=lambda: [])
        installed = self.plan()
        self.assertTrue(installed["ready"], installed["blockers"])
        self.assertFalse(installed["actions"])
        return installed, runtime.installed_runtime_evidence(installed)

    def fingerprints(self):
        return dict(runtime.NATIVE_PROCEDURE_FINGERPRINTS)

    def test_exact_install_and_live_procedures_grant_only_three_capabilities(self):
        with self.supported():
            plan, evidence = self.install_and_evidence()
            contract = runtime.verify_installed_runtime(
                evidence, self.fingerprints()
            )
        class RecordingCursor:
            def __init__(self):
                self.calls = []

            def execute(self, statement, *parameters):
                self.calls.append((statement, parameters))
                return self

        cursor = RecordingCursor()
        server._register_native_capabilities(cursor, contract)
        self.assertEqual(
            runtime.trusted_capabilities(contract),
            runtime.TRUSTED_NATIVE_CAPABILITIES,
        )
        self.assertEqual(
            {parameters[1] for _, parameters in cursor.calls},
            runtime.TRUSTED_NATIVE_CAPABILITIES,
        )
        self.assertTrue(all(
            parameters[0] == contract.evidence
            for _, parameters in cursor.calls
        ))
        self.assertEqual(contract.revision, plan["catalog_revision"])
        self.assertRegex(
            contract.evidence, r"\ASDGO_RUNTIME_V1:[0-9a-f]{64}\Z"
        )
        self.assertLessEqual(len(contract.evidence), 256)

    def test_semantic_manifest_readback_is_pinned_to_observed_bytes(self):
        with self.supported():
            plan = self.plan()
            runtime.install_runtime(plan, process_snapshot_fn=lambda: [])
            host = self.hosts[1]["root"]
            manifests = [
                host / "sdgo/mods/operator_catalog_server/mod.ini",
                host / "sdgo/mods/battle_rewards/mod.ini",
            ]
            for path in manifests:
                rewrite_manifest_representation(path)
            installed = self.plan()
            evidence = runtime.installed_runtime_evidence(installed)
            contract = runtime.verify_installed_runtime(
                evidence, self.fingerprints()
            )
            self.assertEqual(contract.revision, installed["catalog_revision"])
            manifests[0].write_bytes(manifests[0].read_bytes() + b"; changed\n")
            with self.assertRaisesRegex(ValueError, "hash does not match"):
                runtime.verify_installed_runtime(
                    evidence, self.fingerprints()
                )

    def test_arbitrary_mapping_cannot_claim_verified_runtime(self):
        with self.assertRaisesRegex(ValueError, "verifier-owned"):
            runtime.verify_installed_runtime(
                {
                    "verified": True,
                    "capabilities": sorted(
                        runtime.TRUSTED_NATIVE_CAPABILITIES
                    ),
                },
                self.fingerprints(),
            )
        with self.assertRaisesRegex(ValueError, "trusted runtime evidence"):
            runtime.trusted_capabilities({
                "verified": True,
                "capabilities": ["custom_skill_ids"],
            })

    def test_valid_pe_with_wrong_hash_and_procedure_change_are_rejected(self):
        with self.supported():
            _, evidence = self.install_and_evidence()
            fake_pe(
                self.hosts[1]["root"]
                / "sdgo/mods/operator_catalog_server/operator_catalog_server.dll",
                stamp=0x548A891E,
            )
            with self.assertRaisesRegex(ValueError, "hash does not match"):
                runtime.verify_installed_runtime(
                    evidence, self.fingerprints()
                )
        with self.supported():
            _, evidence = self.install_and_evidence()
            fingerprints = self.fingerprints()
            fingerprints["sp_getoperatorskill"] = "0" * 64
            with self.assertRaisesRegex(
                ValueError, "unsupported native procedure fingerprint"
            ):
                runtime.verify_installed_runtime(evidence, fingerprints)


class OperatorRuntimeStatusTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.host = Path(self.temporary.name) / "BattleServer1"
        fake_pe(self.host / "BattleServer.exe")
        self.module = self.host / "sdgo/mods/operator_catalog_server"
        write(self.module / "operator_catalog_server.dll", b"dll")
        write(self.module / "mod.ini", "[mod]\nid=operator_catalog_server\n")
        write(self.module / "effects.ini", "[effects]\nVersion=1\n")
        self.revision = "a" * 64
        write(
            self.module / "status.json",
            json.dumps({
                "pid": 101,
                "version": 1,
                "revision": self.revision,
                "loaded_profiles": 3,
                "native_hook_mode": "shared",
            }),
        )

    def tearDown(self):
        self.temporary.cleanup()

    def test_live_exact_process_and_fresh_status_is_effective(self):
        status = self.module / "status.json"
        result = runtime.read_module_status(
            self.host, 1, self.revision,
            process_snapshot_fn=lambda: [{
                "pid": 101,
                "path": str(self.host / "BattleServer.exe"),
                "creation_time": status.stat().st_mtime - 1,
            }],
        )
        self.assertTrue(result["effective"])
        self.assertEqual(result["state"], "active")
        self.assertFalse(result["pending"])

    def test_pid_reuse_or_unknown_process_is_pending(self):
        status = self.module / "status.json"
        reused = runtime.read_module_status(
            self.host, 1, self.revision,
            process_snapshot_fn=lambda: [{
                "pid": 101,
                "path": str(self.host / "BattleServer.exe"),
                "creation_time": status.stat().st_mtime + 10,
            }],
        )
        self.assertFalse(reused["effective"])
        self.assertTrue(reused["pending"])
        self.assertIn("predates", " ".join(reused["reasons"]))
        unknown = runtime.read_module_status(
            self.host, 1, self.revision,
            process_snapshot_fn=lambda: [{
                "pid": 101,
                "path": None,
                "creation_time": None,
                "probe_error": True,
            }],
        )
        self.assertFalse(unknown["effective"])
        self.assertIn("unknown", " ".join(unknown["reasons"]))


class SupportedNativeContractTests(unittest.TestCase):
    def test_verifies_known_procedures_and_fingerprints_runtime(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            host = fake_pe(root / "BattleServer.exe")
            operator = fake_pe(root / "operator_catalog_server.dll")
            rewards = fake_pe(root / "battle_rewards.dll")
            operator_sha = sha256(operator)
            symbols = write(
                root / f"{runtime.HOST_KEY}.ini",
                """\
[host]
image=BattleServer.exe

[battle.rewards.final.mission]
kind=site
rva=0x9F26A
""",
            )
            files = {
                "operator": operator,
                "rewards": rewards,
                "symbols": symbols,
            }
            hashes = {
                label: sha256(path) for label, path in files.items()
            }
            package_symbols = {
                "host": {"image": "BattleServer.exe"},
                "battle.rewards.final.mission": {
                    "kind": "site", "rva": "0x9F26A",
                },
            }
            with mock.patch.dict(
                runtime.SUPPORTED_BATTLE_HOSTS,
                {runtime.HOST_KEY: sha256(host)},
                clear=True,
            ):
                result = runtime.supported_native_contract(
                    host,
                    {
                        "dbo.sp_GetOperatorSkill":
                            runtime.NATIVE_PROCEDURE_FINGERPRINTS[
                                "sp_getoperatorskill"
                            ],
                        "[dbo].[sp_GetOperatorAddOn]":
                            runtime.NATIVE_PROCEDURE_FINGERPRINTS[
                                "sp_getoperatoraddon"
                            ],
                    },
                    files,
                    expected_runtime_files=hashes,
                    required_symbols=("battle.rewards.final.mission",),
                    package_symbols=package_symbols,
                )
        self.assertTrue(result["supported"], result["reasons"])
        self.assertEqual(
            result["runtime_files"]["operator"]["sha256"],
            operator_sha,
        )
        self.assertFalse(result["database_flags_changed"])

    def test_missing_matching_symbol_section_is_not_supported(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            host = fake_pe(root / "BattleServer.exe")
            operator = fake_pe(root / "operator_catalog_server.dll")
            rewards = fake_pe(root / "battle_rewards.dll")
            symbols = write(
                root / f"{runtime.HOST_KEY}.ini",
                "[host]\nimage=BattleServer.exe\n",
            )
            files = {
                "operator": operator,
                "rewards": rewards,
                "symbols": symbols,
            }
            with mock.patch.dict(
                runtime.SUPPORTED_BATTLE_HOSTS,
                {runtime.HOST_KEY: sha256(host)},
                clear=True,
            ):
                result = runtime.supported_native_contract(
                    host,
                    runtime.NATIVE_PROCEDURE_FINGERPRINTS,
                    files,
                    expected_runtime_files={
                        label: sha256(path)
                        for label, path in files.items()
                    },
                    required_symbols=("battle.rewards.final.mission",),
                    package_symbols={
                        "host": {"image": "BattleServer.exe"},
                        "battle.rewards.final.mission": {
                            "kind": "site", "rva": "0x9F26A",
                        },
                    },
                )
        self.assertFalse(result["supported"])
        self.assertIn("complete matching symbol", " ".join(result["reasons"]))


class OperatorServerAndWorkspaceRuntimeTests(unittest.TestCase):
    def test_database_verified_rows_do_not_authorize_native_capabilities(self):
        configured = catalog()
        for operator in configured["operators"]:
            operator["abilities"]["pilot_exp_percent"] = 0

        class AbilityCursor:
            def execute(self, statement):
                if (
                    "FROM dbo.tbl_OperatorSkill" in statement
                    and "Registry" not in statement
                ):
                    return [
                        (28, 1, 10, 20, 30),
                        (29, 1, 11, 21, 31),
                        (30, 1, 12, 22, 32),
                    ]
                if (
                    "FROM dbo.tbl_OperatorTime" in statement
                    and "Registry" not in statement
                ):
                    return []
                if "sdgo_OperatorSkillRegistry" in statement:
                    return [
                        (28, 1, 10, 20, 30),
                        (29, 1, 11, 21, 31),
                        (30, 1, 12, 22, 32),
                    ]
                if "sdgo_OperatorTimeRegistry" in statement:
                    return []
                if "sdgo_OperatorNativeCapabilities" in statement:
                    return [
                        (name, 1)
                        for name in runtime.TRUSTED_NATIVE_CAPABILITIES
                    ]
                if "sdgo_OperatorAbilityExtensions" in statement:
                    return [("bonus", 1, 1)]
                raise AssertionError("unexpected SQL: " + statement)

        with self.assertRaisesRegex(
            ValueError, "缺少已验证的原生运行能力"
        ):
            server._ability_plan(AbilityCursor(), configured)

    def test_duplicate_case_ini_uses_last_value_without_rewriting(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "Server.ini"
            raw = (
                b"[DB Game]\nDBServerName=host\nDBName=game\n"
                b"id=first\npwd=first-secret\nID=last\nPWD=last-secret\n"
            )
            path.write_bytes(raw)
            parsed = server.read_ini(path)
            self.assertEqual(parsed["DB Game"]["ID"], "last")
            self.assertEqual(parsed["DB Game"]["PWD"], "last-secret")
            self.assertEqual(path.read_bytes(), raw)

    def test_reopen_uses_saved_catalog_without_reimporting_snapshot(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            saved = catalog()
            saved.update({
                "client_index_sha256": "fixture-index",
                "uploaded_assets": {},
                "native_server_data": {},
            })
            model.save(root / "operators.json", saved)
            fake_assets = SimpleNamespace(index_hash="fixture-index")
            with (
                mock.patch.object(
                    workspace.assets, "Assets", return_value=fake_assets
                ),
                mock.patch.object(
                    model, "import_catalog",
                    side_effect=AssertionError("snapshot reimported"),
                ),
            ):
                opened = workspace.Workspace(
                    root, root / "client", root / "snapshot.json"
                )
            self.assertEqual(opened.read(), saved)
            self.assertEqual(
                [row["id"] for row in opened.public()["gallery"]],
                saved["native_operator_ids"],
            )

    def test_status_keeps_files_database_restart_and_loaded_distinct(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            saved = catalog()
            saved.update({
                "client_index_sha256": "fixture-index",
                "uploaded_assets": {},
                "native_server_data": {},
            })
            model.save(root / "operators.json", saved)
            engine = workspace.Workspace.__new__(workspace.Workspace)
            engine.root = root
            engine.path = root / "operators.json"
            engine.assets = SimpleNamespace(index_hash="fixture-index")
            engine.server_root = root / "server"
            engine.package_root = root / "package"
            engine.server_ini = root / "Server.ini"
            pending = runtime.RuntimePlan(
                {
                    "selected_hosts": [{
                        "folder": "BattleServer1",
                        "module_status": {
                            "effective": False,
                            "reasons": ["host has not loaded this revision"],
                        },
                    }],
                    "blockers": [],
                },
                state={"complete": True},
            )
            with (
                mock.patch.object(
                    engine, "_runtime_plan", return_value=pending
                ),
                mock.patch.object(
                    runtime, "inspect_runtime_plan",
                    return_value={"files_installed": True, "reasons": []},
                ),
                mock.patch.object(
                    server, "catalog_status",
                    return_value={
                        "database_applied": True,
                        "applied_revision": model.revision(saved),
                        "reasons": [],
                    },
                ),
            ):
                status = engine.runtime_status()
            self.assertTrue(status["files_installed"])
            self.assertTrue(status["database_applied"])
            self.assertFalse(status["runtime_loaded"])
            self.assertTrue(status["restart_required"])
            self.assertFalse(status["running_effective"])


class CatalogApplyEvidenceTests(unittest.TestCase):
    def test_public_apply_rejects_untrusted_runtime_before_catalog_writes(self):
        connection = mock.MagicMock()
        cursor = connection.cursor.return_value
        cursor.execute.return_value = cursor
        cursor.fetchone.return_value = (0,)
        evidence = object()
        with (
            mock.patch("manager.operations.server_change", return_value=nullcontext(mock.Mock())),
            mock.patch.object(server, "connect", return_value=connection),
            mock.patch.object(server, "definition", return_value="native procedure"),
            mock.patch.object(runtime, "verify_installed_runtime", side_effect=ValueError(
                "runtime evidence rejected")) as verify,
        ):
            with self.assertRaisesRegex(ValueError, "runtime evidence rejected"):
                server.apply_catalog(catalog(), Path("fixture.ini"), runtime_evidence=evidence)
        self.assertIs(verify.call_args.args[0], evidence)
        statements = [str(call.args[0]) for call in cursor.execute.call_args_list]
        self.assertIn("IF XACT_STATE()<>0 ROLLBACK TRANSACTION", statements)
        self.assertFalse(any(sql.startswith("UPDATE dbo.sdgo_OperatorProfiles") for sql in statements))
        connection.commit.assert_not_called()
        connection.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
