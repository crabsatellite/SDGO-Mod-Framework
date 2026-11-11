"""Fail-closed installation of generated operator client packages."""
from __future__ import annotations

import configparser
import hashlib
import io
import json
import os
from pathlib import Path
import re
import stat
import struct
import zipfile

from ... import distribution, storage, updates
from . import native_resources


MODULE = "operator_catalog"
HOST_KEY = "GOnline-54BE1568-00C97000"
ARCHIVE_PREFIX = "client/sdgo/mods/operator_catalog/"
CLIENT_PREFIX = "sdgo/mods/operator_catalog/"
MAX_COMPRESSED = 256 * 1024 * 1024
MAX_EXPANDED = 512 * 1024 * 1024
MAX_FILES = 10_000
MAX_FILE = 64 * 1024 * 1024
MAX_MANIFEST = 2 * 1024 * 1024

_ROOT_FIELDS = {
    "module", "schema_version", "revision", "storefront_changes", "host", "files"
}
_INNER_FIELDS = {"schema_version", "module", "kind", "revision", "files"}
_ROOT_REQUIRED = {"registrations.json", "symbols.fragment.ini", "Install-Operator.ps1"}
_MODULE_REQUIRED = {
    "operator_catalog.dll", "mod.ini", "manager.json", "display.ini",
    "definitions.json", "display.example.ini", "manifest.json",
}
_ACQUISITION_FILES = {
    "acquisition-sources.json", "acquisition-sources.bin",
}
_MODULE_ALLOWED = _MODULE_REQUIRED | _ACQUISITION_FILES
_GENERATED_EXCLUDED = {
    "operator_catalog.dll", "mod.ini", "manager.json", "manifest.json",
}
_RETIRABLE_ASSET_SUFFIXES = {
    ".gdt", ".ini", ".opack", ".png", ".spr", ".txr", ".zdx", ".zpk", ".zsd",
}
_BUNDLE_PATH = re.compile(
    r"assets/(60[0-9]{3})/(resource-overlay\.ini|resources\.zdx|resources\.zpk)"
)
_RESERVED = re.compile(
    r"(?:CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(?:\..*)?", re.IGNORECASE
)
_SHA256 = re.compile(r"[0-9a-f]{64}")
_README = re.compile(
    r"README(?:\.[A-Za-z0-9_-]+)*(?:\.md|\.txt)?", re.IGNORECASE
)
TRUSTED_SYMBOLS = frozenset({
    "operator.load_data",
    "operator.grid_data",
    "operator.grid_find",
    "operator.grid_make_node",
    "operator.grid_insert",
    "operator.native_new",
    "operator.native_delete",
    "operator.native_name_decode",
    "capsule.main.open_file",
    "capsule.main.file_ctor",
    "capsule.main.operator_new",
    "capsule.main.wfopen",
})
TRUSTED_CAPSULE_SYMBOLS = {
    "capsule.main.open_file": {
        "kind": "code",
        "rva": "0x007E5600",
        "expect": (
            "55 8B EC 6A FF 68 D3 3D E1 00 64 A1 00 00 00 00 "
            "50 83 EC 50 A1 10 71 FF"
        ),
    },
    "capsule.main.file_ctor": {
        "kind": "code",
        "rva": "0x007E95E0",
        "expect": (
            "55 8B EC 6A FF 68 AB 40 E1 00 64 A1 00 00 00 00 "
            "50 83 EC 14 A1 10 71 FF"
        ),
    },
    "capsule.main.operator_new": {
        "kind": "code",
        "rva": "0x006F481C",
        "expect": (
            "8B FF 55 8B EC 56 EB 13 A1 58 2A FE 00 85 C0 74 "
            "19 FF 75 08 FF D0 59 85"
        ),
    },
    "capsule.main.wfopen": {
        "kind": "code",
        "rva": "0x00887D78",
        "expect": (
            "8B FF 55 8B EC 6A 40 FF 75 0C FF 75 08 E8 30 FF "
            "FF FF 83 C4 0C 5D C3"
        ),
    },
}


def _json_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("JSON 包含重复键：" + str(key))
        result[key] = value
    return result


def _json(raw, label):
    try:
        return json.loads(
            raw.decode("utf-8-sig"), object_pairs_hook=_json_object
        )
    except (UnicodeError, json.JSONDecodeError, TypeError) as error:
        raise ValueError(label + "无效") from error


def _strict_ini(raw, label):
    try:
        return updates.read_ini(raw)
    except (configparser.Error, UnicodeError) as error:
        raise ValueError(label + "无效") from error


def _safe_name(name, *, label="副官模块包路径"):
    if not isinstance(name, str) or not name or "\\" in name or name.startswith("/"):
        raise ValueError(label + "无效")
    parts = name.split("/")
    if any(
        part in ("", ".", "..")
        or part.endswith((" ", "."))
        or _RESERVED.fullmatch(part)
        or any(ord(char) < 32 or char in ':*?"<>|\0' for char in part)
        for part in parts
    ):
        raise ValueError(label + "无效：" + name)
    return name


def _sha(value, label):
    if not isinstance(value, str) or not _SHA256.fullmatch(value):
        raise ValueError(label + "的 SHA-256 无效")
    return value


def _revision(value, label):
    return _sha(value, label)


def _read_archive(raw):
    if not isinstance(raw, (bytes, bytearray, memoryview)):
        raise ValueError("副官模块包必须是 ZIP 字节")
    raw = bytes(raw)
    if len(raw) > MAX_COMPRESSED:
        raise ValueError("副官模块包压缩数据超过 256 MiB")
    try:
        archive = zipfile.ZipFile(io.BytesIO(raw))
    except zipfile.BadZipFile as error:
        raise ValueError("副官模块包不是有效 ZIP") from error
    with archive:
        entries = archive.infolist()
        if len(entries) > MAX_FILES:
            raise ValueError("副官模块包文件超过 10000 个")
        names = set()
        total = 0
        by_name = {}
        for entry in entries:
            name = _safe_name(entry.filename)
            mode = entry.external_attr >> 16
            file_type = stat.S_IFMT(mode)
            if entry.is_dir() or file_type not in (0, stat.S_IFREG):
                raise ValueError("副官模块包不能包含目录、链接或特殊文件：" + name)
            if entry.flag_bits & 1:
                raise ValueError("副官模块包不能包含加密文件：" + name)
            folded = name.casefold()
            if folded in names:
                raise ValueError("副官模块包文件名重复：" + name)
            names.add(folded)
            if entry.file_size > MAX_FILE:
                raise ValueError("副官模块包单个文件超过 64 MiB：" + name)
            total += entry.file_size
            if total > MAX_EXPANDED:
                raise ValueError("副官模块包解压内容超过 512 MiB")
            by_name[name] = entry
        if "manifest.json" not in by_name:
            raise ValueError("副官模块包缺少根 manifest.json")
        if by_name["manifest.json"].file_size > MAX_MANIFEST:
            raise ValueError("副官模块包发布清单过大")
        try:
            root = _json(archive.read(by_name["manifest.json"]), "副官模块包发布清单")
        except (zipfile.BadZipFile, RuntimeError, EOFError) as error:
            raise ValueError("副官模块包读取失败") from error
        if not isinstance(root, dict) or set(root) != _ROOT_FIELDS:
            raise ValueError("副官模块包发布清单字段无效")
        if (
            root.get("module") != MODULE
            or root.get("schema_version") != 1
            or root.get("host") != HOST_KEY
            or root.get("storefront_changes") is not False
        ):
            raise ValueError("副官模块包发布清单身份无效")
        revision = _revision(root.get("revision"), "副官模块包版本")
        declared = root.get("files")
        if not isinstance(declared, dict):
            raise ValueError("副官模块包文件清单无效")
        for name, expected in declared.items():
            _safe_name(name, label="副官模块包清单路径")
            _sha(expected, name)
        if set(by_name) != set(declared) | {"manifest.json"}:
            raise ValueError("副官模块包文件与发布清单不一致")
        payload = {}
        try:
            for name, expected in declared.items():
                data = archive.read(by_name[name])
                if len(data) != by_name[name].file_size:
                    raise ValueError("副官模块包文件长度不一致：" + name)
                if hashlib.sha256(data).hexdigest() != expected:
                    raise ValueError("副官模块包文件校验失败：" + name)
                payload[name] = data
        except (zipfile.BadZipFile, RuntimeError, EOFError) as error:
            raise ValueError("副官模块包读取失败") from error
    return revision, payload


def _csv(manifest, key):
    values = [value.strip() for value in manifest.get("mod", key, fallback="").split(",")]
    if any(not value for value in values):
        values = [value for value in values if value]
    folded = [value.casefold() for value in values]
    if len(set(folded)) != len(folded):
        raise ValueError("Mod 清单包含重复路径：" + key)
    for value in values:
        _safe_name(value, label="Mod 清单路径")
    return values


def _inner_manifest(raw, *, expected_revision=None):
    value = _json(raw, "副官资源所有权清单")
    if not isinstance(value, dict) or set(value) != _INNER_FIELDS:
        raise ValueError("副官资源所有权清单字段无效")
    if (
        value.get("schema_version") != 1
        or value.get("module") != MODULE
        or value.get("kind") != "generated-client-resources"
    ):
        raise ValueError("副官资源所有权清单身份无效")
    revision = _revision(value.get("revision"), "副官资源所有权清单版本")
    if expected_revision is not None and revision != expected_revision:
        raise ValueError("副官资源所有权清单版本与发布清单不一致")
    files = value.get("files")
    if not isinstance(files, dict):
        raise ValueError("副官资源所有权文件清单无效")
    seen = set()
    for name, metadata in files.items():
        _safe_name(name, label="副官资源所有权路径")
        folded = name.casefold()
        if folded in seen:
            raise ValueError("副官资源所有权路径重复：" + name)
        seen.add(folded)
        if not isinstance(metadata, dict) or set(metadata) != {"bytes", "sha256"}:
            raise ValueError("副官资源所有权记录无效：" + name)
        size = metadata["bytes"]
        if isinstance(size, bool) or not isinstance(size, int) or not 0 <= size <= MAX_FILE:
            raise ValueError("副官资源所有权长度无效：" + name)
        _sha(metadata["sha256"], name)
    return value


def _resource_declarations(raw):
    value = _json(raw, "副官公开定义")
    if not isinstance(value, dict):
        raise ValueError("副官公开定义身份无效")
    operators = value.get("operators")
    if value.get("schema_version") != 1 or not isinstance(operators, list):
        raise ValueError("副官公开定义身份无效")
    expected = {}
    source_packs = set()
    seen = set()
    for operator in operators:
        if not isinstance(operator, dict):
            raise ValueError("副官公开定义记录无效")
        identity = operator.get("id")
        if (
            isinstance(identity, bool)
            or not isinstance(identity, int)
            or not 60001 <= identity <= 60999
            or identity in seen
        ):
            raise ValueError("副官公开定义编号重复或无效")
        seen.add(identity)
        if operator.get("managed") is not True:
            raise ValueError("副官公开定义不能携带未托管记录")
        avatar = operator.get("avatar")
        if not isinstance(avatar, dict):
            raise ValueError("副官公开定义头像无效")
        kind = avatar.get("kind")
        if kind == "native":
            if set(avatar) != {"kind", "operator_id"}:
                raise ValueError("副官原生头像定义无效")
            continue
        if kind not in ("upload", "pack"):
            raise ValueError("副官公开定义头像类型无效")
        digest = avatar.get("sha256")
        if not isinstance(digest, str) or not _SHA256.fullmatch(digest):
            raise ValueError("副官公开定义头像 SHA-256 无效")
        if kind == "upload":
            if set(avatar) != {"kind", "sha256"}:
                raise ValueError("副官上传头像定义无效")
        else:
            if (
                set(avatar) != {"kind", "sha256", "operator_id"}
                or isinstance(avatar.get("operator_id"), bool)
                or not isinstance(avatar.get("operator_id"), int)
                or not 60001 <= avatar["operator_id"] <= 60999
            ):
                raise ValueError("副官分享包头像定义无效")
            source_packs.add(digest)
        expected[identity] = {"kind": kind, "sha256": digest}
    return expected, source_packs


def _validate_package(revision, payload):
    module = {}
    root_files = set()
    for name, data in payload.items():
        if name.startswith(ARCHIVE_PREFIX):
            relative = name[len(ARCHIVE_PREFIX):]
            _safe_name(relative, label="副官模块相对路径")
            module[relative] = data
        else:
            root_files.add(name)
            if "/" in name or (name not in _ROOT_REQUIRED and not _README.fullmatch(name)):
                raise ValueError("副官模块包包含未知文件：" + name)
    if not _ROOT_REQUIRED.issubset(root_files):
        missing = sorted(_ROOT_REQUIRED - root_files)
        raise ValueError("副官模块包缺少根元数据：" + "、".join(missing))
    if not set(module).issuperset(_MODULE_REQUIRED):
        missing = sorted(_MODULE_REQUIRED - set(module))
        raise ValueError("副官模块包缺少客户端文件：" + "、".join(missing))
    acquisition_files = set(module).intersection(_ACQUISITION_FILES)
    if acquisition_files not in (set(), _ACQUISITION_FILES):
        raise ValueError("副官获得方式文件必须成对提供")
    dlls = [name for name in module if Path(name).suffix.casefold() == ".dll"]
    if dlls != ["operator_catalog.dll"]:
        raise ValueError("副官模块 DLL 名称无效")

    try:
        mod = distribution.mod_ini(module["mod.ini"])
        manager = distribution.manager_json(module["manager.json"])
    except (configparser.Error, UnicodeError, ValueError) as error:
        raise ValueError("副官 Mod 或 Manager 登记无效") from error
    if (
        mod.get("mod", "id", fallback="") != MODULE
        or mod.get("mod", "dll", fallback="") != "operator_catalog.dll"
        or [item.strip() for item in mod.get("mod", "hosts", fallback="").split(",") if item.strip()]
        != [HOST_KEY]
    ):
        raise ValueError("副官 Mod 身份、DLL 或宿主声明无效")
    if (
        manager.get("schema_version") != 1
        or manager.get("id") != MODULE
        or manager.get("hosts") != ["client"]
    ):
        raise ValueError("副官 Manager 登记无效")
    try:
        scope = distribution.resolve(
            module["mod.ini"], module["manager.json"], allow_legacy_public=True
        )
    except ValueError as error:
        raise ValueError("副官模块分发声明不一致") from error
    if scope != distribution.PUBLIC or any(
        distribution.NONREDISTRIBUTABLE_MARKER in data for data in module.values()
    ):
        raise ValueError("副官模块包包含 local_only 分发标记")

    declared_files = set(_csv(mod, "files"))
    config_files = set(_csv(mod, "config_files"))
    if declared_files != {"display.example.ini", "manager.json", "manifest.json"}:
        raise ValueError("副官 Mod 运行文件声明无效")
    base_config_files = {"display.ini", "definitions.json", "manifest.json"}
    expected_config_files = base_config_files | acquisition_files
    if config_files != expected_config_files:
        raise ValueError("副官 Mod 配置文件声明无效")
    if not acquisition_files and mod.get("mod", "version", fallback="") != "0.2.0":
        raise ValueError("当前副官模块包缺少获得方式文件")
    expected_resources, expected_packs = _resource_declarations(
        module["definitions.json"]
    )
    bundles = {}
    source_packs = set()
    for relative, data in module.items():
        if not relative.startswith("assets/"):
            if relative not in _MODULE_ALLOWED:
                raise ValueError("副官模块包包含未知客户端文件：" + relative)
            continue
        if relative.startswith("assets/imports/"):
            if (
                not relative.endswith(".opack")
                or not _SHA256.fullmatch(Path(relative).stem)
                or hashlib.sha256(data).hexdigest() != Path(relative).stem
            ):
                raise ValueError("副官分享包路径或校验值无效：" + relative)
            source_packs.add(Path(relative).stem)
            continue
        match = _BUNDLE_PATH.fullmatch(relative)
        if not match:
            raise ValueError("副官资源必须使用编号绑定的原生归档：" + relative)
        identity = int(match.group(1))
        bundle = bundles.setdefault(identity, {})
        bundle[match.group(2)] = data
    if source_packs != expected_packs:
        raise ValueError("副官分享包来源与公开定义不一致")
    if set(bundles) != set(expected_resources):
        raise ValueError("副官原生资源归档与公开定义不一致")
    nested_expanded = 0
    for identity, declaration in expected_resources.items():
        bundle = bundles[identity]
        if set(bundle) != set(native_resources.BUNDLE_FILES):
            raise ValueError("副官原生资源归档文件不完整：" + str(identity))
        receipt = native_resources.validate_bundle(
            identity,
            bundle[native_resources.CONFIG_NAME],
            bundle[native_resources.INDEX_NAME],
            bundle[native_resources.ARCHIVE_NAME],
            expanded_limit=MAX_EXPANDED - nested_expanded,
        )
        nested_expanded += receipt["expanded_bytes"]
        expected_layout = "upload" if declaration["kind"] == "upload" else "compiled"
        if receipt["layout"] != expected_layout:
            raise ValueError("副官原生资源归档布局与头像类型不一致：" + str(identity))
        if expected_layout == "upload":
            row = next(
                (
                    entry for entry in receipt["entries"]
                    if entry["path"] in native_resources.upload_texture_keys(identity)
                ),
                None,
            )
            if row is None or row["sha256"] != declaration["sha256"]:
                raise ValueError("副官上传头像归档与公开定义 SHA-256 不一致：" + str(identity))

    generated = _inner_manifest(
        module["manifest.json"], expected_revision=revision
    )
    expected_generated = set(module) - _GENERATED_EXCLUDED
    if set(generated["files"]) != expected_generated:
        raise ValueError("副官资源所有权清单与客户端文件不一致")
    for relative, metadata in generated["files"].items():
        data = module[relative]
        if (
            len(data) != metadata["bytes"]
            or hashlib.sha256(data).hexdigest() != metadata["sha256"]
        ):
            raise ValueError("副官资源所有权校验失败：" + relative)

    registrations = _json(payload["registrations.json"], "副官登记元数据")
    if (
        not isinstance(registrations, dict)
        or registrations.get("schema_version") != 1
        or registrations.get("provider") != MODULE
        or registrations.get("revision") != revision
        or registrations.get("storefront_changes") is not False
    ):
        raise ValueError("副官登记元数据无效")

    fragment = _strict_ini(payload["symbols.fragment.ini"], "副官符号片段")
    try:
        stamp = int(fragment.get("host", "timestamp"), 0)
        size = int(fragment.get("host", "size_of_image"), 0)
    except (configparser.Error, TypeError, ValueError) as error:
        raise ValueError("副官符号片段宿主无效") from error
    if (
        fragment.get("host", "image", fallback="") != "GOnline.exe"
        or f"GOnline-{stamp:08X}-{size:08X}" != HOST_KEY
    ):
        raise ValueError("副官符号片段不属于目标客户端")
    required_symbols = {
        value.strip()
        for value in mod.get("mod", "requires", fallback="").split(",")
        if value.strip()
    }
    if (
        required_symbols != TRUSTED_SYMBOLS
        or set(fragment.sections()) != TRUSTED_SYMBOLS | {"host"}
        or any(
            dict(fragment[alias]) != expected
            for alias, expected in TRUSTED_CAPSULE_SYMBOLS.items()
        )
    ):
        raise ValueError("副官符号片段与 Mod 依赖不一致")
    return module, generated, fragment


def _check_existing(root):
    mod_path = updates.child(root, CLIENT_PREFIX + "mod.ini")
    if not mod_path.exists():
        return
    if not mod_path.is_file():
        raise ValueError("现有副官 Mod 清单不是普通文件")
    mod_raw = mod_path.read_bytes()
    try:
        existing = distribution.mod_ini(mod_raw)
    except (configparser.Error, UnicodeError, ValueError) as error:
        raise ValueError("现有副官 Mod 清单无效") from error
    if (
        existing.get("mod", "id", fallback="") != MODULE
        or existing.get("mod", "dll", fallback="") != "operator_catalog.dll"
        or {
            value.strip()
            for value in existing.get("mod", "hosts", fallback="").split(",")
            if value.strip()
        } != {HOST_KEY}
    ):
        raise ValueError("现有副官 Mod 与此安装包不兼容")
    manager_path = updates.child(root, CLIENT_PREFIX + "manager.json")
    if manager_path.exists() and not manager_path.is_file():
        raise ValueError("现有副官 Manager 登记不是普通文件")
    manager_raw = manager_path.read_bytes() if manager_path.is_file() else None
    try:
        scope = distribution.resolve(
            mod_raw, manager_raw, allow_legacy_public=True
        )
    except ValueError as error:
        raise ValueError("现有副官 Mod 分发声明不一致") from error
    if scope != distribution.PUBLIC:
        raise ValueError("不能用公共副官包覆盖 local_only Mod")
    dll_path = updates.child(root, CLIENT_PREFIX + "operator_catalog.dll")
    if (
        dll_path.is_file()
        and distribution.NONREDISTRIBUTABLE_MARKER in dll_path.read_bytes()
    ):
        raise ValueError("现有副官 Mod 包含 local_only 分发标记")


def _merged_symbols(root, fragment):
    path = updates.child(root, "sdgo/symbols/" + HOST_KEY + ".ini")
    if not path.is_file():
        raise ValueError("目标客户端缺少匹配的框架符号文件")
    merged = _strict_ini(path.read_bytes(), "目标客户端框架符号文件")
    for section in fragment.sections():
        values = dict(fragment[section])
        if merged.has_section(section):
            if dict(merged[section]) != values:
                raise ValueError("符号定义存在冲突：" + section)
        else:
            merged[section] = values
    return path, updates.serialize_ini(merged)


def _preserve_settings(package_raw, existing_raw):
    if existing_raw is None:
        return package_raw
    existing = _strict_ini(existing_raw, "现有副官 Mod 清单")
    patches = []
    for section in existing.sections():
        for key, value in existing[section].items():
            if section.casefold() == "settings" or (
                section.casefold() == "mod" and key.casefold() == "enabled"
            ):
                patches.append((section, key, value))
    return storage.patch_ini(package_raw, patches)


def _retirement_plan(root, new_generated, revision):
    manifest_path = updates.child(root, CLIENT_PREFIX + "manifest.json")
    if not manifest_path.exists():
        return [], []
    if not manifest_path.is_file():
        raise ValueError("现有副官资源所有权清单不是普通文件")
    old = _inner_manifest(manifest_path.read_bytes())
    retiring = []
    kept = []
    for relative, metadata in old["files"].items():
        if not relative.startswith("assets/") or relative in new_generated["files"]:
            continue
        if Path(relative).suffix.casefold() not in _RETIRABLE_ASSET_SUFFIXES:
            raise ValueError("现有副官资源所有权类型无效：" + relative)
        source = updates.child(root, CLIENT_PREFIX + relative)
        if not source.exists():
            continue
        if not source.is_file():
            raise ValueError("旧副官资源不是普通文件：" + relative)
        raw = source.read_bytes()
        if (
            len(raw) != metadata["bytes"]
            or hashlib.sha256(raw).hexdigest() != metadata["sha256"]
        ):
            kept.append(CLIENT_PREFIX + relative)
            continue
        destination = updates.child(
            root,
            "sdgo/local-state/operator-catalog-retired/"
            + revision + "/" + relative,
        )
        if destination.exists():
            if not destination.is_file() or destination.read_bytes() != raw:
                raise ValueError("副官旧资源隔离位置已被占用：" + relative)
            destination_exists = True
        else:
            destination_exists = False
        retiring.append((source, destination, raw, destination_exists))
    return retiring, kept


def _backup(root, revision, path, raw):
    relative = path.relative_to(root).as_posix()
    identity = hashlib.sha256(raw).hexdigest()[:16]
    backup = updates.child(
        root,
        "sdgo/local-state/operator-catalog-backups/"
        + revision + "/" + relative + "." + identity + ".bak",
    )
    if backup.exists():
        if not backup.is_file() or backup.read_bytes() != raw:
            raise ValueError("副官安装备份位置已被占用：" + relative)
        return None
    else:
        storage.atomic(backup, raw)
        return backup


def _restore(written, moved):
    errors = []
    for source, destination, raw, destination_existed in reversed(moved):
        try:
            if destination_existed:
                storage.atomic(source, raw)
            elif destination.exists():
                source.parent.mkdir(parents=True, exist_ok=True)
                os.replace(destination, source)
            elif not source.exists():
                storage.atomic(source, raw)
        except Exception as error:  # rollback must attempt every remaining path
            errors.append(error)
    for path, previous in reversed(written):
        try:
            if previous is None:
                path.unlink(missing_ok=True)
            else:
                storage.atomic(path, previous)
        except Exception as error:  # rollback must attempt every remaining path
            errors.append(error)
    if errors:
        raise RuntimeError("副官模块安装失败，且部分文件无法回滚") from errors[0]


def install_client(workspace, host, raw_bytes):
    """Install one verified Workspace.package() archive into a stopped client."""
    if isinstance(host, str):
        host = workspace.host(host)
    if not isinstance(host, dict) or host.get("role") != "client":
        raise ValueError("副官模块包只能安装到客户端")
    if not isinstance(host.get("path"), (str, os.PathLike)):
        raise ValueError("客户端路径无效")
    root = Path(host["path"]).resolve()
    executable = updates.child(root, "GOnline.exe")
    if not executable.is_file():
        raise ValueError("目标目录缺少 GOnline.exe")
    if updates.running(executable):
        raise ValueError("请先关闭这个客户端，再安装副官模块包")

    revision, payload = _read_archive(raw_bytes)
    module, generated, fragment = _validate_package(revision, payload)

    with storage.LOCK:
        if updates.running(executable):
            raise ValueError("请先关闭这个客户端，再安装副官模块包")
        try:
            actual_host = updates.host_key(executable)
        except (OSError, ValueError, EOFError, struct.error) as error:
            raise ValueError("无法核对目标客户端版本") from error
        if actual_host != HOST_KEY:
            raise ValueError("此副官模块包不支持目标客户端版本：" + actual_host)
        core = updates.child(root, "sdgo/core.dll")
        if not core.is_file():
            raise ValueError("目标客户端尚未安装兼容的 SDGO Mod Framework")
        _check_existing(root)

        pending = {}
        for relative in sorted(module):
            target = updates.child(root, CLIENT_PREFIX + relative)
            if target.exists() and not target.is_file():
                raise ValueError("副官模块安装目标不是普通文件：" + relative)
            data = module[relative]
            previous = target.read_bytes() if target.is_file() else None
            if relative == "mod.ini":
                data = _preserve_settings(data, previous)
            if previous != data:
                pending[target] = data
        symbol_path, symbol_data = _merged_symbols(root, fragment)
        symbol_previous = symbol_path.read_bytes()
        if symbol_previous != symbol_data:
            pending[symbol_path] = symbol_data
        retiring, kept = _retirement_plan(root, generated, revision)

        previous = {
            path: path.read_bytes() if path.is_file() else None for path in pending
        }
        created_backups = []
        try:
            for path, raw in previous.items():
                if raw is not None:
                    backup = _backup(root, revision, path, raw)
                    if backup is not None:
                        created_backups.append(backup)
            if updates.running(executable):
                raise ValueError("客户端已启动，副官模块包未写入")

            written = []
            moved = []
            try:
                for path, data in pending.items():
                    storage.atomic(path, data)
                    written.append((path, previous[path]))
                for source, destination, raw, destination_existed in retiring:
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    if destination_existed:
                        source.unlink()
                    else:
                        os.replace(source, destination)
                    moved.append((source, destination, raw, destination_existed))
                state = workspace.public()
            except Exception:
                _restore(written, moved)
                raise
        except Exception:
            for backup in reversed(created_backups):
                backup.unlink(missing_ok=True)
            raise

    retired = [
        path.relative_to(root).as_posix()
        for path, _destination, _raw, _existing in retiring
    ]
    warnings = [
        "保留已修改的旧副官资源：" + path for path in kept
    ]
    return {
        "installed_files": len(pending),
        "retired_files": retired,
        "kept_modified_retired": kept,
        "warnings": warnings,
        "activation": "restart_required",
        "state": state,
    }
