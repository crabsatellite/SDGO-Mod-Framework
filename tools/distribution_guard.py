"""Fail closed when public source or package bytes contain local-only work.

This file deliberately uses only the Python standard library so it can be
copied into a repository's .git directory and invoked from Git hooks.
"""
from __future__ import annotations

import argparse
import configparser
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import stat
import subprocess
import sys
import tempfile
import zipfile

PUBLIC = "public"
ALLOWED = {PUBLIC, "owner", "local_only"}
# Construct private markers so this public scanner does not contain either
# complete marker in its own source bytes.
NONREDISTRIBUTABLE_MARKER = b"".join(
    (b"SDGO_LOCAL_", b"ONLY_NONREDISTRIBUTABLE")
)
LOCAL_SOURCE_MARKER = b"".join(
    (b"// SDGO_LOCAL_ONLY_", b"SOURCE_FILE")
)
OWNER_MARKER = b"".join((b"SDGO_OWNER_", b"ONLY_REDISTRIBUTABLE"))
OWNER_SOURCE_MARKER = b"".join((b"// SDGO_OWNER_", b"SOURCE_FILE"))
PUBLIC_BLOCKED_MARKERS = (NONREDISTRIBUTABLE_MARKER, OWNER_MARKER, OWNER_SOURCE_MARKER)
POLICY_LIMIT = 2 * 1024 * 1024
NESTED_ARCHIVE_LIMIT = 512 * 1024 * 1024
ZIP_TAIL_LIMIT = 65_557
SOURCE_PRUNE = {
    ".git", ".hg", ".svn", ".mypy_cache", ".pytest_cache", ".ruff_cache",
    ".venv", "__pycache__", "build", "dist", "node_modules", "out", "venv",
}


def normalized(name: str) -> str:
    value = str(PurePosixPath(name.replace("\\", "/")))
    if value.startswith("/") or any(part in {"", ".", ".."} for part in value.split("/")):
        raise ValueError("Unsafe payload path: " + name)
    if any(part.casefold()in('owner-mods','local-mods')for part in value.split('/')):
        raise ValueError('Private programme directory cannot enter public payload: '+name)
    return value


def _mod_ini(raw: bytes, *, allow_duplicate_settings: bool = False) -> configparser.ConfigParser:
    value = configparser.ConfigParser(interpolation=None, strict=True)
    if raw.startswith(b"\xff\xfe"):
        text = raw[2:].decode("utf-16-le")
    elif raw.startswith(b"\xfe\xff"):
        text = raw[2:].decode("utf-16-be")
    elif raw.startswith(b"\xef\xbb\xbf"):
        text = raw[3:].decode("utf-8")
    else:
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            text = raw.decode("mbcs" if os.name == "nt" else "cp1252")
    if allow_duplicate_settings:
        # Historical native INIs can repeat the non-policy [settings] section.
        # Every other section/key, especially [mod] distribution, remains strict.
        policy=[];settings=False
        for line in text.splitlines(keepends=True):
            section=configparser.ConfigParser.SECTCRE.match(line.strip())
            if section:settings=section.group('header').casefold()=='settings'
            if not settings:policy.append(line)
        configparser.ConfigParser(interpolation=None,strict=True).read_string(''.join(policy))
        value=configparser.ConfigParser(interpolation=None,strict=False)
    value.read_string(text)
    if not value.has_section("mod"):
        raise ValueError("Mod manifest has no [mod] section")
    return value


def _manager_json(raw: bytes) -> dict:
    value = json.loads(raw.decode("utf-8-sig"))
    if not isinstance(value, dict):
        raise ValueError("Manager registration must be an object")
    return value


def _require_public(
    base: str, mod_raw: bytes | None, manager_raw: bytes | None, *,
    allow_legacy_public: bool = False,
) -> None:
    registration = _manager_json(manager_raw) if manager_raw is not None else None
    registered = registration.get("distribution") if registration is not None else None
    if mod_raw is None:
        if registered == PUBLIC or (registered is None and allow_legacy_public):
            return
        if registered not in ALLOWED:
            raise ValueError(base + ": unknown Mod distribution: " + str(registered))
        if registered != PUBLIC:
            raise ValueError(base + ": Manager-only contract must declare public distribution")
    manifest = _mod_ini(mod_raw,allow_duplicate_settings=allow_legacy_public)
    declared = manifest.get("mod", "distribution", fallback=None)
    for value in (declared, registered):
        if value is not None and value not in ALLOWED:
            raise ValueError(base + ": unknown Mod distribution: " + str(value))
    if allow_legacy_public and declared is None and registered is None:
        return
    if declared != PUBLIC or registered != PUBLIC:
        raise ValueError(
            base + ": public Mod must declare distribution=public in mod.ini "
            "and manager.json"
        )


def _manifest_base(name: str) -> tuple[str, str] | None:
    parts = normalized(name).split("/")
    folded = [part.casefold() for part in parts]
    if "local-mods" in folded:
        raise ValueError("The entire local-mods directory is nonredistributable")
    if (len(parts) >= 3 and folded[-3] in {"mods", "addons", "support"}
            and folded[-1] in {"mod.ini", "manager.json"}):
        return "/".join(parts[:-1]), folded[-1]
    return None


def _is_distribution_file(name: str) -> bool:
    parts = normalized(name).split("/")
    return parts[-1].casefold() == "distribution.json" and len(parts) <= 2


def _check_distribution_file(name: str, raw: bytes) -> None:
    value = json.loads(raw.decode("utf-8-sig"))
    if not isinstance(value, dict):
        raise ValueError(name + ": distribution.json must be an object")
    if value.get("distribution") in ("local_only", "owner") or value.get("redistributable") is False:
        raise ValueError(name + ": local-only repository cannot enter public distribution")


def _check_marker_bytes(name: str, raw: bytes) -> None:
    if any(marker in raw for marker in PUBLIC_BLOCKED_MARKERS):
        raise ValueError("Non-redistributable marker found in public bytes: " + name)
    for line in raw.splitlines():
        if line.strip() == LOCAL_SOURCE_MARKER:
            raise ValueError("Local-only source marker found in public bytes: " + name)


def _check_policies(
    policies: dict[str, dict[str, bytes]], *, allow_legacy_public: bool = False
) -> int:
    for base, files in sorted(policies.items()):
        try:
            _require_public(
                base, files.get("mod.ini"), files.get("manager.json"),
                allow_legacy_public=allow_legacy_public,
            )
        except (configparser.Error, json.JSONDecodeError, UnicodeError) as error:
            raise ValueError(base + ": " + str(error)) from error
    return len(policies)


def check_entries(entries: dict[str, bytes]) -> int:
    """Check an in-memory fixture. Production modes below stream large files."""
    policies: dict[str, dict[str, bytes]] = {}
    for supplied_name, raw in entries.items():
        name = normalized(supplied_name)
        _check_marker_bytes(name, raw)
        manifest = _manifest_base(name)
        if manifest:
            base, leaf = manifest
            policies.setdefault(base, {})[leaf] = raw
        if _is_distribution_file(name):
            _check_distribution_file(name, raw)
    return _check_policies(policies)


class _StreamScanner:
    def __init__(self, name: str):
        self.name = name
        self.overlap = b""
        self.line = b""

    def feed(self, chunk: bytes) -> None:
        combined = self.overlap + chunk
        if any(marker in combined for marker in PUBLIC_BLOCKED_MARKERS):
            raise ValueError(
                "Non-redistributable marker found in public bytes: " + self.name
            )
        keep = max(0, max(map(len, PUBLIC_BLOCKED_MARKERS)) - 1)
        self.overlap = combined[-keep:] if keep else b""
        lines = (self.line + chunk).split(b"\n")
        self.line = lines.pop()
        if any(line.rstrip(b"\r").strip() == LOCAL_SOURCE_MARKER for line in lines):
            raise ValueError("Local-only source marker found in public bytes: " + self.name)

    def finish(self) -> None:
        if self.line.rstrip(b"\r").strip() == LOCAL_SOURCE_MARKER:
            raise ValueError("Local-only source marker found in public bytes: " + self.name)


def _scan_stream(name: str, stream, capture: bool = False, copy_to=None) -> bytes | None:
    scanner = _StreamScanner(name)
    collected = bytearray() if capture else None
    total = 0
    while True:
        chunk = stream.read(1024 * 1024)
        if not chunk:
            break
        total += len(chunk)
        if capture and total > POLICY_LIMIT:
            raise ValueError("Distribution manifest is too large: " + name)
        scanner.feed(chunk)
        if collected is not None:
            collected.extend(chunk)
        if copy_to is not None:
            copy_to.write(chunk)
    scanner.finish()
    return bytes(collected) if collected is not None else None


def _record_policy(
    name: str, raw: bytes, policies: dict[str, dict[str, bytes]]
) -> None:
    manifest = _manifest_base(name)
    if manifest:
        base, leaf = manifest
        policies.setdefault(base, {})[leaf] = raw
    if _is_distribution_file(name):
        _check_distribution_file(name, raw)


def _walk_directory(root: Path, source: bool):
    root = root.resolve(strict=True)

    def visit(folder: Path):
        for entry in sorted(os.scandir(folder), key=lambda item: item.name.casefold()):
            path = Path(entry.path)
            if entry.is_symlink():
                raise ValueError("Symlinks are not allowed in public input: " + str(path))
            if entry.is_dir(follow_symlinks=False):
                if source and entry.name in SOURCE_PRUNE:
                    continue
                yield from visit(path)
            elif entry.is_file(follow_symlinks=False):
                yield path
            else:
                raise ValueError("Unsupported public input entry: " + str(path))

    yield from visit(root)


def check_directory(root: Path, *, source: bool = False) -> int:
    root = root.resolve(strict=True)
    policies: dict[str, dict[str, bytes]] = {}
    nested_checked = 0
    for path in _walk_directory(root, source):
        name = path.relative_to(root).as_posix()
        capture = _manifest_base(name) is not None or _is_distribution_file(name)
        with path.open("rb") as stream:
            raw = _scan_stream(name, stream, capture=capture)
        if capture:
            _record_policy(name, raw or b"", policies)
        if zipfile.is_zipfile(path):
            nested_checked += check_archive(path)
    return _check_policies(policies) + nested_checked


def _zip_symlink(entry: zipfile.ZipInfo) -> bool:
    mode = entry.external_attr >> 16
    return bool(mode and stat.S_ISLNK(mode))


def _scan_zipfile(
    archive: zipfile.ZipFile,
    policies: dict[str, dict[str, bytes]],
    *,
    prefix: str = "",
    depth: int = 0,
) -> None:
    seen: set[str] = set()
    for entry in archive.infolist():
        if entry.is_dir():
            continue
        name = normalized(prefix + entry.filename)
        folded = name.casefold()
        if folded in seen:
            raise ValueError("Duplicate archive path: " + name)
        seen.add(folded)
        if _zip_symlink(entry):
            raise ValueError("Archive symlink is not allowed: " + name)
        capture = _manifest_base(name) is not None or _is_distribution_file(name)
        if entry.file_size > NESTED_ARCHIVE_LIMIT:
            raise ValueError("Archive entry is too large to classify: " + name)
        temporary = tempfile.SpooledTemporaryFile(max_size=8 * 1024 * 1024)
        try:
            with archive.open(entry) as stream:
                raw = _scan_stream(name, stream, capture=capture, copy_to=temporary)
            if capture:
                _record_policy(name, raw or b"", policies)
            temporary.seek(0)
            if zipfile.is_zipfile(temporary):
                if depth >= 3:
                    raise ValueError("Nested archive depth is too large: " + name)
                temporary.seek(0)
                with zipfile.ZipFile(temporary) as child:
                    _scan_zipfile(
                        child, policies, prefix=name + "!/", depth=depth + 1
                    )
        finally:
            temporary.close()


def check_archive(path: Path) -> int:
    policies: dict[str, dict[str, bytes]] = {}
    with zipfile.ZipFile(path) as archive:
        _scan_zipfile(archive, policies)
    return _check_policies(policies)


def _git(repo: Path, args: list[str], *, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", *args], cwd=repo, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=check
    )


def _git_records(repo: Path, tree: str | None) -> list[tuple[str, str]]:
    if tree is None:
        raw = _git(repo, ["ls-files", "-s", "-z"]).stdout
        result = []
        for item in raw.split(b"\0"):
            if not item:
                continue
            metadata, path_raw = item.split(b"\t", 1)
            mode, object_raw, stage = metadata.split(b" ", 2)
            if stage != b"0":
                raise ValueError("Git index has unresolved entries")
            if mode == b"120000":
                raise ValueError(
                    "Tracked symlink is not allowed in public source: "
                    + path_raw.decode("utf-8", "surrogateescape")
                )
            result.append((
                object_raw.decode("ascii"),
                path_raw.decode("utf-8", "surrogateescape"),
            ))
        return result
    raw = _git(repo, ["ls-tree", "-rz", tree]).stdout
    result = []
    for item in raw.split(b"\0"):
        if not item:
            continue
        metadata, path_raw = item.split(b"\t", 1)
        mode, kind, object_raw = metadata.split(b" ", 2)
        path = path_raw.decode("utf-8", "surrogateescape")
        if mode == b"120000":
            raise ValueError("Tracked symlink is not allowed in public source: " + path)
        if kind == b"blob":
            result.append((object_raw.decode("ascii"), path))
    return result


def _git_grep(repo: Path, tree: str | None, marker: bytes, label: str) -> None:
    args = ["grep"]
    if tree is None:
        args.append("--cached")
    args.extend(["-a", "-F", "-l", "-z", "-e", marker.decode("ascii")])
    if tree is not None:
        args.append(tree)
    args.extend(["--", "."])
    result = _git(repo, args, check=False)
    if result.returncode == 0:
        path = result.stdout.split(b"\0", 1)[0].decode("utf-8", "surrogateescape")
        raise ValueError(label + ": " + path)
    if result.returncode != 1:
        raise subprocess.CalledProcessError(
            result.returncode, ["git", *args], result.stdout, result.stderr
        )


def _git_blobs(repo: Path, selectors: list[str]) -> list[bytes]:
    if not selectors:
        return []
    if any("\n" in selector or "\r" in selector for selector in selectors):
        raise ValueError("Git paths containing newlines are not supported")
    process = subprocess.run(
        ["git", "-C", str(repo), "cat-file", "--batch"],
        input=("\n".join(selectors) + "\n").encode("utf-8", "surrogateescape"),
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True,
    )
    stream = io.BytesIO(process.stdout)
    blobs = []
    for selector in selectors:
        header = stream.readline().rstrip(b"\n")
        if not header or header.endswith(b" missing"):
            raise ValueError("Missing Git policy blob: " + selector)
        _name, kind, size_raw = header.rsplit(b" ", 2)
        if kind != b"blob":
            raise ValueError("Git policy path is not a file: " + selector)
        size = int(size_raw)
        if size > POLICY_LIMIT:
            raise ValueError("Distribution manifest is too large: " + selector)
        raw = stream.read(size)
        if len(raw) != size or stream.read(1) != b"\n":
            raise ValueError("Incomplete Git policy blob: " + selector)
        blobs.append(raw)
    return blobs


def _git_cache_path(repo: Path, allow_legacy_public: bool) -> Path:
    mode = "legacy" if allow_legacy_public else "strict"
    revision = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()[:16]
    raw = _git(
        repo, ["rev-parse", "--git-path",
               f"sdgo-distribution-guard/safe-blobs-v3-{mode}-{revision}"]
    ).stdout.decode("utf-8", "surrogateescape").strip()
    path = Path(raw)
    return path if path.is_absolute() else (repo / path).resolve()


def _load_git_cache(path: Path) -> set[str]:
    try:
        lines = path.read_text(encoding="ascii").splitlines()
    except FileNotFoundError:
        return set()
    return {
        line for line in lines
        if len(line) == 40 and all(character in "0123456789abcdef" for character in line)
    }


def _write_git_cache(path: Path, values: set[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp-" + str(os.getpid()))
    try:
        temporary.write_text("".join(value + "\n" for value in sorted(values)),
                             encoding="ascii")
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def _request_git_blob(process: subprocess.Popen, object_id: str) -> int:
    assert process.stdin is not None and process.stdout is not None
    process.stdin.write((object_id + "\n").encode("ascii"))
    process.stdin.flush()
    header = process.stdout.readline().rstrip(b"\n")
    if not header or header.endswith(b" missing"):
        raise ValueError("Missing Git blob: " + object_id)
    _name, kind, size_raw = header.rsplit(b" ", 2)
    if kind != b"blob":
        raise ValueError("Git object is not a blob: " + object_id)
    return int(size_raw)


def _read_git_blob(
    process: subprocess.Popen, object_id: str, *,
    scanner: _StreamScanner | None = None, destination=None,
) -> tuple[int, bytes]:
    assert process.stdout is not None
    size = _request_git_blob(process, object_id)
    remaining = size
    tail = b""
    while remaining:
        chunk = process.stdout.read(min(1024 * 1024, remaining))
        if not chunk:
            raise ValueError("Incomplete Git blob: " + object_id)
        remaining -= len(chunk)
        if scanner is not None:
            scanner.feed(chunk)
        if destination is not None:
            destination.write(chunk)
        tail = (tail + chunk)[-ZIP_TAIL_LIMIT:]
    if scanner is not None:
        scanner.finish()
    if process.stdout.read(1) != b"\n":
        raise ValueError("Invalid Git blob boundary: " + object_id)
    return size, tail


def _close_process_pipes(process: subprocess.Popen) -> None:
    for stream in (process.stdin, process.stdout, process.stderr):
        if stream is not None and not stream.closed:
            try:
                stream.close()
            except OSError:
                pass


def _stop_git_process(process: subprocess.Popen) -> None:
    """Release Git's pipes and cwd before forcing a failed batch process."""
    _close_process_pipes(process)
    try:
        process.wait(timeout=10)
        return
    except subprocess.TimeoutExpired:
        process.terminate()
    try:
        process.wait(timeout=5)
        return
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def _audit_git_blob_contents(
    repo: Path, records: list[tuple[str, str]], *,
    allow_legacy_public: bool,
) -> int:
    display_by_object = {}
    for object_id, path in records:
        display_by_object.setdefault(object_id, path)
    cache_path = _git_cache_path(repo, allow_legacy_public)
    cached = _load_git_cache(cache_path)
    pending = [
        (object_id, display)
        for object_id, display in display_by_object.items()
        if object_id not in cached
    ]
    if not pending:
        return 0
    process = subprocess.Popen(
        ["git", "-C", str(repo), "cat-file", "--batch"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    assert process.stdin is not None and process.stdout is not None
    checked_contracts = 0
    safe = set()
    try:
        for object_id, display in pending:
            _size, tail = _read_git_blob(
                process, object_id, scanner=_StreamScanner(display)
            )
            if b"PK\x05\x06" in tail:
                temporary = tempfile.SpooledTemporaryFile(max_size=8 * 1024 * 1024)
                try:
                    size = _request_git_blob(process, object_id)
                    if size > NESTED_ARCHIVE_LIMIT:
                        raise ValueError(
                            "Git archive blob is too large to audit: " + display
                        )
                    remaining = size
                    while remaining:
                        chunk = process.stdout.read(min(1024 * 1024, remaining))
                        if not chunk:
                            raise ValueError("Incomplete Git archive blob: " + object_id)
                        remaining -= len(chunk)
                        temporary.write(chunk)
                    if process.stdout.read(1) != b"\n":
                        raise ValueError("Invalid Git archive blob boundary: " + object_id)
                    temporary.seek(0)
                    if zipfile.is_zipfile(temporary):
                        temporary.seek(0)
                        policies: dict[str, dict[str, bytes]] = {}
                        with zipfile.ZipFile(temporary) as archive:
                            _scan_zipfile(archive, policies)
                        checked_contracts += _check_policies(
                            policies,
                            allow_legacy_public=allow_legacy_public,
                        )
                finally:
                    temporary.close()
            safe.add(object_id)
        process.stdin.close()
        error = process.stderr.read() if process.stderr is not None else b""
        if process.wait() != 0:
            raise subprocess.CalledProcessError(
                process.returncode, ["git", "cat-file", "--batch"], b"", error
            )
    except BaseException:
        _stop_git_process(process)
        raise
    finally:
        _close_process_pipes(process)
    _write_git_cache(cache_path, cached | safe)
    return checked_contracts


def _git_object_types(repo: Path, object_ids: list[str]) -> dict[str, str]:
    process = subprocess.Popen(
        ["git", "-C", str(repo), "cat-file", "--batch-check"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    assert process.stdin is not None and process.stdout is not None
    result = {}
    error = b""
    try:
        for object_id in object_ids:
            process.stdin.write((object_id + "\n").encode("ascii"))
            process.stdin.flush()
            fields = process.stdout.readline().decode("ascii").strip().split()
            if len(fields) != 3 or fields[0] != object_id:
                raise ValueError("Cannot inspect outgoing Git object: " + object_id)
            result[object_id] = fields[1]
        process.stdin.close()
        error = process.stderr.read() if process.stderr is not None else b""
        if process.wait() != 0:
            raise subprocess.CalledProcessError(
                process.returncode, ["git", "cat-file", "--batch-check"], b"", error
            )
    except BaseException:
        _stop_git_process(process)
        raise
    finally:
        _close_process_pipes(process)
    return result


def _check_history_policy(
    name: str, raw: bytes, *, allow_legacy_public: bool = False
) -> None:
    manifest = _manifest_base(name)
    if manifest:
        base, leaf = manifest
        if leaf == "mod.ini":
            value = _mod_ini(raw,allow_duplicate_settings=allow_legacy_public).get("mod", "distribution", fallback=None)
        else:
            value = _manager_json(raw).get("distribution")
        if value != PUBLIC and not (allow_legacy_public and value is None):
            raise ValueError(
                base + ": outgoing history contains a non-public or undeclared contract"
            )
    if _is_distribution_file(name):
        _check_distribution_file(name, raw)


def check_git_range(
    repo: Path, revision_range: str, *, allow_legacy_public: bool = False
) -> int:
    """Scan each distinct blob newly reachable through an outgoing rev range."""
    repo = repo.resolve(strict=True)
    if not revision_range or any(c in revision_range for c in "\r\n\0"):
        raise ValueError("Invalid Git revision range")
    listed = _git(repo, ["rev-list", "--objects", revision_range, "--"]).stdout
    paths_by_object: dict[str, set[str]] = {}
    for line in listed.splitlines():
        object_raw, separator, path_raw = line.partition(b" ")
        if len(object_raw) != 40:
            raise ValueError("Unexpected Git object listing")
        object_id = object_raw.decode("ascii")
        paths_by_object.setdefault(object_id, set())
        if separator:
            paths_by_object[object_id].add(
                path_raw.decode("utf-8", "surrogateescape")
            )
    kinds = _git_object_types(repo, list(paths_by_object))
    blobs = [object_id for object_id in paths_by_object if kinds[object_id] == "blob"]
    records = [
        (object_id, sorted(paths_by_object[object_id])[0]
         if paths_by_object[object_id] else object_id)
        for object_id in blobs
    ]
    checked_policies = _audit_git_blob_contents(
        repo, records, allow_legacy_public=allow_legacy_public
    )
    policy_refs = [
        (object_id, path)
        for object_id in blobs
        for path in paths_by_object[object_id]
        if _manifest_base(path) is not None or _is_distribution_file(path)
    ]
    for (_object_id, path), raw in zip(
        policy_refs, _git_blobs(repo, [object_id for object_id, _path in policy_refs])
    ):
        _check_history_policy(
            path, raw, allow_legacy_public=allow_legacy_public
        )
        checked_policies += 1
    return checked_policies


def check_git(
    repo: Path, tree: str | None, *, allow_legacy_public: bool = False
) -> int:
    repo = repo.resolve(strict=True)
    _git_grep(
        repo, tree, NONREDISTRIBUTABLE_MARKER,
        "Non-redistributable marker found in public Git bytes",
    )
    _git_grep(
        repo, tree, LOCAL_SOURCE_MARKER,
        "Local-only source marker found in public Git bytes",
    )
    for marker in (OWNER_MARKER,OWNER_SOURCE_MARKER):
        _git_grep(repo,tree,marker,'Owner-only bytes cannot enter public Git')
    records = _git_records(repo, tree)
    checked_archives = _audit_git_blob_contents(
        repo, records, allow_legacy_public=allow_legacy_public
    )
    paths = [path for _object_id, path in records]
    selected = [
        path for path in paths
        if _manifest_base(path) is not None or _is_distribution_file(path)
    ]
    selectors = [
        (":" + path) if tree is None else (tree + ":" + path)
        for path in selected
    ]
    policies: dict[str, dict[str, bytes]] = {}
    for name, raw in zip(selected, _git_blobs(repo, selectors)):
        _record_policy(name, raw, policies)
    return checked_archives + _check_policies(
        policies, allow_legacy_public=allow_legacy_public
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--path", type=Path, help="Audit every byte in a public output directory")
    modes.add_argument(
        "--source-path", type=Path,
        help="Audit a working source tree while pruning generated/cache directories",
    )
    modes.add_argument("--archive", type=Path, help="Audit a ZIP and nested ZIP payloads")
    modes.add_argument("--git-index", action="store_true", help="Audit the staged Git index")
    modes.add_argument("--git-tree", help="Audit one exact Git commit/tree")
    modes.add_argument(
        "--git-range",
        help="Audit each distinct blob newly reachable through a revision range",
    )
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument(
        "--allow-legacy-public", action="store_true",
        help="For Git guards only, accept paired undeclared legacy public contracts",
    )
    args = parser.parse_args()
    try:
        if args.allow_legacy_public and not (
            args.git_index or args.git_tree or args.git_range
        ):
            raise ValueError("--allow-legacy-public is available only for Git guards")
        if args.path:
            checked = check_directory(args.path)
        elif args.source_path:
            checked = check_directory(args.source_path, source=True)
        elif args.archive:
            checked = check_archive(args.archive)
        elif args.git_range:
            checked = check_git_range(
                args.repo, args.git_range,
                allow_legacy_public=args.allow_legacy_public,
            )
        else:
            checked = check_git(
                args.repo, args.git_tree,
                allow_legacy_public=args.allow_legacy_public,
            )
        print(f"distribution guard passed: {checked} public Mod contracts")
        return 0
    except (
        OSError, ValueError, KeyError, UnicodeError, configparser.Error,
        json.JSONDecodeError, zipfile.BadZipFile, subprocess.CalledProcessError,
    ) as error:
        print("distribution guard: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
