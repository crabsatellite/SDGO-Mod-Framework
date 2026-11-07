"""Fail-closed planning and installation for the operator Battle runtime.

The helper is deliberately independent from process control and database
mutation.  A plan reads the native startup/channel configuration, validates a
staged framework payload, and records the exact bytes that an install would
write.  Applying that plan only touches the two registered Mods, the compiled
operator effects file, and the required host symbol sections.
"""
from __future__ import annotations

import configparser
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import uuid
import xml.etree.ElementTree as ET

from ... import database, storage
from . import capabilities, model


HOST_KEY = "BattleServer-548A891D-0046C000"
SUPPORTED_BATTLE_HOSTS = {
    HOST_KEY:
        "f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4",
}
NATIVE_PROCEDURE_FINGERPRINTS = {
    "sp_getoperatorskill":
        "d0f78f9db698799e1050cc181046122a2c9e537e3df65e4051fe3017f06db3a5",
    "sp_getoperatoraddon":
        "7a8f905eb791f4f6cb57e68ed8512e66547ae9486cf99d14c98f64b2ffa5cf26",
}
MODULE_STATUS_VERSION = 1
_MODULES = ("operator_catalog_server", "battle_rewards")
_SECTION = re.compile(r"(?m)^[ \t]*\[([^\]\r\n]+)\][ \t]*\r?$")
_SAFE_PAYLOAD = re.compile(r"[A-Za-z0-9_.-]+(?:[\\/][A-Za-z0-9_.-]+)*\Z")
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")


class RuntimePlan(dict):
    """JSON-safe plan with non-serializable install state kept out of mapping."""

    def __init__(self, public, *, writes=None, state=None):
        super().__init__(public)
        self._writes = writes or {}
        self._state = state or {}


_EVIDENCE_TOKEN = object()
_TRUST_TOKEN = object()
TRUSTED_NATIVE_CAPABILITIES = frozenset({
    "custom_skill_ids", "custom_time_ids", "pilot_exp_percent",
})


class InstalledRuntimeEvidence:
    """Opaque evidence derived from an exact RuntimePlan readback."""

    __slots__ = ("_token", "_revision", "_hosts")

    def __init__(self, token, revision, hosts):
        if token is not _EVIDENCE_TOKEN:
            raise TypeError("runtime evidence can only come from a RuntimePlan")
        self._token = token
        self._revision = revision
        self._hosts = tuple(hosts)

    def public(self):
        return {
            "catalog_revision": self._revision,
            "hosts": [
                {
                    "folder": host["folder"],
                    "host": host["host_key"],
                    "runtime_files": len(host["runtime_files"]),
                }
                for host in self._hosts
            ],
        }


class TrustedRuntimeContract:
    """Opaque capability grant verified against files, host and live SQL text."""

    __slots__ = ("_token", "_capabilities", "evidence", "revision")

    def __init__(self, token, capabilities, evidence, revision):
        if token is not _TRUST_TOKEN:
            raise TypeError("trusted runtime contracts are verifier-owned")
        self._token = token
        self._capabilities = frozenset(capabilities)
        self.evidence = evidence
        self.revision = revision


def trusted_capabilities(contract):
    if (
        not isinstance(contract, TrustedRuntimeContract)
        or contract._token is not _TRUST_TOKEN
    ):
        raise ValueError("native capabilities require trusted runtime evidence")
    return contract._capabilities


def _sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def _sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _optional_hash(path):
    path = Path(path)
    return _sha256_file(path) if path.is_file() else None


def _is_reparse(path):
    path = Path(path)
    try:
        stat = path.lstat()
    except FileNotFoundError:
        return False
    return path.is_symlink() or bool(
        getattr(stat, "st_file_attributes", 0) & 0x400
    )


def _bounded_file(path, maximum, label):
    path = Path(path)
    if not path.is_file():
        raise ValueError(f"{label} is missing: {path}")
    if _is_reparse(path):
        raise ValueError(f"{label} cannot be a reparse point: {path}")
    size = path.stat().st_size
    if size > maximum:
        raise ValueError(f"{label} exceeds the supported size")
    return path.read_bytes()


def _assert_contained(root, path, *, must_exist=True, label="path"):
    root = Path(root)
    path = Path(path)
    root_resolved = root.resolve(strict=True)
    if _is_reparse(root):
        raise ValueError(f"{label} root cannot be a reparse point: {root}")
    if must_exist:
        resolved = path.resolve(strict=True)
    else:
        existing = path
        while not existing.exists() and existing != existing.parent:
            existing = existing.parent
        if not existing.exists():
            raise ValueError(f"{label} has no existing parent: {path}")
        resolved_existing = existing.resolve(strict=True)
        resolved = resolved_existing.joinpath(
            *path.relative_to(existing).parts
        )
    if not resolved.is_relative_to(root_resolved):
        raise ValueError(f"{label} escapes its root: {path}")
    relative = path.relative_to(root)
    current = root
    for part in relative.parts:
        current /= part
        if current.exists() and _is_reparse(current):
            raise ValueError(f"{label} crosses a reparse point: {current}")
    return resolved


def _relative_process_path(value):
    if not isinstance(value, str) or not value or len(value) > 1024:
        raise ValueError("startup Process Path is invalid")
    normalized = value.replace("\\", "/")
    if (
        normalized.startswith(("/", "//"))
        or re.match(r"^[A-Za-z]:", normalized)
        or "\0" in normalized
    ):
        raise ValueError("startup Process Path must be relative")
    parts = tuple(part for part in normalized.split("/") if part not in ("", "."))
    if not parts or any(
        part == ".." or part.endswith((" ", ".")) or ":" in part
        for part in parts
    ):
        raise ValueError("startup Process Path is unsafe")
    return Path(*parts)


def _pe_metadata(path, *, require_x86=False):
    data = _bounded_file(path, 128 * 1024 * 1024, "PE image")
    if len(data) < 0x100 or data[:2] != b"MZ":
        raise ValueError(f"unsupported executable (missing MZ): {path}")
    nt = struct.unpack_from("<I", data, 0x3C)[0]
    if nt > len(data) - 0x60 or data[nt:nt + 4] != b"PE\0\0":
        raise ValueError(f"unsupported executable (invalid PE): {path}")
    machine = struct.unpack_from("<H", data, nt + 4)[0]
    timestamp = struct.unpack_from("<I", data, nt + 8)[0]
    optional_size = struct.unpack_from("<H", data, nt + 20)[0]
    if optional_size < 0x3C or nt + 24 + optional_size > len(data):
        raise ValueError(f"unsupported executable (optional header): {path}")
    magic = struct.unpack_from("<H", data, nt + 24)[0]
    image_size = struct.unpack_from("<I", data, nt + 24 + 0x38)[0]
    if timestamp == 0 or image_size == 0 or magic not in (0x10B, 0x20B):
        raise ValueError(f"unsupported executable metadata: {path}")
    if require_x86 and (machine != 0x14C or magic != 0x10B):
        raise ValueError(f"runtime DLL is not x86 PE32: {path}")
    return {
        "machine": machine,
        "timestamp": timestamp,
        "size_of_image": image_size,
        "key": f"{Path(path).stem}-{timestamp:08X}-{image_size:08X}",
        "sha256": _sha256_bytes(data),
        "bytes": len(data),
    }


def _verify_battle_host(path):
    metadata = _pe_metadata(path, require_x86=True)
    expected = SUPPORTED_BATTLE_HOSTS.get(metadata["key"])
    if expected is None or metadata["sha256"] != expected:
        raise ValueError(
            "unsupported BattleServer executable: "
            f"{metadata['key']} sha256={metadata['sha256']}"
        )
    return metadata


def _listen_port(host_root):
    raw = _bounded_file(
        Path(host_root) / "sip.txt", 64 * 1024, "BattleServer sip.txt"
    )
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        text = raw.decode("cp936")
    matches = re.findall(
        r"(?mi)^[ \t]*LISTEN[ \t]+[^\s:]+:(\d{1,5})(?:[ \t]+|$)",
        text,
    )
    if len(matches) != 1:
        raise ValueError("BattleServer sip.txt must contain one LISTEN endpoint")
    port = int(matches[0])
    if not 1 <= port <= 65535:
        raise ValueError("BattleServer LISTEN port is out of range")
    return port


def _load_startup_hosts(server_root):
    server_root = Path(server_root)
    startup = server_root / "ServerStartUp" / "ServerStartUp.xml"
    _assert_contained(
        server_root, startup, label="ServerStartUp.xml"
    )
    raw = _bounded_file(startup, 2 * 1024 * 1024, "ServerStartUp.xml")
    try:
        xml = ET.fromstring(raw)
    except (ET.ParseError, ValueError) as error:
        raise ValueError(f"ServerStartUp.xml is invalid: {error}") from None
    hosts = []
    seen = set()
    for row in xml.findall(".//Process"):
        if row.attrib.get("Run", "").strip().casefold() != "true":
            continue
        value = row.attrib.get("Path", "")
        relative = _relative_process_path(value)
        folder = relative.parent.name
        is_battle_folder = bool(
            re.fullmatch(r"BattleServer[0-9]+", folder, re.IGNORECASE)
        )
        is_battle_exe = relative.name.casefold() == "battleserver.exe"
        if not is_battle_folder and not is_battle_exe:
            continue
        if not is_battle_folder or not is_battle_exe:
            raise ValueError(
                "enabled Battle startup entry has an unsupported executable: "
                + value
            )
        executable = server_root / relative
        _assert_contained(
            server_root, executable, label="startup BattleServer executable"
        )
        identity = os.path.normcase(str(executable.resolve(strict=True)))
        if identity in seen:
            raise ValueError("ServerStartUp.xml contains a duplicate Battle host")
        seen.add(identity)
        host_root = executable.parent
        hosts.append({
            "folder": folder,
            "root": host_root,
            "executable": executable,
            "port": _listen_port(host_root),
        })
    return startup, hosts


def _load_channel_hosts(server_ini, connect_fn):
    connection = None
    try:
        connection = connect_fn(Path(server_ini))
        rows = connection.cursor().execute(
            "SELECT AddrPT,ServerID FROM dbo.tbl_ChannelInfo ORDER BY AddrPT"
        ).fetchall()
        channels = []
        for row in rows:
            port = int(row[0])
            server_id = int(row[1])
            if not 1 <= port <= 65535:
                raise ValueError("tbl_ChannelInfo contains an invalid AddrPT")
            channels.append({"port": port, "server_id": server_id})
        return channels
    finally:
        if connection is not None:
            try:
                connection.rollback()
            except Exception:
                pass
            try:
                connection.close()
            except Exception:
                pass


def _strict_ini(raw, label):
    parser = configparser.ConfigParser(interpolation=None, strict=True)
    parser.optionxform = str
    try:
        text, _, _ = storage.decode_ini(raw)
        parser.read_string(text)
    except (UnicodeError, configparser.Error) as error:
        raise ValueError(f"{label} is not a strict INI: {error}") from None
    return parser


def _payload_names(parser, module):
    try:
        section = parser["mod"]
    except KeyError:
        raise ValueError(f"{module} mod.ini lacks [mod]") from None
    if (
        section.get("id") != module
        or section.get("dll", f"{module}.dll") != f"{module}.dll"
    ):
        raise ValueError(f"{module} package manifest identity is invalid")
    hosts = {
        value.strip()
        for value in section.get("hosts", "").split(",")
        if value.strip()
    }
    if HOST_KEY not in hosts:
        raise ValueError(f"{module} package does not support {HOST_KEY}")

    def values(key):
        result = []
        for value in section.get(key, "").split(","):
            value = value.strip()
            if not value:
                continue
            if (
                not _SAFE_PAYLOAD.fullmatch(value)
                or Path(value).is_absolute()
                or any(part in ("", ".", "..") for part in Path(value).parts)
            ):
                raise ValueError(f"{module} manifest has an unsafe {key} path")
            result.append(value.replace("\\", "/"))
        if len(result) != len(set(result)):
            raise ValueError(f"{module} manifest repeats a {key} path")
        return result

    required = tuple(
        value.strip()
        for value in section.get("requires", "").split(",")
        if value.strip()
    )
    if not required or len(required) != len(set(required)):
        raise ValueError(f"{module} manifest native requirements are invalid")
    files = values("files")
    runtime_files = values("runtime_files")
    configs = set(values("config_files"))
    reserved = {"mod.ini", f"{module}.dll"}
    if (
        set(files) & set(runtime_files)
        or configs & (set(files) | set(runtime_files))
        or reserved & (set(files) | set(runtime_files) | configs)
    ):
        raise ValueError(f"{module} manifest payload/config paths overlap")
    payloads = [f"{module}.dll", "mod.ini", *files, *runtime_files]
    if "manager.json" not in payloads:
        payloads.append("manager.json")
    return {
        "required": required,
        "payloads": tuple(dict.fromkeys(payloads)),
        "configs": configs,
    }


def _ini_sections(text, label):
    values = {}
    blocks = {}
    matches = list(_SECTION.finditer(text))
    for index, match in enumerate(matches):
        name = match.group(1).strip()
        canonical = name.casefold()
        if not name or canonical in values:
            raise ValueError(f"{label} repeats symbol section [{name}]")
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        block = text[match.start():end].rstrip()
        section = {}
        for raw in block.splitlines()[1:]:
            line = raw.strip()
            if not line or line.startswith((";", "#")):
                continue
            if "=" not in line:
                raise ValueError(f"{label} has an invalid row in [{name}]")
            key, value = (item.strip() for item in line.split("=", 1))
            folded = key.casefold()
            if not key or folded in section:
                raise ValueError(f"{label} repeats key {key!r} in [{name}]")
            section[folded] = value
        values[canonical] = section
        blocks[canonical] = block
    if not values:
        raise ValueError(f"{label} has no symbol sections")
    return values, blocks


def _merge_symbols(existing, incoming, required):
    package, blocks = _ini_sections(incoming, "package symbol file")
    selected = ("host", *required)
    missing = [name for name in selected if name.casefold() not in package]
    if missing:
        raise ValueError(
            "package symbol file lacks required sections: " + ", ".join(missing)
        )
    if existing is None:
        return (
            "\n\n".join(blocks[name.casefold()] for name in selected).rstrip()
            + "\n"
        )
    installed, _ = _ini_sections(existing, "installed symbol file")
    additions = []
    for name in selected:
        canonical = name.casefold()
        if canonical in installed:
            if installed[canonical] != package[canonical]:
                raise ValueError(
                    f"installed symbol [{name}] conflicts with the package"
                )
        else:
            additions.append(blocks[canonical])
    if not additions:
        return existing if existing.endswith("\n") else existing + "\n"
    return (
        existing.rstrip()
        + "\n\n; Added by the operator runtime installer.\n"
        + "\n\n".join(additions)
        + "\n"
    )


def _preserve_manifest(incoming, existing):
    if existing is None:
        return incoming
    old = _strict_ini(existing, "installed mod.ini")
    changes = []
    if old.has_section("settings"):
        changes.extend(
            ("settings", key, value)
            for key, value in old.items("settings")
        )
    if old.has_section("mod") and old.has_option("mod", "enabled"):
        changes.append(("mod", "enabled", old.get("mod", "enabled")))
    return storage.patch_ini(incoming, changes)


def _manifest_semantics(raw, label):
    """Canonical strict INI values while ignoring representation-only bytes."""
    parser = _strict_ini(raw, label)
    result = {}

    def values(items, section):
        canonical = {}
        for key, value in items:
            folded = key.casefold()
            if folded in canonical:
                raise ValueError(
                    f"{label} repeats key {key!r} in [{section}]"
                )
            canonical[folded] = value
        return canonical

    defaults = values(parser.defaults().items(), "DEFAULT")
    if defaults:
        result["default"] = defaults
    for section in parser.sections():
        folded = section.casefold()
        if folded in result:
            raise ValueError(f"{label} repeats section [{section}]")
        explicit = parser._sections[section]
        result[folded] = values(
            (
                (key, value)
                for key, value in explicit.items()
                if key != "__name__"
            ),
            section,
        )
    return result


def _runtime_file_digest(path, expected):
    """Return the installed digest after exact or manifest-semantic readback."""
    path = Path(path)
    actual = _bounded_file(
        path, 128 * 1024 * 1024, "operator runtime installed file"
    )
    if path.name.casefold() == "mod.ini":
        if _manifest_semantics(
            actual, "installed runtime mod.ini"
        ) != _manifest_semantics(
            expected, "expected runtime mod.ini"
        ):
            raise ValueError(
                f"operator runtime manifest values do not match: {path}"
            )
    elif actual != expected:
        raise ValueError(f"operator runtime file does not match: {path}")
    return _sha256_bytes(actual)


def _process_snapshot():
    if os.name != "nt":
        return []
    script = r"""
$ErrorActionPreference='Stop'
$rows=@()
Get-CimInstance Win32_Process -Filter "Name='BattleServer.exe'" | ForEach-Object {
  $created=$null
  $denied=$false
  try {
    $created=(Get-Process -Id $_.ProcessId -ErrorAction Stop).StartTime.ToUniversalTime().ToString('o')
  } catch {
    $denied=$true
  }
  $rows += [pscustomobject]@{
    pid=$_.ProcessId
    name=$_.Name
    path=$_.ExecutablePath
    creation_time=$created
    probe_error=$denied
  }
}
$rows | ConvertTo-Json -Compress
"""
    completed = subprocess.run(
        ["powershell", "-NoProfile", "-Command", script],
        check=False,
        capture_output=True,
        text=True,
        timeout=20,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    if completed.returncode:
        return [{
            "pid": None, "name": "BattleServer.exe", "path": None,
            "creation_time": None, "probe_error": True,
        }]
    text = completed.stdout.strip()
    if not text:
        return []
    try:
        value = json.loads(text)
    except json.JSONDecodeError:
        return [{
            "pid": None, "name": "BattleServer.exe", "path": None,
            "creation_time": None, "probe_error": True,
        }]
    return value if isinstance(value, list) else [value]


def _path_identity(value):
    try:
        return os.path.normcase(str(Path(value).resolve(strict=False)))
    except (OSError, TypeError, ValueError):
        return None


def _running_reasons(hosts, rows):
    targets = {
        _path_identity(host["executable"]): host["folder"] for host in hosts
    }
    reasons = []
    for row in rows:
        if not isinstance(row, dict):
            reasons.append("BattleServer process inventory returned an invalid row")
            continue
        path = row.get("path")
        if not path:
            reasons.append(
                "a BattleServer process has an unreadable executable path; "
                "installation is pending"
            )
            continue
        identity = _path_identity(path)
        if identity in targets:
            reasons.append(
                f"{targets[identity]} is running (PID {row.get('pid')}); "
                "installation requires the host to be stopped"
            )
    return reasons


def _package_server_root(package_root):
    package_root = Path(package_root)
    direct = package_root / "server"
    if (direct / "sdgo").is_dir():
        return direct
    if (package_root / "sdgo").is_dir():
        return package_root
    raise ValueError("framework package root lacks the staged server tree")


def compile_effects_ini(catalog):
    """Compile catalog-only effect configuration for every operator profile."""
    model.validate(catalog)
    operators = list(catalog["operators"])
    if len(operators) > 256:
        raise ValueError("operator effects supports at most 256 profiles")
    revision = model.revision(catalog)
    lines = ["[effects]", "Version=1", f"Revision={revision}", ""]
    registry = catalog.get("registered_ability_extensions", ())
    for operator in sorted(operators, key=lambda row: row["id"]):
        pilot_exp = 0
        extensions = {}
        if operator["managed"] and operator["enabled"]:
            ability = capabilities.normalize(operator.get("abilities"), registry)
            if ability is not None and ability["mode"] == "custom":
                pilot_exp = ability["pilot_exp_percent"]
                extensions = ability["extensions"]
        encoded = json.dumps(
            extensions,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
            allow_nan=False,
        ).encode("utf-8").hex().upper()
        lines.extend([
            f"[operator:{operator['id']}]",
            f"PilotExpPercent={pilot_exp}",
            f"ExtensionsHex={encoded}",
            "",
        ])
    return ("\n".join(lines).rstrip() + "\n").encode("ascii")


def _plan_package(package_root, catalog):
    package_input = Path(package_root)
    if _is_reparse(package_input):
        raise ValueError("framework package root cannot be a reparse point")
    package_root = package_input.resolve(strict=True)
    server = _package_server_root(package_root)
    _assert_contained(
        package_root, server, label="framework package server tree"
    )
    modules = {}
    for module in _MODULES:
        root = server / "sdgo" / "mods" / module
        _assert_contained(
            package_root, root, label=f"{module} package directory"
        )
        manifest_path = root / "mod.ini"
        manifest_raw = _bounded_file(
            manifest_path, 2 * 1024 * 1024, f"{module} mod.ini"
        )
        specification = _payload_names(
            _strict_ini(manifest_raw, f"{module} mod.ini"), module
        )
        payload = {}
        for name in specification["payloads"]:
            path = root / name
            if name == "manager.json" and not path.is_file():
                continue
            _assert_contained(
                root, path, label=f"{module} package payload"
            )
            payload[name] = _bounded_file(
                path, 64 * 1024 * 1024, f"{module}/{name}"
            )
        modules[module] = {
            "root": root,
            "manifest": manifest_raw,
            "specification": specification,
            "payload": payload,
        }
    symbol_path = server / "sdgo" / "symbols" / f"{HOST_KEY}.ini"
    _assert_contained(
        package_root, symbol_path, label="package BattleServer symbols"
    )
    symbol_raw = _bounded_file(
        symbol_path, 4 * 1024 * 1024, "package BattleServer symbols"
    )
    try:
        symbol_text = symbol_raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        raise ValueError("package BattleServer symbols are not UTF-8") from None
    effects = compile_effects_ini(catalog)
    return {
        "root": package_root,
        "server": server,
        "modules": modules,
        "symbol_path": symbol_path,
        "symbol_text": symbol_text,
        "effects": effects,
        "revision": model.revision(catalog),
    }


def plan_runtime_install(
    server_root, package_root, catalog, server_ini,
    *, connect_fn=None, process_snapshot_fn=None,
):
    """Return a deterministic, read-only install plan for enabled Battle hosts."""
    blockers = []
    skipped = []
    selected = []
    writes = {}
    state = {}
    server_root_value = str(Path(server_root))
    package_root_value = str(Path(package_root))
    server_ini_value = str(Path(server_ini))
    connect = connect_fn or database.connect
    process_probe = process_snapshot_fn or _process_snapshot
    revision = None
    channels = []
    startup_path = None
    snapshot_rows = []
    try:
        root_input = Path(server_root)
        if _is_reparse(root_input):
            raise ValueError("server root cannot be a reparse point")
        root = root_input.resolve(strict=True)
        ini = Path(server_ini).resolve(strict=True)
        _bounded_file(ini, 2 * 1024 * 1024, "server INI")
        startup_path, startup_hosts = _load_startup_hosts(root)
        channels = _load_channel_hosts(ini, connect)
        package = _plan_package(package_root, catalog)
        revision = package["revision"]
        enabled_ports = {row["port"] for row in channels}
        duplicates = {
            port for port in {host["port"] for host in startup_hosts}
            if sum(host["port"] == port for host in startup_hosts) > 1
        }
        if duplicates:
            raise ValueError(
                "enabled Battle startup hosts repeat LISTEN ports: "
                + ", ".join(map(str, sorted(duplicates)))
            )
        for host in startup_hosts:
            if host["port"] not in enabled_ports:
                skipped.append({
                    "folder": host["folder"],
                    "path": str(host["root"]),
                    "port": host["port"],
                    "reason": "LISTEN port is absent from tbl_ChannelInfo.AddrPT",
                })
                continue
            try:
                metadata = _verify_battle_host(host["executable"])
                missing_framework = [
                    str(path.relative_to(host["root"]).as_posix())
                    for path in (
                        host["root"] / "winmm.dll",
                        host["root"] / "sdgo" / "core.dll",
                    )
                    if not path.is_file()
                ]
                if missing_framework:
                    raise ValueError(
                        "install the framework first; missing "
                        + ", ".join(missing_framework)
                    )
                for framework_path in (
                    host["root"] / "winmm.dll",
                    host["root"] / "sdgo" / "core.dll",
                ):
                    _assert_contained(
                        host["root"], framework_path,
                        label="installed framework prerequisite",
                    )
                    _bounded_file(
                        framework_path, 64 * 1024 * 1024,
                        "installed framework prerequisite",
                    )
            except (OSError, ValueError) as error:
                blockers.append(f"{host['folder']}: {error}")
                continue
            host.update(metadata=metadata)
            selected.append(host)
        if not selected:
            blockers.append(
                "no Battle host is enabled by both ServerStartUp.xml and "
                "tbl_ChannelInfo.AddrPT"
            )
        try:
            snapshot_rows = process_probe()
        except Exception:
            snapshot_rows = [{
                "pid": None, "name": "BattleServer.exe", "path": None,
                "probe_error": True,
            }]
        blockers.extend(_running_reasons(selected, snapshot_rows))

        requirements = tuple(dict.fromkeys(
            requirement
            for module in _MODULES
            for requirement in package["modules"][module]["specification"]["required"]
        ))
        for host in selected:
            host_root = host["root"]
            symbol_target = (
                host_root / "sdgo" / "symbols" / f"{HOST_KEY}.ini"
            )
            _assert_contained(
                host_root, symbol_target, must_exist=symbol_target.exists(),
                label="installed BattleServer symbols",
            )
            existing_symbols = None
            if symbol_target.is_file():
                existing_symbols = _bounded_file(
                    symbol_target, 4 * 1024 * 1024,
                    "installed BattleServer symbols",
                ).decode("utf-8-sig")
            merged = _merge_symbols(
                existing_symbols, package["symbol_text"], requirements
            ).encode("utf-8")
            writes[symbol_target] = merged

            for module in _MODULES:
                source = package["modules"][module]
                target_root = host_root / "sdgo" / "mods" / module
                _assert_contained(
                    host_root, target_root, must_exist=target_root.exists(),
                    label=f"installed {module} directory",
                )
                for name, raw in source["payload"].items():
                    target = target_root / name
                    _assert_contained(
                        host_root, target, must_exist=target.exists(),
                        label=f"installed {module} payload",
                    )
                    if name == "mod.ini":
                        existing = target.read_bytes() if target.is_file() else None
                        raw = _preserve_manifest(raw, existing)
                    writes[target] = raw
            effects_target = (
                host_root / "sdgo" / "mods"
                / "operator_catalog_server" / "effects.ini"
            )
            _assert_contained(
                host_root, effects_target,
                must_exist=effects_target.exists(),
                label="compiled operator effects",
            )
            writes[effects_target] = package["effects"]

        expected_files = dict(writes)
        pending_writes = {}
        for path, raw in writes.items():
            if not path.is_file():
                pending_writes[path] = raw
                continue
            try:
                _runtime_file_digest(path, raw)
            except ValueError:
                pending_writes[path] = raw
        writes = pending_writes
        actions = []
        for path, raw in sorted(writes.items(), key=lambda item: str(item[0]).casefold()):
            host = next(
                host for host in selected
                if path.is_relative_to(host["root"])
            )
            actions.append({
                "host": host["folder"],
                "path": str(path.relative_to(host["root"]).as_posix()),
                "sha256": _sha256_bytes(raw),
                "bytes": len(raw),
                "operation": "update" if path.exists() else "create",
            })
        state = {
            "server_root": root,
            "server_ini": ini,
            "server_ini_hash": _sha256_file(ini),
            "startup_hash": _sha256_file(startup_path),
            "package": package,
            "expected_files": expected_files,
            "selected": selected,
            "skipped": skipped,
            "channels": channels,
            "connect_fn": connect,
            "process_snapshot_fn": process_probe,
            "source_hashes": {
                path: _sha256_file(path)
                for module in package["modules"].values()
                for path in (
                    module["root"] / name
                    for name in module["payload"]
                )
            } | {package["symbol_path"]: _sha256_file(package["symbol_path"])},
            "target_hashes": {path: _optional_hash(path) for path in writes},
            "host_hashes": {
                host["executable"]: host["metadata"]["sha256"]
                for host in selected
            } | {
                host["root"] / "winmm.dll":
                    _sha256_file(host["root"] / "winmm.dll")
                for host in selected
            } | {
                host["root"] / "sdgo" / "core.dll":
                    _sha256_file(host["root"] / "sdgo" / "core.dll")
                for host in selected
            },
        }
    except (OSError, ValueError) as error:
        blockers.append(str(error))
        actions = []
        writes = {}
        state = {}

    public_hosts = []
    for host in selected:
        status = read_module_status(
            host["root"], MODULE_STATUS_VERSION, revision,
            process_snapshot_fn=lambda: snapshot_rows,
        )
        public_hosts.append({
            "folder": host["folder"],
            "path": str(host["root"]),
            "port": host["port"],
            "host": host["metadata"]["key"],
            "sha256": host["metadata"]["sha256"],
            "framework_present": True,
            "module_status": status,
        })
    public = {
        "schema_version": 1,
        "mode": "plan",
        "ready": not blockers,
        "server_root": server_root_value,
        "package_root": package_root_value,
        "server_ini": server_ini_value,
        "startup_xml": str(startup_path) if startup_path else None,
        "catalog_revision": revision,
        "database_channels": channels,
        "selected_hosts": public_hosts,
        "skipped_hosts": skipped,
        "actions": actions,
        "blockers": list(dict.fromkeys(blockers)),
        "writes_performed": False,
    }
    return RuntimePlan(public, writes=writes, state=state)


def _same_channel_selection(plan):
    state = plan._state
    startup, hosts = _load_startup_hosts(state["server_root"])
    channels = _load_channel_hosts(
        state["server_ini"], state["connect_fn"]
    )
    ports = {row["port"] for row in channels}
    current = sorted(
        (
            host["folder"],
            str(host["executable"].resolve(strict=True)),
            host["port"],
        )
        for host in hosts if host["port"] in ports
    )
    expected = sorted(
        (
            host["folder"],
            str(host["executable"].resolve(strict=True)),
            host["port"],
        )
        for host in state["selected"]
    )
    return (
        _sha256_file(startup) == state["startup_hash"]
        and channels == state["channels"]
        and current == expected
    )


def _revalidate_plan(plan, process_snapshot_fn):
    if not isinstance(plan, RuntimePlan) or not plan.get("ready"):
        raise ValueError("runtime install requires a successful RuntimePlan")
    state = plan._state
    if _sha256_file(state["server_ini"]) != state["server_ini_hash"]:
        raise ValueError("server INI changed after planning")
    if not _same_channel_selection(plan):
        raise ValueError("enabled Battle host selection changed after planning")
    try:
        rows = (process_snapshot_fn or state["process_snapshot_fn"])()
    except Exception:
        raise ValueError(
            "BattleServer process inventory is unavailable; installation is pending"
        ) from None
    reasons = _running_reasons(state["selected"], rows)
    if reasons:
        raise ValueError("; ".join(reasons))
    for path, expected in state["source_hashes"].items():
        _assert_contained(
            state["package"]["root"], path,
            label="framework package source",
        )
        if _sha256_file(path) != expected:
            raise ValueError(f"package source changed after planning: {path}")
    for path, expected in state["host_hashes"].items():
        host = next(
            host for host in state["selected"]
            if path.is_relative_to(host["root"])
        )
        _assert_contained(
            host["root"], path, label="host/framework prerequisite"
        )
        if _sha256_file(path) != expected:
            raise ValueError(f"host/framework changed after planning: {path}")
    for path, expected in state["target_hashes"].items():
        host = next(
            host for host in state["selected"]
            if path.is_relative_to(host["root"])
        )
        _assert_contained(
            host["root"], path, must_exist=path.exists(),
            label="operator runtime install target",
        )
        if _optional_hash(path) != expected:
            raise ValueError(f"install target changed after planning: {path}")


def _write_atomic(path, raw, replace_fn=None):
    path = Path(path)
    temporary = path.with_name(f".{path.name}.{uuid.uuid4().hex}.pending")
    try:
        with temporary.open("xb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        (replace_fn or os.replace)(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def install_runtime(plan, *, process_snapshot_fn=None, replace_fn=None):
    """Apply one reviewed plan with scoped backups and all-file rollback."""
    _revalidate_plan(plan, process_snapshot_fn)
    state = plan._state
    writes = plan._writes
    if not writes:
        return {
            "schema_version": 1,
            "mode": "applied",
            "status": "complete",
            "changed": False,
            "catalog_revision": plan["catalog_revision"],
            "hosts": [row["folder"] for row in plan["selected_hosts"]],
            "files": [],
            "backup_directory": None,
            "writes_performed": False,
        }
    backup = (
        state["server_root"] / ".sdgo-manager-backups"
        / "operator-runtime"
        / (
            datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            + "-" + uuid.uuid4().hex[:8]
        )
    )
    _assert_contained(
        state["server_root"], backup, must_exist=False,
        label="operator runtime backup",
    )
    created_directories = []
    original = {}
    touched = []
    try:
        backup.mkdir(parents=True, exist_ok=False)
        for path in writes:
            relative = path.relative_to(state["server_root"])
            original[path] = path.is_file()
            if path.exists() and not path.is_file():
                raise ValueError(f"install target is not a regular file: {path}")
            if path.is_file():
                destination = backup / "files" / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, destination)
        manifest = {
            "schema_version": 1,
            "catalog_revision": plan["catalog_revision"],
            "files": [
                {
                    "path": str(path.relative_to(state["server_root"]).as_posix()),
                    "existed": original[path],
                    "before_sha256": state["target_hashes"][path],
                    "after_sha256": _sha256_bytes(writes[path]),
                }
                for path in writes
            ],
        }
        (backup / "manifest.json").write_text(
            json.dumps(
                manifest, ensure_ascii=False, indent=2, sort_keys=True
            ) + "\n",
            encoding="utf-8",
        )
        for path, raw in sorted(
            writes.items(), key=lambda item: str(item[0]).casefold()
        ):
            host = next(
                host for host in state["selected"]
                if path.is_relative_to(host["root"])
            )
            _assert_contained(
                host["root"], path, must_exist=path.exists(),
                label="operator runtime install target",
            )
            missing = []
            parent = path.parent
            while not parent.exists():
                missing.append(parent)
                parent = parent.parent
            _assert_contained(
                state["server_root"], parent,
                label="operator runtime target parent",
            )
            path.parent.mkdir(parents=True, exist_ok=True)
            created_directories.extend(reversed(missing))
            touched.append(path)
            _write_atomic(path, raw, replace_fn)
            if _sha256_file(path) != _sha256_bytes(raw):
                raise ValueError(f"installed file readback failed: {path}")
        return {
            "schema_version": 1,
            "mode": "applied",
            "status": "complete",
            "changed": True,
            "catalog_revision": plan["catalog_revision"],
            "hosts": [row["folder"] for row in plan["selected_hosts"]],
            "files": plan["actions"],
            "backup_directory": str(backup),
            "original_executables_modified": False,
            "database_modified": False,
            "processes_controlled": False,
            "writes_performed": True,
        }
    except Exception as error:
        rollback_errors = []
        for path in reversed(touched):
            try:
                if original.get(path):
                    saved = backup / "files" / path.relative_to(
                        state["server_root"]
                    )
                    _write_atomic(path, saved.read_bytes())
                elif path.exists():
                    path.unlink()
            except Exception as rollback_error:
                rollback_errors.append(f"{path}: {rollback_error}")
        for directory in sorted(
            set(created_directories),
            key=lambda value: len(value.parts),
            reverse=True,
        ):
            try:
                directory.rmdir()
            except OSError:
                pass
        if not rollback_errors:
            try:
                shutil.rmtree(backup)
                for parent in (
                    backup.parent,
                    backup.parent.parent,
                ):
                    try:
                        parent.rmdir()
                    except OSError:
                        break
            except OSError as rollback_error:
                rollback_errors.append(
                    f"backup cleanup {backup}: {rollback_error}"
                )
        message = f"operator runtime install failed: {error}"
        if rollback_errors:
            message += "; rollback incomplete: " + "; ".join(rollback_errors)
        else:
            message += "; all task changes were rolled back"
        raise RuntimeError(message) from None


def _time_value(value):
    if isinstance(value, datetime):
        current = value
    elif isinstance(value, (int, float)) and not isinstance(value, bool):
        return float(value)
    elif isinstance(value, str):
        microsoft = re.fullmatch(r"/Date\((\d+)(?:[+-]\d+)?\)/", value)
        if microsoft:
            return int(microsoft.group(1)) / 1000.0
        try:
            current = datetime.fromisoformat(value.replace("Z", "+00:00"))
        except ValueError:
            return None
    else:
        return None
    if current.tzinfo is None:
        current = current.replace(tzinfo=timezone.utc)
    return current.timestamp()


def read_module_status(
    host_root, expected_version, expected_revision,
    *, process_snapshot_fn=None,
):
    """Read status without treating stale or unverifiable PIDs as effective."""
    host_root = Path(host_root).resolve(strict=True)
    module_root = host_root / "sdgo" / "mods" / "operator_catalog_server"
    status_path = module_root / "status.json"
    try:
        _assert_contained(
            host_root, module_root, must_exist=module_root.exists(),
            label="operator runtime module directory",
        )
        _assert_contained(
            host_root, status_path, must_exist=status_path.exists(),
            label="operator status.json",
        )
    except (OSError, ValueError) as error:
        return {
            "configured": False,
            "effective": False,
            "pending": True,
            "state": "pending",
            "status_path": str(status_path),
            "status_present": status_path.is_file(),
            "pid": None,
            "version": None,
            "revision": None,
            "mode": None,
            "loaded_profiles": None,
            "reasons": [str(error)],
        }
    configured = all(path.is_file() for path in (
        module_root / "operator_catalog_server.dll",
        module_root / "mod.ini",
        module_root / "effects.ini",
    ))
    result = {
        "configured": configured,
        "effective": False,
        "pending": configured,
        "state": "pending" if configured else "not_configured",
        "status_path": str(status_path),
        "status_present": status_path.is_file(),
        "pid": None,
        "version": None,
        "revision": None,
        "mode": None,
        "loaded_profiles": None,
        "reasons": [],
    }
    if not status_path.is_file():
        if configured:
            result["reasons"].append("status.json has not been written by a host")
        return result
    try:
        raw = _bounded_file(status_path, 64 * 1024, "operator status.json")
        value = json.loads(raw.decode("utf-8-sig"))
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as error:
        result["reasons"].append(f"status.json is invalid: {error}")
        return result
    if not isinstance(value, dict):
        result["reasons"].append("status.json is not an object")
        return result
    result.update(
        pid=value.get("pid"),
        version=value.get("version"),
        revision=value.get("revision"),
        mode=value.get("native_hook_mode"),
        loaded_profiles=value.get("loaded_profiles"),
    )
    pid = value.get("pid")
    if type(pid) is not int or pid <= 0:
        result["reasons"].append("status.json PID is invalid")
    if value.get("version") != expected_version:
        result["reasons"].append("status.json module version is not expected")
    if value.get("revision") != expected_revision:
        result["reasons"].append("status.json catalog revision is not expected")
    if value.get("native_hook_mode") not in ("shared", "fallback"):
        result["reasons"].append("status.json native hook mode is invalid")
    if (
        type(value.get("loaded_profiles")) is not int
        or not 0 <= value["loaded_profiles"] <= 256
    ):
        result["reasons"].append("status.json loaded profile count is invalid")
    try:
        rows = (process_snapshot_fn or _process_snapshot)()
    except Exception:
        rows = []
        result["reasons"].append("BattleServer process inventory was denied")
    matching = [
        row for row in rows
        if isinstance(row, dict) and row.get("pid") == pid
    ]
    if len(matching) != 1:
        result["reasons"].append("status.json PID is not a unique live process")
    else:
        process = matching[0]
        expected_exe = _path_identity(host_root / "BattleServer.exe")
        if not process.get("path"):
            result["reasons"].append("live process executable path is unknown")
        elif _path_identity(process["path"]) != expected_exe:
            result["reasons"].append(
                "status.json PID does not belong to this BattleServer.exe"
            )
        creation = _time_value(process.get("creation_time"))
        if creation is None or process.get("probe_error"):
            result["reasons"].append("live process creation time is unknown")
        elif status_path.stat().st_mtime < creation:
            result["reasons"].append(
                "status.json predates the live process and may be PID-reused"
            )
    if not result["reasons"]:
        result.update(
            configured=True,
            effective=True,
            pending=False,
            state="active",
        )
    else:
        result["pending"] = configured or result["status_present"]
        result["state"] = "pending" if result["pending"] else "not_configured"
    return result


def _procedure_name(value):
    value = str(value).strip().replace("[", "").replace("]", "")
    return value.rsplit(".", 1)[-1].casefold()


def inspect_runtime_plan(plan):
    """Read back a plan's package, host prerequisites and installed targets."""
    reasons = []
    if not isinstance(plan, RuntimePlan) or not plan._state:
        return {
            "files_installed": False,
            "reasons": ["runtime status requires a complete RuntimePlan"],
        }
    state = plan._state
    required = {
        "server_root", "package", "selected", "source_hashes",
        "host_hashes", "expected_files",
    }
    missing = sorted(required - set(state))
    if missing:
        return {
            "files_installed": False,
            "reasons": ["RuntimePlan lacks internal readback state"],
        }
    non_running = [
        reason for reason in plan.get("blockers", ())
        if not (
            " is running (PID " in reason
            and reason.endswith(
                "installation requires the host to be stopped"
            )
        )
    ]
    reasons.extend(non_running)
    for path, expected in state["source_hashes"].items():
        try:
            _assert_contained(
                state["package"]["root"], path,
                label="framework package source",
            )
            if _sha256_file(path) != expected:
                reasons.append(f"framework package source changed: {path}")
        except (OSError, ValueError) as error:
            reasons.append(str(error))
    for path, expected in state["host_hashes"].items():
        try:
            host = next(
                host for host in state["selected"]
                if path.is_relative_to(host["root"])
            )
            _assert_contained(
                host["root"], path, label="host/framework prerequisite"
            )
            if _sha256_file(path) != expected:
                reasons.append(f"host/framework prerequisite changed: {path}")
        except (OSError, StopIteration, ValueError) as error:
            reasons.append(str(error))
    for path, raw in state["expected_files"].items():
        try:
            host = next(
                host for host in state["selected"]
                if path.is_relative_to(host["root"])
            )
            _assert_contained(
                host["root"], path, label="operator runtime installed file"
            )
            _runtime_file_digest(path, raw)
        except (OSError, StopIteration, ValueError) as error:
            reasons.append(str(error))
    reasons = list(dict.fromkeys(reasons))
    return {
        "files_installed": bool(state["selected"]) and not reasons,
        "reasons": reasons,
    }


def installed_runtime_evidence(plan):
    """Mint opaque evidence only from a successful plan and exact readback."""
    if not isinstance(plan, RuntimePlan) or not plan.get("ready"):
        raise ValueError(
            "installed runtime evidence requires a successful RuntimePlan"
        )
    _revalidate_plan(plan, None)
    readback = inspect_runtime_plan(plan)
    if not readback["files_installed"]:
        raise ValueError(
            "installed runtime readback failed: "
            + "; ".join(readback["reasons"])
        )
    state = plan._state
    package = state["package"]
    package_sections, _ = _ini_sections(
        package["symbol_text"], "package symbol file"
    )
    requirements = tuple(dict.fromkeys(
        requirement
        for module in _MODULES
        for requirement in package["modules"][module]["specification"]["required"]
    ))
    symbol_names = ("host", *requirements)
    package_symbols = {
        name.casefold(): dict(package_sections[name.casefold()])
        for name in symbol_names
    }
    hosts = []
    for host in state["selected"]:
        runtime_files = {}
        expected_runtime_files = {}
        for path, raw in state["expected_files"].items():
            if not path.is_relative_to(host["root"]):
                continue
            label = path.relative_to(host["root"]).as_posix()
            runtime_files[label] = path
            try:
                expected_runtime_files[label] = _runtime_file_digest(path, raw)
            except (OSError, ValueError) as error:
                raise ValueError(str(error)) from None
        prerequisites = {
            path.relative_to(host["root"]).as_posix(): {
                "path": path,
                "sha256": digest,
            }
            for path, digest in state["host_hashes"].items()
            if path.is_relative_to(host["root"])
        }
        hosts.append({
            "folder": host["folder"],
            "host_root": host["root"],
            "host_exe": host["executable"],
            "host_key": host["metadata"]["key"],
            "runtime_files": runtime_files,
            "expected_runtime_files": expected_runtime_files,
            "required_symbols": requirements,
            "package_symbols": package_symbols,
            "prerequisites": prerequisites,
        })
    return InstalledRuntimeEvidence(
        _EVIDENCE_TOKEN, plan["catalog_revision"], hosts
    )


def supported_native_contract(
    host_exe, procedure_fingerprints, runtime_files,
    *, expected_runtime_files=None, required_symbols=None,
    package_symbols=None,
):
    """Purely verify the known host/procedure contract and runtime artifacts."""
    reasons = []
    runtime = {}
    host = None
    try:
        host = _verify_battle_host(Path(host_exe))
    except (OSError, ValueError) as error:
        reasons.append(str(error))
    provided = {}
    if isinstance(procedure_fingerprints, dict):
        for name, digest in procedure_fingerprints.items():
            normalized = _procedure_name(name)
            if normalized in provided:
                reasons.append(f"duplicate procedure fingerprint: {name}")
            provided[normalized] = str(digest).casefold()
    else:
        reasons.append("procedure fingerprints must be a mapping")
    for name, expected in NATIVE_PROCEDURE_FINGERPRINTS.items():
        actual = provided.get(name)
        if actual != expected:
            reasons.append(f"unsupported native procedure fingerprint: {name}")
    if set(provided) - set(NATIVE_PROCEDURE_FINGERPRINTS):
        reasons.append("unexpected native procedure fingerprint entries")

    if not isinstance(runtime_files, dict) or not runtime_files:
        reasons.append("runtime files must be a nonempty mapping")
    else:
        if not isinstance(expected_runtime_files, dict):
            reasons.append("expected runtime hashes are required")
            expected_runtime_files = {}
        elif set(expected_runtime_files) != set(runtime_files):
            reasons.append("expected runtime hash labels do not match files")
        for label, digest in expected_runtime_files.items():
            if not isinstance(label, str) or not _SHA256.fullmatch(str(digest)):
                reasons.append(f"expected runtime hash is invalid: {label}")
        if not isinstance(required_symbols, (tuple, list)):
            reasons.append("required symbol names are required")
            required_symbols = ()
        elif (
            not required_symbols
            or any(not isinstance(name, str) or not name for name in required_symbols)
            or len(set(name.casefold() for name in required_symbols))
            != len(required_symbols)
        ):
            reasons.append("required symbol names are invalid")
        if not isinstance(package_symbols, dict):
            reasons.append("package symbol definitions are required")
            package_symbols = {}
        expected_symbol_names = {
            "host", *(name.casefold() for name in required_symbols)
        }
        normalized_package_symbols = {}
        for name, values in package_symbols.items():
            canonical = str(name).casefold()
            if (
                canonical in normalized_package_symbols
                or not isinstance(values, dict)
                or any(
                    not isinstance(key, str) or not isinstance(value, str)
                    for key, value in values.items()
                )
            ):
                reasons.append(f"package symbol definition is invalid: {name}")
                continue
            normalized_package_symbols[canonical] = {
                str(key).casefold(): value for key, value in values.items()
            }
        if set(normalized_package_symbols) != expected_symbol_names:
            reasons.append("package symbol definitions are incomplete")

        basenames = set()
        symbol_matches = 0
        paths = set()
        for label, value in runtime_files.items():
            try:
                path = Path(value).resolve(strict=True)
                identity = _path_identity(path)
                if identity in paths:
                    raise ValueError("runtime artifacts repeat an installed path")
                paths.add(identity)
                raw = _bounded_file(
                    path, 128 * 1024 * 1024, f"runtime artifact {label}"
                )
                basename = path.name.casefold()
                basenames.add(basename)
                if path.suffix.casefold() == ".dll":
                    _pe_metadata(path, require_x86=True)
                elif path.suffix.casefold() == ".ini":
                    try:
                        text = raw.decode("utf-8-sig")
                        sections, _ = _ini_sections(
                            text, f"runtime artifact {label}"
                        )
                    except (UnicodeError, ValueError):
                        sections = {}
                    if "host" in sections:
                        selected = {
                            name: sections.get(name)
                            for name in expected_symbol_names
                        }
                        if (
                            all(value is not None for value in selected.values())
                            and selected == normalized_package_symbols
                        ):
                            symbol_matches += 1
                actual_hash = _sha256_bytes(raw)
                if expected_runtime_files.get(label) != actual_hash:
                    reasons.append(
                        f"runtime artifact hash does not match package: {label}"
                    )
                runtime[str(label)] = {
                    "path": str(path),
                    "sha256": actual_hash,
                    "bytes": len(raw),
                }
            except (OSError, UnicodeError, ValueError) as error:
                reasons.append(str(error))
        for required in (
            "operator_catalog_server.dll", "battle_rewards.dll"
        ):
            if required not in basenames:
                reasons.append(f"runtime artifact is missing {required}")
        if symbol_matches != 1:
            reasons.append(
                "runtime artifacts lack one complete matching symbol profile"
            )
    return {
        "schema_version": 1,
        "supported": not reasons,
        "host": host,
        "procedures": provided,
        "runtime_files": runtime,
        "reasons": list(dict.fromkeys(reasons)),
        "database_flags_changed": False,
        "processes_controlled": False,
    }


def verify_installed_runtime(evidence, procedure_fingerprints):
    """Convert opaque installed evidence into the only capability grant."""
    if (
        not isinstance(evidence, InstalledRuntimeEvidence)
        or evidence._token is not _EVIDENCE_TOKEN
    ):
        raise ValueError(
            "native capabilities require verifier-owned installed evidence"
        )
    if not evidence._hosts:
        raise ValueError("installed runtime evidence has no selected Battle host")
    contracts = []
    for entry in evidence._hosts:
        prerequisite_hashes = {}
        for label, prerequisite in entry["prerequisites"].items():
            path = prerequisite["path"]
            try:
                _assert_contained(
                    entry["host_root"], path,
                    label="host/framework prerequisite",
                )
                actual = _sha256_file(path)
            except (OSError, ValueError) as error:
                raise ValueError(str(error)) from None
            if actual != prerequisite["sha256"]:
                raise ValueError(
                    f"host/framework prerequisite changed: {label}"
                )
            prerequisite_hashes[label] = actual
        contract = supported_native_contract(
            entry["host_exe"],
            procedure_fingerprints,
            entry["runtime_files"],
            expected_runtime_files=entry["expected_runtime_files"],
            required_symbols=entry["required_symbols"],
            package_symbols=entry["package_symbols"],
        )
        if not contract["supported"]:
            raise ValueError(
                f"{entry['folder']} native runtime is unsupported: "
                + "; ".join(contract["reasons"])
            )
        contracts.append({
            "host": contract["host"]["key"],
            "host_sha256": contract["host"]["sha256"],
            "runtime_files": {
                label: value["sha256"]
                for label, value in sorted(contract["runtime_files"].items())
            },
            "prerequisites": dict(sorted(prerequisite_hashes.items())),
            "symbols": entry["package_symbols"],
        })
    procedures = {
        _procedure_name(name): str(digest).casefold()
        for name, digest in procedure_fingerprints.items()
    }
    material = {
        "schema_version": 1,
        "catalog_revision": evidence._revision,
        "hosts": contracts,
        "procedures": dict(sorted(procedures.items())),
        "capabilities": sorted(TRUSTED_NATIVE_CAPABILITIES),
    }
    digest = _sha256_bytes(json.dumps(
        material, sort_keys=True, separators=(",", ":"), allow_nan=False,
    ).encode("utf-8"))
    return TrustedRuntimeContract(
        _TRUST_TOKEN,
        TRUSTED_NATIVE_CAPABILITIES,
        "SDGO_RUNTIME_V1:" + digest,
        evidence._revision,
    )
