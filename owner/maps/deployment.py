"""Install a map_catalog client package (built by owner.maps.bundle) into a stopped client.

Only these targets are written: ``sdgo/mods/map_catalog/<declared files>`` and loose ``BGM/m<NNN>*.mp3``.
Every file is checked against the package manifest digest before anything is written; existing files are
replaced atomically. No script from the package is executed.
"""
from __future__ import annotations

import hashlib
import io
import json
import re
import zipfile
from pathlib import Path

MAX_PACKAGE_BYTES = 256 * 1024 * 1024
MAX_FILE_BYTES = 128 * 1024 * 1024
MOD_FILES = {'catalog.bin', 'definitions.json', 'resource-overlay.ini', 'resources.zdx', 'resources.zpk', 'missions.lua',
             'map_catalog.dll', 'mod.ini', 'manager.json', 'symbols.fragment.ini'}
PROGRAM_FILES = {'map_catalog.dll', 'mod.ini', 'manager.json', 'symbols.fragment.ini'}
REQUIRED = {'catalog.bin', 'definitions.json', 'resource-overlay.ini', 'resources.zdx', 'resources.zpk'}


def _target(name: str) -> str:
    if name.startswith('client/sdgo/mods/map_catalog/'):
        leaf = name[len('client/sdgo/mods/map_catalog/'):]
        if leaf in MOD_FILES:
            return 'sdgo/mods/map_catalog/' + leaf
    if re.fullmatch(r'client/BGM/m[0-9]{3}[a-z0-9]*\.mp3', name):
        return name[len('client/'):]
    raise ValueError('地图包包含未登记的文件：' + name)


def _running(client: Path) -> bool:
    try:
        from manager.packages import _running as running
    except Exception:
        return False
    return running(client / 'GOnline.exe')


def install_package(client, raw: bytes) -> dict:
    client = Path(client).resolve()
    if not (client / 'GOnline.exe').is_file():
        raise ValueError('目标目录不是客户端')
    if len(raw) > MAX_PACKAGE_BYTES:
        raise ValueError('地图客户端包超过 256 MiB')
    if _running(client):
        raise ValueError('请先关闭这个客户端，再导入地图包')
    with zipfile.ZipFile(io.BytesIO(raw)) as archive:
        names = [entry.filename for entry in archive.infolist() if not entry.is_dir()]
        if 'manifest.json' not in names or len(names) > 64:
            raise ValueError('地图包缺少清单或文件过多')
        manifest = json.loads(archive.read('manifest.json'))
        if manifest.get('module') != 'map_catalog' or manifest.get('schema_version') != 1:
            raise ValueError('不是地图导入客户端包')
        declared = manifest.get('files')
        if not isinstance(declared, dict) or set(declared) != set(names) - {'manifest.json'}:
            raise ValueError('地图包文件与清单不一致')
        pending: dict[str, bytes] = {}
        for name in sorted(declared):
            info = archive.getinfo(name)
            if info.file_size > MAX_FILE_BYTES:
                raise ValueError('地图包文件过大：' + name)
            data = archive.read(name)
            if hashlib.sha256(data).hexdigest() != declared[name]:
                raise ValueError('地图包文件摘要不符：' + name)
            pending[_target(name)] = data
    leaves = {path.rsplit('/', 1)[1] for path in pending if path.startswith('sdgo/mods/map_catalog/')}
    if not REQUIRED <= leaves:
        raise ValueError('地图包缺少必需的内容文件')
    mod_dir = client / 'sdgo/mods/map_catalog'
    if 'mod.ini' not in leaves and not (mod_dir / 'mod.ini').is_file():
        raise ValueError('客户端尚未安装地图导入模块（map_catalog）')

    # Content files a previous package installed (per its receipt) and this one no longer ships are removed,
    # so e.g. an old missions.lua cannot outlive its catalog. Program files stay (the Manager owns them).
    stale = []
    receipt_path = mod_dir / 'install-receipt.json'
    try:
        previous = json.loads(receipt_path.read_text(encoding='utf-8')).get('files', {})
    except (OSError, ValueError, AttributeError):
        previous = {}
    for relative in previous if isinstance(previous, dict) else ():
        if relative in pending or not isinstance(relative, str):
            continue
        leaf = relative.rsplit('/', 1)[-1]
        owned = ((relative.startswith('sdgo/mods/map_catalog/') and leaf in MOD_FILES - PROGRAM_FILES)
                 or re.fullmatch(r'BGM/m[0-9]{3}[a-z0-9]*\.mp3', relative))
        target = (client / relative).resolve()
        if owned and target.is_relative_to(client) and target.is_file():
            stale.append((relative, target))

    written = []
    for relative, data in pending.items():
        target = (client / relative).resolve()
        if not target.is_relative_to(client):
            raise ValueError('地图包路径越界')
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_name(target.name + '.sdgo-new')
        temporary.write_bytes(data)
        temporary.replace(target)
        written.append(relative)
    for relative, target in stale:
        target.unlink()
    receipt = {'module': 'map_catalog', 'revision': manifest.get('revision'), 'maps': manifest.get('maps', []),
               'files': {path: hashlib.sha256(data).hexdigest() for path, data in sorted(pending.items())}}
    (mod_dir / 'install-receipt.json').write_text(json.dumps(receipt, ensure_ascii=False, indent=2), encoding='utf-8')
    return {'module': 'map_catalog', 'activation': 'restart_required', 'receipt': receipt, 'written': len(written),
            'removed': [relative for relative, _ in stale]}
