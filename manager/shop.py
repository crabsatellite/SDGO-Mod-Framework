"""Headless domain bridge; the retired shop EXE is never launched or needed."""
from __future__ import annotations
import configparser
import hashlib
import io
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import threading
import zipfile
from .providers import Provider, Binary
from . import storage
from .engines.operators import assets as native_assets
from owner.units import acquisition_sources as acquisition_protocol


PACKAGE_FILES = {
    'shop_localization': ('shop_localization.dll', 'mod.ini', 'manager.json', 'zh-CN.ini',
                          'symbols.fragment.ini'),
    'shop_labels': ('shop_labels.dll', 'mod.ini', 'manager.json', 'display.ini'),
}
MAX_PACKAGE_FILE = 32 * 1024 * 1024
INT_MAX = 2147483647
ACQUISITION_MAXIMUM_BYTES = 8 * 1024 * 1024


def _native_product_ids(client: Path):
    """Reserve destination-client originals; a SQL gap need not be a free product."""
    rows = native_assets.Assets(client).records('gdrs/0038.gdt', 1279)
    identities = set()
    for raw in rows.values():
        if len(raw) != 1279:
            raise ValueError('目标客户端的原生商品记录布局不匹配')
        identity = int.from_bytes(raw[5:9], 'little')
        if not 0 < identity <= INT_MAX:
            raise ValueError('目标客户端的原生商品编号无效')
        identities.add(identity)
    return sorted(identities)


def _shop_category_details(client: Path):
    units = {}
    blueprints = {}
    try:
        rows = native_assets.Assets(client).records('gdrs/0031.gdt', native_assets.ITEM_SIZE)
        for category_id, raw in rows.items():
            category_class = int(category_id) // 100000
            stable_id = int.from_bytes(raw[105:109], 'little')
            if stable_id <= 0:
                continue
            if category_class == 11 and raw[109] in (0, 1):
                units[int(category_id)] = (stable_id, int(raw[109]))
            elif category_class == 15 and raw[109] == 0:
                blueprints[int(category_id)] = stable_id
    except (OSError, ValueError, KeyError, TypeError, UnicodeDecodeError, struct.error):
        pass
    definitions = client / 'sdgo/mods/unit_catalog/definitions.json'
    try:
        if definitions.is_symlink() or not definitions.is_file():
            return units, blueprints
        resolved = definitions.resolve()
        if not resolved.is_relative_to(client) or resolved.stat().st_size > ACQUISITION_MAXIMUM_BYTES:
            return units, blueprints
        value = json.loads(resolved.read_text(encoding='utf-8-sig'))
        if not isinstance(value, dict) or value.get('protocol') != 'sdgo.unit-family' or value.get('schema_version') != 1:
            return units, blueprints
        registrations = value.get('registrations')
        if not isinstance(registrations, list):
            return units, blueprints
        for registration in registrations:
            if not isinstance(registration, dict) or registration.get('enabled') is not True:
                continue
            category_id = registration.get('category_id')
            unit_id = registration.get('unit_id')
            if (type(category_id) is int and type(unit_id) is int and
                    0 < category_id <= 0xffffffff and category_id // 100000 == 11 and
                    0 < unit_id <= 0xffffffff):
                units[category_id] = (unit_id, 0)
            for blueprint in registration.get('blueprints', ()):
                if not isinstance(blueprint, dict) or blueprint.get('enabled') is not True:
                    continue
                recipe_id = blueprint.get('id')
                item_category_id = blueprint.get('item_category_id')
                if (type(recipe_id) is int and type(item_category_id) is int and
                        0 < recipe_id <= 0xffffffff and
                        0 < item_category_id <= 0xffffffff and
                        item_category_id // 100000 == 15):
                    blueprints[item_category_id] = recipe_id
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, TypeError, ValueError):
        pass
    return units, blueprints


def _shop_category_maps(client: Path) -> tuple[dict[int, int], dict[int, int]]:
    unit_details, blueprints = _shop_category_details(client)
    return {category: value[0] for category, value in unit_details.items()}, blueprints


def _blueprint_category_map(client: Path) -> dict[int, int]:
    return _shop_category_maps(client)[1]


def _shop_acquisition_sources(client: Path, active_products, source: str) -> bytes:
    if source not in ('store_billing_visible_readback', 'demo_visible_readback'):
        raise ValueError('商城有效商品来源无效')
    if not isinstance(active_products, list) or len(active_products) > 100000:
        raise ValueError('商城有效商品 readback 无效或超过容量')
    normalized = []
    seen = set()
    for product in active_products:
        if not isinstance(product, dict):
            raise ValueError('商城有效商品条目无效')
        values = tuple(product.get(key) for key in ('ProductId', 'OptionId', 'Category', 'Validity'))
        if any(type(value) is not int or not 0 < value <= INT_MAX for value in values):
            raise ValueError('商城有效商品编号或有效期无效')
        identity = values[:2]
        if identity in seen:
            raise ValueError('商城有效商品 readback 含重复选项')
        seen.add(identity)
        normalized.append({
            'product_id': values[0], 'option_id': values[1],
            'category_id': values[2], 'validity': values[3],
        })
    normalized.sort(key=lambda row: (row['product_id'], row['option_id']))
    revision = hashlib.sha256(storage.json_bytes({
        'source': source, 'products': normalized,
    })).hexdigest()

    unit_categories, blueprint_categories = _shop_category_details(client)
    units = {}
    recipes = set()
    mapped = 0
    for product in normalized:
        category_id = product['category_id']
        unit = unit_categories.get(category_id)
        if unit is not None:
            unit_id, time_limit = unit
            mode = 'rental' if time_limit == 1 else 'purchase'
            units.setdefault(unit_id, {})[mode] = {
                'key': f'shop:unit:{unit_id}:{mode}',
                'text': '商城租用' if mode == 'rental' else '商城购买',
            }
            mapped += 1
        recipe_id = blueprint_categories.get(category_id)
        if recipe_id is not None:
            recipes.add(recipe_id)
            mapped += 1
    value = {
        'schema_version': 1,
        'provider': 'shop_localization',
        'revision': revision,
        'provenance': {
            'source': source,
            'active_product_options': len(normalized),
            'mapped_unit_or_blueprint_options': mapped,
        },
        'units': {
            str(unit_id): [modes[key] for key in sorted(modes)]
            for unit_id, modes in sorted(units.items())
        },
        'blueprints': {
            str(recipe_id): [{
                'key': 'shop:blueprint:' + str(recipe_id),
                'text': '商城购买',
            }]
            for recipe_id in sorted(recipes)
        },
    }
    return (json.dumps(value, ensure_ascii=False, indent=2) + '\n').encode('utf-8')


def _install_acquisition_sidecars(folder: Path, source_json: bytes, provider: int):
    folder = Path(folder)
    if folder.is_symlink() or not folder.is_dir():
        raise ValueError('客户端获得方式目录无效')
    source_document = json.loads(source_json)
    files = {
        folder / 'acquisition-sources.json': source_json,
        folder / 'acquisition-sources.bin': acquisition_protocol.encode_sources(
            source_document, provider),
    }
    previous = {}
    written = []
    with storage.LOCK:
        for path in files:
            if path.is_symlink():
                raise ValueError('客户端获得方式文件不能使用链接')
            previous[path] = path.read_bytes() if path.is_file() else None
        try:
            for path, raw in files.items():
                storage.atomic(path, raw)
                written.append(path)
        except Exception as error:
            rollback = []
            for path in reversed(written):
                try:
                    if previous[path] is None:
                        storage.remove(path)
                    else:
                        storage.atomic(path, previous[path])
                except Exception as rollback_error:
                    rollback.append(str(rollback_error))
            message = '客户端获得方式文件更新失败：' + str(error)
            if rollback:
                message += '；回滚未完成：' + '；'.join(rollback)
            raise ValueError(message) from None
    return source_document


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
            if not 1000000 < identity < 2000000:
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
    effective = {}
    for identity in ('shop_localization', 'shop_labels'):
        sections, declared = _fragment_sections(client, identity, manifests[identity])
        if identity == 'shop_localization':
            current_namespace = {
                section for section in current.sections()
                if section.startswith('shop_localization.')
            }
            declared_namespace = set(sections)
            extras = current_namespace - declared_namespace
            # An older official shared map retained this superseded call-site
            # name. Accept only its exact same-site, signature-prefix alias;
            # never ignore arbitrary owner symbols or conflicting definitions.
            retired = 'shop_localization.call.006EF404'
            replacement = 'shop_localization.unit_name_display.assign_call.006EF404'
            if retired in extras and declared is not None and declared.has_section(replacement):
                old, new = current[retired], declared[replacement]
                if (set(old) <= {'kind', 'rva', 'expect'} and old.get('kind') == new.get('kind')
                        and int(old.get('rva', '-1'), 0) == int(new.get('rva', '-2'), 0)
                        and len(bytes.fromhex(old.get('expect', ''))) >= 5
                        and bytes.fromhex(new.get('expect', '')).startswith(bytes.fromhex(old.get('expect', '')))):
                    extras.remove(retired)
            if extras:
                detail = sorted(extras)[:3]
                raise ValueError(
                    '商城汉化符号清单与当前客户端不完整或版本不一致，请重新安装：' +
                    '、'.join(detail))
        if declared is not None and declared.has_section('host'):
            for key in ('timestamp', 'size_of_image'):
                if int(declared.get('host', key), 0) != int(current.get('host', key), 0):
                    raise ValueError('Mod 符号清单与所选客户端版本不一致：' + identity)
        for section in sections:
            shared = dict(current[section]) if current.has_section(section) else None
            own = dict(declared[section]) if declared is not None and declared.has_section(section) else None
            if shared is not None and own is not None and shared != own:
                raise ValueError('商城符号声明与共享表冲突，请使用匹配版本：' + section)
            # Match the core's shared-plus-own-fragment resolution. A complete
            # module fragment does not need duplication in an older shared map.
            definition = own if own is not None else shared
            if definition is None:
                raise ValueError('商城符号不完整或版本不一致，缺少：' + section)
            effective[section] = definition
            if section not in seen:
                selected.append(section)
                seen.add(section)
    output = configparser.ConfigParser(interpolation=None)
    output['host'] = dict(current['host'])
    for section in selected:
        output[section] = effective[section]
    text = io.StringIO()
    output.write(text, space_around_delimiters=False)
    try:
        return text.getvalue().encode('ascii')
    except UnicodeEncodeError:
        raise ValueError('所选客户端的符号表包含非 ASCII 数据') from None


def client_package(client_value: str, shop_revision: str, active_products,
                   active_products_source: str, preflight_only: bool = False) -> bytes:
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
        display = storage.ini_values(payload['client/sdgo/mods/shop_labels/display.ini'])
        products = [name for name in display.sections() if name.startswith('product:')]
        if len(products) > 256 or any(int(name[8:]) % 10000 < 9001 for name in products):
            from .updates import version
            for identity, minimum in (('shop_localization', '0.4.0'), ('shop_labels', '0.2.0')):
                if version(manifests[identity].get('mod','version',fallback='0.0.0')) < version(minimum):
                    raise ValueError('扩展商城的玩家更新需要 ' + identity + ' ' + minimum + ' 或以上版本，请先安装配套程序')
        host = next(iter(hosts))
        symbols = _symbol_fragment(client, manifests, host)
        if preflight_only:
            return b''
        source_json = _shop_acquisition_sources(
            client, active_products, active_products_source)
        source_document = json.loads(source_json)
        payload['client/sdgo/mods/shop_localization/acquisition-sources.json'] = source_json
        payload['client/sdgo/mods/shop_localization/acquisition-sources.bin'] = acquisition_protocol.encode_sources(
            source_document, acquisition_protocol.SHOP_PROVIDER)
        payload['symbols.fragment.ini'] = symbols
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
        'acquisition_revision': source_document['revision'],
        'acquisition_provenance': source_document['provenance'],
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
            if value.get('action') in ('create', 'initialize-presets') and value.get('client') and value.get('demo') != '1':
                client = Path(value['client']).resolve()
                files = [client/'GPackData.zdx', client/'GPackData.zpk', client/'gdrs/0038.gdt']
                signature = (str(client), tuple((str(p), p.stat().st_size, p.stat().st_mtime_ns) for p in files if p.exists()))
                cached = getattr(self, '_native_reservations', None)
                if cached is None or cached[0] != signature:
                    cached = (signature, _native_product_ids(client))
                    self._native_reservations = cached
                value = {**value, 'reserved_product_ids': cached[1]}
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


_OPERATOR_STOREFRONT_FIELDS = {'name', 'label', 'price', 'currency', 'validity'}
_CHINESE_TEXT = re.compile(r'[\u3400-\u4DBF\u4E00-\u9FFF\uF900-\uFAFF]')


def _shop_text(value, field):
    if (not isinstance(value, str) or not value or len(value.encode('utf-16-le')) // 2 > 50
            or any(character in value for character in '\r\n\0')
            or value.startswith('"') or value.endswith('"')):
        raise ValueError(field + '需要 1–50 个字符，不可换行或以双引号开头/结尾')
    if not _CHINESE_TEXT.search(value):
        raise ValueError(field + '必须包含至少一个汉字')
    return value


def _operator_registrations(workspace, shop_world=None):
    provider = workspace.providers.get('operators')
    if provider is None:
        return []
    config = provider.config
    engine = getattr(provider, 'engine', None)
    if engine is None:
        if not all(config.get(key) for key in ('workspace', 'client', 'snapshot')):
            return []
        engine = provider.get_engine()
    catalog = engine.read()
    from .engines.operators import model
    exported = model.registrations(catalog)
    operators = {op['id']: op for op in catalog.get('operators', [])}
    result = []
    operator_ids, category_ids = {}, {}
    for registration in exported.get('items', []):
        operator_id = registration.get('operator_id')
        op = operators.get(operator_id)
        if (not op or not op.get('managed') or not op.get('enabled')
                or op.get('delivery') != 'product'):
            continue
        category_id = registration.get('category_id')
        if (type(operator_id) is not int or not 60001 <= operator_id <= 60999
                or type(category_id) is not int or category_id // 100000 != 12
                or registration.get('key') != 'operator:' + str(operator_id)
                or registration.get('kind') != 'operator'):
            raise ValueError('副官商品登记身份无效')
        if operator_id in operator_ids:
            if operator_ids[operator_id] == category_id:
                raise ValueError('副官商品登记重复')
            raise ValueError('同一副官登记了多个商城物品类别')
        if category_id in category_ids:
            if category_ids[category_id] == operator_id:
                raise ValueError('副官商品登记重复')
            raise ValueError('多个副官登记了同一商城物品类别')
        operator_ids[operator_id] = category_id
        category_ids[category_id] = operator_id
        registered_storefront = registration.get('storefront')
        operator_storefront = op.get('storefront')
        if registered_storefront is not None and operator_storefront is not None and registered_storefront != operator_storefront:
            raise ValueError('副官商品预填配置不一致')
        storefront = registered_storefront if registered_storefront is not None else operator_storefront
        if storefront is None:
            storefront = {}
        item = dict(registration)
        item['storefront'] = dict(storefront) if isinstance(storefront, dict) else storefront
        result.append(item)
    if result and config.get('server_ini') and shop_world:
        from owner.units.repository import database_target_key
        operator_target = database_target_key(Path(config['server_ini']))
        shop_target = database_target_key(Path(shop_world) / 'WorldServer.ini')
        if operator_target != shop_target:
            raise ValueError('副官目录与商城必须使用同一宿主数据库')
    return result


def _operator_draft(registration, item):
    operator_id = registration['operator_id']
    if type(item.get('RefIndex')) is not int or item['RefIndex'] != operator_id:
        raise ValueError('副官商品类别与原生副官编号冲突')
    storefront = registration['storefront']
    if not isinstance(storefront, dict) or set(storefront) - _OPERATOR_STOREFRONT_FIELDS:
        raise ValueError('副官商品预填配置包含不支持的字段')
    name = _shop_text(storefront.get('name', registration.get('display_name')), '副官商品中文名称')
    values = {'name': name, 'enabled': False}
    if 'label' in storefront:
        values['label'] = _shop_text(storefront['label'], '副官商品销售标签')
    if 'price' in storefront:
        price = storefront['price']
        if type(price) is not int or price < 1 or price > INT_MAX:
            raise ValueError('副官商品预填售价必须为正整数')
        values['price'] = price
    if 'currency' in storefront:
        if storefront['currency'] not in ('GB', 'MB'):
            raise ValueError('副官商品预填币种只能是 GB 或 MB')
        values['currency'] = storefront['currency']
    if 'validity' in storefront:
        validity = storefront['validity']
        maximum = 36500 if item.get('TimeLimit') == 1 else 1000000
        if type(validity) is not int or not 1 <= validity <= maximum:
            raise ValueError('副官商品预填期限或数量无效')
        values['validity'] = validity
    return {'category_id': registration['category_id'], 'name': name}, values


def _decorate_shop_state(value, operator_registrations, unit_imports):
    value['unit_imports'] = unit_imports
    operator_imports = {}
    items = {}
    for item in value.get('items', []):
        identity = str(item.get('Id'))
        if identity in items:
            raise ValueError('商城物品目录包含重复编号')
        items[identity] = item
    products = value.get('catalog', {}).get('products', [])
    for registration in operator_registrations:
        identity = str(registration['category_id'])
        public = dict(registration)
        item = items.get(identity)
        public['available'] = item is not None
        public['shop_product_keys'] = [
            product['key'] for product in products
            if product.get('category_id') == registration['category_id'] and not product.get('retired')
        ]
        if identity in unit_imports:
            public['draft_error'] = '机体与副官登记使用了同一商城物品类别'
        elif item is None:
            public['draft_error'] = '副官商品类别尚未应用到当前商城宿主'
        else:
            try:
                create, values = _operator_draft(registration, item)
            except ValueError as error:
                public['draft_error'] = str(error)
            else:
                item['Name'] = create['name']
                public['draft_defaults'] = values
                if public['shop_product_keys']:
                    public['draft_error'] = '此副官已经存在未删除的商城商品草稿'
        operator_imports[identity] = public
    value['operator_imports'] = operator_imports
    return value


class Shop(Provider):
    id, title = 'shop', '商城商品'
    fields = {'workspace':'商城配置目录（首次空目录自动上架默认商品）','world':'WorldServer 目录','client':'客户端目录（显示标签更新）','demo':'演示模式（仅测试时填 1）'}

    def configure(self, values):
        with self.lock:
            result = super().configure(values)
            config = self.config
            if not config.get('workspace') or not config.get('world') or config.get('demo') == '1':
                return result
            helper = Path(config['world']) / 'sdgo/mods/shop_catalog/shop_catalog_apply.exe'
            if not helper.is_file():
                result['default_presets'] = 'waiting_for_server_module'
                return result
            bridge = self.workspace.services.resource('shop.bridge', Bridge)
            state = bridge.call({**config, 'action': 'read', 'body': {}, 'revision': ''})
            if state['catalog'].get('default_presets_version', 0) or state['catalog']['products']:
                return result
            applied = bridge.call({**config, 'action': 'initialize-presets', 'body': {}, 'revision': state['revision']})
            if config.get('client'):
                self._install_applied_acquisition_sources(applied)
            result['default_presets'] = 'installed'
            return result

    def _effective_applied(self):
        bridge = self.workspace.services.resource('shop.bridge', Bridge)
        return bridge.call({**self.config, 'action':'read-effective', 'body':{}, 'revision':''})

    def applied_acquisition_sources(self, client_value=None):
        client_value = client_value if client_value is not None else self.config.get('client', '')
        if not client_value:
            raise ValueError('请先选择客户端以解析商城物品类别')
        client = Path(client_value).resolve()
        if not client.is_dir() or not (client / 'GOnline.exe').is_file():
            raise ValueError('所选目录不是可用的游戏客户端')
        applied = self._effective_applied()
        return json.loads(_shop_acquisition_sources(
            client, applied.get('active_products'),
            applied.get('active_products_source')))

    def _install_applied_acquisition_sources(self, value):
        client_value = self.config.get('client', '')
        if not client_value:
            return None
        client = Path(client_value).resolve()
        source_json = _shop_acquisition_sources(
            client, value.get('active_products'),
            value.get('active_products_source'))
        document = _install_acquisition_sidecars(
            client / 'sdgo/mods/shop_localization', source_json,
            acquisition_protocol.SHOP_PROVIDER)
        return {
            'revision': document['revision'],
            'provenance': document['provenance'],
            'client_installed': True,
            'restart_required': True,
        }

    def request(self, method, path, body, headers):
        with self.lock:
            return self._request_locked(method, path, body, headers)

    def _request_locked(self, method, path, body, headers):
        if not self.config.get('workspace'): raise ValueError('请先选择商城配置工作区')
        if method=='GET' and path=='/api/shop/client-package':
            client_value = self.config.get('client','')
            revision = headers.get('X-Shop-Revision','')
            # Validate all local package inputs before opening the read-only DB bridge.
            client_package(client_value, revision, [],
                           'store_billing_visible_readback', preflight_only=True)
            applied = self._effective_applied()
            return Binary(client_package(
                client_value, revision, applied.get('active_products'),
                applied.get('active_products_source')),
                'application/zip','SDGO-shop-client-update.zip')
        bridge = self.workspace.services.resource('shop.bridge', Bridge)
        action = 'read' if method=='GET' else path.rsplit('/',1)[-1]
        if method == 'GET' and path.endswith('/export'):
            value = bridge.call({**self.config, 'action':'read', 'body':body, 'revision':''})
            return Binary(storage.json_bytes(value['catalog']), 'application/json', 'shop.json')
        operator_registrations = _operator_registrations(self.workspace, self.config.get('world'))
        by_category = {registration['category_id']: registration for registration in operator_registrations}
        units = self.workspace.providers.get('units')
        registrations = units.for_server(Path(self.config['world'])/'WorldServer.ini') if units and self.config.get('world') else []
        unit_imports = {str(r['category_id']): r for r in registrations}
        requested_category = body.get('category_id') if action == 'create' else None
        if action == 'create' and type(requested_category) is not int:
            candidate = None
            if isinstance(requested_category, str) and re.fullmatch(r'[0-9]+', requested_category):
                candidate = int(requested_category)
            elif isinstance(requested_category, float) and requested_category.is_integer():
                candidate = int(requested_category)
            if candidate in by_category:
                raise ValueError('副官商品类别必须使用整数编号')
        if action == 'create' and requested_category in by_category:
            if str(requested_category) in unit_imports:
                raise ValueError('机体与副官登记使用了同一商城物品类别')
            before = bridge.call({**self.config, 'action':'read', 'body':{}, 'revision':''})
            item = next((row for row in before.get('items', []) if row.get('Id') == requested_category), None)
            if item is None:
                raise ValueError('副官商品类别尚未应用到当前商城宿主')
            registration = by_category[requested_category]
            create, values = _operator_draft(registration, item)
            if any(product.get('category_id') == requested_category and not product.get('retired')
                   for product in before.get('catalog', {}).get('products', [])):
                raise ValueError('此副官已经存在未删除的商城商品草稿')
            created = bridge.call({**self.config, 'action':'create', 'body':create,
                                   'revision':headers.get('X-Shop-Revision','')})
            previous = {product.get('key') for product in before.get('catalog', {}).get('products', [])}
            added = [product for product in created.get('catalog', {}).get('products', [])
                     if product.get('key') not in previous]
            if len(added) != 1 or added[0].get('category_id') != requested_category:
                if len(added) == 1 and isinstance(added[0].get('key'), str):
                    try:
                        bridge.call({**self.config, 'action':'delete',
                                     'body':{'key':added[0]['key']}, 'revision':created['revision']})
                    except Exception as cleanup_error:
                        raise ValueError(
                            '商城没有返回唯一的副官商品草稿且自动删除未完成；请刷新商城检查。'
                            + ' 删除错误：' + str(cleanup_error)) from None
                raise ValueError('商城没有返回唯一的副官商品草稿')
            current_revision = created['revision']
            try:
                label = values.pop('label', None) if 'validity' in values else None
                value = bridge.call({**self.config, 'action':'update',
                                     'body':{'key':added[0]['key'], 'values':values},
                                     'revision':current_revision})
                current_revision = value['revision']
                if label is not None:
                    value = bridge.call({**self.config, 'action':'update',
                                         'body':{'key':added[0]['key'], 'values':{'label':label}},
                                         'revision':current_revision})
                    current_revision = value['revision']
            except Exception as update_error:
                try:
                    bridge.call({**self.config, 'action':'delete',
                                 'body':{'key':added[0]['key']}, 'revision':current_revision})
                except Exception as cleanup_error:
                    raise ValueError(
                        '副官商城草稿预填失败且自动删除未完成；请刷新商城并删除该草稿。'
                        + ' 预填错误：' + str(update_error)
                        + '；删除错误：' + str(cleanup_error)) from None
                raise update_error
        else:
            value=bridge.call({**self.config,'action':action,'body':body,'revision':headers.get('X-Shop-Revision','')})
            if action == 'apply':
                try:
                    publication = self._install_applied_acquisition_sources(value)
                except Exception as error:
                    raise ValueError('商城服务器应用已完成，但客户端获得方式发布失败：' + str(error)) from None
                if publication is not None:
                    value['acquisition_sources'] = publication
        for item in value.get('items', []):
            if str(item['Id']) in unit_imports:
                item['Name'] = unit_imports[str(item['Id'])]['display_name']
        return _decorate_shop_state(value, operator_registrations, unit_imports)
