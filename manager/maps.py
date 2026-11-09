"""Map and mission import behind the Manager's session: register map packages, edit how each map is listed,
build one client package + server plan, install the client package and apply the server rows."""
from __future__ import annotations

import json
from pathlib import Path

from .providers import Provider, Binary


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
