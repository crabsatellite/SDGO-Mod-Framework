from pathlib import Path
import contextlib
import io
import json
import tempfile
import unittest

from tests import test_game_test as runner_fixture
from tools import game_test_operator_scenario as scope


game_test = runner_fixture.game_test


def operator_flow(**changes):
    value = {
        "operator_id": 60047,
        "product_id": 1039001,
        "template_id": 60003,
        "option_id": 9547,
        "item_id": "290001",
        "category_id": 1219047,
        "price": 500,
        "validity": 30,
        "payment": 1,
        "name": "测试副官",
        "label": "测试副官 30日",
        "nickname": "Operator_01",
    }
    value.update(changes)
    return value


def operator_plan(**changes):
    value = {
        "target_host": "127.0.0.1",
        "network_policy": "direct",
        "retry_count": 1,
        "expect_first_fail": False,
        "mode_sequence": "1",
    }
    value.update(changes)
    return value


class OperatorScenarioContractTests(unittest.TestCase):
    def test_binds_exact_owner_catalog_and_purchase_defaults_off(self):
        binding = scope.bind(operator_flow(), operator_plan())
        self.assertIs(binding["operator_flow"]["purchase"], False)
        shop = scope.shop_bytes(binding["operator_flow"]).decode("utf-8")
        display = scope.display_bytes(binding["operator_flow"]).decode("utf-8")
        self.assertIn("Scope=OwnerCatalog", shop)
        self.assertIn("ProductId=1039001", shop)
        self.assertIn("ItemId=290001", shop)
        self.assertIn("TemplateId=60003", shop)
        self.assertIn("Name=测试副官", shop)
        self.assertIn("[option:9547]", display)
        self.assertNotIn("[option:290001]", display)
        self.assertIn("Text=测试副官 30日", display)
        self.assertEqual(scope.run_fields(binding)["operator_flow_purchase"], "0")
        self.assertEqual(scope.run_fields(binding)["operator_flow_item_id"], "290001")

    def test_second_sku_keeps_owner_item_and_native_option_independent(self):
        value = operator_flow(
            operator_id=60048,
            product_id=1039002,
            template_id=60004,
            option_id=9548,
            item_id="290002",
            category_id=1219048,
            name="第二副官",
            label="第二副官 30日",
        )
        binding = scope.bind(value, operator_plan())
        shop = scope.shop_bytes(binding["operator_flow"]).decode("utf-8")
        display = scope.display_bytes(binding["operator_flow"]).decode("utf-8")
        self.assertIn("ItemId=290002", shop)
        self.assertNotIn("ItemId=9548", shop)
        self.assertIn("[option:9548]", display)
        self.assertNotIn("[option:290002]", display)

    def test_purchase_requires_a_real_json_boolean(self):
        self.assertIs(scope.bind(operator_flow(purchase=True), operator_plan())["operator_flow"]["purchase"], True)
        for value in (1, "true", None):
            with self.subTest(value=value), self.assertRaises(ValueError):
                scope.bind(operator_flow(purchase=value), operator_plan())

    def test_identity_text_and_numeric_contract_rejects_ambiguous_values(self):
        changes = (
            {"operator_id": 60046},
            {"operator_id": 61000},
            {"operator_id": True},
            {"category_id": 1219048},
            {"product_id": 60003},
            {"option_id": 0},
            {"option_id": 2**31},
            {"item_id": 290001},
            {"item_id": "29001"},
            {"item_id": "29000x"},
            {"price": 0},
            {"validity": 1_000_001},
            {"payment": 0},
            {"nickname": "含中文"},
            {"nickname": "x" * 17},
            {"name": ""},
            {"name": "😀" * 26},
            {"label": "x\nhidden"},
            {"unknown": 1},
        )
        for change in changes:
            with self.subTest(change=change), self.assertRaises(ValueError):
                scope.bind(operator_flow(**change), operator_plan())

    def test_rejects_nonloopback_retries_and_conflicting_scopes(self):
        plans = (
            operator_plan(target_host="10.0.0.1"),
            operator_plan(network_policy="preserve"),
            operator_plan(retry_count=2),
            operator_plan(expect_first_fail=True),
            operator_plan(bootstrap_only=True),
            operator_plan(mission_mode=5),
            operator_plan(friends_only=True),
            operator_plan(collection_draw=True, pages_only=True),
            operator_plan(shop_revision="stock"),
            operator_plan(unit_scenario={"schema": 1}),
        )
        for plan in plans:
            with self.subTest(plan=plan), self.assertRaises(ValueError):
                scope.bind(operator_flow(), plan)

    def test_cli_loader_rejects_duplicate_nonobject_and_oversized_json(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            valid = root / "valid.json"
            valid.write_text(json.dumps(operator_flow(), ensure_ascii=False), encoding="utf-8")
            self.assertEqual(scope.load(valid), operator_flow())
            for name, content in (
                ("duplicate.json", '{"operator_id":60047,"operator_id":60048}'),
                ("list.json", "[]"),
                ("syntax.json", "{"),
            ):
                path = root / name
                path.write_text(content, encoding="utf-8")
                with self.subTest(name=name), self.assertRaises(ValueError):
                    scope.load(path)
            large = root / "large.json"
            large.write_bytes(b"x" * (64 * 1024 + 1))
            with self.assertRaises(ValueError):
                scope.load(large)


class SealedOperatorScenarioTests(unittest.TestCase):
    setUp = runner_fixture.GameTestRunnerTests.setUp
    tearDown = runner_fixture.GameTestRunnerTests.tearDown
    make_normal_client = runner_fixture.GameTestRunnerTests.make_normal_client
    make_stage = runner_fixture.GameTestRunnerTests.make_stage
    prepare = runner_fixture.GameTestRunnerTests.prepare

    def register_account(self):
        text = self.account_profile.read_text(encoding="utf-8")
        self.account_profile.write_text(
            text.replace("state=auth_accepted", "state=registered"),
            encoding="utf-8",
        )

    def test_absent_flow_preserves_existing_cases_keys_and_inputs(self):
        run = self.prepare("ordinary-without-operator")
        metadata = game_test.read_json(run / "run.json")
        job = game_test.read_ini(run / "client/sdgo/gametest/run.ini")["run"]
        self.assertEqual(tuple(job["required_cases"].split(",")), game_test.FLOW_CASES)
        self.assertFalse(any(field.startswith("operator_") for field in job))
        self.assertFalse(any(field.startswith("operator_") for field in metadata["plan"]))
        self.assertFalse((run / "client" / scope.SHOP_RELATIVE).exists())
        self.assertFalse((run / "client" / scope.DISPLAY_RELATIVE).exists())

    def test_pages_flow_is_sealed_after_onboarding_before_active_unit(self):
        self.register_account()
        run = self.prepare(
            "operator-pages",
            pages_only=True,
            play_unit_uid=17,
            operator_flow=operator_flow(purchase=True),
        )
        metadata = game_test.validate_metadata(run)
        plan = metadata["plan"]
        public = game_test.read_public_plan(run / "client")
        self.assertEqual(plan, public)
        cases = game_test.required_cases_for_plan(plan)
        self.assertEqual(
            cases[cases.index("flow.onboarding") + 1:cases.index("setup.active_unit")],
            (scope.CASE,),
        )
        job = game_test.read_ini(run / "client/sdgo/gametest/run.ini")["run"]
        self.assertEqual(job["operator_flow_purchase"], "1")
        self.assertEqual(job["operator_flow_name"], "测试副官")
        self.assertEqual(job["operator_shop_path"], scope.SHOP_INPUT)
        self.assertEqual(job["operator_shop_display_path"], scope.DISPLAY_INPUT)
        hashes = metadata["client"]["immutable_hashes"]
        self.assertEqual(hashes[scope.SHOP_RELATIVE.as_posix()], plan["operator_shop_sha256"])
        self.assertEqual(
            hashes[scope.DISPLAY_RELATIVE.as_posix()],
            plan["operator_shop_display_sha256"],
        )
        game_test.verify_hashes(metadata)

    def test_second_sku_survives_prepare_public_readback_and_immutable_inputs(self):
        self.register_account()
        second = operator_flow(
            operator_id=60048,
            product_id=1039002,
            template_id=60004,
            option_id=9548,
            item_id="290002",
            category_id=1219048,
            name="第二副官",
            label="第二副官 30日",
        )
        run = self.prepare(
            "operator-second-sku",
            pages_only=True,
            operator_flow=second,
        )
        metadata = game_test.validate_metadata(run)
        public = game_test.read_public_plan(run / "client")
        self.assertEqual(metadata["plan"], public)
        self.assertEqual(public["operator_flow"]["item_id"], "290002")
        self.assertEqual(public["operator_flow"]["option_id"], 9548)
        job = game_test.read_ini(run / "client/sdgo/gametest/run.ini")["run"]
        self.assertEqual(job["operator_flow_item_id"], "290002")
        self.assertEqual(job["operator_flow_option_id"], "9548")
        shop = (run / "client" / scope.SHOP_RELATIVE).read_text(encoding="utf-8")
        display = (run / "client" / scope.DISPLAY_RELATIVE).read_text(encoding="utf-8")
        self.assertIn("ItemId=290002", shop)
        self.assertNotIn("ItemId=9548", shop)
        self.assertIn("[option:9548]", display)
        self.assertNotIn("[option:290002]", display)
        game_test.verify_hashes(metadata)

    def test_ordinary_pvp_flow_keeps_all_existing_case_order(self):
        self.register_account()
        run = self.prepare("operator-pvp", operator_flow=operator_flow())
        plan = game_test.validate_metadata(run)["plan"]
        self.assertEqual(
            game_test.required_cases_for_plan(plan),
            game_test.FLOW_CASES[:3] + (scope.CASE,) + game_test.FLOW_CASES[3:],
        )
        self.assertIs(plan["operator_flow"]["purchase"], False)

    def test_requires_reused_registered_account_before_copy(self):
        with self.assertRaises(game_test.GameTestError):
            self.prepare("operator-unregistered", operator_flow=operator_flow())
        self.assertFalse((self.root / "operator-unregistered").exists())
        with self.assertRaises(game_test.GameTestError):
            self.prepare(
                "operator-create",
                create_test_account=True,
                account_profile=self.root / "new-account.ini",
                operator_flow=operator_flow(),
            )
        self.assertFalse((self.root / "operator-create").exists())
        self.assertFalse((self.root / "new-account.ini").exists())

    def test_scope_conflicts_fail_before_capsule_creation(self):
        self.register_account()
        conflicts = (
            {"bootstrap_only": True},
            {"mission_mode": 5},
            {"friends_only": True},
            {"collection_draw": True, "pages_only": True},
            {"random_loadout": True},
            {"network_policy": "preserve"},
            {"target_host": "192.0.2.10"},
            {"unit_scenario": {"schema": 1}, "pages_only": True, "play_unit_uid": 17},
        )
        for index, options in enumerate(conflicts):
            target = f"operator-conflict-{index}"
            with self.subTest(options=options), self.assertRaises(game_test.GameTestError):
                self.prepare(target, operator_flow=operator_flow(), **options)
            self.assertFalse((self.root / target).exists())

    def test_run_metadata_and_generated_input_tampering_fail_closed(self):
        self.register_account()
        for field in ("run", "item", "metadata", "shop", "display"):
            run = self.prepare("operator-tamper-" + field, operator_flow=operator_flow())
            if field == "run":
                path = run / "client/sdgo/gametest/run.ini"
                config = game_test.read_ini(path)
                config["run"]["operator_flow_price"] = "501"
                game_test.write_ini(path, config)
            elif field == "item":
                path = run / "client/sdgo/gametest/run.ini"
                config = game_test.read_ini(path)
                config["run"]["operator_flow_item_id"] = "290002"
                game_test.write_ini(path, config)
            elif field == "metadata":
                path = run / "run.json"
                metadata = json.loads(path.read_text(encoding="utf-8"))
                metadata["plan"]["operator_flow"]["purchase"] = True
                path.write_text(json.dumps(metadata), encoding="utf-8")
            elif field == "shop":
                path = run / "client" / scope.SHOP_RELATIVE
                path.write_bytes(path.read_bytes().replace(b"Price=500", b"Price=501"))
            else:
                path = run / "client" / scope.DISPLAY_RELATIVE
                path.write_bytes(path.read_bytes() + b"\n")
            with self.subTest(field=field), self.assertRaises(game_test.GameTestError):
                game_test.validate_metadata(run)

    def test_partial_unknown_and_forged_operator_keys_are_rejected(self):
        self.register_account()
        for change in ("remove", "unknown"):
            run = self.prepare("operator-keys-" + change, operator_flow=operator_flow())
            path = run / "client/sdgo/gametest/run.ini"
            config = game_test.read_ini(path)
            if change == "remove":
                del config["run"]["operator_flow_nickname"]
            else:
                config["run"]["operator_flow_secret"] = "hidden"
            game_test.write_ini(path, config)
            with self.subTest(change=change), self.assertRaises(game_test.GameTestError):
                game_test.read_public_plan(run / "client")

    def test_cli_parser_exposes_operator_json_path(self):
        parsed = game_test.parse_args(
            [
                "prepare",
                "--client-template",
                str(self.template),
                "--stage",
                str(self.stage),
                "--run-dir",
                str(self.root / "cli"),
                "--operator-flow",
                str(self.root / "operator.json"),
            ]
        )
        self.assertEqual(parsed.operator_flow, self.root / "operator.json")

    def test_cli_loads_and_forwards_the_sealed_operator_json(self):
        self.register_account()
        source = self.root / "operator.json"
        source.write_text(
            json.dumps(operator_flow(purchase=True), ensure_ascii=False),
            encoding="utf-8",
        )
        target = self.root / "operator-cli"
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            result = game_test.main(
                [
                    "prepare",
                    "--client-template",
                    str(self.template),
                    "--stage",
                    str(self.stage),
                    "--run-dir",
                    str(target),
                    "--account-profile",
                    str(self.account_profile),
                    "--operator-flow",
                    str(source),
                ]
            )
        self.assertEqual(result, 0, stderr.getvalue())
        plan = game_test.validate_metadata(target)["plan"]
        self.assertIs(plan["operator_flow"]["purchase"], True)


if __name__ == "__main__":
    unittest.main()
