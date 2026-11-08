"""Read Mod-owned management contracts; UI never guesses writable settings."""
from __future__ import annotations
import json
from pathlib import Path
import re

ID = re.compile(r'[a-z][a-z0-9_-]{0,63}\Z')
ROLES = {'client', 'battle', 'agent', 'world'}
DISTIBUTIONS = {'public', 'owner', 'local_only'}
CLIENT_XML_SETTING = ('config.xml', 'Framework/Timer', 'updatefps')


def load(root):
    records = {}
    for path in sorted(Path(root).glob('*/manager.json')):
        value = json.loads(path.read_text(encoding='utf-8-sig'))
        identity = value.get('id', '')
        if value.get('schema_version') != 1 or not ID.fullmatch(identity) or identity != path.parent.name:
            raise ValueError('管理注册无效：' + str(path))
        if identity in records or not value.get('title') or not set(value.get('hosts', [])) <= ROLES:
            raise ValueError('管理注册重复或宿主无效：' + identity)
        if value.get('distribution') is not None and value['distribution'] not in DISTIBUTIONS:
            raise ValueError('Mod 分发范围无效：' + identity)
        provider_audience=value.get('provider_audience','owner')
        if provider_audience not in ('owner','user') or provider_audience=='user' and not value.get('provider'):
            raise ValueError('管理面板可见层级无效：'+identity)
        seen = set()
        for field in value.get('settings', []):
            key = field['id']
            if not ID.fullmatch(key) or key in seen or field.get('audience') not in ('user','developer'):
                raise ValueError('配置必须登记可见层级：' + identity)
            seen.add(key)
            if field['type'] not in ('boolean', 'integer', 'number', 'string', 'integer_list', 'choice'):
                raise ValueError('字段类型不受支持：' + identity)
            location = (field['file'], field.get('section', 'settings'), field['key'])
            ini_setting = field['file'] in ('mod.ini', 'config.ini') and location[1] == 'settings'
            client_xml_setting = location == CLIENT_XML_SETTING and value.get('hosts') == ['client']
            if not ini_setting and not client_xml_setting:
                raise ValueError('字段不能写入运行代码或任意文件：' + identity)
            activation=field.get('activation','restart_required')
            if activation not in ('restart','restart_required','hot_reload'):
                raise ValueError('配置生效方式无效：'+identity)
            field['activation']='restart_required'if activation=='restart'else activation
        from .dependencies import validate
        validate(value)
        from .configuration import relative
        if not isinstance(value.get('configuration_files',[]),list):raise ValueError('运行配置文件列表无效')
        for name in value.get('configuration_files',[]):relative(name)
        retirement=value.get('retirement',{})
        if not isinstance(retirement,dict)or set(retirement)-{'content_files'}:
            raise ValueError('内容停用声明无效：'+identity)
        if not isinstance(retirement.get('content_files',[]),list):raise ValueError('内容引用文件列表无效')
        for name in retirement.get('content_files',[]):
            if not isinstance(name,str)or not re.fullmatch(r'[A-Za-z0-9_.-]+',name)or name in('.','..'):
                raise ValueError('内容引用文件无效：'+identity)
        records[identity] = value
    return records


def validate(field, value):
    kind = field['type']
    if kind == 'boolean':
        if type(value) is not bool:
            raise ValueError(field['label'] + '必须是开关')
        return '1' if value else '0'
    if kind in ('integer', 'number'):
        if isinstance(value, bool) or not isinstance(value, (int, float)) or (kind == 'integer' and type(value) is not int):
            raise ValueError(field['label'] + '数值无效')
        if not field.get('minimum', -2147483648) <= value <= field.get('maximum', 2147483647):
            raise ValueError(field['label'] + '超出允许范围')
        return str(value)
    if not isinstance(value, str) or any(c in value for c in '\r\n\0') or len(value) > field.get('max_length', 4096):
        raise ValueError(field['label'] + '文本无效')
    if kind == 'integer_list' and value:
        if not re.fullmatch(r'\d+(?:\s*,\s*\d+)*', value) or any(not 1 <= int(x) <= 2147483647 for x in value.split(',')):
            raise ValueError(field['label'] + '需要用逗号分隔正整数编号')
    if kind == 'choice' and value not in field['options']:
        raise ValueError(field['label'] + '选项无效')
    if field.get('pattern') and not re.fullmatch(field['pattern'],value):
        raise ValueError(field['label']+'格式无效')
    return value
