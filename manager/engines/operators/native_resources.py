"""Build and authenticate operator-owned native ZDX/ZPK resource bundles."""
from __future__ import annotations

import gzip
import hashlib
import re
import struct
import zlib


FORMAT = "sdgo-native-resource-overlay-v1"
INDEX_NAME = "resources.zdx"
ARCHIVE_NAME = "resources.zpk"
CONFIG_NAME = "resource-overlay.ini"
BUNDLE_FILES = (CONFIG_NAME, INDEX_NAME, ARCHIVE_NAME)
CHUNK_SIZE = 512
PHYSICAL_CHUNK_SIZE = CHUNK_SIZE + 8
MAX_INDEX = 64 * 1024 * 1024
MAX_ARCHIVE = 64 * 1024 * 1024
MAX_RESOURCE = 64 * 1024 * 1024
MAX_EXPANDED = 512 * 1024 * 1024
MAX_ENTRIES = 100_000
MAX_PATH = 240
_HASH = re.compile(r"[0-9a-f]{64}")
_PATH = re.compile(r"[a-z0-9][a-z0-9_.-]*(?:/[a-z0-9][a-z0-9_.-]*)+")
_COMPILED_LEAF = re.compile(r"[0-9]+\.(?:gdt|spr|zsd|txr)")
_TEXTURE_ALIAS = re.compile(r"sdgo/optex/(60[0-9]{3})/[0-9]+\.txr")
_SUFFIXES = (".gdt", ".spr", ".zsd", ".txr")
_CONFIG_KEYS = (
    "schema", "format", "operator_id", "index", "index_sha256",
    "archive", "archive_sha256",
)


def _operator_id(value):
    if isinstance(value, bool) or not isinstance(value, int) or not 60001 <= value <= 60999:
        raise ValueError("副官原生资源编号无效")
    return value


def _owned_directory(operator_id):
    return f"sdgo/mods/operator_catalog/assets/{operator_id}/"


def upload_texture_key(operator_id):
    operator_id = _operator_id(operator_id)
    return f"sdgo/optex/{operator_id}/0.txr"


def legacy_upload_texture_key(operator_id):
    operator_id = _operator_id(operator_id)
    return f"sdgo/mods/operator_catalog/assets/{operator_id}.txr"


def upload_texture_keys(operator_id):
    return (upload_texture_key(operator_id), legacy_upload_texture_key(operator_id))


def _upload_sprite_keys(operator_id):
    root = "sdgo/mods/operator_catalog/assets/"
    return {
        *(root + f"{operator_id}_{index}.spr" for index in range(7)),
    }


def _resource_key(operator_id, supplied):
    if not isinstance(supplied, str):
        raise ValueError("副官原生资源路径无效")
    name = supplied.replace("\\", "/").lower()
    if (
        supplied != name
        or len(name) > MAX_PATH
        or not _PATH.fullmatch(name)
        or any(part in (".", "..") or part.endswith((".", " ")) for part in name.split("/"))
        or not name.endswith(_SUFFIXES)
    ):
        raise ValueError("副官原生资源路径不是规范的小写虚拟路径：" + name)
    alias = _TEXTURE_ALIAS.fullmatch(name)
    if alias:
        if int(alias.group(1)) != operator_id:
            raise ValueError("副官原生资源越过其唯一编号范围：" + name)
        if len(name.encode("utf-16-le")) >= 64:
            raise ValueError("副官纹理虚拟路径超过原生 32 wchar 字段：" + name)
        return name, "texture-alias"
    owned = _owned_directory(operator_id)
    if name.startswith(owned):
        if not _COMPILED_LEAF.fullmatch(name[len(owned):]):
            raise ValueError("副官目录资源路径或类型无效：" + name)
        return name, "compiled"
    if name in _upload_sprite_keys(operator_id):
        return name, "upload-sprite"
    if name == legacy_upload_texture_key(operator_id):
        return name, "upload-texture-legacy"
    raise ValueError("副官原生资源越过其唯一编号范围：" + name)


def _layout(operator_id, names):
    names = set(names)
    if not names:
        raise ValueError("副官原生资源包不能为空")
    kinds = {_resource_key(operator_id, name)[1] for name in names}
    sprites = _upload_sprite_keys(operator_id)
    upload_layouts = [
        sprites | {texture} for texture in upload_texture_keys(operator_id)
    ]
    if names in upload_layouts:
        return "upload"
    if kinds <= {"compiled", "texture-alias"}:
        return "compiled"
    raise ValueError("副官原生资源包不能混合目录资源与上传头像，上传头像须包含一个纹理和七个精灵")


def _config(operator_id, index, archive):
    values = {
        "schema": "1",
        "format": FORMAT,
        "operator_id": str(operator_id),
        "index": INDEX_NAME,
        "index_sha256": hashlib.sha256(index).hexdigest(),
        "archive": ARCHIVE_NAME,
        "archive_sha256": hashlib.sha256(archive).hexdigest(),
    }
    return (
        "[runtime]\n"
        + "".join(f"{key}={values[key]}\n" for key in _CONFIG_KEYS)
    ).encode("ascii")


def build_bundle(operator_id, resources):
    """Return one deterministic Manager package bundle for an operator."""
    operator_id = _operator_id(operator_id)
    if not isinstance(resources, dict) or not 1 <= len(resources) <= MAX_ENTRIES:
        raise ValueError("副官原生资源数量无效")
    normalized = {}
    expanded = 0
    for supplied, raw in resources.items():
        name, _kind = _resource_key(operator_id, supplied)
        if name in normalized:
            raise ValueError("副官原生资源包含重复的规范路径：" + name)
        if not isinstance(raw, bytes) or not 0 < len(raw) <= MAX_RESOURCE:
            raise ValueError("副官原生资源长度无效：" + name)
        expanded += len(raw)
        if expanded > MAX_EXPANDED:
            raise ValueError("单个副官原生资源展开后超过 512 MiB")
        normalized[name] = raw
    layout = _layout(operator_id, normalized)

    entries = []
    chunks = []
    for name, raw in sorted(normalized.items()):
        packed = b"ZOAGZIP1" + struct.pack("<I", len(raw)) + gzip.compress(raw, mtime=0)
        if len(packed) > MAX_RESOURCE:
            raise ValueError("副官原生资源压缩后超过 64 MiB：" + name)
        first = len(chunks)
        count = (len(packed) + CHUNK_SIZE - 1) // CHUNK_SIZE
        if (first + count) * PHYSICAL_CHUNK_SIZE >= MAX_ARCHIVE:
            raise ValueError("单个副官原生资源归档必须小于 64 MiB")
        for index in range(count):
            cursor = len(chunks)
            chunks.append(
                struct.pack("<II", cursor + 1, max(0, cursor - 1))
                + packed[index * CHUNK_SIZE:(index + 1) * CHUNK_SIZE].ljust(
                    CHUNK_SIZE, b"\0"
                )
            )
        entries.append(
            {
                "path": name,
                "stored_bytes": len(packed),
                "first_chunk": first,
                "chunks": count,
                "bytes": len(raw),
                "sha256": hashlib.sha256(raw).hexdigest(),
            }
        )
    archive = b"".join(chunks)
    if not archive or len(archive) >= MAX_ARCHIVE:
        raise ValueError("单个副官原生资源归档必须小于 64 MiB")

    index = bytearray(
        struct.pack("<6I", 1, CHUNK_SIZE, len(chunks), 0, len(entries), 0)
    )
    for entry in entries:
        for character in entry["path"].replace("/", "\\") + "\0":
            index.extend(struct.pack("<H", ord(character) ^ 0x00AA))
        index.extend(
            struct.pack(
                "<9I",
                entry["stored_bytes"],
                entry["first_chunk"],
                entry["chunks"],
                0, 0, 0, 0, 0, 0,
            )
        )
    index = bytes(index)
    if len(index) > MAX_INDEX:
        raise ValueError("副官原生资源索引超过 64 MiB")
    return {
        "operator_id": operator_id,
        "layout": layout,
        "entries": entries,
        "files": {
            CONFIG_NAME: _config(operator_id, index, archive),
            INDEX_NAME: index,
            ARCHIVE_NAME: archive,
        },
    }


def _parse_config(operator_id, raw):
    if not isinstance(raw, bytes):
        raise ValueError("副官原生资源配置不是字节内容")
    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError as error:
        raise ValueError("副官原生资源配置必须为 ASCII") from error
    if "\r" in text or not text.endswith("\n"):
        raise ValueError("副官原生资源配置换行格式无效")
    lines = text.splitlines()
    if len(lines) != len(_CONFIG_KEYS) + 1 or lines[0] != "[runtime]":
        raise ValueError("副官原生资源配置结构无效")
    values = {}
    for key, line in zip(_CONFIG_KEYS, lines[1:]):
        if not line.startswith(key + "="):
            raise ValueError("副官原生资源配置字段顺序或名称无效")
        values[key] = line[len(key) + 1:]
    if (
        values["schema"] != "1"
        or values["format"] != FORMAT
        or values["operator_id"] != str(operator_id)
        or values["index"] != INDEX_NAME
        or values["archive"] != ARCHIVE_NAME
        or not _HASH.fullmatch(values["index_sha256"])
        or not _HASH.fullmatch(values["archive_sha256"])
    ):
        raise ValueError("副官原生资源配置身份无效")
    return values


def _u32(raw, offset):
    return struct.unpack_from("<I", raw, offset)[0]


def _decode_path(index, cursor):
    result = []
    for _count in range(MAX_PATH + 1):
        if cursor + 2 > len(index):
            raise ValueError("副官原生资源索引路径被截断")
        encoded = struct.unpack_from("<H", index, cursor)[0]
        cursor += 2
        value = encoded ^ 0x00AA
        if value == 0:
            raw = "".join(result)
            if "/" in raw:
                raise ValueError("副官原生资源索引未使用原生反斜线")
            return raw.replace("\\", "/"), cursor
        if not 0x20 <= value <= 0x7E:
            raise ValueError("副官原生资源索引路径不是安全 ASCII")
        result.append(chr(value))
    raise ValueError("副官原生资源索引路径过长")


def _decode_resource(packed, name, expanded_limit):
    if len(packed) < 12 or not packed.startswith(b"ZOAGZIP1"):
        raise ValueError("副官原生资源压缩头无效：" + name)
    expected = _u32(packed, 8)
    if not 0 < expected <= min(MAX_RESOURCE, expanded_limit):
        raise ValueError("副官原生资源展开长度无效：" + name)
    decoder = zlib.decompressobj(31)
    try:
        raw = decoder.decompress(packed[12:], expected + 1)
    except zlib.error as error:
        raise ValueError("副官原生资源压缩内容无效：" + name) from error
    if (
        len(raw) != expected
        or not decoder.eof
        or decoder.unused_data
        or decoder.unconsumed_tail
    ):
        raise ValueError("副官原生资源展开边界无效：" + name)
    return raw


def validate_bundle(
    operator_id, config, index, archive, *, include_payload=False,
    expanded_limit=MAX_EXPANDED,
):
    """Authenticate one package bundle and return its bounded inventory."""
    operator_id = _operator_id(operator_id)
    if (
        isinstance(expanded_limit, bool)
        or not isinstance(expanded_limit, int)
        or not 0 < expanded_limit <= MAX_EXPANDED
    ):
        raise ValueError("副官原生资源展开预算无效")
    values = _parse_config(operator_id, config)
    if not isinstance(index, bytes) or not isinstance(archive, bytes):
        raise ValueError("副官原生资源索引或归档不是字节内容")
    if not 24 <= len(index) <= MAX_INDEX:
        raise ValueError("副官原生资源索引长度无效")
    if not archive or len(archive) >= MAX_ARCHIVE:
        raise ValueError("单个副官原生资源归档必须小于 64 MiB")
    if hashlib.sha256(index).hexdigest() != values["index_sha256"]:
        raise ValueError("副官原生资源索引 SHA-256 不匹配")
    if hashlib.sha256(archive).hexdigest() != values["archive_sha256"]:
        raise ValueError("副官原生资源归档 SHA-256 不匹配")

    version, chunk_size, chunk_count, reserved0, count, reserved1 = struct.unpack_from(
        "<6I", index
    )
    if (
        version != 1
        or chunk_size != CHUNK_SIZE
        or reserved0
        or reserved1
        or not 1 <= count <= MAX_ENTRIES
        or len(archive) != chunk_count * PHYSICAL_CHUNK_SIZE
    ):
        raise ValueError("副官原生资源索引头或归档边界无效")
    for number in range(chunk_count):
        next_chunk, previous_chunk = struct.unpack_from(
            "<II", archive, number * PHYSICAL_CHUNK_SIZE
        )
        if next_chunk != number + 1 or previous_chunk != max(0, number - 1):
            raise ValueError("副官原生资源归档区块链无效")

    cursor = 24
    next_chunk = 0
    entries = []
    names = set()
    payload = {} if include_payload else None
    expanded = 0
    for _entry in range(count):
        name, cursor = _decode_path(index, cursor)
        canonical, _kind = _resource_key(operator_id, name)
        if canonical != name or name in names or cursor + 36 > len(index):
            raise ValueError("副官原生资源索引包含重复或非规范身份")
        metadata = struct.unpack_from("<9I", index, cursor)
        cursor += 36
        stored, first, chunks, *reserved = metadata
        if (
            not 0 < stored <= MAX_RESOURCE
            or first != next_chunk
            or not chunks
            or any(reserved)
            or stored > chunks * CHUNK_SIZE
            or first + chunks > chunk_count
        ):
            raise ValueError("副官原生资源索引区块范围无效")
        packed = b"".join(
            archive[(number * PHYSICAL_CHUNK_SIZE) + 8:
                    (number + 1) * PHYSICAL_CHUNK_SIZE]
            for number in range(first, first + chunks)
        )[:stored]
        raw = _decode_resource(packed, name, expanded_limit - expanded)
        expanded += len(raw)
        row = {
            "path": name,
            "stored_bytes": stored,
            "first_chunk": first,
            "chunks": chunks,
            "bytes": len(raw),
            "sha256": hashlib.sha256(raw).hexdigest(),
        }
        entries.append(row)
        if include_payload:
            payload[name] = raw
        names.add(name)
        next_chunk += chunks
    if cursor != len(index) or next_chunk != chunk_count:
        raise ValueError("副官原生资源索引含尾随数据或未声明区块")
    ordered = sorted(names)
    if [entry["path"] for entry in entries] != ordered:
        raise ValueError("副官原生资源索引未按规范路径排序")
    layout = _layout(operator_id, names)
    result = {
        "operator_id": operator_id,
        "layout": layout,
        "entries": entries,
        "expanded_bytes": expanded,
        "index_sha256": values["index_sha256"],
        "archive_sha256": values["archive_sha256"],
    }
    if include_payload:
        result["resources"] = payload
    return result
