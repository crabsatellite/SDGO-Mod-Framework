import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import uuid


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import game_test_fps


class FpsMatrixTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.template = self.root / "template"
        self.stage = self.root / "stage"
        self.work = self.root / "work"
        self.results = self.root / "results"
        self.template.mkdir()
        (self.template / "GOnline.exe").write_bytes(b"game")
        (self.stage / "client").mkdir(parents=True)

    def tearDown(self):
        self.temporary.cleanup()

    def fake_prepare(self, client_template, stage, run_dir, **kwargs):
        run_dir.mkdir()
        client = run_dir / "client"
        events = client / "sdgo" / "gametest" / "events.jsonl"
        events.parent.mkdir(parents=True)
        events.write_text('{"event":"run_end","status":"pass"}\n', encoding="utf-8")
        (client / "sdgo" / "logs").mkdir(parents=True)
        (client / "sdgo" / "logs" / "sdgo.log").write_text(
            "fps test ready\n", encoding="utf-8"
        )
        run_id = str(uuid.uuid4())
        metadata = {
            "run_id": run_id,
            "suite": game_test_fps.game_test.FPS_SUITE,
            "template": str(Path(client_template).resolve()),
            "plan": {
                "fps_cap": kwargs["fps_cap"],
                "fps_mod_enabled": kwargs["fps_mod_enabled"],
            },
            "client": {
                "root": str(client.resolve()),
                "events": str(events.resolve()),
                "game": str((client / "GOnline.exe").resolve()),
            },
            "execution": None,
        }
        (run_dir / "run.json").write_text(
            json.dumps(metadata), encoding="utf-8"
        )
        self.prepare_calls.append((run_dir.name, kwargs))
        return metadata

    def fake_run(self, run_dir, timeout):
        metadata = json.loads((run_dir / "run.json").read_text(encoding="utf-8"))
        metadata["execution"] = {"status": "completed", "game_pid": None}
        (run_dir / "run.json").write_text(json.dumps(metadata), encoding="utf-8")
        receipts = run_dir / "receipts"
        receipts.mkdir()
        report = {"status": "pass", "run_id": metadata["run_id"]}
        (receipts / "report.json").write_text(json.dumps(report), encoding="utf-8")
        (receipts / "junit.xml").write_text("<testsuite/>", encoding="utf-8")
        (receipts / "quiet-runtime.json").write_text(
            json.dumps({"status": "closed", "proof": {"verified": True}}),
            encoding="utf-8",
        )
        (receipts / "client.stdout.log").write_text("", encoding="utf-8")
        (receipts / "client.stderr.log").write_text("", encoding="utf-8")
        return report

    def run_successful_matrix(self):
        self.prepare_calls = []
        with (
            mock.patch.object(game_test_fps.game_test, "prepare", side_effect=self.fake_prepare),
            mock.patch.object(game_test_fps.game_test, "run_prepared", side_effect=self.fake_run),
            mock.patch.object(game_test_fps.game_test, "process_snapshot", return_value=[]),
        ):
            return game_test_fps.run_matrix(
                self.template, self.stage, self.work, self.results
            )

    def test_runs_required_matrix_and_retains_evidence_after_cleanup(self):
        summary = self.run_successful_matrix()
        self.assertEqual(summary["status"], "pass")
        self.assertEqual(
            [
                (
                    name,
                    values["fps_mod_enabled"],
                    values["fps_cap"],
                    values["quiet"],
                    values["network_policy"],
                )
                for name, values in self.prepare_calls
            ],
            [
                ("disabled160", False, 160, True, "direct"),
                ("enabled30", True, 30, True, "direct"),
                ("enabled60", True, 60, True, "direct"),
                ("enabled120", True, 120, True, "direct"),
                ("enabled160", True, 160, True, "direct"),
            ],
        )
        results_session = Path(summary["results"])
        self.assertTrue((results_session / "matrix-summary.json").is_file())
        for label, _, _ in game_test_fps.MATRIX:
            result = results_session / label
            self.assertTrue((result / "run.json").is_file())
            self.assertTrue((result / "receipts" / "report.json").is_file())
            self.assertTrue((result / "receipts" / "junit.xml").is_file())
            self.assertTrue((result / "receipts" / "quiet-runtime.json").is_file())
            self.assertTrue((result / "native" / "events.jsonl").is_file())
            self.assertTrue((result / "diagnostics" / "sdgo.log").is_file())
        self.assertFalse((self.work / Path(summary["results"]).name).exists())

    def test_refuses_overlapping_work_and_results_roots(self):
        with self.assertRaisesRegex(game_test_fps.FpsMatrixError, "must be disjoint"):
            game_test_fps.run_matrix(
                self.template,
                self.stage,
                self.work,
                self.work / "results",
            )

    def test_cleanup_requires_matching_owner_and_run_id(self):
        session = self.work / "fps-matrix-deadbeef"
        run_dir = session / "disabled160"
        client = run_dir / "client"
        client.mkdir(parents=True)
        run_id = str(uuid.uuid4())
        metadata = {
            "run_id": run_id,
            "client": {"root": str(client.resolve())},
        }
        (run_dir / "run.json").write_text(json.dumps(metadata), encoding="utf-8")
        marker = {
            "owner": game_test_fps.OWNER,
            "session_id": "deadbeef",
            "label": "disabled160",
            "run_id": run_id,
            "run_dir": str(run_dir.resolve()),
            "client_root": str(client.resolve()),
        }
        (run_dir / game_test_fps.OWNER_FILE).write_text(
            json.dumps({**marker, "run_id": str(uuid.uuid4())}), encoding="utf-8"
        )
        with self.assertRaisesRegex(game_test_fps.FpsMatrixError, "marker does not match"):
            game_test_fps.cleanup_owned_capsule(
                run_dir,
                session,
                self.results / "session",
                self.template.resolve(),
                marker,
            )
        self.assertTrue(run_dir.is_dir())

    def test_cleanup_failure_marks_matrix_failed_and_keeps_results(self):
        self.prepare_calls = []
        real_rmtree = game_test_fps.shutil.rmtree

        def fail_capsule_cleanup(path):
            if Path(path).name in {row[0] for row in game_test_fps.MATRIX}:
                raise OSError("simulated cleanup refusal")
            return real_rmtree(path)

        with (
            mock.patch.object(game_test_fps.game_test, "prepare", side_effect=self.fake_prepare),
            mock.patch.object(game_test_fps.game_test, "run_prepared", side_effect=self.fake_run),
            mock.patch.object(game_test_fps.game_test, "process_snapshot", return_value=[]),
            mock.patch.object(game_test_fps.shutil, "rmtree", side_effect=fail_capsule_cleanup),
        ):
            summary = game_test_fps.run_matrix(
                self.template, self.stage, self.work, self.results
            )
        self.assertEqual(summary["status"], "fail")
        self.assertTrue(Path(summary["results"], "matrix-summary.json").is_file())
        self.assertTrue(
            all(case["cleanup"]["status"] == "fail" for case in summary["cases"])
        )

    def test_finalized_partial_prepare_is_exported_and_cleaned(self):
        self.prepare_calls = []

        def fail_after_finalize(*args, **kwargs):
            self.fake_prepare(*args, **kwargs)
            raise game_test_fps.game_test.GameTestError("simulated finalization failure")

        with (
            mock.patch.object(
                game_test_fps.game_test, "prepare", side_effect=fail_after_finalize
            ),
            mock.patch.object(game_test_fps.game_test, "run_prepared") as run_prepared,
            mock.patch.object(game_test_fps.game_test, "process_snapshot", return_value=[]),
        ):
            summary = game_test_fps.run_matrix(
                self.template, self.stage, self.work, self.results
            )
        self.assertEqual(summary["status"], "fail")
        run_prepared.assert_not_called()
        self.assertEqual(
            [case["cleanup"]["status"] for case in summary["cases"]],
            ["removed"] * len(game_test_fps.MATRIX),
        )
        self.assertFalse((self.work / Path(summary["results"]).name).exists())
        for label, _, _ in game_test_fps.MATRIX:
            self.assertTrue(
                (Path(summary["results"]) / label / "native" / "events.jsonl").is_file()
            )

    def test_reparse_path_is_rejected(self):
        with mock.patch.object(
            game_test_fps, "_is_reparse", side_effect=lambda path: Path(path).resolve() == self.work.resolve()
        ):
            with self.assertRaisesRegex(game_test_fps.FpsMatrixError, "reparse point"):
                game_test_fps.validate_roots(
                    self.template, self.stage, self.work, self.results
                )

    def test_cleanup_rejects_ancestor_link_without_deleting_external_files(self):
        actual = self.root / "external-work"
        session = actual / "fps-matrix-linked"
        run_dir = session / "disabled160"
        client = run_dir / "client"
        client.mkdir(parents=True)
        sentinel = run_dir / "outside-sentinel.txt"
        sentinel.write_text("keep", encoding="utf-8")
        alias = self.root / "work-link"
        patch = None
        try:
            os.symlink(actual, alias, target_is_directory=True)
        except OSError:
            alias.mkdir()
            simulated_run = alias / session.name / run_dir.name
            simulated_run.mkdir(parents=True)
            linked_session = alias / session.name
            linked_run = simulated_run
            patch = mock.patch.object(
                game_test_fps,
                "_is_reparse",
                side_effect=lambda path: Path(path) == alias,
            )
            patch.start()
        else:
            linked_session = alias / session.name
            linked_run = linked_session / run_dir.name
        try:
            with self.assertRaisesRegex(game_test_fps.FpsMatrixError, "reparse point"):
                game_test_fps.cleanup_owned_capsule(
                    linked_run,
                    linked_session,
                    self.results / "session",
                    self.template,
                    {},
                )
            self.assertEqual(sentinel.read_text(encoding="utf-8"), "keep")
        finally:
            if patch is not None:
                patch.stop()
                game_test_fps.shutil.rmtree(alias)
            else:
                alias.unlink()


if __name__ == "__main__":
    unittest.main()
