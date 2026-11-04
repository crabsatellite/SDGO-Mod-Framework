"""Hand framework program ownership to the unit content install receipt.

Public programs and an optional per-Mod symbol fragment may be reconciled against a verified
framework artifact. All recorded unit content remains immutable during this
operation. This module never writes files; its caller owns the transaction.
"""
from __future__ import annotations

import configparser
import base64
import copy
import hashlib
import json
from pathlib import Path
import re

from . import storage


BASE = 'sdgo/mods/unit_catalog/'
RECEIPT = BASE + 'install-receipt.json'
PROGRAMS = frozenset(BASE + name for name in ('unit_catalog.dll', 'mod.ini', 'manager.json'))
OPTIONAL_PROGRAMS = frozenset({BASE+'symbols.fragment.ini'})
CONTENT = frozenset(BASE + name for name in (
    'catalog.bin', 'definitions.json', 'resource-overlay.ini', 'resources.zdx', 'resources.zpk'))
RESOURCE_ROOTS = {'anrs', 'fxrs', 'mdrs', 'scp', 'sdrs', 'sol', 'sprs', 'trns', 'txrs', 'uirs'}
BLOCKED = {'.bat', '.cmd', '.com', '.dll', '.exe', '.js', '.lnk', '.msi', '.ps1', '.scr', '.vbs'}
HASH = re.compile('[0-9a-f]{64}\\Z')


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def _ini(raw):
    parser = configparser.ConfigParser(interpolation=None, strict=True)
    try:parser.read_string(storage.decode_ini(raw)[0])
    except (configparser.Error,UnicodeError):raise ValueError('机体程序清单格式无效') from None
    values = {section.casefold(): dict(parser[section]) for section in parser.sections()}
    if len(values) != len(parser.sections()):raise ValueError('机体程序清单存在重复节名')
    values.pop('settings', None)
    values.get('mod', {}).pop('enabled', None)
    return values


def _mutable_manifest_matches(receipt, current, expected_hash):
    encoded=receipt.get('framework_mod_ini_base64')
    if encoded is None:return False
    if not isinstance(encoded,str) or len(encoded)>128*1024:
        raise ValueError('机体程序清单快照无效')
    try:original=base64.b64decode(encoded,validate=True)
    except ValueError:raise ValueError('机体程序清单快照无效') from None
    if digest(original)!=expected_hash:raise ValueError('机体程序清单快照与收据哈希不一致')
    return _ini(current)==_ini(original)


def _program_matches(name, actual, expected, *, legacy):
    if actual == expected:
        return True
    if name.endswith(('/unit_catalog.dll','/symbols.fragment.ini')):
        return False
    if name.endswith('/mod.ini'):
        current, source = _ini(actual), _ini(expected)
        if legacy and 'distribution' not in source.get('mod', {}):
            source.setdefault('mod', {})['distribution'] = 'public'
        return current == source
    current, source = json.loads(actual), json.loads(expected)
    if legacy and 'distribution' not in source:
        source['distribution'] = 'public'
    return current == source


def _relative(value):
    if not isinstance(value, str):
        raise ValueError('机体安装收据路径无效')
    name = value.replace('\\', '/')
    if (not name or name.startswith('/') or any(part in ('', '.', '..') for part in name.split('/'))
        or re.search(r'[\x00-\x1f:*?"<>|]', name)):
        raise ValueError('机体安装收据路径无效')
    key = name.casefold()
    if key not in PROGRAMS | OPTIONAL_PROGRAMS | CONTENT:
        if key.split('/', 1)[0] not in RESOURCE_ROOTS or Path(key).suffix in BLOCKED:
            raise ValueError('机体收据包含其所有权范围以外的文件')
    return name, key


def _artifact(source, manifest, name, child):
    key = 'client/' + name
    entry = manifest.get('files', {}).get(key)
    if not isinstance(entry, dict):
        raise ValueError('框架更新包缺少机体程序校验信息')
    raw = child(source, name).read_bytes()
    if len(raw) != entry.get('bytes') or digest(raw) != entry.get('sha256'):
        raise ValueError('框架更新包中的机体程序校验失败')
    return raw


def prepare(root, source, manifest, planned, child, host_key, *, repair=False):
    """Return a receipt write and a current-file CAS snapshot, or None.

    ``manifest`` and ``source`` must come from the existing verified release
    loader. Legacy scope declarations may be made explicit without changing
    program identity. Unknown DLLs or altered unit content are never adopted.
    """
    path = child(root, RECEIPT)
    if not path.is_file():
        if repair:raise ValueError('该客户端没有可核对的机体安装收据')
        return None
    raw = path.read_bytes(); receipt = json.loads(raw)
    if (receipt.get('module') != 'unit_catalog' or receipt.get('schema_version') != 1
        or receipt.get('host') != host_key
        or receipt.get('host_sha256') != digest(child(root, 'GOnline.exe').read_bytes())):
        raise ValueError('机体安装收据与当前客户端不匹配')
    files = receipt.get('files')
    if not isinstance(files, list) or not 1 <= len(files) <= 4096:
        raise ValueError('机体安装收据文件清单无效')
    if (manifest.get('repository') != 'crabsatellite/SDGO-Mod-Framework'
        or not re.fullmatch('[0-9a-f]{40}', str(manifest.get('source_commit', '')))):
        raise ValueError('机体程序必须来自已校验的框架发行包')
    match = re.fullmatch(r'(\d+)\.(\d+)\.(\d+)(?:-beta\.\d+)?', str(manifest.get('version', '')))
    if match is None:raise ValueError('框架发行版本无效')
    legacy = tuple(map(int, match.groups())) < (1, 2, 0)
    source_programs = {name: _artifact(source, manifest, name, child) for name in PROGRAMS}
    source_programs.update({name:_artifact(source,manifest,name,child) for name in OPTIONAL_PROGRAMS
                            if 'client/'+name in manifest.get('files',{})})
    result = copy.deepcopy(receipt); seen = set(); snapshot = {path: digest(raw)}; changed = []
    snapshot[child(root,'GOnline.exe')]=receipt['host_sha256']
    for entry in result['files']:
        name, key = _relative(entry.get('path')); target = child(root, name)
        expected = entry.get('installed_sha256')
        if key in seen or not isinstance(expected, str) or HASH.fullmatch(expected) is None or type(entry.get('owned')) is not bool:
            raise ValueError('机体安装收据存在重复或无效的文件记录')
        seen.add(key)
        if not target.is_file():raise ValueError('机体收据中的文件已丢失：' + name)
        current = target.read_bytes(); current_hash = digest(current); snapshot[target] = current_hash
        if key in source_programs:
            if repair and not _program_matches(key, current, source_programs[key], legacy=legacy):
                raise ValueError('现有机体程序不属于所选框架发行包：' + name)
            mutable=(key==BASE+'mod.ini' and _mutable_manifest_matches(receipt,current,expected))
            if current_hash != expected and not mutable and not _program_matches(key, current, source_programs[key], legacy=legacy):
                raise ValueError('机体程序在更新器之外发生改动：' + name)
            new = current if repair else planned.get(target, current)
            if key==BASE+'mod.ini':result['framework_mod_ini_base64']=base64.b64encode(new).decode('ascii')
            new_hash = digest(new)
            if new_hash != expected:
                changed.append({'path': name, 'recorded_sha256': expected,
                                'previous_sha256': current_hash, 'installed_sha256': new_hash,
                                'artifact_sha256': digest(source_programs[key])})
                entry['installed_sha256'] = new_hash
        elif current_hash != expected:
            raise ValueError('机体内容在安装收据之外发生改动：' + name)
    if not CONTENT <= seen or not {BASE+'unit_catalog.dll', BASE+'mod.ini'} <= seen:
        raise ValueError('机体安装收据缺少必要的内容或程序记录')
    if not changed:return None
    history = result.get('framework_updates', [])
    if not isinstance(history, list) or any(not isinstance(item, dict) for item in history):
        raise ValueError('机体框架更新记录无效')
    history = history[-63:]
    history.append({'schema_version': 1, 'kind': 'verified-framework-reconciliation' if repair else 'framework-update',
                    'version': manifest['version'], 'source_commit': manifest['source_commit'],
                    'manifest_sha256': digest(storage.json_bytes(manifest)), 'previous_receipt_sha256': digest(raw),
                    'files': changed})
    result['framework_updates'] = history
    return {'path': path, 'data': storage.json_bytes(result), 'snapshot': snapshot, 'receipt': result}


def verify_snapshot(plan):
    if plan is None:return
    for path, expected in plan['snapshot'].items():
        if not path.is_file() or digest(path.read_bytes()) != expected:
            raise ValueError('机体文件在框架更新确认后发生变化')


def verify_installed(root, plan, child):
    if plan is None:return
    if plan['path'].read_bytes() != plan['data']:
        raise ValueError('机体框架更新收据读回不一致')
    if digest(child(root,'GOnline.exe').read_bytes())!=plan['receipt']['host_sha256']:
        raise ValueError('客户端程序在机体框架更新期间发生变化')
    for entry in plan['receipt']['files']:
        path = child(root, entry['path'].replace('\\', '/'))
        if not path.is_file() or digest(path.read_bytes()) != entry['installed_sha256']:
            raise ValueError('机体框架更新文件读回不一致')
