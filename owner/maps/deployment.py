"""Install a map_catalog client package (built by owner.maps.bundle) into a stopped client.

Only these targets are written: ``sdgo/mods/map_catalog/<declared files>`` and loose ``BGM/m<NNN>*.mp3``.
Every file is checked against the package manifest digest before anything is written; existing files are
replaced atomically. No script from the package is executed.
"""
from __future__ import annotations

import configparser
import hashlib
import io
import json
import re
import zipfile
from pathlib import Path

from . import boundary_format

MAX_PACKAGE_BYTES = 256 * 1024 * 1024
MAX_FILE_BYTES = 128 * 1024 * 1024
MOD_FILES = {'catalog.bin', 'definitions.json', 'resource-overlay.ini', 'resources.zdx', 'resources.zpk', 'missions.lua', 'boundaries.bin',
             'damage-gates.bin',
             'retreats.bin',
             'map_catalog.dll', 'mod.ini', 'manager.json', 'symbols.fragment.ini'}
PROGRAM_FILES = {'map_catalog.dll', 'mod.ini', 'manager.json', 'symbols.fragment.ini'}
REQUIRED = {'catalog.bin', 'definitions.json', 'resource-overlay.ini', 'resources.zdx', 'resources.zpk'}
BOUNDARY_PROGRAMME_VERSION = (0, 6, 0)


def _programme_version(raw: bytes) -> tuple[int, int, int]:
    try:
        parser = configparser.ConfigParser(interpolation=None, strict=True)
        parser.read_string(raw.decode('utf-8-sig'))
        if parser.get('mod', 'id') != 'map_catalog':
            raise ValueError
        value = parser.get('mod', 'version')
    except (UnicodeError, configparser.Error, KeyError, ValueError):
        raise ValueError('地图导入模块元数据无效') from None
    match = re.fullmatch(r'(\d+)\.(\d+)\.(\d+)', value.strip())
    if not match:
        raise ValueError('地图导入模块版本格式无效')
    return tuple(int(part) for part in match.groups())


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
        schema = manifest.get('schema_version')
        if manifest.get('module') != 'map_catalog' or type(schema) is not int or schema not in (1, 2):
            raise ValueError('不是地图导入客户端包')
        if schema == 2 and manifest.get('minimum_programme') != '0.7.0':
            raise ValueError('新版地图包的配套程序声明无效')
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
    if schema == 2:
        programme_manifest = pending.get('sdgo/mods/map_catalog/mod.ini')
        if programme_manifest is None:
            try:
                programme_manifest = (mod_dir / 'mod.ini').read_bytes()
            except OSError:
                raise ValueError('客户端尚未安装地图导入模块（map_catalog）') from None
        if _programme_version(programme_manifest) < (0, 7, 0):
            raise ValueError('队伍过场与目标保护需要 map_catalog 0.7.0 或更高版本，未写入任何文件')
    gates_path = 'sdgo/mods/map_catalog/damage-gates.bin'
    if gates_path in pending:
        from . import damage_gate_format
        damage_gate_format.decode(pending[gates_path])
        if schema != 2:
            raise ValueError('目标保护需要新版地图包协议，未写入任何文件')
    retreats_path = 'sdgo/mods/map_catalog/retreats.bin'
    if retreats_path in pending:
        from . import retreat_format
        retreat_format.decode(pending[retreats_path])
        if schema != 2:
            raise ValueError('友军撤离需要新版地图包协议，未写入任何文件')
    boundary_path = 'sdgo/mods/map_catalog/boundaries.bin'
    if boundary_path in pending:
        boundary_format.decode(pending[boundary_path])
        programme_manifest = pending.get('sdgo/mods/map_catalog/mod.ini')
        if programme_manifest is None:
            try:
                programme_manifest = (mod_dir / 'mod.ini').read_bytes()
            except OSError:
                raise ValueError('客户端尚未安装地图导入模块（map_catalog）') from None
        if _programme_version(programme_manifest) < BOUNDARY_PROGRAMME_VERSION:
            raise ValueError('任务活动范围需要 map_catalog 0.6.0 或更高版本，未写入任何文件')

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
