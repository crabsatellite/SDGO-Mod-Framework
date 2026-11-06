"""Check real guarded file I/O and outside bytes in disposable owned fixtures."""
from pathlib import Path
import hashlib
import os
import subprocess
import sys
import tempfile


def main():
    probe, library = (Path(value).resolve() for value in sys.argv[1:3])
    with tempfile.TemporaryDirectory(prefix="sql-file-guard-", dir=Path.cwd()) as temporary:
        root = Path(temporary).resolve()
        assert root.parent == Path.cwd().resolve() and root.name.startswith("sql-file-guard-")
        allowed, outside = root / "allowed", root / "outside"
        for directory in (allowed / "resources", allowed / "unmarked", outside, root / "allowed-other"):
            directory.mkdir(parents=True, exist_ok=True)
        (allowed / "TEST-ONLY.txt").write_text("Native file guard fixture only\n")
        for path in (allowed / "local.mdf", allowed / "rename-inside.mdf", allowed / "delete-inside.mdf",
                     outside / "model.mdf", outside / "shared-original.mdf",
                     outside / "note.txt", root / "allowed-other/model.mdf",
                     allowed / "resources/mssqlsystemresource.mdf", allowed / "resources/mssqlsystemresource.ldf"):
            path.write_bytes(b"original owned fixture bytes\n")
        os.link(outside / "shared-original.mdf", allowed / "shared.mdf")
        junction = subprocess.run(["cmd", "/d", "/c", "mklink", "/J", str(allowed / "junction"), str(outside)],
                                  capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW)
        if junction.returncode:
            raise RuntimeError("Cannot create the owned junction fixture")
        before = {path: hashlib.sha256(path.read_bytes()).hexdigest()
                  for path in [*outside.iterdir(), root / "allowed-other/model.mdf"]}
        result = subprocess.run([str(probe), str(library), str(root)], capture_output=True,
                                text=True, timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        if result.returncode:
            return result.returncode
        for mode in ("missing-marker", "outside-resource"):
            rejected = subprocess.run([str(probe), str(library), str(root), mode], capture_output=True,
                                      text=True, timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
            if rejected.returncode:
                print(rejected.stderr, file=sys.stderr)
                return rejected.returncode
        for path, expected in before.items():
            assert hashlib.sha256(path.read_bytes()).hexdigest() == expected, path.name
        assert not (outside / "absent.ldf").exists()
        assert not (outside / "renamed.tmp").exists()
        assert not (outside / "new-relative.ndf").exists()
        assert not (outside / "bypass.tmp").exists()
        assert (allowed / "new.ndf").is_file()
        assert not (allowed / "new-link.mdf").exists()
        assert (allowed / "local.mdf").read_bytes().startswith(b"clone")
        assert (allowed / "renamed-inside.mdf").is_file() and not (allowed / "rename-inside.mdf").exists()
        assert not (allowed / "delete-inside.mdf").exists()
        denied_log = (allowed / "sql-file-guard-denied.log").read_text(encoding="utf-8")
        assert "op=DeleteFile" in denied_log and "op=RenameFile" in denied_log
        assert "op=DatabaseOpened" in denied_log and str(allowed / "local.mdf") in denied_log
        unresolved = [line for line in denied_log.splitlines() if "op=DatabaseHandleDenied" in line]
        assert not unresolved, unresolved
        assert str(outside / "model.mdf") in denied_log
        # Remove the junction explicitly before TemporaryDirectory walks the fixture.
        os.rmdir(allowed / "junction")
    print("Outside files unchanged; owned fixture removed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
