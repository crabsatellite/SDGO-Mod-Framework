"""Paired OC/GCO mission-behavior configuration for client and Battle hosts."""
from __future__ import annotations

import base64
import binascii
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import zipfile

from . import distribution, storage
from .providers import Binary, Provider


CLIENT_MOD = "mission_behavior"
SERVER_MOD = "mission_behavior_server"
FEATURE = "mission-behavior"
PROFILES = {"oc", "gco"}
PROFILE_LIMIT = 8 * 1024 * 1024


def decode_profile(raw: bytes):
    """Use the shared public codec so Manager and both native components agree."""
    from .engines.mission_behavior.profile_codec import decode_bundle

    return decode_bundle(raw)


def _profile_metadata(raw: bytes) -> dict:
    if not raw or len(raw) > PROFILE_LIMIT:
        raise ValueError("任务配置文件大小无效")
    decoded = decode_profile(raw)
    if not isinstance(decoded, dict):
        raise ValueError("任务配置文件元数据无效")
    server_rows = decoded.get(
        "entry_count",
        decoded.get("server_delta_count"),
    )
    if (
        isinstance(server_rows, bool)
        or not isinstance(server_rows, int)
        or server_rows <= 0
    ):
        raise ValueError("OC 任务配置没有可应用的服务端任务差异")
    result = {
        "sha256": hashlib.sha256(raw).hexdigest(),
        "bytes": len(raw),
    }
    # Keep the ordinary UI useful without exposing the source rows or private paths.
    for key in (
        "version",
        "format_version",
        "bindings_generation",
        "entry_count",
        "mission_count",
        "server_delta_count",
        "client_override_count",
        "label",
    ):
        value = decoded.get(key)
        if type(value) in (str, int) and len(str(value)) <= 128:
            result[key] = value
    return result


class MissionBehavior(Provider):
    id, title = "mission_behavior", "任务行为"
    fields = {}

    def describe(self):
        return {
            "id": self.id,
            "title": self.title,
            "fields": {},
            "config": {},
            "configured": True,
            "audience": "user",
            "embedded_configuration": True,
        }

    def configure(self, values):
        if values:
            raise ValueError("任务行为使用同一页面选择客户端、服务端组和行为版本")
        return self.describe()

    @property
    def bundle_path(self) -> Path:
        return self.workspace.root / "data/mission_behavior/profile.bin"

    @staticmethod
    def _component(role):
        if role == "client":
            return CLIENT_MOD, "GOnline.exe"
        if role == "battle":
            return SERVER_MOD, "BattleServer.exe"
        raise ValueError("任务行为目标类型无效")

    def _target(self, host_id, role):
        host = self.workspace.host(host_id)
        if host.get("role") != role:
            raise ValueError("任务行为目标类型不匹配")
        mod_id, executable = self._component(role)
        root = self.workspace._mod_directory(host, mod_id)
        manifest_path = root / "mod.ini"
        if not manifest_path.is_file():
            raise ValueError("所选目标尚未安装配套任务行为组件")
        manifest_raw = manifest_path.read_bytes()
        manifest = storage.ini_values(manifest_raw)
        if (
            manifest.get("mod", "id", fallback="") != mod_id
            or manifest.get("mod", "feature", fallback="") != FEATURE
        ):
            raise ValueError("所选目标的任务行为组件身份无效")
        global_path = Path(host["path"]) / "sdgo/sdgo.ini"
        global_raw = global_path.read_bytes() if global_path.is_file() else b""
        global_ini = storage.ini_values(global_raw)
        enabled = manifest.getint("mod", "enabled", fallback=1) != 0
        for section, key in (
            ("categories", manifest.get("mod", "category", fallback="gameplay")),
            ("features", FEATURE),
            ("mods", mod_id),
        ):
            enabled = enabled and global_ini.get(section, key, fallback="1").strip() != "0"
        return {
            "host": host,
            "root": root,
            "mod_id": mod_id,
            "executable": Path(host["path"]) / executable,
            "enabled": enabled,
            "manifest_revision": storage.revision(manifest_raw),
            "global_revision": storage.revision(global_raw),
        }

    def _available(self):
        clients = []
        battles = []
        for host in self.workspace.settings.get("hosts", []):
            role = host.get("role")
            if role not in ("client", "battle"):
                continue
            try:
                self._target(host["id"], role)
            except (ValueError, FileNotFoundError):
                continue
            record = {"id": host["id"], "name": host.get("name") or role}
            (clients if role == "client" else battles).append(record)
        clients.sort(key=lambda item: (item["name"].casefold(), item["id"]))

        grouped = {}
        for record in battles:
            host = self.workspace.host(record["id"])
            parent = Path(host["path"]).resolve().parent
            key = hashlib.sha256(os.path.normcase(str(parent)).encode("utf-8")).hexdigest()[:16]
            grouped.setdefault(
                key,
                {
                    "id": key,
                    "name": parent.name or "Battle",
                    "hosts": [],
                },
            )["hosts"].append(record)
        groups = []
        for group in grouped.values():
            group["hosts"].sort(key=lambda item: (item["name"].casefold(), item["id"]))
            group["host_ids"] = [item["id"] for item in group["hosts"]]
            group["label"] = f"{group['name']} · 全部 {len(group['hosts'])} 个战斗频道"
            groups.append(group)
        groups.sort(key=lambda item: (item["label"].casefold(), item["id"]))
        return clients, groups

    def _selection(self, requested=None):
        clients, groups = self._available()
        saved = self.config
        explicit = requested is not None
        requested = requested or {}
        client_host = requested.get("client_host", saved.get("client_host"))
        if client_host not in {item["id"] for item in clients}:
            client_host = clients[0]["id"] if len(clients) == 1 else None

        battle_hosts = requested.get("battle_hosts", saved.get("battle_hosts", []))
        if not isinstance(battle_hosts, list) or any(not isinstance(item, str) for item in battle_hosts):
            raise ValueError("战斗服务器组选择无效")
        selected = next(
            (group for group in groups if set(group["host_ids"]) == set(battle_hosts)),
            None,
        )
        if selected is None and not explicit and saved.get("battle_group"):
            selected = next(
                (group for group in groups if group["id"] == saved["battle_group"]),
                None,
            )
        if selected is None and len(groups) == 1 and (not battle_hosts or not explicit):
            selected = groups[0]
        battle_hosts = selected["host_ids"] if selected else battle_hosts

        profile = requested.get("profile", saved.get("profile", "oc"))
        if profile not in PROFILES:
            raise ValueError("任务行为只能选择 OC 或 G改")
        return {
            "client_host": client_host,
            "battle_hosts": battle_hosts,
            "battle_group": selected["id"] if selected else None,
            "profile": profile,
            "clients": clients,
            "battle_groups": groups,
        }

    def _validate_selection(self, selection):
        if not selection["client_host"]:
            raise ValueError("请选择已安装任务行为组件的客户端")
        group = next(
            (
                item
                for item in selection["battle_groups"]
                if item["id"] == selection["battle_group"]
                and item["host_ids"] == selection["battle_hosts"]
            ),
            None,
        )
        if group is None or not group["host_ids"]:
            raise ValueError("请选择完整的战斗服务器组，不能只更新其中一个频道")
        client = self._target(selection["client_host"], "client")
        battles = [self._target(host_id, "battle") for host_id in group["host_ids"]]
        return client, battles

    @staticmethod
    def _read_profile(path):
        raw = path.read_bytes() if path.is_file() else b""
        if not raw:
            return raw, None, None
        try:
            return raw, _profile_metadata(raw), None
        except (ValueError, KeyError, TypeError) as error:
            return raw, None, str(error)

    def _read_target(self, target):
        config_path = target["root"] / "config.ini"
        profile_path = target["root"] / "profile.bin"
        config_raw = config_path.read_bytes() if config_path.is_file() else b""
        try:
            value = storage.ini_values(config_raw).get(
                "settings", "Profile", fallback="oc"
            ).strip().casefold()
        except (ValueError, UnicodeError):
            value = "invalid"
        profile_raw, metadata, error = self._read_profile(profile_path)
        return {
            "id": target["host"]["id"],
            "name": target["host"].get("name") or target["host"]["role"],
            "role": target["host"]["role"],
            "config_path": config_path,
            "config_raw": config_raw,
            "config_revision": storage.revision(config_raw),
            "profile": value if value in PROFILES else "invalid",
            "profile_path": profile_path,
            "profile_raw": profile_raw,
            "profile_revision": storage.revision(profile_raw),
            "profile_metadata": metadata,
            "profile_error": error,
            "running": self._is_running(target),
            "enabled": target["enabled"],
            "manifest_revision": target["manifest_revision"],
            "global_revision": target["global_revision"],
        }

    @staticmethod
    def _is_running(target):
        if os.name != "nt":
            return False
        from .windows import process_image_paths

        expected = target["executable"].resolve()
        return any(
            Path(value).resolve() == expected
            for value in process_image_paths(expected.name)
            if value
        )

    def _manager_bundle(self):
        raw, metadata, error = self._read_profile(self.bundle_path)
        return {
            "raw": raw,
            "revision": storage.revision(raw),
            "metadata": metadata,
            "error": error,
        }

    @staticmethod
    def _source_bundle(manager_bundle, targets):
        if manager_bundle["metadata"]:
            return manager_bundle["raw"], manager_bundle["metadata"], "manager"
        present = [item for item in targets if item["profile_raw"]]
        if not present or any(item["profile_metadata"] is None for item in present):
            return None, None, None
        digests = {item["profile_metadata"]["sha256"] for item in present}
        if len(digests) != 1:
            return None, None, None
        return present[0]["profile_raw"], present[0]["profile_metadata"], "installed"

    def _preview(self, requested=None):
        selection = self._selection(requested)
        client = battles = None
        target_rows = []
        selection_error = None
        try:
            client, battles = self._validate_selection(selection)
            target_rows = [self._read_target(client), *[self._read_target(item) for item in battles]]
        except ValueError as error:
            selection_error = str(error)

        manager_bundle = self._manager_bundle()
        source_raw, source_metadata, source_kind = self._source_bundle(manager_bundle, target_rows)
        transaction = {
            "workspace": storage.revision(
                self.workspace.path.read_bytes() if self.workspace.path.is_file() else b""
            ),
            "manager_bundle": manager_bundle["revision"],
            "client_host": selection["client_host"],
            "battle_hosts": selection["battle_hosts"],
            "targets": [
                {
                    "id": item["id"],
                    "config": item["config_revision"],
                    "profile": item["profile_revision"],
                    "manifest": item["manifest_revision"],
                    "global": item["global_revision"],
                }
                for item in target_rows
            ],
        }
        transaction_revision = storage.revision(
            json.dumps(transaction, sort_keys=True, separators=(",", ":")).encode("utf-8")
        )
        public_targets = []
        for item in target_rows:
            row = {
                key: item[key]
                for key in (
                    "id",
                    "name",
                    "role",
                    "config_revision",
                    "profile",
                    "profile_revision",
                    "profile_metadata",
                    "running",
                    "enabled",
                )
            }
            if self.workspace.mode == "developer" and item["profile_error"]:
                row["profile_error"] = item["profile_error"]
            public_targets.append(row)

        profiles = {item["profile"] for item in target_rows}
        invalid_bundles = any(item["profile_raw"] and not item["profile_metadata"] for item in target_rows)
        bundle_hashes = {
            item["profile_metadata"]["sha256"] if item["profile_metadata"] else None
            for item in target_rows
        }
        ready = selection_error is None and all(item["enabled"] for item in public_targets) and (
            selection["profile"] == "gco" or source_metadata is not None
        )
        return {
            "selection": {
                "client_host": selection["client_host"],
                "battle_hosts": selection["battle_hosts"],
                "battle_group": selection["battle_group"],
                "profile": selection["profile"],
            },
            "clients": selection["clients"],
            "battle_groups": selection["battle_groups"],
            "targets": public_targets,
            "selection_error": selection_error,
            "manager_bundle": {
                "revision": manager_bundle["revision"],
                "ready": manager_bundle["metadata"] is not None,
                "metadata": manager_bundle["metadata"],
                **(
                    {"error": manager_bundle["error"]}
                    if self.workspace.mode == "developer" and manager_bundle["error"]
                    else {}
                ),
            },
            "source_bundle": {
                "ready": source_metadata is not None,
                "source": source_kind,
                "metadata": source_metadata,
            },
            "consistent": {
                "profile": len(profiles) <= 1 and "invalid" not in profiles,
                "bundle": not invalid_bundles and len(bundle_hashes) <= 1,
            },
            "ready_to_apply": ready and not any(item["running"] for item in public_targets),
            "transaction_revision": transaction_revision,
            "activation": "restart_required",
            "scope": {
                "default": "oc",
                "keeps_current_tasks": True,
                "fallback_for_unverified_modes": "gco",
                "changes_rewards": False,
                "writes_database": False,
            },
            "_source_raw": source_raw,
            "_target_rows": target_rows,
        }

    @staticmethod
    def _public(preview):
        return {key: value for key, value in preview.items() if not key.startswith("_")}

    @staticmethod
    def _restore(path, raw):
        if raw is None:
            path.unlink(missing_ok=True)
        else:
            storage.atomic(path, raw)

    def _import(self, body):
        encoded = body.get("profile")
        if not isinstance(encoded, str) or len(encoded) > PROFILE_LIMIT * 2:
            raise ValueError("任务配置文件无效")
        try:
            raw = base64.b64decode(encoded, validate=True)
        except (ValueError, TypeError, binascii.Error) as error:
            raise ValueError("任务配置文件编码无效") from error
        metadata = _profile_metadata(raw)
        with storage.LOCK:
            previous = self.bundle_path.read_bytes() if self.bundle_path.is_file() else b""
            if body.get("expected_revision") != storage.revision(previous):
                raise storage.Conflict("任务配置文件已被其他操作改变，请刷新后重试")
            storage.atomic(self.bundle_path, raw)
        result = self._public(self._preview())
        result.update(imported=True, imported_profile=metadata)
        return result

    def _apply(self, body):
        requested = {
            "client_host": body.get("client_host"),
            "battle_hosts": body.get("battle_hosts"),
            "profile": body.get("profile"),
        }
        if requested["profile"] not in PROFILES:
            raise ValueError("任务行为只能选择 OC 或 G改")
        with storage.LOCK:
            preview = self._preview(requested)
            if preview["selection_error"]:
                raise ValueError(preview["selection_error"])
            if body.get("expected_revision") != preview["transaction_revision"]:
                raise storage.Conflict("任务行为配置或安装目标已改变，请刷新后重试")
            running = [item["name"] for item in preview["targets"] if item["running"]]
            if running:
                raise ValueError("请先关闭这些目标再切换任务行为：" + "、".join(running))
            disabled = [item["name"] for item in preview["targets"] if not item["enabled"]]
            if disabled:
                raise ValueError("请先启用这些配套任务行为组件：" + "、".join(disabled))
            source = preview["_source_raw"]
            if requested["profile"] == "oc" and source is None:
                raise ValueError("缺少服主提供的有效 OC 任务配置，未修改任何客户端或服务器")
            client, battles = self._validate_selection(self._selection(requested))
            if any(self._is_running(target) for target in (client, *battles)):
                raise ValueError("目标刚刚开始运行，请关闭客户端和全部战斗频道后重试")

            candidates = {}
            for row in preview["_target_rows"]:
                candidates[row["config_path"]] = (
                    row["config_raw"] if row["config_path"].is_file() else None,
                    storage.patch_ini(
                        row["config_raw"],
                        [("settings", "Profile", requested["profile"])],
                    ),
                )
                if source is not None:
                    candidates[row["profile_path"]] = (
                        row["profile_raw"] if row["profile_path"].is_file() else None,
                        source,
                    )
                elif requested["profile"] == "gco" and row["profile_raw"]:
                    candidates[row["profile_path"]] = (row["profile_raw"], None)

            settings = copy.deepcopy(self.workspace.settings)
            settings.setdefault("modules", {})[self.id] = {
                "client_host": preview["selection"]["client_host"],
                "battle_hosts": list(preview["selection"]["battle_hosts"]),
                "battle_group": preview["selection"]["battle_group"],
                "profile": requested["profile"],
            }
            workspace_raw = self.workspace.path.read_bytes() if self.workspace.path.is_file() else None
            candidates[self.workspace.path] = (workspace_raw, storage.json_bytes(settings))

            written = []
            try:
                for path, (old, new) in candidates.items():
                    if old == new:
                        continue
                    if new is None:
                        path.unlink(missing_ok=True)
                    else:
                        storage.atomic(path, new)
                    written.append(path)
            except Exception:
                for path in reversed(written):
                    self._restore(path, candidates[path][0])
                raise
            self.workspace.settings = settings

        result = self._public(self._preview(requested))
        result.update(
            saved=True,
            activation="restart_required",
            restart_roles=["client", "battle"],
        )
        return result

    def _export_client(self, body):
        requested = {
            "client_host": body.get("client_host"),
            "battle_hosts": body.get("battle_hosts"),
            "profile": body.get("profile"),
        }
        with storage.LOCK:
            preview = self._preview(requested)
            if preview["selection_error"]:
                raise ValueError(preview["selection_error"])
            if body.get("expected_revision") != preview["transaction_revision"]:
                raise storage.Conflict("任务行为配置或安装目标已改变，请刷新后重试")
            client = self._target(requested["client_host"], "client")
            self.workspace.require_redistributable(requested["client_host"], [CLIENT_MOD])
            row = preview["_target_rows"][0]
            if (
                not preview["consistent"]["profile"]
                or not preview["consistent"]["bundle"]
                or any(
                    item["profile"] != requested["profile"]
                    for item in preview["_target_rows"]
                )
            ):
                raise ValueError("请先把所选行为同步应用到客户端和全部战斗频道")
            if any(not item["enabled"] for item in preview["_target_rows"]):
                raise ValueError("请先启用客户端和全部战斗频道的配套任务行为组件")
            if requested["profile"] == "oc" and any(
                item["profile_metadata"] is None
                for item in preview["_target_rows"]
            ):
                raise ValueError("客户端或战斗频道缺少有效 OC 任务配置")

            required = (
                "mission_behavior.dll",
                "mod.ini",
                "manager.json",
                "config.ini",
                "symbols.fragment.ini",
            )
            payload = {}
            for name in required:
                path = client["root"] / name
                if not path.is_file():
                    raise ValueError("客户端任务行为组件不完整：" + name)
                payload[f"client/sdgo/mods/{CLIENT_MOD}/{name}"] = path.read_bytes()
            if row["profile_raw"]:
                payload[f"client/sdgo/mods/{CLIENT_MOD}/profile.bin"] = row["profile_raw"]
            fragment = payload[
                f"client/sdgo/mods/{CLIENT_MOD}/symbols.fragment.ini"
            ]
            payload["symbols.fragment.ini"] = fragment

            manifest = storage.ini_values(payload[f"client/sdgo/mods/{CLIENT_MOD}/mod.ini"])
            scope = distribution.resolve(
                payload[f"client/sdgo/mods/{CLIENT_MOD}/mod.ini"],
                payload[f"client/sdgo/mods/{CLIENT_MOD}/manager.json"],
            )
            if scope != distribution.PUBLIC:
                raise ValueError("任务行为客户端组件不可公开分发")
            release = {
                "schema_version": 1,
                "feature": FEATURE,
                "profile": requested["profile"],
                "component_version": manifest.get("mod", "version", fallback=""),
                "profile_sha256": (
                    row["profile_metadata"]["sha256"] if row["profile_metadata"] else None
                ),
                "files": {
                    name: hashlib.sha256(raw).hexdigest()
                    for name, raw in sorted(payload.items())
                },
            }
            payload["manifest.json"] = storage.json_bytes(release)
            payload["README.txt"] = (
                "SDGO mission behavior player package / 任务行为玩家配置包\n"
                "Close the game, import this ZIP with SDGO Mod Manager, then restart normally.\n"
                "关闭游戏，用 SDGO Mod Manager 导入本 ZIP，再照常启动。\n"
                f"Selected profile / 所选行为: {requested['profile']}\n"
                "The server owner must use the same profile on every Battle channel.\n"
                "服主必须在全部 Battle 频道使用同一行为和配置。\n"
            ).encode("utf-8")

            output = io.BytesIO()
            with zipfile.ZipFile(
                output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
            ) as archive:
                for name, raw in sorted(payload.items()):
                    archive.writestr(name, raw)
        return Binary(
            output.getvalue(),
            "application/zip",
            "SDGO-mission-behavior-client.zip",
        )

    def request(self, method, path, body, headers):
        if method == "GET" and path == "/api/mission-behavior":
            return self._public(self._preview())
        if method == "POST" and path == "/api/mission-behavior/preview":
            return self._public(self._preview(body))
        if method == "POST" and path == "/api/mission-behavior/import":
            return self._import(body)
        if method == "POST" and path == "/api/mission-behavior/apply":
            return self._apply(body)
        if method == "POST" and path == "/api/mission-behavior/export-client":
            return self._export_client(body)
        raise ValueError("任务行为操作未登记")
