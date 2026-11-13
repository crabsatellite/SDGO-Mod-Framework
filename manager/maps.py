"""Map and mission import behind the Manager's session: register map packages, edit how each map is listed,
build one client package + server plan, install the client package and apply the server rows."""
from __future__ import annotations

import hashlib
import json
import shutil
import sys
import zipfile
from pathlib import Path, PurePosixPath
from urllib.parse import quote

from .providers import Provider, Binary

# Public examples bundled into the standalone Manager. The package is copied into a versioned, verified cache on
# first view; it is not registered, compiled, installed or applied until the owner chooses the matching actions.
EXAMPLES = {
    'twin-valley-fortress': {
        'name': {'zh': '双谷要塞 · 奥德萨作战', 'en': 'Twin Valley Fortress · Operation Odessa'},
        'description': 'OPEN 默认任务包：4 档难度、8 人进场、分难度 HP、友军撤退与发射井倒计时终局。',
        'path': 'examples/maps/twin-valley-fortress',
        'bundled': 'examples/maps/twin-valley-fortress',
        'map_json': 'd884a6a969f90c84bf2cdd81b817ec267c6e50ab00ce7f313bc8034e67a4a024',
        'open_files': ['OPEN-ME.md', 'map.json', 'missions/odessa.json', 'scripts/add_mission.py', 'CREDITS.md'],
        'default': True,
    },
}
EXAMPLE_FILE_LIMIT = 16 * 1024 * 1024
EXAMPLE_TOTAL_LIMIT = 96 * 1024 * 1024


def _is_link(path: Path) -> bool:
    return path.is_symlink() or bool(getattr(path, 'is_junction', lambda: False)())


def _regular_file_under(root: Path, name: str) -> Path | None:
    """Return a regular package file only when no package path component is a link."""
    root = Path(root)
    try:
        relative = _example_path(name)
        if _is_link(root):
            return None
        current = root
        for part in PurePosixPath(relative).parts:
            current /= part
            if _is_link(current):
                return None
        if not current.is_file():
            return None
        current.resolve(strict=True).relative_to(root.resolve(strict=True))
        return current
    except (OSError, RuntimeError, ValueError):
        return None


def _preserve_changed_cache(path: Path) -> Path | None:
    """Move an unverified cache aside before restoring the canonical bundled copy."""
    if not path.exists() and not _is_link(path):
        return None
    index = 1
    while True:
        candidate = path.with_name(f'{path.name}.changed-{index}')
        if not candidate.exists() and not _is_link(candidate):
            path.replace(candidate)
            return candidate
        index += 1


def bundled_example_datas(repository_root: Path) -> list[tuple[str, str]]:
    """Return only map.json and its reviewed, digest-pinned members for PyInstaller."""
    rows = []
    for spec in EXAMPLES.values():
        bundled = spec.get('bundled')
        if not bundled:
            continue
        source = Path(repository_root) / Path(*PurePosixPath(_example_path(spec['path'])).parts)
        if not _example_is_verified(spec, source):
            raise ValueError('内置示例地图文件清单校验失败')
        manifest_path = _regular_file_under(source, 'map.json')
        manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
        destination_root = PurePosixPath(_example_path(bundled))
        rows.append((str(manifest_path), str(destination_root)))
        for name in sorted(manifest['files']):
            relative = _example_path(name)
            path = _regular_file_under(source, relative)
            if path is None:
                raise ValueError('内置示例地图缺少文件：' + relative)
            destination = destination_root / PurePosixPath(relative).parent
            rows.append((str(path), str(destination)))
    return rows


def _bundled_root() -> Path:
    return Path(getattr(sys, '_MEIPASS', Path(__file__).resolve().parents[1]))


def _example_is_verified(spec: dict, target: Path) -> bool:
    try:
        manifest_path = _regular_file_under(target, 'map.json')
        if manifest_path is None:
            return False
        manifest_raw = manifest_path.read_bytes()
        if hashlib.sha256(manifest_raw).hexdigest() != spec['map_json']:
            return False
        files = json.loads(manifest_raw.decode('utf-8')).get('files')
        if not isinstance(files, dict) or not files:
            return False
        total = 0
        for name, digest in sorted(files.items()):
            relative = _example_path(name)
            path = _regular_file_under(target, relative)
            if path is None:
                return False
            raw = path.read_bytes()
            total += len(raw)
            if (len(raw) > EXAMPLE_FILE_LIMIT or total > EXAMPLE_TOTAL_LIMIT
                    or hashlib.sha256(raw).hexdigest() != digest):
                return False
        return bool(files)
    except (OSError, TypeError, ValueError, UnicodeDecodeError, json.JSONDecodeError):
        return False


def _copy_example(spec: dict, source: Path, target: Path) -> Path:
    """Copy one complete hash-checked public example into a Manager-owned cache."""
    manifest_path = _regular_file_under(source, 'map.json')
    if manifest_path is None:
        raise ValueError('内置示例地图 map.json 不是普通文件')
    manifest_raw = manifest_path.read_bytes()
    if hashlib.sha256(manifest_raw).hexdigest() != spec['map_json']:
        raise ValueError('内置示例地图 map.json 校验失败')
    files = json.loads(manifest_raw.decode('utf-8')).get('files')
    if not isinstance(files, dict) or not files:
        raise ValueError('内置示例地图没有文件清单')
    staging = target.with_name(target.name + '.bundled')
    _preserve_changed_cache(staging)
    total = 0
    for name, digest in sorted(files.items()):
        relative = _example_path(name)
        path = _regular_file_under(source, relative)
        if path is None:
            raise ValueError('内置示例地图缺少文件：' + relative)
        raw = path.read_bytes()
        if hashlib.sha256(raw).hexdigest() != digest:
            raise ValueError('内置示例地图文件校验失败：' + relative)
        total += len(raw)
        if len(raw) > EXAMPLE_FILE_LIMIT or total > EXAMPLE_TOTAL_LIMIT:
            raise ValueError('内置示例地图超过大小限制')
        out = staging / relative
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(raw)
    (staging / 'map.json').write_bytes(manifest_raw)
    if target.exists() or _is_link(target):
        raise ValueError('内置示例地图缓存目标已存在')
    staging.replace(target)
    return target


def ensure_example(example_id: str, root: Path) -> Path:
    """Materialize a bundled default without registering or applying it."""
    spec = EXAMPLES.get(example_id)
    if not spec:
        raise ValueError('没有这个示例地图')
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    if _is_link(root):
        raise ValueError('内置示例地图缓存目录不能是链接')
    target = root / (example_id + '-' + spec['map_json'][:12])
    if _example_is_verified(spec, target):
        return target
    bundled = spec.get('bundled')
    if not bundled:
        return fetch_example(example_id, root)
    _preserve_changed_cache(target)
    source = _bundled_root() / Path(*PurePosixPath(_example_path(bundled)).parts)
    return _copy_example(spec, source, target)


def _example_source(example_id: str, root: Path) -> tuple[dict, Path]:
    spec = EXAMPLES.get(example_id)
    if not spec:
        raise ValueError('没有这个示例地图')
    return spec, ensure_example(example_id, root)


def _example_path(name) -> str:
    path = PurePosixPath(name) if isinstance(name, str) else None
    if (path is None or not name or '\\' in name or path.is_absolute() or ':' in name
            or any(part in ('', '.', '..') for part in path.parts)):
        raise ValueError('示例地图包含无效文件路径')
    return str(path)


def fetch_example(example_id: str, root: Path, fetch=None, repository: str | None = None) -> Path:
    """Download one example map package into root/<id> and verify it; unchanged files already there are kept."""
    from . import updates
    spec = EXAMPLES.get(example_id)
    if not spec:
        raise ValueError('没有这个示例地图')
    if fetch is None and spec.get('bundled'):
        return ensure_example(example_id, root)
    fetch = fetch or updates.fetch
    if not spec.get('commit'):
        raise ValueError('示例地图没有远程下载版本')
    base = 'https://raw.githubusercontent.com/%s/%s/%s/' % (repository or updates.REPOSITORY, spec['commit'], spec['path'])
    manifest_raw = fetch(base + 'map.json', EXAMPLE_FILE_LIMIT)
    if hashlib.sha256(manifest_raw).hexdigest() != spec['map_json']:
        raise ValueError('示例地图 map.json 校验失败')
    files = json.loads(manifest_raw.decode('utf-8')).get('files')
    if not isinstance(files, dict) or not files:
        raise ValueError('示例地图没有文件清单')
    target = Path(root) / example_id
    staging = Path(root) / (example_id + '.download')
    if staging.exists():
        shutil.rmtree(staging)
    total = 0
    for name, digest in sorted(files.items()):
        relative = _example_path(name)
        cached = target / relative
        if cached.is_file() and hashlib.sha256(cached.read_bytes()).hexdigest() == digest:
            raw = cached.read_bytes()
        else:
            raw = fetch(base + quote(relative), EXAMPLE_FILE_LIMIT)
            if hashlib.sha256(raw).hexdigest() != digest:
                raise ValueError('示例地图文件校验失败：' + relative)
        total += len(raw)
        if total > EXAMPLE_TOTAL_LIMIT:
            raise ValueError('示例地图超过大小限制')
        out = staging / relative
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(raw)
    (staging / 'map.json').write_bytes(manifest_raw)
    if target.exists():
        shutil.rmtree(target)
    staging.replace(target)
    return target


def _text(body, key, limit=1024):
    value = body.get(key) if isinstance(body, dict) else None
    if not isinstance(value, str) or not value.strip() or len(value) > limit or '\0' in value:
        raise ValueError('请求参数无效：' + key)
    return value.strip()


class Maps(Provider):
    id, title = 'maps', '地图与任务导入'
    fields = {'workspace': '地图工作区目录（留空使用 Manager 数据目录）',
              'client': '配套客户端目录（编译基准与安装目标）',
              'server_ini': 'WorldServer INI（留空只生成离线发布物）'}

    def describe(self):
        # Bundled source browsing needs no client or server configuration.
        # Build/install/apply keep their own explicit target validation.
        return {**super().describe(), 'panel_without_configuration': True}

    def _maps(self):
        from owner.maps.workspace import MapWorkspace
        return MapWorkspace(self.config.get('workspace') or self.workspace.root / 'data/maps')

    def _client(self):
        client = self.config.get('client')
        if not client or not (Path(client) / 'GOnline.exe').is_file():
            raise ValueError('请先配置配套客户端目录')
        return Path(client)

    def _server_ini(self):
        ini = self.config.get('server_ini')
        if not ini:
            raise ValueError('请先配置 WorldServer INI')
        return ini

    def _publication(self):
        return self._maps().root / 'publication'

    def _plan(self):
        from owner.maps import server
        path = self._publication() / 'server-plan.json'
        if not path.is_file():
            raise ValueError('还没有生成发布物，请先编译')
        return server.load_plan(path)

    def _state(self):
        maps = self._maps()
        state = maps.describe()
        report = self._publication() / 'workspace-build.json'
        state['publication'] = json.loads(report.read_text(encoding='utf-8')) if report.is_file() else None
        receipt = Path(self.config['client']) / 'sdgo/mods/map_catalog/install-receipt.json' \
            if self.config.get('client') else None
        state['client_receipt'] = json.loads(receipt.read_text(encoding='utf-8')) \
            if receipt and receipt.is_file() else None
        examples = []
        for key, spec in EXAMPLES.items():
            source = ensure_example(key, maps.root / 'examples') if spec.get('default') else None
            package_files = sorted(json.loads((source / 'map.json').read_text(encoding='utf-8'))['files']) \
                if source else []
            examples.append({'id': key, 'name': spec['name'], 'description': spec['description'],
                             'default': bool(spec.get('default')), 'cached': bool(source),
                             'source_files': list(spec.get('open_files', [])),
                             'package_files': package_files})
        state['examples'] = examples
        client_package = self._publication() / 'SDGO-MapImport-Client.zip'
        state['publication_files'] = []
        state['publication_previews'] = []
        if client_package.is_file():
            with zipfile.ZipFile(client_package) as archive:
                state['publication_files'] = sorted(archive.namelist())
            for name in state['publication_files']:
                try:
                    safe_name = _example_path(name)
                except ValueError:
                    continue
                if (safe_name == 'manifest.json' or safe_name.endswith('/definitions.json')
                        or safe_name.endswith('/missions.lua')):
                    state['publication_previews'].append(safe_name)
        state['server_configured'] = bool(self.config.get('server_ini'))
        state['client_configured'] = bool(self.config.get('client'))
        state['changes_applied'] = False
        return state

    def request(self, method, path, body, headers):
        from owner.maps import server
        from owner.maps.deployment import install_package
        with self.lock:
            if method == 'GET' and path == '/api/maps':
                return self._state()
            if method == 'GET' and path == '/api/maps/client-package':
                raw = (self._publication() / 'SDGO-MapImport-Client.zip').read_bytes()
                return Binary(raw, 'application/zip', 'SDGO-MapImport-Client.zip')
            if method != 'POST':
                raise ValueError('地图导入操作未登记')
            if path == '/api/maps/add':
                entry = self._maps().add(_text(body, 'folder'))
                return {'map': entry, 'state': self._state(),
                        'message': '地图包已登记。编译、安装客户端包并应用服务端后，重启服务端和客户端生效。'}
            if path == '/api/maps/publication/file':
                relative = _example_path(_text(body, 'file', 200))
                state = self._state()
                if relative not in state['publication_previews']:
                    raise ValueError('这个发布文件不可预览')
                with zipfile.ZipFile(self._publication() / 'SDGO-MapImport-Client.zip') as archive:
                    info = archive.getinfo(relative)
                    if info.file_size > 512 * 1024:
                        raise ValueError('发布文件过大，不能在 Manager 中预览')
                    raw = archive.read(info)
                try:
                    text = raw.decode('utf-8-sig')
                except UnicodeDecodeError:
                    raise ValueError('发布文件不是 UTF-8 文本') from None
                return {'file': relative, 'text': text}
            if path == '/api/maps/examples/file':
                maps = self._maps()
                example_id = _text(body, 'id', 80)
                relative = _example_path(_text(body, 'file', 200))
                spec, folder = _example_source(example_id, maps.root / 'examples')
                if relative not in spec.get('open_files', []):
                    raise ValueError('这个示例文件不可预览')
                raw = (folder / relative).read_bytes()
                if len(raw) > 256 * 1024:
                    raise ValueError('示例文件过大，不能在 Manager 中预览')
                try:
                    text = raw.decode('utf-8-sig')
                except UnicodeDecodeError:
                    raise ValueError('示例文件不是 UTF-8 文本') from None
                return {'file': relative, 'text': text}
            if path == '/api/maps/examples/add':
                maps = self._maps()
                folder = ensure_example(_text(body, 'id', 80), maps.root / 'examples')
                entry = maps.add(folder)
                return {'map': entry, 'state': self._state(),
                        'message': '默认示例副本已登记。此步骤未编译、安装、写数据库或启用任务；后续操作仍需分别执行。'}
            if path == '/api/maps/update':
                values = body.get('values') if isinstance(body, dict) else None
                entry = self._maps().update(_text(body, 'key', 80), values)
                return {'map': entry, 'state': self._state()}
            if path == '/api/maps/remove':
                result = self._maps().remove(_text(body, 'key', 80))
                return {'result': result, 'state': self._state(),
                        'message': '已从工作区移除。重新编译并应用后，游戏内对应地图和任务行会被撤下。'}
            if path == '/api/maps/build':
                output = self._publication()
                lines = []
                report = self._maps().build(self._client(), self._server_ini(), output, log=lines.append)
                return {'report': report, 'log': lines, 'state': self._state(), 'changes_applied': False}
            if path == '/api/maps/install-client':
                raw = (self._publication() / 'SDGO-MapImport-Client.zip').read_bytes()
                result = install_package(self._client(), raw)
                return {'result': result, 'state': self._state(), 'changes_applied': True,
                        'message': '客户端地图包已安装，重启客户端生效。'}
            if path == '/api/maps/server/preflight':
                from owner.maps.workspace import OWNER_KEY
                plan = self._plan()
                summary = server.preflight(plan, self._server_ini(), OWNER_KEY)
                summary.pop('_changes', None)
                summary['runtime_policy'] = server.runtime_policy_state(plan, self._server_ini())
                return {'preflight': summary, 'changes_applied': False}
            if path == '/api/maps/server/apply':
                from owner.maps.workspace import OWNER_KEY
                plan = self._plan()
                result = server.apply(plan, self._server_ini(), OWNER_KEY)
                runtime = server.apply_runtime_policy(plan, self._server_ini())
                result['runtime_policy'] = runtime
                if runtime['status'] == 'topology_error':
                    message = ('服务端地图与任务行已写入；无法检查 BattleServer 拓扑，未改动节奏策略配置：' +
                               runtime['topology_error'])
                elif runtime['status'] == 'pending_programme_deployment':
                    message = ('服务端地图与任务行已写入；导入任务节奏仍为原生等待。请先在 ' +
                               '、'.join(runtime['missing_hosts']) + ' 部署 map_catalog_server，再重新应用。')
                elif runtime['status'] == 'activation_required':
                    message = ('服务端地图、任务行和节奏名单已写入，但全局 Mod/功能开关仍关闭；'
                               '启用后重启 World/Battle 服务。')
                elif runtime['status'] == 'native':
                    message = '服务端地图与任务行已写入；本发布保留原生阶段等待。请重启 World/Battle 服务使其生效。'
                else:
                    message = '服务端地图、任务行和节奏策略已写入并读回。请重启 World/Battle 服务使其生效。'
                return {'result': result, 'state': self._state(), 'changes_applied': True, 'message': message}
            if path == '/api/maps/server/readback':
                plan = self._plan()
                return {'readback': server.readback(plan, self._server_ini()),
                        'runtime_policy': server.runtime_policy_state(plan, self._server_ini()),
                        'changes_applied': False}
        raise ValueError('地图导入操作未登记')
