"""Shared distribution declarations and non-redistributable payload checks."""
from __future__ import annotations

import configparser
import io
import json
import os
from pathlib import PurePosixPath
import stat
import tempfile
import zipfile

ALLOWED = {"public", "owner", "local_only"}
PUBLIC = "public"
OWNER = "owner"
LOCAL_ONLY = "local_only"
# Keep the complete marker out of the public Manager executable itself.
NONREDISTRIBUTABLE_MARKER = b"".join(
    (b"SDGO_LOCAL_", b"ONLY_NONREDISTRIBUTABLE")
)
LOCAL_SOURCE_MARKER = b"".join(
    (b"// SDGO_LOCAL_ONLY_", b"SOURCE_FILE")
)
OWNER_MARKER = b"".join((b"SDGO_OWNER_", b"ONLY_REDISTRIBUTABLE"))
OWNER_SOURCE_MARKER = b"".join((b"// SDGO_OWNER_", b"SOURCE_FILE"))
PUBLIC_BLOCKED_MARKERS = (NONREDISTRIBUTABLE_MARKER, OWNER_MARKER, OWNER_SOURCE_MARKER)


def _decode_ini(raw: bytes) -> str:
    if raw.startswith(b"\xff\xfe"):
        return raw[2:].decode("utf-16-le")
    if raw.startswith(b"\xfe\xff"):
        return raw[2:].decode("utf-16-be")
    if raw.startswith(b"\xef\xbb\xbf"):
        return raw[3:].decode("utf-8")
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        encoding = "mbcs" if os.name == "nt" else "cp1252"
        return raw.decode(encoding)


def mod_ini(raw: bytes) -> configparser.ConfigParser:
    value = configparser.ConfigParser(interpolation=None, strict=True)
    value.read_string(_decode_ini(raw))
    if not value.has_section("mod"):
        raise ValueError("Mod manifest has no [mod] section")
    return value


def manager_json(raw: bytes) -> dict:
    value = json.loads(raw.decode("utf-8-sig"))
    if not isinstance(value, dict):
        raise ValueError("Manager registration must be an object")
    return value


def resolve(mod_raw: bytes, manager_raw: bytes | None, *,
            allow_legacy_public: bool = False) -> str:
    manifest = mod_ini(mod_raw)
    registration = manager_json(manager_raw) if manager_raw is not None else {}
    declared = manifest.get("mod", "distribution", fallback=None)
    registered = registration.get("distribution")
    for value in (declared, registered):
        if value is not None and value not in ALLOWED:
            raise ValueError("Unknown Mod distribution: " + str(value))
    if declared is None and registered is None:
        if allow_legacy_public:
            return PUBLIC
        raise ValueError("Mod distribution must be declared in mod.ini and manager.json")
    if declared != registered:
        if allow_legacy_public and (declared, registered) in {
                (PUBLIC, None), (None, PUBLIC)}:
            return PUBLIC
        raise ValueError("Mod distribution declarations disagree or are incomplete")
    return declared


def require_public(mod_raw: bytes, manager_raw: bytes | None) -> None:
    if resolve(mod_raw, manager_raw) != PUBLIC:
        raise ValueError("Local-only Mod cannot enter a public package")


def require_redistributable(name: str, raw: bytes) -> None:
    if any(marker in raw for marker in PUBLIC_BLOCKED_MARKERS):
        raise ValueError(
            "Non-redistributable marker found in public payload: " + name
        )
    if any(line.strip() == LOCAL_SOURCE_MARKER for line in raw.splitlines()):
        raise ValueError("Local-only source found in public payload: " + name)


def _scan_public_stream(name: str, stream, *, capture: bool, copy_to=None) -> bytes | None:
    overlap = b""
    line = b""
    collected = bytearray() if capture else None
    total = 0
    while True:
        chunk = stream.read(1024 * 1024)
        if not chunk:
            break
        total += len(chunk)
        combined = overlap + chunk
        if any(marker in combined for marker in PUBLIC_BLOCKED_MARKERS):
            raise ValueError(
                "Non-redistributable marker found in public payload: " + name
            )
        overlap = combined[-(max(map(len, PUBLIC_BLOCKED_MARKERS)) - 1):]
        lines = (line + chunk).split(b"\n")
        line = lines.pop()
        if any(item.rstrip(b"\r").strip() == LOCAL_SOURCE_MARKER for item in lines):
            raise ValueError("Local-only source found in public payload: " + name)
        if collected is not None:
            if total > 2 * 1024 * 1024:
                raise ValueError("Public distribution manifest is too large: " + name)
            collected.extend(chunk)
        if copy_to is not None:
            copy_to.write(chunk)
    if line.rstrip(b"\r").strip() == LOCAL_SOURCE_MARKER:
        raise ValueError("Local-only source found in public payload: " + name)
    return bytes(collected) if collected is not None else None


def _normalized(name: str) -> str:
    value = str(PurePosixPath(name.replace("\\", "/")))
    if value.startswith("/") or any(part in {"", ".", ".."} for part in value.split("/")):
        raise ValueError("Unsafe public payload path: " + name)
    if any(part.casefold()in('owner-mods','local-mods')for part in value.split('/')):
        raise ValueError('Private programme directory cannot enter public payload: '+name)
    return value


def _require_redistributable_archive(archive: zipfile.ZipFile, *, depth: int) -> None:
    if depth > 3:
        raise ValueError("Public payload nested ZIP depth is too large")
    manifests = {}
    seen = set()
    for entry in archive.infolist():
        if entry.is_dir():
            continue
        name = _normalized(entry.filename)
        if name.casefold() in seen:
            raise ValueError("Duplicate public payload path: " + name)
        seen.add(name.casefold())
        mode = entry.external_attr >> 16
        if mode and stat.S_ISLNK(mode):
            raise ValueError("Public payload contains a symlink: " + name)
        if entry.file_size > 512 * 1024 * 1024:
            raise ValueError("Public payload entry is too large to classify: " + name)
        parts = name.split("/")
        policy_entry = (
            len(parts) >= 3 and parts[-3] in {"mods", "addons", "support"}
            and parts[-1] in {"mod.ini", "manager.json"}
        ) or (parts[-1] == "distribution.json" and len(parts) <= 2)
        temporary = tempfile.SpooledTemporaryFile(max_size=8 * 1024 * 1024)
        try:
            with archive.open(entry) as stream:
                data = _scan_public_stream(
                    name, stream, capture=policy_entry, copy_to=temporary
                )
            if (len(parts) >= 3 and parts[-3] in {"mods", "addons", "support"}
                    and parts[-1] in {"mod.ini", "manager.json"}):
                manifests.setdefault("/".join(parts[:-1]), {})[parts[-1]] = data or b""
            if parts[-1] == "distribution.json" and len(parts) <= 2:
                policy = manager_json(data or b"")
                if (policy.get("distribution") in (LOCAL_ONLY, OWNER)
                        or policy.get("redistributable") is False):
                    raise ValueError("Local-only repository cannot enter public payload")
            temporary.seek(0)
            if zipfile.is_zipfile(temporary):
                if depth >= 3:
                    raise ValueError("Public payload nested ZIP depth is too large: " + name)
                temporary.seek(0)
                with zipfile.ZipFile(temporary) as child:
                    _require_redistributable_archive(child, depth=depth + 1)
        finally:
            temporary.close()
    for base, files in manifests.items():
        mod_raw = files.get("mod.ini")
        manager_raw = files.get("manager.json")
        if mod_raw is None:
            registered = manager_json(manager_raw).get("distribution") if manager_raw else None
            if registered not in (None, PUBLIC):
                raise ValueError(base + ": Manager-only contract must declare public distribution")
        else:
            try:
                if resolve(
                    mod_raw, manager_raw, allow_legacy_public=True
                ) != PUBLIC:
                    raise ValueError("Local-only Mod cannot enter a public package")
            except (ValueError, KeyError) as error:
                raise ValueError(base + ": " + str(error)) from error


def require_redistributable_archive(raw: bytes) -> None:
    """Validate an exported ZIP before the Manager returns it to a player."""
    with zipfile.ZipFile(io.BytesIO(raw)) as archive:
        _require_redistributable_archive(archive, depth=0)


def require_redistributable_payload(name: str, raw: bytes) -> None:
    require_redistributable(name, raw)
    stream = io.BytesIO(raw)
    if zipfile.is_zipfile(stream):
        try:
            require_redistributable_archive(raw)
        except zipfile.BadZipFile as error:
            raise ValueError("Invalid ZIP payload: " + name) from error


def require_owner_payload(name: str, raw: bytes) -> None:
    """An owner package never grants permission to export local-only work."""
    if NONREDISTRIBUTABLE_MARKER in raw or LOCAL_SOURCE_MARKER in raw:
        raise ValueError('仅本地文件不能进入本服程序包：' + name)
    if OWNER_SOURCE_MARKER in raw:raise ValueError('本服源码不能进入玩家程序包：'+name)
    if name.endswith('.dll') and OWNER_MARKER not in raw:
        raise ValueError('本服 DLL 缺少明确的分发标记：' + name)
