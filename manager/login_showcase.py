"""Configure the native login-screen unit chooser from the installed client catalog."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import struct
from urllib.parse import parse_qs, urlsplit

from . import storage
from .providers import Provider


MOD_ID = "login_showcase"
CATALOG_MOD_ID = "unit_catalog"
FIRST_INDEX_ID = 0x375BE1
MAX_INDEX_ROWS = 20000
MAX_EXPLICIT_UNITS = 2048
MODES = {"grades", "all", "recent", "units"}
GRADES = tuple(
    rank + suffix
    for rank in ("C", "B", "A", "S")
    for suffix in ("", "S", "R", "U")
)
GRADE_FIELDS = {
    (rank, subgrade): GRADES[(rank - 1) * 4 + subgrade - 2]
    for rank in range(1, 5)
    for subgrade in range(2, 6)
}


def _name(raw: bytes, identity: int) -> str:
    if len(raw) != 4976 or struct.unpack_from("<I", raw)[0] != identity or raw[4] != 0:
        raise ValueError("机体定义身份或长度无效")
    field = raw[5:105]
    end = next((offset for offset in range(0, len(field), 2) if field[offset:offset + 2] == b"\0\0"), None)
    if end is None:
        raise ValueError("机体名称没有结束标记")
    try:
        value = field[:end].decode("utf-16-le", errors="strict").strip()
    except UnicodeDecodeError:
        raise ValueError("机体名称编码无效") from None
    if not value or any(ord(character) < 32 for character in value):
        raise ValueError("机体名称无效")
    return value


def _installed_records(path: Path) -> dict[tuple[int, int], bytes]:
    """Read and verify the exact container accepted by the native unit-catalog Mod."""
    from owner.units.compiler import CATALOG_RECORD_LIMIT, native_catalog

    raw = path.read_bytes()
    if len(raw) < 32 or raw[:8] != b"SDGOUN1\0":
        raise ValueError("已安装机体目录容器无效")
    version, header_bytes, count, payload_bytes, _, reserved = struct.unpack_from("<6I", raw, 8)
    if (
        version != 1
        or header_bytes != 32
        or count > CATALOG_RECORD_LIMIT
        or payload_bytes > 64 * 1024 * 1024
        or reserved != 0
        or len(raw) != header_bytes + payload_bytes
    ):
        raise ValueError("已安装机体目录布局无效")
    cursor = header_bytes
    rows: list[tuple[int, int, bytes]] = []
    result: dict[tuple[int, int], bytes] = {}
    for _ in range(count):
        if cursor + 24 > len(raw):
            raise ValueError("已安装机体目录记录头截断")
        kind, identity, extent, _, flags, entry_reserved = struct.unpack_from("<6I", raw, cursor)
        cursor += 24
        if cursor + extent > len(raw) or flags != 0 or entry_reserved != 0:
            raise ValueError("已安装机体目录记录无效")
        record = raw[cursor:cursor + extent]
        cursor += extent
        key = (kind, identity)
        if key in result:
            raise ValueError("已安装机体目录记录重复")
        result[key] = record
        rows.append((kind, identity, record))
    if cursor != len(raw) or not rows or native_catalog(rows) != raw:
        raise ValueError("已安装机体目录校验失败")
    return result


def _catalog_enabled(workspace, host) -> bool:
    installed = {item["id"]: item for item in workspace._installed(host, True)}
    item = installed.get(CATALOG_MOD_ID)
    return bool(item and item.get("runtime_enabled", item.get("enabled", False)))


def client_catalog(workspace, host) -> tuple[list[dict], str]:
    """Return the same eligible roots and ordering used by the native chooser."""
    if host.get("role") != "client":
        raise ValueError("登录页面机体只能配置客户端")
    client = Path(host["path"]).resolve()
    if not (client / "GOnline.exe").is_file():
        raise ValueError("客户端目录缺少 GOnline.exe")

    from owner.units.assets import Baseline

    baseline = Baseline(client)
    indexes = dict(baseline.unit_index())
    additions: dict[tuple[int, int], bytes] = {}
    catalog_path = workspace._mod_directory(host, CATALOG_MOD_ID) / "catalog.bin"
    if _catalog_enabled(workspace, host) and catalog_path.is_file():
        additions = _installed_records(catalog_path)
        for (kind, identity), raw in additions.items():
            if kind == 85:
                previous = indexes.get(identity)
                if previous is not None and previous != raw:
                    raise ValueError("机体图鉴编号与原生目录冲突")
                indexes[identity] = raw
            elif kind == 2:
                previous = baseline.record("unit", identity)
                if previous is not None and previous != raw:
                    raise ValueError("机体编号与原生定义冲突")

    found: dict[int, tuple[dict, bytes, bytes]] = {}
    for catalog_id in range(FIRST_INDEX_ID, FIRST_INDEX_ID + MAX_INDEX_ROWS):
        index = indexes.get(catalog_id)
        if index is None:
            continue
        if (
            len(index) != 614
            or struct.unpack_from("<I", index)[0] != catalog_id
            or index[4] != 0
            or index[5] != 1
        ):
            continue
        unit_id = struct.unpack_from("<I", index, 6)[0]
        definition = additions.get((2, unit_id))
        if definition is None:
            definition = baseline.record("unit", unit_id)
        if definition is None:
            continue
        name = _name(definition, unit_id)
        rank, subgrade = definition[308], definition[309]
        grade = GRADE_FIELDS.get((rank, subgrade))
        if grade is None:
            continue
        item = {
            "id": unit_id,
            "name": name,
            "grade": grade,
            "rank": rank,
            "subgrade": subgrade,
            "catalog_id": catalog_id,
        }
        previous = found.get(unit_id)
        if previous is None or catalog_id > previous[0]["catalog_id"]:
            found[unit_id] = (item, index, definition)

    ordered = sorted(
        found.values(),
        key=lambda entry: (-entry[0]["catalog_id"], entry[0]["id"]),
    )
    digest = hashlib.sha256()
    digest.update(b"sdgo.login-showcase.catalog.v1\0")
    for item, index, definition in ordered:
        digest.update(struct.pack("<II", item["catalog_id"], item["id"]))
        digest.update(hashlib.sha256(index).digest())
        digest.update(hashlib.sha256(definition).digest())
    return [entry[0] for entry in ordered], digest.hexdigest()


def _settings(values: dict) -> dict:
    grades = [part.strip().upper() for part in str(values.get("grades", "")).split(",") if part.strip()]
    unit_ids = []
    text = str(values.get("unit_ids", "")).strip()
    if text:
        try:
            unit_ids = [int(part.strip()) for part in text.split(",")]
        except ValueError:
            unit_ids = []
    return {
        "enabled": values.get("enabled") is True,
        "mode": values.get("mode", "grades"),
        "grades": grades,
        "recent_count": values.get("recent_count", 10),
        "unit_ids": unit_ids,
    }


def _validate_settings(value, eligible_ids: set[int]) -> tuple[dict, dict]:
    keys = {"enabled", "mode", "grades", "recent_count", "unit_ids"}
    if not isinstance(value, dict) or set(value) != keys:
        raise ValueError("登录展示设置字段不完整或含未登记字段")
    if type(value["enabled"]) is not bool:
        raise ValueError("启用状态无效")
    if not isinstance(value["mode"], str) or value["mode"] not in MODES:
        raise ValueError("机体选择方式无效")
    grades = value["grades"]
    if (
        not isinstance(grades, list)
        or not grades
        or len(grades) > len(GRADES)
        or any(not isinstance(grade, str) or grade not in GRADES for grade in grades)
        or len(set(grades)) != len(grades)
    ):
        raise ValueError("请至少选择一个有效等级")
    recent = value["recent_count"]
    if type(recent) is not int or not 1 <= recent <= MAX_INDEX_ROWS:
        raise ValueError("最近注册机体数量须为 1 到 20000 的整数")
    unit_ids = value["unit_ids"]
    if (
        not isinstance(unit_ids, list)
        or len(unit_ids) > MAX_EXPLICIT_UNITS
        or any(type(identity) is not int or identity <= 0 for identity in unit_ids)
        or len(set(unit_ids)) != len(unit_ids)
    ):
        raise ValueError("指定机体名单无效或超过 2048 台")
    unknown = [identity for identity in unit_ids if identity not in eligible_ids]
    if unknown:
        raise ValueError("指定机体不在当前已安装图鉴中：" + "、".join(map(str, unknown[:12])))
    if value["mode"] == "units" and not unit_ids:
        raise ValueError("指定机体模式至少需要选择一台机体")

    canonical_grades = [grade for grade in GRADES if grade in grades]
    public = {
        "enabled": value["enabled"],
        "mode": value["mode"],
        "grades": canonical_grades,
        "recent_count": recent,
        "unit_ids": unit_ids,
    }
    stored = {
        "enabled": value["enabled"],
        "mode": value["mode"],
        "grades": ",".join(canonical_grades),
        "recent_count": recent,
        "unit_ids": ",".join(map(str, unit_ids)),
    }
    return public, stored


def _selection(settings: dict, catalog: list[dict]) -> dict:
    by_id = {item["id"]: item for item in catalog}
    if settings["mode"] == "units":
        selected = [by_id[identity] for identity in settings["unit_ids"] if identity in by_id]
    elif settings["mode"] == "grades":
        wanted = set(settings["grades"])
        selected = [item for item in catalog if item["grade"] in wanted]
    elif settings["mode"] == "recent":
        selected = catalog[: settings["recent_count"]]
    else:
        selected = catalog
    return {"count": len(selected), "preview": selected[:24]}


class LoginShowcase(Provider):
    id, title = MOD_ID, "登录页面机体展示"
    fields = {}

    def describe(self):
        return {
            "id": self.id,
            "title": self.title,
            "fields": {},
            "config": {},
            "configured": bool(self._clients()),
            "audience": "user",
            "embedded_configuration": True,
        }

    def configure(self, values):
        if values:
            raise ValueError("登录页面机体展示在同一页面选择客户端和展示方式")
        return self.describe()

    def _clients(self):
        result = []
        for host in self.workspace.settings.get("hosts", []):
            if host.get("role") != "client":
                continue
            if not (self.workspace._mod_directory(host, MOD_ID) / "mod.ini").is_file():
                continue
            result.append({"id": host["id"], "name": host.get("name") or Path(host["path"]).name})
        return result

    def _host(self, host_id):
        clients = self._clients()
        if not clients:
            raise ValueError("请先添加已经安装此功能的游戏客户端")
        identity = host_id or clients[0]["id"]
        if not isinstance(identity, str) or identity not in {item["id"] for item in clients}:
            raise ValueError("请选择有效的客户端")
        host = self.workspace.host(identity)
        root = self.workspace._mod_directory(host, MOD_ID)
        if not (root / "mod.ini").is_file():
            raise ValueError("所选客户端尚未安装登录页面机体展示功能")
        return host

    def _public(self, host_id=None, details=None, catalog=None, catalog_revision=None):
        clients = self._clients()
        if not clients:
            return {
                "clients": [],
                "host_id": None,
                "selection_error": "请先添加已经安装此功能的游戏客户端。",
            }
        host = self._host(host_id)
        if details is None:
            details = self.workspace.details(host["id"], MOD_ID)
        if catalog is None or catalog_revision is None:
            catalog, catalog_revision = client_catalog(self.workspace, host)
        settings = _settings(details["values"])
        return {
            "clients": clients,
            "host_id": host["id"],
            "settings": settings,
            "revisions": details["revisions"],
            "runtime": details["runtime"],
            "catalog_revision": catalog_revision,
            "catalog_count": len(catalog),
            "catalog": catalog,
            "selection": _selection(settings, catalog),
        }

    def request(self, method, uri, body, headers):
        parsed = urlsplit(uri)
        if parsed.path != "/api/login-showcase":
            raise ValueError("登录页面机体展示操作未登记")
        if method == "GET":
            host_id = parse_qs(parsed.query).get("host_id", [None])[0]
            return self._public(host_id)
        if method != "POST":
            raise ValueError("登录页面机体展示操作未登记")
        if not isinstance(body, dict) or set(body) != {"host_id", "settings", "revisions", "catalog_revision"}:
            raise ValueError("登录页面机体展示保存请求无效")
        host = self._host(body["host_id"])
        with self.workspace._change_lease(), storage.LOCK:
            catalog, catalog_revision = client_catalog(self.workspace, host)
            if body["catalog_revision"] != catalog_revision:
                raise storage.Conflict("机体目录已改变，请刷新后重新选择")
            public, stored = _validate_settings(body["settings"], {item["id"] for item in catalog})
            details = self.workspace.configure(
                host["id"],
                MOD_ID,
                stored,
                body["revisions"],
            )
            result = self._public(
                host["id"],
                details=details,
                catalog=catalog,
                catalog_revision=catalog_revision,
            )
            result["settings"] = public
            result["selection"] = _selection(public, catalog)
            result["saved"] = True
            result["activation"] = details["activation"]
            return result
