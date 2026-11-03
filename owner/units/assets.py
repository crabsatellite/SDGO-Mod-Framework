"""Bounded, read-only access to the current SDGO client resource baseline."""
from __future__ import annotations

import functools
import hashlib
import io
import json
import math
import os
from pathlib import Path
import re
import stat
import struct
from typing import Any
import zlib

from . import codec


UNIT_FILES = ("0237", "0242", "0243", "0244", "0246", "0245")
WEAPON_FILES = ("0067", "0068", "0107", "0130", "0157", "0164", "0200", "0238", "0274")
ITEM_FILES = ("0031",)
RECORD_FILES = {"unit": (UNIT_FILES, codec.UNIT_SIZE), "weapon": (WEAPON_FILES, codec.WEAPON_SIZE), "item": (ITEM_FILES, 614),
                "unit_index": (("0220",),614)}
RUNTIME_TYPES = {"unit": 2, "weapon": 10, "item": 34, "unit_index":85}

MAX_RESOURCE_BYTES = 64 * 1024 * 1024
MAX_INDEX_BYTES = 16 * 1024 * 1024
MAX_GDT_RECORDS = 1_000_000
MAX_GDT_SIZE = 1024 * 1024
MAX_LOOSE_GDT_FILES = 2048
_RESOURCE_RE = re.compile(
    r"^(?:gdrs|mdrs|anrs|txrs|fxrs|sprs|sdrs|scp)/"
    r"(?:[a-z0-9][a-z0-9_.-]*/)*[a-z0-9][a-z0-9_.-]*$",
    re.IGNORECASE,
)


def _resource_path(value: str) -> str:
    if not isinstance(value, str):
        raise ValueError("resource path must be text")
    value = value.replace("\\", "/").lower()
    if (
        len(value) > 240
        or ":" in value
        or not _RESOURCE_RE.fullmatch(value)
        or any(part in ("", ".", "..") for part in value.split("/"))
    ):
        raise ValueError("invalid resource path")
    return value


def _is_reparse(path: Path) -> bool:
    try:
        value = path.lstat()
    except OSError:
        return False
    return bool(
        stat.S_ISLNK(value.st_mode)
        or getattr(value, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    )


def _decode_gdt(data: bytes, expected_size: int | None = None) -> list[bytes]:
    if len(data) < 14 or data[:2] != b"\xf9\xf8":
        raise ValueError("unsupported GDT header")
    size, count, extra = struct.unpack_from("<iii", data, 2)
    if (
        not 1 <= size <= MAX_GDT_SIZE
        or expected_size is not None
        and size != expected_size
        or not 0 <= count <= MAX_GDT_RECORDS
        or extra < 0
    ):
        raise ValueError("unsupported GDT record layout")
    key_size = (size + count + extra) % 65 + 192
    key = data[14 : 14 + key_size]
    stride = ((size + key_size - 1) // key_size) * key_size
    if (
        len(key) != key_size
        or len(set(key)) != key_size
        or set(key) != set(range(key_size))
        or len(data) != 14 + key_size + count * stride
    ):
        raise ValueError("truncated or invalid GDT")
    mask = [key[(0x300 + index * 3) % key_size] for index in range(key_size)]
    rows: list[bytes] = []
    for record_index in range(count):
        start = 14 + key_size + record_index * stride
        decoded = bytearray()
        for offset in range(0, stride, key_size):
            block = data[start + offset : start + offset + key_size]
            needed = min(key_size, size - len(decoded))
            decoded.extend(block[key[index]] ^ mask[index] for index in range(needed))
        rows.append(bytes(decoded))
    return rows


def _image_to_png(raw: bytes, *, require_png: bool, crop: tuple[float, float, float, float] | None = None) -> bytes:
    try:
        from PIL import Image, UnidentifiedImageError
    except ImportError:
        raise ValueError("PNG normalization needs Pillow") from None
    if not raw or len(raw) > MAX_RESOURCE_BYTES:
        raise ValueError("image exceeds the read limit")
    try:
        with Image.open(io.BytesIO(raw)) as probe:
            if require_png and probe.format != "PNG":
                raise ValueError("please upload a PNG image")
            if getattr(probe, "n_frames", 1) != 1:
                raise ValueError("animated images are not supported")
            width, height = probe.size
            if not 1 <= width <= 8192 or not 1 <= height <= 8192 or width * height > 16_777_216:
                raise ValueError("image dimensions exceed the decode limit")
            probe.verify()
        with Image.open(io.BytesIO(raw)) as image:
            image.load()
            if crop is not None:
                u0, v0, u1, v1 = crop
                if not all(math.isfinite(value) and 0 <= value <= 1 for value in crop) or u1 <= u0 or v1 <= v0:
                    raise ValueError("invalid portrait crop")
                box = (
                    round(u0 * image.width),
                    round(v0 * image.height),
                    round(u1 * image.width),
                    round(v1 * image.height),
                )
                image = image.crop(box)
            if not 1 <= image.width <= 2048 or not 1 <= image.height <= 2048:
                raise ValueError("normalized image dimensions must be 1..2048 pixels")
            target = image.convert("RGBA" if "A" in image.getbands() or "transparency" in image.info else "RGB")
            output = io.BytesIO()
            target.save(output, format="PNG", optimize=False)
            value = output.getvalue()
            if len(value) > 8 * 1024 * 1024:
                raise ValueError("normalized PNG exceeds 8 MiB")
            return value
    except ValueError:
        raise
    except (UnidentifiedImageError, OSError, SyntaxError):
        raise ValueError("invalid or truncated image") from None


def normalize_png(raw: bytes) -> bytes:
    """Verify an uploaded PNG and return a metadata-free normalized PNG."""
    if not isinstance(raw, bytes) or len(raw) > 8 * 1024 * 1024 or not raw.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("please upload a PNG no larger than 8 MiB")
    return _image_to_png(raw, require_png=True)


def sprite_binary(texture_virtual_path: str, width: int, height: int) -> bytes:
    """Build the verified 176-byte SPR v10000 wrapper used by managed previews."""
    path = _resource_path(texture_virtual_path)
    if not path.startswith("txrs/") or not path.endswith(".txr"):
        raise ValueError("SPR texture must be a txrs/*.txr resource")
    if type(width) is not int or type(height) is not int or not 1 <= width <= 2048 or not 1 <= height <= 2048:
        raise ValueError("SPR dimensions must be integers in 1..2048")
    encoded = path.replace("/", "\\").encode("utf-16-le")
    if len(encoded) >= 66 * 2:
        raise ValueError("SPR texture path exceeds the native field")
    output = bytearray(176)
    struct.pack_into("<5I4f", output, 0, 10000, 1, 20, height, width, 0.0, 0.0, 1.0, 1.0)
    output[44 : 44 + 132] = encoded.ljust(132, b"\0")
    return bytes(output)


def render_portrait(virtual_path: str, read: Any) -> bytes:
    """Render an SPR/TXR through a caller-supplied package/baseline resolver.

    The resolver receives normalized virtual paths.  This lets the authoring UI
    preview package-owned sprites before anything is installed.
    """
    if not callable(read):
        raise ValueError("portrait resolver must be callable")
    path = _resource_path(virtual_path)
    data = read(path)
    if not isinstance(data, bytes):
        raise ValueError("portrait resolver returned a non-bytes resource")
    if path.endswith(".txr"):
        texture = data[12:] if data.startswith(b"ZOATEX1\0") and len(data) >= 12 else data
        return _image_to_png(texture, require_png=False)
    if not path.endswith(".spr") or len(data) < 44:
        raise ValueError("portrait must reference a supported SPR or TXR")
    version = struct.unpack_from("<I", data, 0)[0]
    if version not in (10000, 11000):
        raise ValueError("unsupported portrait SPR version")
    crop = struct.unpack_from("<4f", data, 20)
    textures = [
        reference
        for reference in codec.scan_resource_references(data, ".spr")
        if reference.startswith("txrs/")
    ]
    if not textures:
        raise ValueError("portrait SPR has no texture reference")
    texture = read(textures[0])
    if not isinstance(texture, bytes):
        raise ValueError("portrait resolver returned a non-bytes texture")
    if texture.startswith(b"ZOATEX1\0"):
        if len(texture) < 12:
            raise ValueError("truncated ZOATEX texture")
        texture = texture[12:]
    return _image_to_png(texture, require_png=False, crop=crop)


class Baseline:
    """Read existing client assets without changing the game archive or loose files."""

    covered_types = dict(RUNTIME_TYPES)

    def __init__(self, client: Path | None, snapshot: Path | None = None):
        self.root = Path(client).resolve() if client is not None else None
        self.snapshot = Path(snapshot).resolve() if snapshot is not None else None
        self._loose_roots = [root for root in (self.root, self.snapshot if self.snapshot and self.snapshot.is_dir() else None) if root]
        self.entries: dict[str, tuple[int, int, int]] = {}
        self.index_sha256: str | None = None
        self.index_total_count = 0
        self.chunk_size: int | None = None
        self._zpk: Path | None = None
        self._any_ids: set[int] | None = None
        self._any_id_tables: int | None = None
        self._any_id_skipped: int | None = None
        if self.root is not None:
            index_path = self.root / "GPackData.zdx"
            package_path = self.root / "GPackData.zpk"
            if not index_path.is_file() or not package_path.is_file():
                raise ValueError("client baseline is missing GPackData.zdx/zpk")
            if index_path.stat().st_size > MAX_INDEX_BYTES:
                raise ValueError("resource index exceeds the read limit")
            index = index_path.read_bytes()
            self.index_sha256 = hashlib.sha256(index).hexdigest()
            self._parse_index(index)
            self._zpk = package_path

    def _parse_index(self, data: bytes) -> None:
        if len(data) < 24:
            raise ValueError("truncated game resource index")
        version, chunk_size, _, _, count, _ = struct.unpack_from("<6I", data, 0)
        if version != 1 or not 0 < chunk_size <= 1024 * 1024 or count > 1_000_000:
            raise ValueError("unsupported game resource index")
        position = 24
        self.index_total_count = count
        entries: dict[str, tuple[int, int, int]] = {}
        for _ in range(count):
            characters: list[str] = []
            while True:
                if position + 2 > len(data):
                    raise ValueError("truncated game resource name")
                code = struct.unpack_from("<H", data, position)[0] ^ 0xAA
                position += 2
                if code == 0:
                    break
                if code < 32 or code > 126 or len(characters) >= 240:
                    raise ValueError("unsupported game resource name")
                characters.append(chr(code))
            if position + 36 > len(data):
                raise ValueError("truncated game resource entry")
            size, first, chunks = struct.unpack_from("<III", data, position)
            position += 36
            raw_name = "".join(characters).replace("\\", "/").lower()
            try:
                name = _resource_path(raw_name)
            except ValueError:
                # The importer deliberately exposes only the bounded game roots.
                continue
            if name in entries:
                raise ValueError(f"duplicate game resource path {name}")
            if size > MAX_RESOURCE_BYTES or chunks > 200_000:
                raise ValueError(f"game resource {name} exceeds the read limit")
            entries[name] = (size, first, chunks)
        self.entries = entries
        self.chunk_size = chunk_size

    def _loose(self, name: str) -> Path | None:
        for root in self._loose_roots:
            candidate = root.joinpath(*name.split("/"))
            if not candidate.is_file():
                continue
            resolved = candidate.resolve()
            try:
                resolved.relative_to(root)
            except ValueError:
                raise ValueError("loose resource escapes the configured baseline") from None
            cursor = root
            for part in name.split("/"):
                cursor = cursor / part
                if _is_reparse(cursor):
                    raise ValueError("loose resource uses a reparse point")
            if candidate.stat().st_size > MAX_RESOURCE_BYTES:
                raise ValueError("loose resource exceeds the read limit")
            return candidate
        return None

    def exists(self, virtual_path: str) -> bool:
        name = _resource_path(virtual_path)
        return self._loose(name) is not None or name in self.entries

    def owns_unchanged_resource(self,virtual_path: str) -> bool:
        """An installed, receipt-owned loose resource may receive a new version.

        Archive originals, borrowed files and external edits stay protected.
        The trusted installer repeats the check when applying the package.
        """
        name=_resource_path(virtual_path)
        if self.root is None or name in self.entries:return False
        receipt=self.root/'sdgo/mods/unit_catalog/install-receipt.json'
        if not receipt.is_file() or receipt.stat().st_size>2*1024*1024:return False
        try:data=json.loads(receipt.read_text('utf-8-sig'))
        except (ValueError,OSError):return False
        if (not isinstance(data,dict) or not isinstance(data.get('files'),list) or
                data.get('module')!='unit_catalog' or data.get('schema_version')!=1 or
                data.get('host')!='GOnline-54BE1568-00C97000' or
                data.get('host_sha256')!='e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92'):
            return False
        rows=[entry for entry in data.get('files',[]) if isinstance(entry,dict) and
              str(entry.get('path','')).replace('\\','/').lower()==name]
        if len(rows)!=1 or rows[0].get('owned') is not True:return False
        file=self._loose(name)
        return file is not None and hashlib.sha256(file.read_bytes()).hexdigest()==rows[0].get('installed_sha256')

    @functools.lru_cache(maxsize=256)
    def read(self, virtual_path: str) -> bytes:
        name = _resource_path(virtual_path)
        loose = self._loose(name)
        if loose is not None:
            data = loose.read_bytes()
        else:
            entry = self.entries.get(name)
            if entry is None or self._zpk is None or self.chunk_size is None:
                raise FileNotFoundError(name)
            size, cursor, chunks = entry
            result = bytearray()
            seen: set[int] = set()
            package_size = self._zpk.stat().st_size
            with self._zpk.open("rb") as package:
                for _ in range(chunks):
                    if cursor in seen:
                        raise ValueError(f"{name}: resource chunk cycle")
                    seen.add(cursor)
                    offset = cursor * (self.chunk_size + 8)
                    if offset < 0 or offset + self.chunk_size + 8 > package_size:
                        raise ValueError(f"{name}: resource chunk is outside GPackData.zpk")
                    package.seek(offset)
                    block = package.read(self.chunk_size + 8)
                    if len(block) != self.chunk_size + 8:
                        raise ValueError(f"{name}: truncated resource chunk")
                    cursor = struct.unpack_from("<I", block, 0)[0]
                    result.extend(block[8:])
                    if len(result) > MAX_RESOURCE_BYTES + self.chunk_size:
                        raise ValueError(f"{name}: resource exceeds the read limit")
            if len(result) < size:
                raise ValueError(f"{name}: resource chain is shorter than its index size")
            data = bytes(result[:size])
        if data.startswith(b"ZOAGZIP1"):
            if len(data) < 12:
                raise ValueError(f"{name}: truncated ZOAGZIP header")
            expected = struct.unpack_from("<I", data, 8)[0]
            if expected > MAX_RESOURCE_BYTES:
                raise ValueError(f"{name}: decompressed resource exceeds the read limit")
            decoder = zlib.decompressobj(31)
            expanded = decoder.decompress(data[12:], expected + 1)
            if len(expanded) != expected or not decoder.eof or decoder.unconsumed_tail:
                raise ValueError(f"{name}: compressed resource validation failed")
            data = expanded
        if len(data) > MAX_RESOURCE_BYTES:
            raise ValueError(f"{name}: resource exceeds the read limit")
        return data

    @functools.lru_cache(maxsize=32)
    def _records(self, virtual_path: str, expected_size: int) -> dict[int, bytes]:
        rows: dict[int, bytes] = {}
        for raw in _decode_gdt(self.read(virtual_path), expected_size):
            identity = struct.unpack_from("<I", raw, 0)[0]
            if not 1 <= identity <= codec.MAX_NATIVE_ID:
                raise ValueError(f"{virtual_path}: invalid native record id {identity}")
            previous = rows.get(identity)
            if previous is not None and previous != raw:
                raise ValueError(f"{virtual_path}: conflicting record id {identity}")
            rows[identity] = raw
        return rows

    @functools.lru_cache(maxsize=4096)
    def record(self, kind: str, identity: int) -> bytes | None:
        if kind not in RECORD_FILES:
            raise ValueError("record kind must be unit, weapon or item")
        if type(identity) is not int or not 1 <= identity <= codec.MAX_NATIVE_ID:
            raise ValueError("invalid native record id")
        files, size = RECORD_FILES[kind]
        result: bytes | None = None
        for filename in files:
            path = f"gdrs/{filename}.gdt"
            if not self.exists(path):
                continue
            row = self._records(path, size).get(identity)
            if row is None:
                continue
            if kind in ("unit", "weapon") and row[4] != 0:
                continue
            if result is not None and result != row:
                raise ValueError(f"baseline has conflicting {kind} record {identity}")
            result = row
        return result

    def unit_index(self) -> dict[int, bytes]:
        """The original encyclopedia enumerates this table until its first gap."""
        rows=self._records('gdrs/0220.gdt',614)
        first=0x375BE1
        if not rows or set(rows)!=set(range(first,first+len(rows))):
            raise ValueError('原生机体一览目录不连续，不能安全添加图鉴条目')
        return rows

    def _gdt_paths(self) -> list[str]:
        paths = {name for name in self.entries if name.startswith("gdrs/") and name.endswith(".gdt")}
        for root in self._loose_roots:
            folder = root / "gdrs"
            if not folder.is_dir() or _is_reparse(folder):
                continue
            count = 0
            for candidate in folder.rglob("*.gdt"):
                count += 1
                if count > MAX_LOOSE_GDT_FILES:
                    raise ValueError("too many loose GDT files for native-id coverage")
                if not candidate.is_file() or _is_reparse(candidate):
                    raise ValueError("loose GDT coverage contains a reparse entry")
                relative = candidate.relative_to(root).as_posix().lower()
                paths.add(_resource_path(relative))
        if len(paths) > MAX_LOOSE_GDT_FILES:
            raise ValueError("too many GDT files for native-id coverage")
        return sorted(paths)

    def _load_any_ids(self) -> None:
        identities: set[int] = set()
        paths = self._gdt_paths()
        total = 0
        skipped = 0
        for path in paths:
            data = self.read(path)
            # Some indexed .gdt resources use other native table codecs.  Keep
            # this precheck useful for every F9F8 table and report that coverage
            # as incomplete; the runtime native-tree check remains authoritative.
            if data[:2] != b"\xf9\xf8":
                skipped += 1
                continue
            try:
                rows = _decode_gdt(data)
            except ValueError:
                skipped += 1
                continue
            for row in rows:
                total += 1
                if total > 5_000_000:
                    raise ValueError("native-id coverage exceeds the record limit")
                identity = struct.unpack_from("<I", row, 0)[0]
                if identity:
                    identities.add(identity)
        self._any_ids = identities
        self._any_id_tables = len(paths)
        self._any_id_skipped = skipped

    def any_id(self, identity: int) -> bool:
        """Conservatively check key use across every readable gdrs/*.gdt table."""
        if type(identity) is not int or not 1 <= identity <= 0xFFFFFFFF:
            raise ValueError("invalid native grid id")
        if self._any_ids is None:
            self._load_any_ids()
        return identity in self._any_ids

    @functools.lru_cache(maxsize=128)
    def portrait(self, virtual_path: str) -> bytes:
        return render_portrait(virtual_path, self.read)

    def public_info(self) -> dict[str, Any]:
        """Return non-secret baseline facts suitable for UI diagnostics."""
        coverage: dict[str, Any] = {
            "strict_record_types": dict(self.covered_types),
            "all_gdrs_ids_scanned": self._any_ids is not None,
        }
        if self._any_ids is not None:
            coverage.update(
                table_count=self._any_id_tables,
                scanned_f9f8_table_count=(self._any_id_tables or 0) - (self._any_id_skipped or 0),
                skipped_other_codec_table_count=self._any_id_skipped,
                complete=self._any_id_skipped == 0,
                distinct_ids=len(self._any_ids),
            )
        return {
            "configured": bool(self.root or self.snapshot),
            "client_configured": self.root is not None,
            "archive_configured": self.root is not None,
            "snapshot_configured": self.snapshot is not None,
            "index_sha256": self.index_sha256,
            "indexed_resource_count": self.index_total_count,
            "bounded_indexed_resource_count": len(self.entries),
            "native_id_coverage": coverage,
        }
