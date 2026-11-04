"""Edit shared announcement rules and compile the native client inputs."""
from __future__ import annotations

import datetime as dt
import html
import json
from pathlib import Path
import re
import unicodedata

from . import storage
from .providers import Provider
from owner import timezones

EVENTS = {'enter': 1, 'timer': 2, 'login': 4, 'lobby': 8, 'room': 16,
          'battle_start': 32, 'battle_end': 64}
SURFACES = {'login': 1, 'game': 2}
DEFAULT = {'schema_version': 1, 'utc_offset_minutes': None, 'rules': []}


def integer(value, low, high, name):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(name + '超出允许范围')
    return value


def validate(value):
    if not isinstance(value, dict) or set(value) != set(DEFAULT) or value['schema_version'] != 1:
        raise ValueError('公告规则版本无效')
    timezones.validate_offset(value['utc_offset_minutes'])
    if not isinstance(value['rules'], list) or len(value['rules']) > 128:
        raise ValueError('最多支持 128 条公告')
    ids = set()
    keys = {'id', 'enabled', 'title', 'text', 'surfaces', 'triggers', 'weekdays',
            'start_time', 'end_time', 'start_date', 'end_date', 'duration_seconds',
            'interval_seconds', 'max_session', 'max_day', 'max_total', 'priority'}
    for rule in value['rules']:
        if not isinstance(rule, dict) or set(rule) != keys:
            raise ValueError('公告字段不完整或含未登记字段')
        identity = rule['id']
        if not isinstance(identity, str) or not re.fullmatch('[a-z][a-z0-9_]{0,47}', identity) or identity in ids:
            raise ValueError('公告标识无效或重复')
        ids.add(identity)
        if type(rule['enabled']) is not bool:
            raise ValueError('启用状态无效')
        for key, maximum in [('title', 80), ('text', 960)]:
            text = rule[key]
            if (not isinstance(text, str) or not text.strip() or len(text) > maximum
                    or any(ord(c) < 32 and c not in '\n\t' for c in text)
                    or any(0xD800 <= ord(c) <= 0xDFFF for c in text)):
                raise ValueError('公告标题或正文无效（标题 80、正文 960 字以内）')
        if '\n' in rule['title'] or len(lines(rule['text'])) > 64:
            raise ValueError('标题须为一行，正文换行后最多 64 行')
        for key, options in [('surfaces', SURFACES), ('triggers', EVENTS)]:
            selected = rule[key]
            if (not isinstance(selected, list) or not selected
                    or any(not isinstance(v, str) or v not in options for v in selected)
                    or len(set(selected)) != len(selected)):
                raise ValueError('公告位置或触发条件无效')
        days = rule['weekdays']
        if not isinstance(days, list) or not days or any(type(d) is not int for d in days) or len(set(days)) != len(days):
            raise ValueError('请选择有效的星期')
        for day in days:
            integer(day, 0, 6, '星期')
        for key in ('start_time', 'end_time'):
            if not isinstance(rule[key], str) or not re.fullmatch(r'(?:[01][0-9]|2[0-3]):[0-5][0-9]', rule[key]):
                raise ValueError('时间使用 HH:MM')
        for key in ('start_date', 'end_date'):
            if not isinstance(rule[key], str):
                raise ValueError('日期使用 YYYY-MM-DD，或留空')
            if rule[key]:
                try:
                    if dt.date.fromisoformat(rule[key]).isoformat() != rule[key]: raise ValueError()
                except (ValueError, TypeError):
                    raise ValueError('日期使用 YYYY-MM-DD') from None
        if rule['start_date'] and rule['end_date'] and rule['start_date'] > rule['end_date']:
            raise ValueError('结束日期不能早于开始日期')
        for key, low, high in [('duration_seconds', 3, 120), ('interval_seconds', 3, 86400),
                               ('max_session', 0, 1000000), ('max_day', 0, 1000000),
                               ('max_total', 0, 1000000), ('priority', 0, 100)]:
            integer(rule[key], low, high, key)
    return value


def lines(text):
    result = []
    for paragraph in text.replace('\t', '    ').splitlines():
        current = ''; width = 0
        for char in paragraph:
            cells = 0 if unicodedata.combining(char) else 2 if unicodedata.east_asian_width(char) in 'WFA' else 1
            if current and width + cells > 40:
                result.append(current);current = '';width = 0
            current += char;width += cells
        result.append(current or ' ')
    return result


def compile_rules(value):
    validate(value)
    output = ['[announcements]', 'schema=1', f"offset={timezones.effective_offset(value['utc_offset_minutes'])}",
              f"count={len(value['rules'])}"]
    pages = {}
    for i, rule in enumerate(value['rules']):
        body = lines(rule['text'])
        minute = lambda t: int(t[:2]) * 60 + int(t[3:])
        data = {'id': rule['id'], 'enabled': int(rule['enabled']), 'title': '"' + rule['title'] + '"',
                'surfaces': sum(SURFACES[x] for x in rule['surfaces']),
                'triggers': sum(EVENTS[x] for x in rule['triggers']),
                'days': sum(1 << x for x in rule['weekdays']),
                'start': minute(rule['start_time']), 'end': minute(rule['end_time']),
                'date_start': rule['start_date'].replace('-', '') or '0',
                'date_end': rule['end_date'].replace('-', '') or '0',
                'lines': len(body), **{k: rule[k] for k in ('duration_seconds', 'interval_seconds',
                                                        'max_session', 'max_day', 'max_total', 'priority')}}
        output += ['', f'[rule{i}]'] + [f'{k}={v}' for k, v in data.items()]
        # INI quoting preserves a leading semicolon, whitespace and literal quotes.
        output += [f'line{j}="{line}"' for j, line in enumerate(body)]
        title = html.escape(rule['title'])
        content = '<br>'.join(html.escape(x) for x in rule['text'].splitlines())
        pages[rule['id'] + '.html'] = (f'<!doctype html><html><head><meta charset="utf-8">'
            '<meta http-equiv="X-UA-Compatible" content="IE=edge">'
            '<style>body{margin:0;padding:14px;background:#101b29;color:#edf3fa;'
            'font:15px/1.6 "Microsoft YaHei",sans-serif;word-wrap:break-word}'
            'h1{font-size:20px;color:#69dce8;margin:0 0 12px}p{margin:0}</style>'
            f'</head><body><h1>{title}</h1><p>{content}</p></body></html>').encode('utf-8')
    return ('\r\n'.join(output) + '\r\n').encode('utf-16'), pages


def runtime_inputs(value):
    import hashlib
    compiled, pages = compile_rules(value)
    text = compiled.decode('utf-16')
    files = {}
    for name, data in pages.items():
        identity = name[:-5]
        page = identity + '_' + hashlib.sha256(data).hexdigest()[:16] + '.html'
        files['pages/' + page] = data
        text = text.replace(f'id={identity}\r\n', f'id={identity}\r\npage={page}\r\n')
    return {**files, 'rules.json': storage.json_bytes(value), 'runtime.ini': text.encode('utf-16')}


class Announcements(Provider):
    id, title = 'announcements', '公告与展示规则'
    fields = {'client': '要配置的游戏客户端目录'}

    def __init__(self, workspace):
        super().__init__(workspace)
        from .announcement_news import NewsService
        self.news_service=NewsService()
        if hasattr(workspace,'services'):
            workspace.services.register('announcements.news','公告资讯更新',self.news_service,
                lambda:self.news_service.start(self))

    def configure(self, values):
        if self.news_service.state()['running']:
            raise ValueError('更换客户端目录前，请先停止公告资讯更新；展示规则仍可直接编辑')
        return super().configure(values)

    def news_settings(self):
        from . import announcement_news as news
        path=self.directory()/'news-settings.json'
        raw=path.read_bytes() if path.is_file() else storage.json_bytes(news.DEFAULT)
        return news.validate(json.loads(raw)),storage.revision(raw)

    def describe(self):
        with self.lock:
            clients = [host for host in self.workspace.settings.get('hosts', []) if host.get('role') == 'client']
            if not self.config and len(clients) == 1:
                self.workspace.settings.setdefault('modules', {})[self.id] = {'client': clients[0]['path']}
                self.workspace.save_workspace()
        return super().describe()

    def directory(self):
        client = Path(self.config.get('client', '')).resolve()
        if not (client / 'GOnline.exe').is_file():
            raise ValueError('请选择包含 GOnline.exe 的客户端目录')
        target = client / 'sdgo/mods/announcements'
        if not (target / 'announcements.dll').is_file():
            raise ValueError('请先安装包含公告功能的框架更新')
        if target.resolve() != target or target.is_symlink():
            raise ValueError('公告目录不能重定向到其他位置')
        return target

    def request(self, method, path, body, headers):
        directory = self.directory()
        if path.startswith('/api/announcements/news'):
            from . import announcement_news as news
            settings,revision=self.news_settings()
            if method=='GET' and path=='/api/announcements/news':
                return {**news.public(directory,settings),'revision':revision,'service':self.news_service.state()}
            if method=='POST' and path=='/api/announcements/news/settings':
                with storage.LOCK:
                    if body.get('revision')!=revision:raise storage.Conflict('资讯设置已改变，请重新打开面板')
                    settings=news.validate(body.get('settings'))
                    storage.atomic(directory/'news-settings.json',storage.json_bytes(settings))
                    storage.atomic(directory/'news.ini',news.compile_settings(settings))
                self.news_service.request()
                return self.request('GET','/api/announcements/news',{}, {})
            if method=='POST' and path=='/api/announcements/news/refresh':
                with self.news_service.lock:news.refresh(directory,settings)
                return self.request('GET','/api/announcements/news',{}, {})
            raise ValueError('资讯操作未登记')
        file = directory / 'rules.json'
        with storage.LOCK:
            raw = file.read_bytes() if file.is_file() else storage.json_bytes(DEFAULT)
            if method == 'GET' and path == '/api/announcements':
                return {'policy': validate(json.loads(raw)), 'revision': storage.revision(raw)}
            if method == 'GET' and path == '/api/announcements/export':
                import io
                import zipfile
                from .distribution import require_public
                from .providers import Binary
                require_public((directory/'mod.ini').read_bytes(), (directory/'manager.json').read_bytes())
                exported=validate(json.loads(raw))
                exported['utc_offset_minutes']=timezones.effective_offset(exported['utc_offset_minutes'])
                files = runtime_inputs(exported)
                for name in ('announcements.dll', 'mod.ini', 'manager.json', 'symbols.fragment.ini'):
                    files[name] = (directory/name).read_bytes()
                from . import announcement_news as news
                settings=self.news_settings()[0]
                files['news-settings.json']=storage.json_bytes(settings)
                files['news.ini']=news.compile_settings(settings)
                result = io.BytesIO()
                with zipfile.ZipFile(result, 'w', zipfile.ZIP_DEFLATED) as archive:
                    for name, data in sorted(files.items()):
                        archive.writestr('sdgo/mods/announcements/' + name, data)
                return Binary(result.getvalue(), 'application/zip', 'SDGO-announcements.zip')
            if method == 'POST' and path == '/api/announcements':
                if body.get('revision') != storage.revision(raw):
                    raise storage.Conflict('公告已在另一处修改，请重新打开后编辑')
                value = validate(body.get('policy'))
                # Content-addressed page names make runtime.ini the final commit point.
                files = runtime_inputs(value)
                for name, data in files.items():
                    if name not in ('rules.json', 'runtime.ini'):
                        storage.atomic(directory / name, data)
                storage.atomic(file, files['rules.json'])
                storage.atomic(directory / 'runtime.ini', files['runtime.ini'])
                return self.request('GET', path, {}, {})
        raise ValueError('公告操作未登记')
