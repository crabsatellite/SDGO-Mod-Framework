"""Run the account-free native FPS GameTest matrix and retain only evidence."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import stat
import sys
import uuid

import game_test


MATRIX = (
    ("disabled160", False, 160),
    ("enabled30", True, 30),
    ("enabled60", True, 60),
    ("enabled120", True, 120),
    ("enabled160", True, 160),
)
OWNER = "sdgo-native-fps-matrix"
OWNER_FILE = ".fps-matrix-owner.json"
SENSITIVE_KEYS = ("account", "credential", "password", "secret", "username")
LOG_SECRET = re.compile(
    r"(?i)\b(account|credential|password|secret|username)\b(\s*[:=]\s*)(\S+)"
)


class FpsMatrixError(RuntimeError):
    pass


def _is_relative_to(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def _is_reparse(path: Path) -> bool:
    try:
        info = path.lstat()
    except FileNotFoundError:
        return False
    attributes = getattr(info, "st_file_attributes", 0)
    reparse_flag = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    return path.is_symlink() or bool(attributes & reparse_flag)


def _lexical_absolute(path: Path) -> Path:
    return Path(os.path.abspath(os.path.expanduser(str(path))))


def _assert_existing_ancestors_are_plain(path: Path) -> None:
    path = _lexical_absolute(path)
    current = path.anchor
    if not current:
        return
    cursor = Path(current)
    for part in path.parts[1:]:
        cursor /= part
        if cursor.exists() and _is_reparse(cursor):
            raise FpsMatrixError(f"path crosses a reparse point: {cursor}")


def _overlaps(first: Path, second: Path) -> bool:
    return _is_relative_to(first, second) or _is_relative_to(second, first)


def validate_roots(
    client_template: Path, stage: Path, work_dir: Path, results_dir: Path
) -> tuple[Path, Path, Path, Path]:
    raw = {
        "client template": _lexical_absolute(client_template),
        "stage": _lexical_absolute(stage),
        "work directory": _lexical_absolute(work_dir),
        "results directory": _lexical_absolute(results_dir),
    }
    for label, path in raw.items():
        _assert_existing_ancestors_are_plain(path)
        if path.exists() and _is_reparse(path):
            raise FpsMatrixError(f"{label} is a reparse point: {path}")
    client_template = raw["client template"].resolve()
    stage = raw["stage"].resolve()
    work_dir = raw["work directory"].resolve()
    results_dir = raw["results directory"].resolve()
    if not client_template.is_dir() or not (client_template / game_test.GAME_EXE).is_file():
        raise FpsMatrixError(f"client template lacks {game_test.GAME_EXE}: {client_template}")
    if not (stage / "client").is_dir():
        raise FpsMatrixError(f"stage lacks client tree: {stage / 'client'}")
    named = {
        "client template": client_template,
        "stage": stage,
        "work directory": work_dir,
        "results directory": results_dir,
    }
    for left, right in (
        ("client template", "work directory"),
        ("client template", "results directory"),
        ("stage", "work directory"),
        ("stage", "results directory"),
        ("work directory", "results directory"),
    ):
        if _overlaps(named[left], named[right]):
            raise FpsMatrixError(f"{left} and {right} must be disjoint")
    work_dir.mkdir(parents=True, exist_ok=True)
    results_dir.mkdir(parents=True, exist_ok=True)
    for label, path in (("work directory", work_dir), ("results directory", results_dir)):
        if _is_reparse(path):
            raise FpsMatrixError(f"{label} is a reparse point: {path}")
    return client_template, stage, work_dir, results_dir


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    os.replace(temporary, path)


def _read_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise FpsMatrixError(f"cannot read owned metadata {path}: {error}") from error
    if not isinstance(value, dict):
        raise FpsMatrixError(f"owned metadata is not an object: {path}")
    return value


def _sanitize(value: object) -> object:
    if isinstance(value, dict):
        return {
            str(key): (
                "<redacted>"
                if any(part in str(key).lower() for part in SENSITIVE_KEYS)
                else _sanitize(item)
            )
            for key, item in value.items()
        }
    if isinstance(value, list):
        return [_sanitize(item) for item in value]
    return value


def _copy_json(source: Path, target: Path) -> bool:
    if not source.is_file():
        return False
    _write_json(target, _sanitize(_read_json(source)))
    return True


def _copy_text(source: Path, target: Path, *, redact: bool = False) -> bool:
    if not source.is_file():
        return False
    if _is_reparse(source):
        raise FpsMatrixError(f"evidence source is a reparse point: {source}")
    text = source.read_text(encoding="utf-8", errors="replace")
    if redact:
        text = LOG_SECRET.sub(lambda match: match.group(1) + match.group(2) + "<redacted>", text)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding="utf-8")
    return True


def _copy_json_lines(source: Path, target: Path) -> bool:
    if not source.is_file():
        return False
    rows = []
    for number, raw in enumerate(source.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        try:
            rows.append(json.dumps(_sanitize(json.loads(raw)), sort_keys=True))
        except json.JSONDecodeError:
            rows.append(json.dumps({"invalid_json_line": number}))
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text("\n".join(rows) + ("\n" if rows else ""), encoding="utf-8")
    return True


def _validate_owned_metadata(
    metadata: dict,
    run_dir: Path,
    client_template: Path,
    session_id: str,
    label: str,
    fps_mod_enabled: bool,
    fps_cap: int,
) -> dict:
    try:
        run_id = str(uuid.UUID(str(metadata["run_id"])))
        client_root = Path(metadata["client"]["root"]).resolve()
        template = Path(metadata["template"]).resolve()
    except (KeyError, TypeError, ValueError) as error:
        raise FpsMatrixError("prepared capsule lacks valid ownership metadata") from error
    if run_dir.parent.name != f"fps-matrix-{session_id}":
        raise FpsMatrixError("prepared capsule is outside the owned matrix session")
    if client_root != (run_dir / "client").resolve():
        raise FpsMatrixError("prepared client root does not match the owned capsule")
    if template != client_template:
        raise FpsMatrixError("prepared capsule does not name the requested template")
    plan = metadata.get("plan")
    if (
        metadata.get("suite") != game_test.FPS_SUITE
        or not isinstance(plan, dict)
        or plan.get("fps_cap") != fps_cap
        or plan.get("fps_mod_enabled") is not fps_mod_enabled
    ):
        raise FpsMatrixError("prepared capsule does not match the requested native FPS case")
    if (client_root / "sdgo" / "gametest" / "private").exists():
        raise FpsMatrixError("native FPS capsule unexpectedly contains private account data")
    marker = {
        "owner": OWNER,
        "session_id": session_id,
        "label": label,
        "run_id": run_id,
        "fps_mod_enabled": fps_mod_enabled,
        "fps_cap": fps_cap,
        "run_dir": str(run_dir),
        "client_root": str(client_root),
    }
    _write_json(run_dir / OWNER_FILE, marker)
    return marker


def export_evidence(run_dir: Path, result_dir: Path, metadata: dict) -> list[str]:
    client_root = Path(metadata["client"]["root"]).resolve()
    events = Path(metadata["client"]["events"]).resolve()
    if client_root != (run_dir / "client").resolve() or not _is_relative_to(events, client_root):
        raise FpsMatrixError("native evidence paths escape the owned client")
    copied = []
    json_files = (
        (run_dir / "run.json", result_dir / "run.json"),
        (run_dir / "receipts" / "report.json", result_dir / "receipts" / "report.json"),
        (
            run_dir / "receipts" / "quiet-runtime.json",
            result_dir / "receipts" / "quiet-runtime.json",
        ),
    )
    for source, target in json_files:
        if _copy_json(source, target):
            copied.append(str(target.relative_to(result_dir)))
    text_files = (
        (run_dir / "receipts" / "junit.xml", result_dir / "receipts" / "junit.xml", False),
        (
            run_dir / "receipts" / "client.stdout.log",
            result_dir / "receipts" / "client.stdout.log",
            True,
        ),
        (
            run_dir / "receipts" / "client.stderr.log",
            result_dir / "receipts" / "client.stderr.log",
            True,
        ),
        (
            client_root / "sdgo" / "gametest" / "quiet-sockets.ini",
            result_dir / "native" / "quiet-sockets.ini",
            False,
        ),
        (
            client_root / "sdgo" / "gametest" / "quiet-verified.ini",
            result_dir / "native" / "quiet-verified.ini",
            False,
        ),
        (
            client_root / "sdgo" / "logs" / "sdgo.log",
            result_dir / "diagnostics" / "sdgo.log",
            True,
        ),
    )
    for source, target, redact in text_files:
        if _copy_text(source, target, redact=redact):
            copied.append(str(target.relative_to(result_dir)))
    if _copy_json_lines(events, result_dir / "native" / "events.jsonl"):
        copied.append("native/events.jsonl")
    return copied


def _assert_plain_tree(root: Path) -> None:
    if _is_reparse(root):
        raise FpsMatrixError(f"owned capsule is a reparse point: {root}")
    for current, directories, files in os.walk(root, topdown=True, followlinks=False):
        base = Path(current)
        for name in (*directories, *files):
            path = base / name
            if _is_reparse(path):
                raise FpsMatrixError(f"owned capsule contains a reparse point: {path}")


def cleanup_owned_capsule(
    run_dir: Path,
    session_work: Path,
    results_session: Path,
    client_template: Path,
    expected_marker: dict,
) -> dict:
    raw_run_dir = _lexical_absolute(run_dir)
    raw_session_work = _lexical_absolute(session_work)
    raw_results_session = _lexical_absolute(results_session)
    raw_client_template = _lexical_absolute(client_template)
    for path in (
        raw_run_dir,
        raw_session_work,
        raw_results_session,
        raw_client_template,
    ):
        _assert_existing_ancestors_are_plain(path)
    run_dir = raw_run_dir.resolve()
    session_work = raw_session_work.resolve()
    results_session = raw_results_session.resolve()
    client_template = raw_client_template.resolve()
    if run_dir.parent != session_work or run_dir == session_work:
        raise FpsMatrixError("cleanup target is not a direct child of the owned work session")
    if _overlaps(run_dir, results_session) or _overlaps(run_dir, client_template):
        raise FpsMatrixError("cleanup target overlaps protected template or results")
    metadata = _read_json(run_dir / "run.json")
    marker = _read_json(run_dir / OWNER_FILE)
    if marker != expected_marker or marker.get("owner") != OWNER:
        raise FpsMatrixError("cleanup ownership marker does not match")
    marker_run = _lexical_absolute(Path(str(marker.get("run_dir", ""))))
    marker_client = _lexical_absolute(Path(str(marker.get("client_root", ""))))
    _assert_existing_ancestors_are_plain(marker_run)
    _assert_existing_ancestors_are_plain(marker_client)
    if marker_run.resolve() != run_dir or marker_client.resolve() != (run_dir / "client"):
        raise FpsMatrixError("cleanup marker paths do not match the owned capsule")
    if metadata.get("run_id") != marker.get("run_id"):
        raise FpsMatrixError("cleanup run id does not match the owned marker")
    client_root = Path(metadata.get("client", {}).get("root", "")).resolve()
    if client_root != (run_dir / "client").resolve():
        raise FpsMatrixError("cleanup client root does not match the owned capsule")
    execution = metadata.get("execution")
    if isinstance(execution, dict) and execution.get("status") == "running":
        raise FpsMatrixError("owned client execution is still marked running")
    owned_pids = {
        value
        for value in (
            execution.get("entry_pid") if isinstance(execution, dict) else None,
            execution.get("game_pid") if isinstance(execution, dict) else None,
        )
        if isinstance(value, int) and value > 0
    }
    for row in game_test.process_snapshot():
        image = row.get("image_path")
        if row.get("pid") in owned_pids or (
            image and _is_relative_to(Path(image).resolve(), client_root)
        ):
            raise FpsMatrixError(
                f"owned client process is still running: pid {row.get('pid')}"
            )
    _assert_plain_tree(run_dir)
    shutil.rmtree(run_dir)
    return {"status": "removed", "run_dir": str(run_dir)}


def _finish_work_session(session_work: Path, session_marker: dict) -> dict:
    marker_path = session_work / OWNER_FILE
    if _read_json(marker_path) != session_marker:
        raise FpsMatrixError("work-session ownership marker changed")
    remaining = [path for path in session_work.iterdir() if path != marker_path]
    if remaining:
        raise FpsMatrixError("owned work session still contains uncleared capsules")
    marker_path.unlink()
    session_work.rmdir()
    return {"status": "removed", "work_session": str(session_work)}


def run_matrix(
    client_template: Path,
    stage: Path,
    work_dir: Path,
    results_dir: Path,
    *,
    timeout: float = 600.0,
    target_host: str = "127.0.0.1",
    login_port: int = 5001,
) -> dict:
    client_template, stage, work_dir, results_dir = validate_roots(
        client_template, stage, work_dir, results_dir
    )
    session_id = uuid.uuid4().hex
    session_name = f"fps-matrix-{session_id}"
    session_work = work_dir / session_name
    results_session = results_dir / session_name
    session_work.mkdir()
    results_session.mkdir()
    session_marker = {
        "owner": OWNER,
        "session_id": session_id,
        "client_template": str(client_template),
        "stage": str(stage),
        "results": str(results_session),
    }
    _write_json(session_work / OWNER_FILE, session_marker)
    cases = []
    for label, enabled, cap in MATRIX:
        run_dir = session_work / label
        result_dir = results_session / label
        result_dir.mkdir()
        case = {
            "label": label,
            "fps_mod_enabled": enabled,
            "fps_cap": cap,
            "status": "fail",
            "run_id": None,
            "report_status": None,
            "evidence": [],
            "error": None,
            "cleanup": {"status": "not_started"},
        }
        metadata = None
        marker = None
        try:
            metadata = game_test.prepare(
                client_template,
                stage,
                run_dir,
                target_host=target_host,
                login_port=login_port,
                network_policy="direct",
                quiet=True,
                fps_cap=cap,
                fps_mod_enabled=enabled,
            )
            marker = _validate_owned_metadata(
                metadata,
                run_dir,
                client_template,
                session_id,
                label,
                enabled,
                cap,
            )
            case["run_id"] = marker["run_id"]
            report = game_test.run_prepared(run_dir, timeout=timeout)
            case["report_status"] = report.get("status")
            case["status"] = "pass" if report.get("status") == "pass" else "fail"
        except Exception as error:
            case["error"] = f"{type(error).__name__}: {error}"
        finally:
            if metadata is None and (run_dir / "run.json").is_file():
                try:
                    metadata = _read_json(run_dir / "run.json")
                    marker = _validate_owned_metadata(
                        metadata,
                        run_dir,
                        client_template,
                        session_id,
                        label,
                        enabled,
                        cap,
                    )
                    case["run_id"] = marker["run_id"]
                except Exception as error:
                    case["error"] = case["error"] or f"{type(error).__name__}: {error}"
            if metadata is not None:
                try:
                    case["evidence"] = export_evidence(run_dir, result_dir, metadata)
                except Exception as error:
                    case["status"] = "fail"
                    case["error"] = case["error"] or f"{type(error).__name__}: {error}"
            if marker is not None:
                try:
                    case["cleanup"] = cleanup_owned_capsule(
                        run_dir,
                        session_work,
                        results_session,
                        client_template,
                        marker,
                    )
                except Exception as error:
                    case["status"] = "fail"
                    case["cleanup"] = {
                        "status": "fail",
                        "error": f"{type(error).__name__}: {error}",
                    }
            elif run_dir.exists():
                case["status"] = "fail"
                case["cleanup"] = {
                    "status": "fail",
                    "error": "capsule lacks a validated ownership marker; left in place",
                }
            else:
                case["cleanup"] = {"status": "not_needed"}
            if case["cleanup"]["status"] not in {"removed", "not_needed"}:
                case["status"] = "fail"
            _write_json(result_dir / "case-summary.json", case)
            cases.append(case)
    session_cleanup = {"status": "not_started"}
    try:
        session_cleanup = _finish_work_session(session_work, session_marker)
    except Exception as error:
        session_cleanup = {
            "status": "fail",
            "error": f"{type(error).__name__}: {error}",
        }
    status = (
        "pass"
        if all(case["status"] == "pass" for case in cases)
        and session_cleanup["status"] == "removed"
        else "fail"
    )
    summary = {
        "schema": 1,
        "status": status,
        "session_id": session_id,
        "client_template": str(client_template),
        "stage": str(stage),
        "results": str(results_session),
        "cases": cases,
        "cleanup": session_cleanup,
    }
    _write_json(results_session / "matrix-summary.json", summary)
    return summary


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client-template", type=Path, required=True)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--results-dir", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=600.0,
                        help="whole-client timeout, including native resource loading (seconds)")
    parser.add_argument("--target-host", default="127.0.0.1")
    parser.add_argument("--login-port", type=int, default=5001)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        result = run_matrix(
            args.client_template,
            args.stage,
            args.work_dir,
            args.results_dir,
            timeout=args.timeout,
            target_host=args.target_host,
            login_port=args.login_port,
        )
    except (FpsMatrixError, game_test.GameTestError, OSError) as error:
        print(f"game_test_fps: {error}", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
