"""Safe inspection of legacy RAR/ZIP unit packages.

The archive is treated only as data.  Members are inventoried and validated
before RAR extraction, and no file supplied by the package is executed.
"""
from __future__ import annotations

import io
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import tempfile
from typing import Any
import zipfile

from . import codec


MAX_UPLOAD = 32 * 1024 * 1024
MAX_EXPANDED = 128 * 1024 * 1024
MAX_FILES = 512
MAX_RATIO = 1000
MAX_TOOL_OUTPUT = 4 * 1024 * 1024

_PROTECTED_TOPS = {
    *codec.RESOURCE_ROOTS,
    "assets",
    "records",
    "dependencies",
    "manifest.json",
    "family.json",
    "project.json",
}
_DANGEROUS_EXTENSIONS = {
    ".apk",
    ".appx",
    ".bat",
    ".chm",
    ".cmd",
    ".com",
    ".cpl",
    ".dll",
    ".drv",
    ".exe",
    ".hta",
    ".inf",
    ".jar",
    ".js",
    ".jse",
    ".lnk",
    ".msi",
    ".msp",
    ".ps1",
    ".psd1",
    ".psm1",
    ".py",
    ".pyc",
    ".reg",
    ".scr",
    ".sh",
    ".sql",
    ".sys",
    ".url",
    ".vbe",
    ".vbs",
    ".wsf",
}
_RESOURCE_KINDS = {
    ".mod": "model",
    ".ani": "animation",
    ".txr": "texture",
    ".zfx": "effect",
    ".spr": "sprite",
    ".wav": "sound",
    ".ogg": "sound",
    ".zsd": "sound",
    ".gdt": "table",
    ".obs": "object",
    ".lua": "script-resource",
    ".scp": "script-resource",
}
_RAR_MAGIC = (b"Rar!\x1a\x07\x00", b"Rar!\x1a\x07\x01\x00")


def _archive_path(value: str) -> str:
    if not value or "\0" in value or any(ord(character) < 32 for character in value):
        raise ValueError("archive contains an empty or control-character path")
    value = value.replace("\\", "/")
    if value.startswith(("/", "//")) or re.match(r"^[a-zA-Z]:", value) or ":" in value:
        raise ValueError(f"unsafe archive path: {value!r}")
    parts = value.split("/")
    if any(part in ("", ".", "..") for part in parts):
        raise ValueError(f"unsafe archive path: {value!r}")
    if len(value) > 512 or any(len(part) > 240 for part in parts):
        raise ValueError("archive path exceeds the supported length")
    return PurePosixPath(*parts).as_posix()


def _reject_payload(path: str) -> None:
    lower = path.casefold()
    if PurePosixPath(lower).suffix in _DANGEROUS_EXTENSIONS or PurePosixPath(lower).name == "autorun.inf":
        raise ValueError(f"archive contains executable/install payload: {path}")


def _expansion_limits(entries: list[tuple[str, int, int]], upload_size: int) -> None:
    if len(entries) > MAX_FILES:
        raise ValueError(f"archive contains more than {MAX_FILES} files")
    total = sum(size for _, size, _ in entries)
    if total > MAX_EXPANDED:
        raise ValueError("archive expansion exceeds 128 MiB")
    if total > max(1024 * 1024, upload_size * MAX_RATIO):
        raise ValueError("archive has an unsafe compression ratio")
    for path, size, packed in entries:
        if size < 0 or packed < 0:
            raise ValueError(f"{path}: invalid archive size")
        if size > MAX_EXPANDED:
            raise ValueError(f"{path}: member exceeds the expansion limit")
        if size > 1024 * 1024 and packed and size > packed * MAX_RATIO:
            raise ValueError(f"{path}: unsafe member compression ratio")


def _strip_wrapper(files: list[tuple[str, bytes]]) -> dict[str, bytes]:
    split = [(path.split("/"), data) for path, data in files]
    while split and all(len(parts) > 1 for parts, _ in split):
        first = split[0][0][0]
        if first.casefold() in _PROTECTED_TOPS or any(parts[0].casefold() != first.casefold() for parts, _ in split):
            break
        split = [(parts[1:], data) for parts, data in split]
    result: dict[str, bytes] = {}
    seen: set[str] = set()
    for parts, data in split:
        path = PurePosixPath(*parts).as_posix()
        folded = path.casefold()
        if folded in seen:
            raise ValueError(f"archive has a case-insensitive duplicate path: {path}")
        seen.add(folded)
        result[path] = data
    return result


def _read_zip(raw: bytes) -> dict[str, bytes]:
    files: list[tuple[str, bytes]] = []
    inventory: list[tuple[str, int, int]] = []
    members: list[tuple[str, zipfile.ZipInfo]] = []
    raw_seen: set[str] = set()
    try:
        with zipfile.ZipFile(io.BytesIO(raw)) as archive:
            for info in archive.infolist():
                path = _archive_path(info.filename.rstrip("/"))
                folded = path.casefold()
                if folded in raw_seen:
                    raise ValueError(f"archive has a case-insensitive duplicate path: {path}")
                raw_seen.add(folded)
                unix_type = (info.external_attr >> 16) & 0o170000
                if info.is_dir():
                    if unix_type not in (0, stat.S_IFDIR):
                        raise ValueError(f"{path}: unsupported ZIP entry type")
                    continue
                if unix_type not in (0, stat.S_IFREG):
                    raise ValueError(f"{path}: links and special ZIP entries are not allowed")
                if info.flag_bits & 1:
                    raise ValueError(f"{path}: encrypted archive members are not supported")
                _reject_payload(path)
                inventory.append((path, info.file_size, info.compress_size))
                members.append((path, info))
            _expansion_limits(inventory, len(raw))
            total = 0
            for (path, info), (_, expected, _) in zip(members, inventory):
                chunks = bytearray()
                with archive.open(info, "r") as source:
                    while True:
                        block = source.read(1024 * 1024)
                        if not block:
                            break
                        chunks.extend(block)
                        total += len(block)
                        if len(chunks) > expected or total > MAX_EXPANDED:
                            raise ValueError(f"{path}: ZIP member exceeds its declared size")
                if len(chunks) != expected:
                    raise ValueError(f"{path}: ZIP member size mismatch")
                files.append((path, bytes(chunks)))
    except ValueError:
        raise
    except (zipfile.BadZipFile, RuntimeError, OSError, EOFError):
        raise ValueError("invalid or truncated ZIP archive") from None
    return _strip_wrapper(files)


def _find_7z(explicit: str | None) -> str:
    if explicit:
        candidate = Path(explicit)
        if not candidate.is_file():
            raise ValueError("configured archive executable does not exist")
        return str(candidate)
    for name in ("7z", "7zz", "7za"):
        value = shutil.which(name)
        if value:
            return value
    for environment in ("ProgramFiles", "ProgramW6432"):
        root = os.environ.get(environment)
        if root:
            candidate = Path(root) / "7-Zip" / "7z.exe"
            if candidate.is_file():
                return str(candidate)
    raise ValueError("RAR inspection needs a configured 7-Zip executable")


def _run_7z(command: list[str], *, timeout: int = 60) -> str:
    try:
        result = subprocess.run(
            command,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="strict",
            timeout=timeout,
            check=False,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
    except (OSError, subprocess.TimeoutExpired, UnicodeError):
        raise ValueError("archive tool failed while inspecting the RAR") from None
    output = result.stdout + result.stderr
    if len(output.encode("utf-8", errors="replace")) > MAX_TOOL_OUTPUT:
        raise ValueError("archive tool output exceeds the safety limit")
    if result.returncode != 0:
        raise ValueError("invalid, encrypted or unsupported RAR archive")
    return result.stdout


def _rar_inventory(output: str, upload_size: int) -> list[tuple[str, int]]:
    marker = re.search(r"(?m)^-{10,}\s*$", output)
    if marker is None:
        raise ValueError("archive tool returned no RAR inventory")
    tail = output[marker.end() :]
    records: list[dict[str, str]] = []
    for block in re.split(r"\r?\n\r?\n+", tail.strip()):
        fields: dict[str, str] = {}
        for line in block.splitlines():
            if " = " in line:
                key, value = line.split(" = ", 1)
                fields[key.strip()] = value.strip()
        if "Path" in fields:
            records.append(fields)
    entries: list[tuple[str, int]] = []
    limits: list[tuple[str, int, int]] = []
    seen: set[str] = set()
    for fields in records:
        path = _archive_path(fields["Path"])
        folded = path.casefold()
        if folded in seen:
            raise ValueError(f"archive has a case-insensitive duplicate path: {path}")
        seen.add(folded)
        if fields.get("Alternate Stream", "-") not in ("", "-"):
            raise ValueError(f"{path}: alternate data streams are not allowed")
        if any(fields.get(key, "-") not in ("", "-") for key in ("Symbolic Link", "Hard Link", "Copy Link")):
            raise ValueError(f"{path}: links are not allowed")
        if fields.get("Encrypted", "-") not in ("", "-"):
            raise ValueError(f"{path}: encrypted members are not supported")
        attributes = fields.get("Attributes", "")
        is_folder = fields.get("Folder") == "+" or "D" in attributes
        if is_folder:
            continue
        _reject_payload(path)
        try:
            size = int(fields.get("Size", "-1"))
            packed = int(fields.get("Packed Size", "0") or "0")
        except ValueError:
            raise ValueError(f"{path}: archive tool returned an invalid size") from None
        entries.append((path, size))
        limits.append((path, size, packed))
    if not entries:
        raise ValueError("archive contains no files")
    _expansion_limits(limits, upload_size)
    return entries


def _read_rar(raw: bytes, archive_executable: str | None) -> dict[str, bytes]:
    executable = _find_7z(archive_executable)
    with tempfile.TemporaryDirectory(prefix="sdgo-unit-rar-") as directory:
        root = Path(directory)
        archive_path = root / "upload.rar"
        output_root = root / "output"
        output_root.mkdir()
        archive_path.write_bytes(raw)
        listing = _run_7z([executable, "l", "-slt", "-sccUTF-8", "--", str(archive_path)])
        entries = _rar_inventory(listing, len(raw))
        _run_7z(
            [executable, "x", "-y", "-bd", "-bb0", "-sccUTF-8", f"-o{output_root}", "--", str(archive_path)],
            timeout=120,
        )
        expected = {path.casefold(): (path, size) for path, size in entries}
        actual: dict[str, Path] = {}
        inodes: set[tuple[int, int]] = set()
        for candidate in output_root.rglob("*"):
            if candidate.is_symlink() or getattr(candidate.lstat(), "st_file_attributes", 0) & getattr(
                stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400
            ):
                raise ValueError("RAR extraction produced a reparse entry")
            if candidate.is_dir():
                continue
            if not candidate.is_file():
                raise ValueError("RAR extraction produced a special entry")
            relative = candidate.relative_to(output_root).as_posix()
            folded = relative.casefold()
            if folded in actual:
                raise ValueError("RAR extraction produced duplicate paths")
            actual[folded] = candidate
            metadata = candidate.stat()
            inode = (metadata.st_dev, metadata.st_ino)
            if metadata.st_ino and inode in inodes:
                raise ValueError("RAR extraction produced hard-linked files")
            inodes.add(inode)
        if set(actual) != set(expected):
            raise ValueError("RAR extraction inventory does not match the validated listing")
        files: list[tuple[str, bytes]] = []
        total = 0
        for folded, (path, expected_size) in expected.items():
            candidate = actual[folded]
            if candidate.stat().st_size != expected_size:
                raise ValueError(f"{path}: extracted size does not match the inventory")
            data = candidate.read_bytes()
            total += len(data)
            if total > MAX_EXPANDED:
                raise ValueError("RAR expansion exceeds 128 MiB")
            files.append((path, data))
        return _strip_wrapper(files)


def _resource_virtual_path(path: str) -> str | None:
    parts = path.replace("\\", "/").split("/")
    for index, part in enumerate(parts):
        if part.casefold() not in codec.RESOURCE_ROOTS:
            continue
        candidate = "/".join(parts[index:]).lower()
        if re.fullmatch(
            r"(?:gdrs|mdrs|anrs|txrs|fxrs|sprs|sdrs|scp)/"
            r"(?:[a-z0-9][a-z0-9_.-]*/)*[a-z0-9][a-z0-9_.-]*",
            candidate,
        ):
            return candidate
    return None


def _baseline_exists(baseline: object | None, path: str) -> bool:
    return bool(baseline is not None and getattr(baseline, "exists")(path))


def _baseline_record(baseline: object | None, kind: str, identity: int) -> bytes | None:
    if baseline is None:
        return None
    return getattr(baseline, "record")(kind, identity)


def inspect_upload(
    filename: str,
    raw: bytes,
    baseline: object | None = None,
    archive_executable: str | None = None,
) -> dict[str, Any]:
    """Inspect an uploaded legacy package and return decoded authoring inputs."""
    if not isinstance(filename, str) or not filename.strip():
        raise ValueError("upload filename is required")
    if not isinstance(raw, bytes) or not raw:
        raise ValueError("upload is empty")
    if len(raw) > MAX_UPLOAD:
        raise ValueError("upload exceeds 32 MiB")
    if raw.startswith(_RAR_MAGIC):
        files = _read_rar(raw, archive_executable)
    elif zipfile.is_zipfile(io.BytesIO(raw)):
        files = _read_zip(raw)
    else:
        raise ValueError("upload must be a RAR or ZIP archive")

    from .project import read_project
    project=read_project(files)

    forms: list[dict[str, Any]] = []
    weapons: list[dict[str, Any]] = []
    blueprints: list[dict[str, Any]] = []
    resources: list[dict[str, str]] = []
    package_resources: set[str] = set()
    global_references: set[str] = set()
    identities: dict[int, str] = {}
    warnings: list[str] = []
    typed_members: set[str] = set()
    typed_resource_members: dict[str, list[dict[str, str]]] = {}
    if project is not None:
        for binding in project.get('native_content_bindings',[]):
            typed_members.update(binding['members'])
            for record in binding['records']:
                if record['id'] in identities:raise ValueError('duplicate native content identity')
                identities[record['id']]='native:'+str(record['kind'])
            global_references.update(binding['direct_resource_refs'])
            for resource in binding['resources']:
                typed_resource_members.setdefault(resource['member'],[]).append(resource)
        for binding in project.get('native_program_bindings',[]):
            typed_members.update(binding['members'])
        for binding in project.get("client_template_bindings", []):
            typed_members.update(binding["members"])
            for resource in binding["manifest"]["resources"]+binding['manifest']['resource_dependencies']:
                typed_resource_members.setdefault(resource["member"], []).append(
                    resource
                )

    for path, data in files.items():
        _reject_payload(path)
        if path in typed_members:
            for resource in typed_resource_members.get(path, []):
                virtual = resource["path"]
                if virtual in package_resources:
                    existing = next(
                        row for row in resources if row["path"] == virtual
                    )
                    if existing.get("sha256") != resource["sha256"]:
                        raise ValueError(
                            "blueprint template resource conflicts with "
                            + virtual
                        )
                    continue
                package_resources.add(virtual)
                suffix = PurePosixPath(virtual).suffix.lower()
                resources.append(
                    {
                        "path": virtual,
                        "kind": _RESOURCE_KINDS.get(suffix, "resource"),
                        "member": path,
                        "sha256": resource["sha256"],
                    }
                )
                global_references.update(
                    codec.scan_resource_references(data, suffix)
                )
            continue
        suffix = PurePosixPath(path).suffix.lower()
        if suffix == ".bin":
            decoded = codec.decode_record(data)
            identity = decoded["id"]
            if identity in identities:
                raise ValueError(
                    f"duplicate native id {identity} across {identities[identity]} and {decoded['kind']} records"
                )
            identities[identity] = decoded["kind"]
            if decoded["kind"] == "unit":
                forms.append({key: value for key, value in decoded.items() if key != "kind"})
            elif decoded['kind']=='blueprint':
                blueprints.append(decoded)
            else:
                weapons.append({key: value for key, value in decoded.items() if key != "kind"})
            global_references.update(decoded["resource_refs"])
            continue
        virtual = _resource_virtual_path(path)
        if virtual is not None:
            if virtual in package_resources:
                raise ValueError(f"multiple archive members map to the same game resource: {virtual}")
            package_resources.add(virtual)
            resources.append({"path": virtual, "kind": _RESOURCE_KINDS.get(suffix, "resource")})
            global_references.update(codec.scan_resource_references(data, suffix))
        elif suffix not in (".json", ".txt", ".md"):
            warnings.append(f"未分类文件不会作为游戏资源发布：{path}")

    if not forms and project is None:
        raise ValueError("archive contains no 4976-byte unit record")
    form_ids = {form["id"] for form in forms}
    weapon_ids = {weapon["id"] for weapon in weapons}
    for form in forms:
        model = form["model"]
        if not model:
            raise ValueError(f"unit {form['id']} has no model path")
        if model not in package_resources and not _baseline_exists(baseline, model):
            raise ValueError(f"unit {form['id']} model is missing: {model}")
        override=form.get('model_override')
        if override and override not in package_resources and not _baseline_exists(baseline,override):
            raise ValueError(f"unit {form['id']} model override @738 is missing: {override}")
        target = form["transform_target"]
        if target and target not in form_ids and _baseline_record(baseline, "unit", target) is None:
            raise ValueError(f"unit {form['id']} transform target is missing: {target}")

    external: set[str] = set()
    for form in forms:
        for identity in form["weapons"]:
            if identity and identity not in weapon_ids and _baseline_record(baseline, "weapon", identity) is None:
                external.add(f"weapon:{identity}")
    for reference in global_references:
        if reference not in package_resources and not _baseline_exists(baseline, reference):
            external.add(reference)

    warnings.append(
        "资源字符串扫描只覆盖可见的 ASCII/UTF-16LE 直接引用；它不是完整语义解析或递归依赖闭包证明。"
    )
    result = {
        "files": files,
        "forms": sorted(forms, key=lambda value: value["id"]),
        "weapons": sorted(weapons, key=lambda value: value["id"]),
        "blueprints": sorted(blueprints,key=lambda value:value['id']),
        "resources": sorted(resources, key=lambda value: value["path"]),
        "external_refs": sorted(external),
        "warnings": warnings,
    }
    if project is not None:result['project']=project
    return result
