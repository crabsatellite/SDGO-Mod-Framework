import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from manager.providers import Binary
from tools import distribution_guard as guard


MARKER = b"".join((b"SDGO_LOCAL_", b"ONLY_NONREDISTRIBUTABLE"))
SOURCE_MARKER = b"".join((b"// SDGO_LOCAL_ONLY_", b"SOURCE_FILE"))
ROOT = Path(__file__).resolve().parents[1]


def manifests(scope="public"):
    return {
        "client/sdgo/mods/example/mod.ini":
            ("[mod]\nid=example\ndistribution=" + scope + "\n").encode(),
        "client/sdgo/mods/example/manager.json":
            json.dumps({"schema_version": 1, "id": "example",
                        "distribution": scope}).encode(),
    }


def archive(entries):
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as writer:
        for name, raw in entries.items():
            writer.writestr(name, raw)
    return output.getvalue()


def init_repo(repo: Path) -> None:
    subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
    subprocess.run(["git", "config", "user.name", "Test"], cwd=repo, check=True)
    subprocess.run(
        ["git", "config", "user.email", "test@example.invalid"],
        cwd=repo, check=True,
    )
    # Background auto-maintenance keeps a Windows temporary repository's
    # current directory open after the test has finished.
    subprocess.run(["git", "config", "gc.auto", "0"], cwd=repo, check=True)
    subprocess.run(
        ["git", "config", "maintenance.auto", "false"],
        cwd=repo, check=True,
    )


class DistributionGuardTests(unittest.TestCase):
    def test_scanner_changes_invalidate_safe_blob_cache_automatically(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            script = root / "scanner.py"
            def git_path(_repo, arguments):
                return subprocess.CompletedProcess(arguments, 0, (".git/" + arguments[-1]).encode())
            with patch.object(guard, "__file__", str(script)), patch.object(guard, "_git", side_effect=git_path):
                script.write_text("first scanner revision", encoding="utf-8")
                first = guard._git_cache_path(root, True)
                script.write_text("new scanner rules", encoding="utf-8")
                second = guard._git_cache_path(root, True)
                strict = guard._git_cache_path(root, False)
                self.assertNotEqual(first, second)
                self.assertNotEqual(second, strict)

    def test_contracts_are_strict_and_utf16_ini_is_supported(self):
        self.assertEqual(guard.check_entries(manifests()), 1)
        utf16 = "[mod]\nid=example\ndistribution=public\n".encode("utf-16")
        entries = manifests()
        entries["client/sdgo/mods/example/mod.ini"] = utf16
        self.assertEqual(guard.check_entries(entries), 1)
        for changed in (
            {**manifests(), "renamed.bin": b"prefix" + MARKER + b"suffix"},
            manifests("local_only"),
            {"mods/example/mod.ini": manifests()["client/sdgo/mods/example/mod.ini"]},
        ):
            with self.assertRaises(ValueError):
                guard.check_entries(changed)

    def test_repository_and_source_markers_are_detected_without_a_manifest(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "distribution.json").write_text(json.dumps({
                "distribution": "local_only", "redistributable": False
            }), encoding="utf-8")
            with self.assertRaises(ValueError):
                guard.check_directory(root, source=True)
            (root / "distribution.json").unlink()
            (root / "renamed.cpp").write_bytes(SOURCE_MARKER + b"\nint value;\n")
            with self.assertRaises(ValueError):
                guard.check_directory(root, source=True)

    def test_manager_binary_export_rejects_local_zip_and_accepts_public_zip(self):
        public = archive({**manifests(), "client/sdgo/mods/example/example.dll": b"public"})
        self.assertEqual(Binary(public, "application/zip", "public.zip").data, public)
        private = archive({
            **manifests("local_only"),
            "client/sdgo/mods/example/renamed.dll": MARKER,
        })
        with self.assertRaises(ValueError):
            Binary(private, "application/zip", "private.zip")
        nested = archive({"renamed-toolkit.dat": private})
        with self.assertRaises(ValueError):
            Binary(nested, "application/zip", "outer.zip")
        with self.assertRaises(ValueError):
            Binary(b"MZ-stub" + private, "application/octet-stream", "setup.exe")
        addon = archive({
            "manager/addons/personal_fixture/manager.json": json.dumps({
                "schema_version": 1, "id": "personal_fixture",
                "distribution": "local_only",
            }).encode(),
        })
        with self.assertRaises(ValueError):
            Binary(addon, "application/zip", "manager-data.zip")

    def test_archive_guard_detects_content_regardless_of_name_or_prefix(self):
        private = archive({
            **manifests("local_only"),
            "client/sdgo/mods/example/example.dll": MARKER,
        })
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            nested = root / "public-package.zip"
            nested.write_bytes(archive({"renamed-private.dat": private}))
            with self.assertRaises(ValueError):
                guard.check_archive(nested)
            sfx = root / "renamed-package.dat"
            sfx.write_bytes(b"MZ-stub" + private)
            with self.assertRaises(ValueError):
                guard.check_archive(sfx)
            with self.assertRaises(ValueError):
                guard.check_directory(root)

    def test_copied_scanner_is_standalone(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            script = root / "guard.py"
            shutil.copyfile(ROOT / "tools/distribution_guard.py", script)
            payload = root / "payload"
            for name, raw in manifests().items():
                path = payload / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(raw)
            result = subprocess.run(
                [sys.executable, str(script), "--path", str(payload)],
                cwd=root, capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_git_range_finds_private_blob_deleted_from_final_head(self):
        with tempfile.TemporaryDirectory() as folder:
            repo = Path(folder)
            init_repo(repo)
            for name, raw in manifests().items():
                path = repo / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(raw)
            subprocess.run(["git", "add", "."], cwd=repo, check=True)
            subprocess.run(["git", "commit", "-qm", "public"], cwd=repo, check=True)
            base = subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=repo, text=True
            ).strip()
            private = repo / "private.cpp"
            private.write_bytes(SOURCE_MARKER + b"\n")
            subprocess.run(["git", "add", "."], cwd=repo, check=True)
            subprocess.run(["git", "commit", "-qm", "private"], cwd=repo, check=True)
            private.unlink()
            subprocess.run(["git", "add", "-u"], cwd=repo, check=True)
            subprocess.run(["git", "commit", "-qm", "remove"], cwd=repo, check=True)
            self.assertEqual(guard.check_git(repo, "HEAD"), 1)
            with self.assertRaises(ValueError):
                guard.check_git_range(repo, base + "..HEAD")
            renamed = repo / "asset.dat"
            renamed.write_bytes(MARKER)
            subprocess.run(["git", "add", "asset.dat"], cwd=repo, check=True)
            with self.assertRaises(ValueError):
                guard.check_git(repo, None)

    def test_git_modes_inspect_compressed_archives_by_blob_content(self):
        with tempfile.TemporaryDirectory() as folder:
            repo = Path(folder)
            init_repo(repo)
            for name, raw in manifests().items():
                path = repo / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(raw)
            bundle = repo / "bundle.dat"
            bundle.write_bytes(archive({
                **manifests(),
                "client/sdgo/mods/example/example.dll": b"public",
            }))
            subprocess.run(["git", "add", "."], cwd=repo, check=True)
            self.assertGreaterEqual(guard.check_git(repo, None), 1)
            subprocess.run(["git", "commit", "-qm", "public archive"], cwd=repo, check=True)
            public_commit = subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=repo, text=True
            ).strip()
            self.assertGreaterEqual(guard.check_git(repo, "HEAD"), 1)
            bundle.write_bytes(archive({
                **manifests("local_only"),
                "client/sdgo/mods/example/example.dll": MARKER,
            }))
            subprocess.run(["git", "add", "bundle.dat"], cwd=repo, check=True)
            with self.assertRaises(ValueError):
                guard.check_git(repo, None)
            subprocess.run(["git", "commit", "-qm", "private archive"], cwd=repo, check=True)
            private_commit = subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=repo, text=True
            ).strip()
            with self.assertRaises(ValueError):
                guard.check_git(repo, private_commit)
            with self.assertRaises(ValueError):
                guard.check_git_range(repo, public_commit + ".." + private_commit)
            bundle.unlink()
            subprocess.run(["git", "add", "-u"], cwd=repo, check=True)
            subprocess.run(["git", "commit", "-qm", "delete private archive"],
                           cwd=repo, check=True)
            self.assertGreaterEqual(guard.check_git(repo, "HEAD"), 1)
            with self.assertRaises(ValueError):
                guard.check_git_range(repo, public_commit + "..HEAD")

    @unittest.skipUnless(os.name == "nt", "Windows cwd-lock regression")
    def test_rejected_git_archives_release_temporary_repository_immediately(self):
        for attempt in range(4):
            temporary = tempfile.TemporaryDirectory(
                prefix=f"sdgo-guard-lock-{attempt}-"
            )
            repo = Path(temporary.name)

            def run_git(*args: str, capture: bool = False) -> str:
                completed = subprocess.run(
                    ["git", *args],
                    cwd=repo,
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
                    stderr=subprocess.PIPE,
                    check=True,
                    text=capture,
                )
                return completed.stdout.strip() if capture else ""

            try:
                run_git("init", "-q")
                for key, value in (
                    ("user.name", "Test"),
                    ("user.email", "test@example.invalid"),
                    ("gc.auto", "0"),
                    ("maintenance.auto", "false"),
                ):
                    run_git("config", key, value)
                (repo / "public.txt").write_text("public\n", encoding="ascii")
                run_git("add", ".")
                run_git("commit", "-qm", "base")
                base = run_git("rev-parse", "HEAD", capture=True)
                (repo / "bundle.dat").write_bytes(archive({
                    **manifests("local_only"),
                    "client/sdgo/mods/example/example.dll": MARKER,
                }))
                run_git("add", "bundle.dat")
                with self.assertRaises(ValueError):
                    guard.check_git(repo, None)
                run_git("commit", "-qm", "bad archive")
                with self.assertRaises(ValueError):
                    guard.check_git(repo, "HEAD")
                with self.assertRaises(ValueError):
                    guard.check_git_range(repo, base + "..HEAD")
            finally:
                temporary.cleanup()
            self.assertFalse(repo.exists())

    def test_legacy_git_mode_accepts_only_fully_undeclared_contracts(self):
        with tempfile.TemporaryDirectory() as folder:
            repo = Path(folder)
            init_repo(repo)
            mod = repo / "mods/example"
            mod.mkdir(parents=True)
            (mod / "mod.ini").write_text("[mod]\nid=example\n", encoding="utf-8")
            (mod / "manager.json").write_text(
                json.dumps({"schema_version": 1, "id": "example"}), encoding="utf-8"
            )
            subprocess.run(["git", "add", "."], cwd=repo, check=True)
            self.assertEqual(
                guard.check_git(repo, None, allow_legacy_public=True), 1
            )
            with self.assertRaises(ValueError):
                guard.check_git(repo, None)
            (mod / "manager.json").write_text(json.dumps({
                "schema_version": 1, "id": "example", "distribution": "public"
            }), encoding="utf-8")
            subprocess.run(["git", "add", "."], cwd=repo, check=True)
            with self.assertRaises(ValueError):
                guard.check_git(repo, None, allow_legacy_public=True)


if __name__ == "__main__":
    unittest.main()
