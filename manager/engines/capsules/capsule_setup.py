"""First owner installation: bind shipped defaults to selected client/server data."""
from __future__ import annotations

import uuid
from pathlib import Path

from .capsule_catalog import import_catalog, write_json
from .capsule_presets import apply


def prepare(workspace, client, server_ini):
    from .capsule_deployment import _assert_client_stopped
    from owner.collection_capsule.deployment import connection
    from contextlib import closing
    client, server_ini = Path(client).resolve(), Path(server_ini).resolve()
    _assert_client_stopped(client)
    # Reconstructing from the original archive cannot recover authored pools.
    with closing(connection(server_ini)) as db:
        exists = db.cursor().execute("SELECT CASE WHEN OBJECT_ID(N'dbo.sdgo_mod_CapsuleRuntimeState',N'U') IS NULL THEN 0 ELSE 1 END").fetchval()
        if exists:
            raise ValueError('这个服务端已有扭蛋配置，请选择原目录 JSON；默认模板不会覆盖已有配置')
    from tools.export_capsule_snapshot import export_snapshot
    root = Path(workspace).resolve() / 'data' / 'capsules' / ('install-' + uuid.uuid4().hex)
    snapshot = root / 'source'
    export_snapshot(server_ini, snapshot, databases=('GOnlineGame',))
    catalog, receipt = apply(import_catalog(snapshot, client))
    if receipt['status'] != 'initialized':
        raise ValueError('当前宿主不匹配内置普通扭蛋模板；没有应用客户端或服务端')
    path = root / 'catalog.json'
    write_json(path, catalog)
    return {'catalog': str(path), 'snapshot': str(snapshot), 'client': str(client),
            'install_client': str(client), 'server_ini': str(server_ini),
            'bundle_output': str(root / 'bundle')}
