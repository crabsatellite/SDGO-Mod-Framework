"""Headless domain bridge; the retired shop EXE is never launched or needed."""
from __future__ import annotations
import configparser
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import threading
import zipfile
from .providers import Provider, Binary
from . import storage


PACKAGE_FILES = {
    'shop_localization': ('shop_localization.dll', 'mod.ini', 'manager.json', 'zh-CN.ini',
                          'symbols.fragment.ini'),
    'shop_labels': ('shop_labels.dll', 'mod.ini', 'manager.json', 'display.ini'),
}
MAX_PACKAGE_FILE = 32 * 1024 * 1024
INT_MAX = 2147483647


def _read_file(path: Path, label: str, client: Path) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise ValueError('所选客户端缺少玩家更新包文件：' + label)
    resolved = path.resolve()
    if not resolved.is_relative_to(client):
        raise ValueError('玩家更新包文件位于所选客户端之外：' + label)
    if resolved.stat().st_size > MAX_PACKAGE_FILE:
        raise ValueError('玩家更新包文件过大：' + label)
    return resolved.read_bytes()


def _mod_host(manifest, identity: str) -> str:
    if manifest.get('mod', 'id', fallback='') != identity:
        raise ValueError('所选客户端的 Mod 身份无效：' + identity)
    hosts = [value.strip() for value in manifest.get('mod', 'hosts', fallback='').split(',') if value.strip()]
    if len(hosts) != 1 or not re.fullmatch(r'GOnline-[0-9A-Fa-f]{8}-[0-9A-Fa-f]{8}', hosts[0]):
        raise ValueError('所选客户端的 Mod 宿主版本无效：' + identity)
    return hosts[0]


def _fragment_path(client: Path, identity: str) -> Path | None:
    installed = client / 'sdgo/mods' / identity / 'symbols.fragment.ini'
    if installed.is_symlink():
        raise ValueError('Mod 符号清单不能指向客户端外部：' + identity)
    return installed if installed.is_file() else None


def _strict_ini(raw: bytes, label: str):
    text, _, _ = storage.decode_ini(raw)
    value = configparser.ConfigParser(interpolation=None, strict=True, empty_lines_in_values=False)
    try:
        value.read_string(text)
    except configparser.Error as error:
        raise ValueError(label + ' 包含重复或无效的 INI 内容') from error
    return value


def _fragment_sections(client: Path, identity: str, manifest) -> tuple[list[str], object | None]:
    required = [value.strip() for value in manifest.get('mod', 'requires', fallback='').split(',') if value.strip()]
    fragment_path = _fragment_path(client, identity)
    if identity == 'shop_localization' and fragment_path is None:
        raise ValueError('商城汉化缺少完整符号清单，请重新安装服主客户端组件')
    if fragment_path is None:
        return required, None
    fragment = _strict_ini(
        _read_file(fragment_path, identity + '/symbols.fragment.ini', client),
        identity + '/symbols.fragment.ini')
    sections = [section for section in fragment.sections() if section.casefold() != 'host']
    if not sections or any(name not in sections for name in required):
        raise ValueError('Mod 符号清单不完整：' + identity)
    if identity == 'shop_localization' and any(not name.startswith('shop_localization.') for name in sections):
        raise ValueError('商城汉化符号清单包含未登记命名空间')
    return sections, fragment


def _validate_display(raw: bytes):
    value = _strict_ini(raw, 'display.ini')
    required = {
        'option': {'category', 'validity', 'payment', 'price', 'text'},
        'product': {'name', 'templateid', 'category', 'validity', 'payment', 'price'},
    }
    if not value.has_section('shop'):
        raise ValueError('显示配置尚未由商城工具生成，请先应用商城配置')
    if set(value['shop']) - {'revision', 'schema', 'catalogid'}:
        raise ValueError('显示配置包含未登记字段，未生成玩家更新包：shop')
    product_count = 0
    for section in value.sections():
        folded = section.casefold()
        if folded == 'shop':
            continue
        else:
            match = re.fullmatch(r'(option|product):[1-9][0-9]*', section)
            if not match:
                raise ValueError('显示配置包含未登记内容，未生成玩家更新包：' + section)
            kind = match[1]
        if set(value[section]) != required[kind]:
            raise ValueError('显示配置字段不完整或包含未登记字段：' + section)
        identity = int(folded.split(':', 1)[1])
        limits = {
            'category': (1, INT_MAX),
            'validity': (1, INT_MAX),
            'payment': (1, 2),
            'price': (0 if kind == 'option' else 1, INT_MAX),
        }
        if kind == 'product':
            limits['templateid'] = (1, INT_MAX)
            product_count += 1
            if product_count > 256 or not 1000000 < identity < 2000000:
                raise ValueError('显示配置中的商品编号或数量无效：' + section)
        elif identity > INT_MAX:
            raise ValueError('显示配置中的选项编号无效：' + section)
        for key, (minimum, maximum) in limits.items():
            text = value.get(section, key, fallback='')
            if not re.fullmatch(r'[0-9]+', text):
                raise ValueError('显示配置中的数字字段无效：' + section + '/' + key)
            number = int(text)
            if not minimum <= number <= maximum:
                raise ValueError('显示配置中的数字字段越界：' + section + '/' + key)
        text_key = 'name' if kind == 'product' else 'text'
        text = value.get(section, text_key, fallback='')
        maximum = 50 if kind == 'product' else 254
        units = len(text.encode('utf-16-le')) // 2
        if not text or units > maximum or any(character in text for character in '\r\n\0'):
            raise ValueError('显示配置中的文字字段无效：' + section)
        if kind == 'product' and not re.search(r'[\u3400-\u4DBF\u4E00-\u9FFF\uF900-\uFAFF]', text):
            raise ValueError('显示配置中的商品名称必须包含汉字：' + section)
    published_revision = value.get('shop', 'revision', fallback='').strip()
    if not published_revision or len(published_revision) > 128:
        raise ValueError('显示配置缺少有效的已发布版本')
    return published_revision


def _validate_dictionary(raw: bytes):
    value = storage.ini_values(raw)
    if not value.sections():
        raise ValueError('商城汉化词典为空')
    for section in value.sections():
        if not re.fullmatch(r'entry:[0-9a-f]{24}', section.casefold()) or set(value[section]) != {'kind', 'source', 'text'}:
            raise ValueError('商城汉化词典格式无效')


def _symbol_fragment(client: Path, manifests: dict[str, object], host: str) -> bytes:
    match = re.fullmatch(r'GOnline-([0-9A-Fa-f]{8})-([0-9A-Fa-f]{8})', host)
    symbol_path = client / 'sdgo/symbols' / (host + '.ini')
    current = storage.ini_values(_read_file(symbol_path, 'sdgo/symbols/' + host + '.ini', client))
    if not current.has_section('host'):
        raise ValueError('所选客户端的符号表缺少宿主信息')
    if int(current.get('host', 'timestamp'), 0) != int(match[1], 16) or int(current.get('host', 'size_of_image'), 0) != int(match[2], 16):
        raise ValueError('所选客户端的符号表与 Mod 宿主版本不一致')
    selected = []
    seen = set()
    for identity in ('shop_localization', 'shop_labels'):
        sections, declared = _fragment_sections(client, identity, manifests[identity])
        if identity == 'shop_localization':
            current_namespace = {
                section for section in current.sections()
                if section.startswith('shop_localization.')
            }
            declared_namespace = set(sections)
            if declared_namespace != current_namespace:
                missing = sorted(current_namespace - declared_namespace)
                stale = sorted(declared_namespace - current_namespace)
                detail = (missing or stale)[:3]
                raise ValueError(
                    '商城汉化符号清单与当前客户端不完整或版本不一致，请重新安装：' +
                    '、'.join(detail))
        if declared is not None and declared.has_section('host'):
            for key in ('timestamp', 'size_of_image'):
                if int(declared.get('host', key), 0) != int(current.get('host', key), 0):
                    raise ValueError('Mod 符号清单与所选客户端版本不一致：' + identity)
        for section in sections:
            if section not in seen:
                selected.append(section)
                seen.add(section)
    missing = [section for section in selected if not current.has_section(section)]
    if missing:
        raise ValueError('所选客户端的符号表缺少：' + '、'.join(missing[:5]))
    output = configparser.ConfigParser(interpolation=None)
    output['host'] = dict(current['host'])
    for section in selected:
        output[section] = dict(current[section])
    text = io.StringIO()
    output.write(text, space_around_delimiters=False)
    try:
        return text.getvalue().encode('ascii')
    except UnicodeEncodeError:
        raise ValueError('所选客户端的符号表包含非 ASCII 数据') from None


def client_package(client_value: str, shop_revision: str) -> bytes:
    if not client_value:
        raise ValueError('请先选择已安装商城 Mod 的客户端')
    if not re.fullmatch(r'[0-9a-f]{64}', shop_revision or ''):
        raise ValueError('商城配置版本无效，请刷新面板后重试')
    client = Path(client_value).resolve()
    if not client.is_dir() or not (client / 'GOnline.exe').is_file():
        raise ValueError('所选目录不是可用的游戏客户端')
    payload = {}
    manifests = {}
    hosts = set()
    with storage.LOCK:
        for identity, names in PACKAGE_FILES.items():
            folder = client / 'sdgo/mods' / identity
            for name in names:
                relative = 'client/sdgo/mods/' + identity + '/' + name
                payload[relative] = _read_file(folder / name, relative, client)
            manifest = storage.ini_values(payload['client/sdgo/mods/' + identity + '/mod.ini'])
            manifests[identity] = manifest
            hosts.add(_mod_host(manifest, identity))
            try:
                contract = json.loads(payload['client/sdgo/mods/' + identity + '/manager.json'].decode('utf-8-sig'))
            except (UnicodeDecodeError, json.JSONDecodeError):
                raise ValueError('所选客户端的 Manager 登记无效：' + identity) from None
            if not isinstance(contract, dict) or contract.get('id') != identity:
                raise ValueError('所选客户端的 Manager 登记身份无效：' + identity)
        if len(hosts) != 1:
            raise ValueError('商城汉化和显示补丁不属于同一客户端版本')
        _validate_dictionary(payload['client/sdgo/mods/shop_localization/zh-CN.ini'])
        published_revision = _validate_display(payload['client/sdgo/mods/shop_labels/display.ini'])
        host = next(iter(hosts))
        payload['symbols.fragment.ini'] = _symbol_fragment(client, manifests, host)
    versions = {identity: manifests[identity].get('mod', 'version', fallback='') for identity in manifests}
    hashes = {name: hashlib.sha256(data).hexdigest() for name, data in sorted(payload.items())}
    package_revision = hashlib.sha256(storage.json_bytes(hashes)).hexdigest()
    manifest = {
        'schema_version': 1,
        'id': 'shop_client_update',
        'activation': 'restart_required',
        'host': host,
        'shop_revision': shop_revision,
        'published_revision': published_revision,
        'package_revision': package_revision,
        'mods': versions,
        'files': hashes,
    }
    payload['manifest.json'] = storage.json_bytes(manifest)
    output = io.BytesIO()
    with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(payload.items()):
            entry = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o100644 << 16
            archive.writestr(entry, data)
    return output.getvalue()


class Bridge:
    def __init__(self):
        here = Path(__file__).parent / 'engines/shop'
        configured = os.environ.get('SDGO_MANAGER_SERVICES')
        library = Path(configured) if configured else here / 'SdgoShopServices.dll'
        if not configured and not library.exists(): library = Path(__file__).resolve().parents[1] / 'build/manager-services/SdgoShopServices.dll'
        if not library.exists(): raise ValueError('请先构建 Manager 的商城业务程序集')
        self.process = subprocess.Popen(['powershell.exe','-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass',
            '-File',str(here / 'bridge.ps1'),'-Library',str(library)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, encoding='utf-8', creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        self.lock = threading.RLock()

    def call(self, value):
        with self.lock:
            if self.process.poll() is not None: raise ValueError('商城业务进程已退出，请刷新工作区')
            self.process.stdin.write(json.dumps(value,ensure_ascii=False)+'\n');self.process.stdin.flush()
            line = self.process.stdout.readline()
            if not line: raise ValueError('商城业务进程没有返回结果')
            result = json.loads(line)
            if not result.get('ok'): raise ValueError(result.get('error','商城操作未完成'))
            return result['value']

    def close(self):
        try:
            if self.process.poll() is None:
                self.process.stdin.close()
                try: self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.terminate();self.process.wait(timeout=5)
        finally:
            if self.process.stdin and not self.process.stdin.closed:self.process.stdin.close()
            if self.process.stdout:self.process.stdout.close()


class Shop(Provider):
    id, title = 'shop', '商城商品'
    fields = {'workspace':'现有商城配置目录','world':'WorldServer 目录','client':'客户端目录（显示标签更新）','demo':'演示模式（仅测试时填 1）'}
    def request(self, method, path, body, headers):
        if not self.config.get('workspace'): raise ValueError('请先选择商城配置工作区')
        if method=='GET' and path=='/api/shop/client-package':
            return Binary(client_package(self.config.get('client',''),headers.get('X-Shop-Revision','')),
                          'application/zip','SDGO-shop-client-update.zip')
        bridge = self.workspace.services.resource('shop.bridge', Bridge)
        action = 'read' if method=='GET' else path.rsplit('/',1)[-1]
        value=bridge.call({**self.config,'action':action,'body':body,'revision':headers.get('X-Shop-Revision','')})
        if method=='GET' and path.endswith('/export'): return Binary(storage.json_bytes(value['catalog']),'application/json','shop.json')
        units = self.workspace.providers.get('units')
        registrations = units.for_server(Path(self.config['world'])/'WorldServer.ini') if units and self.config.get('world') else []
        value['unit_imports'] = {str(r['category_id']): r for r in registrations}
        for item in value.get('items', []):
            if str(item['Id']) in value['unit_imports']:
                item['Name'] = value['unit_imports'][str(item['Id'])]['display_name']
        return value
