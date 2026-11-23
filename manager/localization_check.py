"""Read-only checks of selected-client display ownership and shop dictionary."""
from __future__ import annotations
import hashlib
from pathlib import Path
from . import storage


def _units(text):
    return len(text.encode('utf-16-le')) // 2


def dictionary(raw):
    """Match dictionary.h's exact keys, escapes, field and length constraints."""
    if not raw or len(raw) > 8 * 1024 * 1024:
        raise ValueError('词典大小必须为1字节至8 MiB')
    text = raw.decode('utf-8-sig', errors='strict')
    entries = {}
    section = ''
    fields = {'Kind': '', 'Source': '', 'Text': ''}
    line_number = 0

    def fail(message):
        raise ValueError(f'词典 line={line_number} section={section or "(none)"}: {message}')

    def unescape(value):
        output = []
        index = 0
        escapes = {'n': '\n', 'r': '\r', 't': '\t', 's': ' ', '\\': '\\'}
        while index < len(value):
            char = value[index]
            if char == '\\':
                index += 1
                if index == len(value) or value[index] not in escapes:
                    fail('无效的转义；支持 \\n、\\r、\\t、\\s 和 \\\\')
                char = escapes[value[index]]
            if char == '\0':
                fail('条目不能包含 NUL')
            output.append(char)
            index += 1
        result = ''.join(output)
        if not 1 <= _units(result) <= 8192:
            fail('解码后的文本须为1至8192个UTF-16单元')
        return result

    def commit():
        if not section:
            return
        if not all(fields.values()) or fields['Kind'] not in ('name', 'description', 'option'):
            fail('需要非空 Source、Text 和有效 Kind=name/description/option')
        source, target = unescape(fields['Source']), unescape(fields['Text'])
        if source in ('GB', 'MB') or all(char in '0123456789., +-\t' for char in source):
            fail('金额和币种不能作为翻译键')
        if source in entries:
            fail('Source 解码后重复，必须按完整原文去重')
        entries[source] = (fields['Kind'], target)
        if len(entries) > 20000:
            fail('词条超过20000条')

    # split on LF only; other line separators remain native source characters.
    for line_number, line in enumerate(text.split('\n'), 1):
        if line.endswith('\r'):
            line = line[:-1]
        if not line or line[0] in ';#':
            continue
        if _units(line) > 8192 * 2 + 8:
            fail('单行超过原生长度限制')
        if line.startswith('[') and line.endswith(']'):
            commit()
            section = line[1:-1]
            fields = {'Kind': '', 'Source': '', 'Text': ''}
            if not section.startswith('entry:') or _units(section) > 80:
                fail('节名必须以 entry: 开头且不超过80个UTF-16单元')
        else:
            if '=' not in line or not section:
                fail('字段不在条目节中或缺少等号')
            key, value = line.split('=', 1)
            if key not in fields or fields[key]:
                fail('字段未知或重复；字段名须为 Kind、Source、Text，区分大小写')
            fields[key] = value
    commit()
    if not entries:
        fail('词典没有条目')
    return entries


def _read(client, path, limit):
    if path.is_symlink() or not path.resolve().is_relative_to(client.resolve()):
        raise ValueError('检查文件必须位于所选客户端内，不能使用外部链接')
    with path.open('rb') as stream:
        raw = stream.read(limit + 1)
    if len(raw) > limit:
        raise ValueError('检查文件超过容量限制')
    return raw


def check(workspace, host_id):
    host = workspace.host(host_id)
    if host['role'] != 'client':
        raise ValueError('汉化数据检查需要选择客户端目录')
    client = Path(host['path'])
    installed = {row['id']: row for row in workspace._installed(host, True)}
    results = []
    files = []
    count = fields = 0
    unit_enabled = installed.get('unit_catalog', {}).get('runtime_enabled', False)
    shop_enabled = installed.get('shop_localization', {}).get('runtime_enabled', False)
    unit_root = workspace._mod_directory(host, 'unit_catalog')
    seen = set()
    from owner.units.display_text import FIELDS, issues
    from owner.units.indexes import catalog_records
    for scope, folder in (('owner', unit_root), ('presets', unit_root / 'presets')):
        path = folder / 'catalog.bin'
        if not path.exists():
            continue
        try:
            raw = _read(client, path, 64 * 1024 * 1024 + 32)
            rows = catalog_records(raw)
            if not rows or len(seen) + len(rows) > 8192 or seen.intersection(rows):
                raise ValueError('机体目录为空、合并记录超限或两个分区存在重复编号')
            seen.update(rows)
            count += len(rows)
            for identity, (kind, record) in rows.items():
                fields += len(FIELDS.get(kind, ()))
                results.extend(dict(row, scope=scope) for row in issues(kind, identity, record))
            files.append({'scope': scope, 'sha256': hashlib.sha256(raw).hexdigest(), 'records': len(rows)})
        except (OSError, ValueError) as error:
            results.append({'code': 'invalid_catalog', 'scope': scope, 'message': str(error)})
    ownership_issues = len(results)
    dictionary_state = {'valid': False, 'entries': 0}
    try:
        raw = _read(client, workspace._mod_directory(host, 'shop_localization') / 'zh-CN.ini', 8 * 1024 * 1024)
        dictionary_state = {'valid': True, 'entries': len(dictionary(raw)), 'sha256': hashlib.sha256(raw).hexdigest()}
    except (OSError, ValueError) as error:
        results.append({'code': 'invalid_dictionary', 'message': str(error)})
    warnings = []
    if not shop_enabled:
        warnings.append('商城汉化未安装或开关已关闭；开启并重启游戏后才可能翻译。')
    shop_manifest = workspace._mod_directory(host, 'shop_localization') / 'mod.ini'
    if shop_manifest.is_file():
        cfg = storage.ini_values(_read(client, shop_manifest, 1024 * 1024))
        for key, title in (('TranslateNames', '商品名称汉化'), ('TranslateDescriptions', '商品说明汉化'),
                           ('TranslateOptions', '商城选项汉化')):
            if cfg.get('settings', key, fallback='1').strip() == '0':
                warnings.append(title + '已关闭；保存开关并重启游戏后才会翻译。')
    if not unit_enabled and ownership_issues:
        warnings.append('机体目录目前未启用；修正列出的问题后再启用。')
    blocked = (bool(ownership_issues) and unit_enabled) or not dictionary_state['valid']
    message = ('发现汉化数据问题；归属不确定时普通商品保留原文，请修正内容源包并重新发布。' if blocked else
               '静态数据检查通过；实际加载、挂钩和游戏画面仍需启动游戏后确认。')
    return {'status': 'blocked' if blocked else 'warning' if results or warnings else 'ready',
            'message': message, 'changes_applied': False, 'runtime_verified': False,
            'records': count, 'protected_fields': fields, 'files': files,
            'dictionary': dictionary_state, 'issue_count': len(results), 'issues': results[:32],
            'warnings': warnings, 'truncated': len(results) > 32}
