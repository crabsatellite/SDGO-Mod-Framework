"""Map and mission import behind the Manager's session: register map packages, edit how each map is listed,
build one client package + server plan, install the client package and apply the server rows."""
from __future__ import annotations

import hashlib
import json
import shutil
from pathlib import Path, PurePosixPath
from urllib.parse import quote

from .providers import Provider, Binary

# Example maps published in the framework's public repository. Each one is pinned to a commit and to the SHA-256
# of its map.json; map.json in turn lists the SHA-256 of every other file, so the whole download is verified.
EXAMPLES = {
    'twin-valley-fortress': {
        'name': {'zh': '双谷要塞 · 奥德萨作战', 'en': 'Twin Valley Fortress · Operation Odessa'},
        'description': '南谷对战场、山脊要塞闸门和北谷基地，附 6 阶段 PvE 任务（过场、横幅、首领、核弹倒计时）。',
        'path': 'examples/maps/twin-valley-fortress',
        'commit': 'd26bf89bcca652849affe7900b62863e35d090ea',
        'map_json': '510def898c0c9b6f636764c78d87cd828c76bc568cd734b22c7206ec90480d7f',
    },
}
EXAMPLE_FILE_LIMIT = 16 * 1024 * 1024
EXAMPLE_TOTAL_LIMIT = 96 * 1024 * 1024


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
    fetch = fetch or updates.fetch
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
        state['examples'] = [{'id': key, 'name': spec['name'], 'description': spec['description']}
                             for key, spec in EXAMPLES.items()]
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
            if path == '/api/maps/examples/add':
                maps = self._maps()
                folder = fetch_example(_text(body, 'id', 80), maps.root / 'examples')
                entry = maps.add(folder)
                return {'map': entry, 'state': self._state(),
                        'message': '示例地图已下载并登记。编译、安装客户端包并应用服务端后，重启服务端和客户端生效。'}
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
                summary = server.preflight(self._plan(), self._server_ini())
                summary.pop('_changes', None)
                return {'preflight': summary, 'changes_applied': False}
            if path == '/api/maps/server/apply':
                from owner.maps.workspace import OWNER_KEY
                result = server.apply(self._plan(), self._server_ini(), OWNER_KEY)
                return {'result': result, 'state': self._state(), 'changes_applied': True,
                        'message': '服务端地图与任务行已写入并读回。请重启 World/Battle 服务使地图表生效。'}
            if path == '/api/maps/server/readback':
                return {'readback': server.readback(self._plan(), self._server_ini()), 'changes_applied': False}
        raise ValueError('地图导入操作未登记')
