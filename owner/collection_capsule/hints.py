"""Keep the native catalog explanation in step with the live collection pool.

The client marks catalog (图鉴) entries as obtainable from the collection
machine from ``collection-acquisition.bin`` in the shared capsule registry.
That list is derived here from the installed draw pool after exclusions, and
the registry's declared hash and manifest are rewritten together. Programmes,
tables, archives and every other registry file are left untouched.
"""
from __future__ import annotations

import configparser
import hashlib
import json
import os
from pathlib import Path
import struct

from .acquisition import FILE, encode
from .rules import validate

REGISTRY = "sdgo/mods/capsule_catalog"


def _sha(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def decode(raw: bytes) -> list[int]:
    if len(raw) < 20 or raw[:8] != b"SDGOACQ1":
        raise ValueError("图鉴获取说明文件格式无效")
    count = struct.unpack_from("<I", raw, 8)[0]
    if not 0 < count <= 20000 or len(raw) < 20 + 4 * count:
        raise ValueError("图鉴获取说明文件格式无效")
    return list(struct.unpack_from("<%dI" % count, raw, 20))


def live_rules(db) -> dict:
    """Use the installed machine's own price and quota, not an unsaved Manager draft."""
    row = db.cursor().execute("SELECT PriceGP,LimitPeriod,DrawLimit,UtcOffsetMinutes "
                              "FROM sdgo_collection_capsule.State WHERE Singleton=1").fetchone()
    if row is None:
        raise ValueError("目标数据库没有安装收藏扭蛋机台")
    return validate({"price_gp": int(row[0]), "limit_period": str(row[1]).strip(),
                     "draw_limit": int(row[2]), "utc_offset_minutes": int(row[3])})


def _runtime(root: Path):
    path = root / "runtime.ini"
    if not path.is_file():
        return None, None
    ini = configparser.ConfigParser(interpolation=None)
    ini.read(path, encoding="utf-8-sig")
    return path, ini


def installed(client) -> dict | None:
    """Return the client's declared explanation, or None when it carries none."""
    root = Path(client) / REGISTRY
    path, ini = _runtime(root)
    if path is None or not ini.has_section("acquisition"):
        return None
    if ini.get("acquisition", "file", fallback="") != FILE:
        raise ValueError("客户端的图鉴获取说明声明无效")
    raw = (root / FILE).read_bytes()
    declared = ini.get("acquisition", "sha256", fallback="")
    return {"roots": decode(raw), "raw": raw, "consistent": _sha(raw) == declared}


def _atomic(path: Path, raw: bytes):
    temporary = path.with_name(path.name + ".sdgo-pending")
    with open(temporary, "wb") as stream:
        stream.write(raw)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def _set_declared_hash(text: str, value: str) -> str:
    lines, section, done = text.splitlines(), None, False
    for index, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1].strip().lower()
        elif section == "acquisition" and stripped.split("=", 1)[0].strip().lower() == "sha256":
            lines[index] = "sha256=" + value
            done = True
    if not done:
        raise ValueError("客户端扭蛋注册表缺少图鉴获取说明摘要")
    return "\n".join(lines) + "\n"


def sync(client, roots, rules, *, client_running=None) -> dict:
    """Rewrite the installed explanation when the pool changed.

    ``client_running`` raises when the target game is open; a running client
    keeps its current files and the caller reports the pending change.
    """
    roots = sorted(set(roots))
    if not roots:
        raise ValueError("收藏扭蛋没有可抽取的机体，不能生成图鉴获取说明")
    current = installed(client)
    if current is None:
        return {"status": "not_installed", "roots": len(roots)}
    raw, metadata = encode(roots, [], rules)
    added = sorted(set(roots) - set(current["roots"]))
    removed = sorted(set(current["roots"]) - set(roots))
    summary = {"roots": len(roots), "added": added, "removed": removed}
    if raw == current["raw"] and current["consistent"]:
        return {"status": "current", **summary}
    if client_running is not None:
        try:
            client_running(Path(client))
        except Exception as error:  # the capsule deployment's own running-client check
            return {"status": "client_running", "message": str(error), **summary}
    root = Path(client) / REGISTRY
    runtime_path, _ = _runtime(root)
    manifest_path = root / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig")) if manifest_path.is_file() else None
    digest = _sha(raw)
    runtime = _set_declared_hash(runtime_path.read_text(encoding="utf-8-sig"), digest).encode("ascii")
    # The declared hash is written last; an interrupted refresh leaves a valid
    # list whose declaration the next refresh repairs from the same pool.
    _atomic(root / FILE, raw)
    if manifest is not None:
        manifest["collection_acquisition"] = metadata
        if isinstance(manifest.get("files"), dict):
            manifest["files"][FILE] = digest
        _atomic(manifest_path, (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    _atomic(runtime_path, runtime)
    after = installed(client)
    if after is None or after["raw"] != raw or not after["consistent"]:
        raise RuntimeError("图鉴获取说明写入后读回不一致")
    return {"status": "updated", "sha256": digest, **summary}
