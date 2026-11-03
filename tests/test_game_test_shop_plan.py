from pathlib import Path
import json
import os
import tempfile
import unittest

from tests import test_game_test as runner_tests


game_test = runner_tests.game_test
FIXTURE_REVISION = "test-stock-native-v1"
SHOP_ASSERTIONS_FIXTURE = f"""[shop]
Schema=1
Scope=StockAssertions
CatalogId=test-stock
Revision={FIXTURE_REVISION}

[product:fixture-1001]
ProductId=1001
ItemId=123456
TemplateId=1001
Category=1
Price=250
Currency=GB
Validity=30
Enabled=1
Name=Fixture Product
Label=Fixture Product Label
"""
SHOP_DISPLAY_FIXTURE = f"""[shop]
Revision={FIXTURE_REVISION}

[option:1]
Category=1
Validity=30
Payment=1
Price=250
Text=Fixture Option
"""


class ShopPlanTests(unittest.TestCase):
    def setUp(self):
        self.fixture = runner_tests.GameTestRunnerTests(methodName="runTest")
        self.fixture.setUp()
        self.addCleanup(self._cleanup_fixture)
        self.stock_assertions = self.fixture.root / "synthetic-shop-assertions.ini"
        self.stock_display = self.fixture.root / "synthetic-shop-display.ini"
        self.stock_assertions.write_text(SHOP_ASSERTIONS_FIXTURE, encoding="utf-8")
        self.stock_display.write_text(SHOP_DISPLAY_FIXTURE, encoding="utf-8")

    def _cleanup_fixture(self):
        try:
            self.fixture.tearDown()
        finally:
            self.fixture.doCleanups()

    def prepare(self, name, **kwargs):
        return self.fixture.prepare(name, **kwargs)

    def shop_prepare(self, name):
        return self.prepare(
            name,
            pages_only=True,
            shop_assertions=self.stock_assertions,
            shop_display=self.stock_display,
        )

    def test_visual_stock_scope_binds_expected_rendered_names_and_excludes_other_pages(self):
        self.stock_assertions.write_text(
            SHOP_ASSERTIONS_FIXTURE.replace('Scope=StockAssertions', 'Scope=StockVisualAssertions')
            + 'DisplayName=中文商品\n', encoding='utf-8')
        run_dir = self.shop_prepare('visual-stock')
        plan = game_test.read_json(run_dir/'run.json')['plan']
        self.assertTrue(plan['shop_visual_only'])
        required = game_test.required_cases_for_plan(plan)
        self.assertEqual(required, game_test.FLOW_CASES[:3] + ('pages.shop.catalog',))
        self.assertNotIn('pages.storage.claim', required)
        self.assertTrue(game_test.read_public_plan(run_dir/'client')['shop_visual_only'])

    def test_visual_stock_scope_requires_independent_rendered_name(self):
        self.stock_assertions.write_text(
            SHOP_ASSERTIONS_FIXTURE.replace('Scope=StockAssertions', 'Scope=StockVisualAssertions'), encoding='utf-8')
        with self.assertRaises(game_test.GameTestError):
            game_test.load_shop_test_inputs(self.stock_assertions, self.stock_display)

    def test_default_pages_plan_has_no_shop_fields_or_case(self):
        run_dir = self.prepare("pages-without-shop", pages_only=True)
        metadata = game_test.read_json(run_dir / "run.json")
        job = game_test.read_ini(run_dir / "client/sdgo/gametest/run.ini")["run"]
        self.assertFalse(any(field in metadata["plan"] for field in game_test.SHOP_PLAN_FIELDS))
        self.assertFalse(any(field in job for field in game_test.SHOP_PLAN_FIELDS))
        self.assertNotIn("pages.shop.catalog", job["required_cases"].split(","))

    def test_synthetic_stock_inputs_are_copied_read_only_and_bound_to_native_pages(self):
        assertions_before = self.stock_assertions.read_bytes()
        display_before = self.stock_display.read_bytes()
        run_dir = self.shop_prepare("stock-shop")
        metadata = game_test.read_json(run_dir / "run.json")
        plan = metadata["plan"]
        client = run_dir / "client"
        assertions_copy = client / game_test.SHOP_ASSERTIONS_RELATIVE
        display_copy = client / game_test.SHOP_DISPLAY_RELATIVE
        job = game_test.read_ini(client / "sdgo/gametest/run.ini")["run"]

        self.assertEqual(self.stock_assertions.read_bytes(), assertions_before)
        self.assertEqual(self.stock_display.read_bytes(), display_before)
        self.assertEqual(assertions_copy.read_bytes(), assertions_before)
        self.assertEqual(display_copy.read_bytes(), display_before)
        self.assertEqual(metadata["suite"], "native-pages")
        self.assertEqual(job["suite"], "pvp-ai")
        self.assertEqual(plan["shop_assertions_path"], game_test.SHOP_ASSERTIONS_JOB_PATH)
        self.assertEqual(plan["shop_display_path"], game_test.SHOP_DISPLAY_JOB_PATH)
        self.assertEqual(plan["shop_assertions_sha256"], game_test.sha256(self.stock_assertions))
        self.assertEqual(plan["shop_display_sha256"], game_test.sha256(self.stock_display))
        self.assertEqual(
            plan["shop_revision"],
            FIXTURE_REVISION,
        )
        for field in game_test.SHOP_PLAN_FIELDS:
            self.assertEqual(job[field], str(plan[field]))

        required = job["required_cases"].split(",")
        self.assertEqual(
            required,
            list(
                game_test.required_flow_cases(
                    "1", pages_only=True, shop_catalog=True
                )
            ),
        )
        self.assertEqual(
            required.index("pages.shop.catalog"),
            required.index("catalog.ai") + 1,
        )
        self.assertEqual(
            required.index("pages.storage.read"),
            required.index("pages.shop.catalog") + 1,
        )
        hashes = metadata["client"]["immutable_hashes"]
        self.assertEqual(
            hashes[game_test.SHOP_ASSERTIONS_RELATIVE.as_posix()],
            plan["shop_assertions_sha256"],
        )
        self.assertEqual(
            hashes[game_test.SHOP_DISPLAY_RELATIVE.as_posix()],
            plan["shop_display_sha256"],
        )
        self.assertEqual(game_test.validate_metadata(run_dir)["plan"], plan)
        game_test.verify_hashes(metadata)

    def test_shop_inputs_are_an_atomic_pages_only_pair(self):
        for name, options in (
            (
                "assertions-only",
                {"pages_only": True, "shop_assertions": self.stock_assertions},
            ),
            (
                "display-only",
                {"pages_only": True, "shop_display": self.stock_display},
            ),
            (
                "not-pages",
                {
                    "shop_assertions": self.stock_assertions,
                    "shop_display": self.stock_display,
                },
            ),
        ):
            with self.subTest(name=name), self.assertRaises(game_test.GameTestError):
                self.prepare(name, **options)
            self.assertFalse((self.fixture.root / name).exists())

    def test_schema_rejects_owner_catalog_mismatch_empty_products_and_unknown_keys(self):
        assertions = self.stock_assertions.read_text(encoding="utf-8")
        display = self.stock_display.read_text(encoding="utf-8")
        variants = {
            "owner-sku": (
                assertions.replace("Scope=StockAssertions", "Scope=OwnerCatalog"),
                display,
            ),
            "revision-mismatch": (
                assertions,
                display.replace(
                    FIXTURE_REVISION,
                    "stock-native-different",
                ),
            ),
            "empty-products": (
                assertions[: assertions.index("[product:")],
                display,
            ),
            "unknown-secret": (
                assertions.replace(
                    f"Revision={FIXTURE_REVISION}",
                    f"Revision={FIXTURE_REVISION}\nPassword=forbidden",
                ),
                display,
            ),
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, (assertion_text, display_text) in variants.items():
                with self.subTest(name=name):
                    assertion_path = root / f"{name}-assertions.ini"
                    display_path = root / f"{name}-display.ini"
                    assertion_path.write_text(assertion_text, encoding="utf-8")
                    display_path.write_text(display_text, encoding="utf-8")
                    with self.assertRaises(game_test.GameTestError):
                        game_test.load_shop_test_inputs(assertion_path, display_path)

    def test_path_content_and_optional_field_tampering_all_fail_closed(self):
        run_dir = self.shop_prepare("missing-public-field")
        job_path = run_dir / "client/sdgo/gametest/run.ini"
        job = game_test.read_ini(job_path)
        del job["run"]["shop_display_sha256"]
        game_test.write_ini(job_path, job)
        with self.assertRaises(game_test.GameTestError):
            game_test.validate_metadata(run_dir)

        run_dir = self.shop_prepare("changed-content")
        copied = run_dir / "client" / game_test.SHOP_DISPLAY_RELATIVE
        copied.write_bytes(copied.read_bytes() + b"\n; changed\n")
        with self.assertRaisesRegex(game_test.GameTestError, "sealed plan"):
            game_test.validate_metadata(run_dir)

        run_dir = self.shop_prepare("missing-json-field")
        metadata = game_test.read_json(run_dir / "run.json")
        del metadata["plan"]["shop_revision"]
        game_test.atomic_write(run_dir / "run.json", json.dumps(metadata))
        with self.assertRaisesRegex(game_test.GameTestError, "partial|add or remove"):
            game_test.validate_metadata(run_dir)

        run_dir = self.shop_prepare("changed-path")
        job_path = run_dir / "client/sdgo/gametest/run.ini"
        job = game_test.read_ini(job_path)
        job["run"]["shop_assertions_path"] = "../owner/catalog.ini"
        game_test.write_ini(job_path, job)
        with self.assertRaisesRegex(game_test.GameTestError, "fixed copied-client paths"):
            game_test.validate_metadata(run_dir)

        run_dir = self.shop_prepare("unknown-optional")
        job_path = run_dir / "client/sdgo/gametest/run.ini"
        job = game_test.read_ini(job_path)
        job["run"]["shop_password"] = "forbidden"
        game_test.write_ini(job_path, job)
        with self.assertRaisesRegex(game_test.GameTestError, "unknown shop"):
            game_test.validate_metadata(run_dir)

    def test_copied_input_link_fails_closed(self):
        run_dir = self.shop_prepare("linked-input")
        copied = run_dir / "client" / game_test.SHOP_ASSERTIONS_RELATIVE
        outside = self.fixture.root / "outside-shop-assertions.ini"
        outside.write_bytes(copied.read_bytes())
        copied.unlink()
        os.link(outside, copied)
        with self.assertRaisesRegex(game_test.GameTestError, "linked or escapes"):
            game_test.validate_metadata(run_dir)

    def test_immutable_map_tampering_fails_closed(self):
        for name, mutate in (
            (
                "missing-immutable-entry",
                lambda metadata: metadata["client"]["immutable_hashes"].pop(
                    game_test.SHOP_ASSERTIONS_RELATIVE.as_posix()
                ),
            ),
            (
                "wrong-immutable-entry",
                lambda metadata: metadata["client"]["immutable_hashes"].__setitem__(
                    game_test.SHOP_DISPLAY_RELATIVE.as_posix(),
                    "0" * 64,
                ),
            ),
        ):
            with self.subTest(name=name):
                run_dir = self.shop_prepare(name)
                metadata = game_test.read_json(run_dir / "run.json")
                mutate(metadata)
                with self.assertRaisesRegex(
                    game_test.GameTestError,
                    "immutable payload map does not bind sealed shop input",
                ):
                    game_test.verify_hashes(metadata)

    def test_cli_exposes_both_shop_inputs_without_enabling_them_by_default(self):
        base = [
            "prepare",
            "--client-template",
            str(self.fixture.template),
            "--stage",
            str(self.fixture.stage),
            "--run-dir",
            str(self.fixture.root / "cli"),
            "--pages-only",
        ]
        default = game_test.parse_args(base)
        self.assertIsNone(default.shop_assertions)
        self.assertIsNone(default.shop_display)
        configured = game_test.parse_args(
            base
            + [
                "--shop-assertions",
                str(self.stock_assertions),
                "--shop-display",
                str(self.stock_display),
            ]
        )
        self.assertEqual(configured.shop_assertions, self.stock_assertions)
        self.assertEqual(configured.shop_display, self.stock_display)


if __name__ == "__main__":
    unittest.main()
