"""Bounded public feed fetching, cache and the Manager-owned refresh service."""
from __future__ import annotations
import copy
import datetime as dt
import html
import ipaddress
import json
import os
from pathlib import Path
import re
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from contextlib import contextmanager

from . import storage

KINDS = ('news', 'tech', 'epic', 'history')
PATHS = dict(zip(KINDS, ('/v2/60s', '/v2/it-news', '/v2/epic', '/v2/today_in_history')))
LABELS = {'news': '每日新闻', 'tech': '科技资讯', 'epic': '游戏限免', 'history': '历史上的今天'}
DEFAULT = dict(schema_version=1, enabled=True,
    sources=['https://60s.crystelf.top', 'https://60s.okbaike.com'], categories=list(KINDS),
    refresh_minutes=360, max_age_hours=48, login_enabled=True, login_news_count=3,
    login_tech_count=2, login_duration_seconds=25, game_enabled=True,
    game_max_day=5, game_max_session=3, game_interval_seconds=600, game_duration_seconds=10,
    start_time='00:00',end_time='00:00',weekdays=list(range(7)),
    game_triggers=['login','lobby','timer'],game_lobby_only=True)


def endpoint(value):
    if not isinstance(value, str) or len(value)>400 or any(ord(c)<33 for c in value):
        raise ValueError('资讯来源地址无效')
    parsed=urllib.parse.urlsplit(value)
    try:port=parsed.port
    except ValueError:raise ValueError('资讯来源端口无效') from None
    if not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment:
        raise ValueError('来源地址须为不含账号和查询参数的 HTTPS 根地址')
    loopback=False
    try:loopback=ipaddress.ip_address(parsed.hostname).is_loopback
    except ValueError:pass
    if parsed.scheme!='https' and not (parsed.scheme=='http' and loopback):
        raise ValueError('资讯来源使用 HTTPS；本机接口可使用回环 HTTP')
    if port is not None and not 1<=port<=65535:raise ValueError('资讯来源端口无效')
    if any(piece in ('.','..') for piece in parsed.path.split('/')):raise ValueError('来源路径无效')
    return value.rstrip('/')


def validate(value):
    from .announcements import integer
    if not isinstance(value,dict) or set(value)!=set(DEFAULT) or type(value['schema_version']) is not int or value['schema_version']!=1:
        raise ValueError('动态资讯配置版本无效')
    for key in ('enabled','login_enabled','game_enabled','game_lobby_only'):
        if type(value[key]) is not bool:raise ValueError('资讯开关无效')
    if not isinstance(value['sources'],list) or not 1<=len(value['sources'])<=3:raise ValueError('配置一至三个资讯来源')
    sources=[endpoint(x) for x in value['sources']]
    if sources!=value['sources'] or len(set(sources))!=len(sources):raise ValueError('来源重复或末尾含多余斜线')
    if (not isinstance(value['categories'],list) or not value['categories'] or
        any(not isinstance(k,str) or k not in KINDS for k in value['categories']) or
        len(set(value['categories']))!=len(value['categories'])):raise ValueError('资讯类别无效')
    for key in ('start_time','end_time'):
        if not isinstance(value[key],str) or not re.fullmatch(r'(?:[01][0-9]|2[0-3]):[0-5][0-9]',value[key]):raise ValueError('资讯时段使用 HH:MM')
    if (not isinstance(value['weekdays'],list) or not value['weekdays'] or
        any(type(day) is not int or not 0<=day<=6 for day in value['weekdays']) or len(set(value['weekdays']))!=len(value['weekdays'])):raise ValueError('资讯星期无效')
    from .announcements import EVENTS
    if (not isinstance(value['game_triggers'],list) or not value['game_triggers'] or
        any(not isinstance(event,str) or event not in EVENTS for event in value['game_triggers']) or
        len(set(value['game_triggers']))!=len(value['game_triggers'])):raise ValueError('资讯触发事件无效')
    for key,low,high in [('refresh_minutes',30,1440),('max_age_hours',1,72),('login_news_count',0,5),
                         ('login_tech_count',0,5),('login_duration_seconds',5,120),('game_max_day',1,20),
                         ('game_max_session',1,10),('game_interval_seconds',30,86400),('game_duration_seconds',3,60)]:
        integer(value[key],low,high,key)
    return value


def compile_settings(value):
    validate(value)
    values={k:int(v) if isinstance(v,bool) else ','.join(map(str,v)) if isinstance(v,list) else v for k,v in value.items()}
    return ('[news]\r\n'+'\r\n'.join(f'{k}={v}' for k,v in values.items())+'\r\n').encode('utf-16')


def clean(text, maximum=180):
    if not isinstance(text,str):raise ValueError('feed text is not a string')
    # Literal text only. No source markup, instructions or scripts become UI code.
    text=' '.join(html.unescape(re.sub('<[^>]*>',' ',text)).split())
    if not text or len(text)>maximum or any(ord(c)<32 or 0xD800<=ord(c)<=0xDFFF for c in text):raise ValueError('invalid feed text')
    return text


def identity(kind,title,url):
    number=14695981039346656037
    for byte in (kind+'\n'+title+'\n'+url).encode('utf-8'):
        number=((number^byte)*1099511628211)&0xffffffffffffffff
    return 'dailynews_'+kind+'_'+format(number,'016x')


def url(value):
    if not isinstance(value,str) or len(value)>2048 or any(ord(c)<33 for c in value):raise ValueError('invalid article URL')
    parsed=urllib.parse.urlsplit(value)
    if parsed.scheme!='https' or not parsed.hostname or parsed.username or parsed.password:raise ValueError('invalid article URL')
    return value


def normalized(kind,payload,now,max_age):
    if not isinstance(payload,dict) or type(payload.get('code')) is not int or payload['code']!=200:raise ValueError('feed response rejected')
    data=payload.get('data');items=[]
    def add(title,link,source,published,expires,date,offer_end=0):
        title=clean(title);link=url(link);source=clean(source,60)
        if type(published) is not int or type(expires) is not int or published>now+300 or published<=0 or expires<=now:return
        expires=min(expires,published+max_age)
        if expires<=now:return
        item=dict(id=identity(kind,title,link),kind=kind,title=title,url=link,source=source,
                  date=date,published_at=published,expires_at=expires,offer_end_at=offer_end)
        if not any(p['id']==item['id'] for p in items):items.append(item)
    utc8=dt.timezone(dt.timedelta(hours=8))
    if kind=='news':
        if not isinstance(data,dict) or not isinstance(data.get('news'),list):raise ValueError('invalid daily feed')
        date=dt.date.fromisoformat(data['date']);published=int(dt.datetime.combine(date,dt.time(),utc8).timestamp())
        for text in data['news'][:30]:
            try:add(text,data['link'],'60秒读懂世界',published,published+max_age,date.isoformat())
            except ValueError:continue
    elif kind=='tech':
        if not isinstance(data,list):raise ValueError('invalid technology feed')
        for row in data[:40]:
            try:
                milliseconds=row['created_at']
                if type(milliseconds) is not int:continue
                published=milliseconds//1000;date=dt.datetime.fromtimestamp(published,utc8).date().isoformat()
                add(row['title'],row['link'],'IT之家',published,published+max_age,date)
            except (ValueError,KeyError,TypeError,OverflowError,OSError):continue
    elif kind=='epic':
        if not isinstance(data,list):raise ValueError('invalid game promotion feed')
        for row in data[:30]:
            try:
                start,end=row['free_start_at'],row['free_end_at']
                if type(start) is not int or type(end) is not int or not start//1000<=now<end//1000:continue
                if not isinstance(row.get('original_price'),(int,float)) or isinstance(row['original_price'],bool) or row['original_price']<=0:continue
                # The API's is_free_now flag can include future offers. Use the
                # explicit validity interval instead; permanent free games stay out.
                add(row['title'],row['link'],'Epic Games',now,end//1000,dt.datetime.fromtimestamp(start//1000,utc8).date().isoformat(),end//1000)
            except (ValueError,KeyError,TypeError,OverflowError,OSError):continue
    elif kind=='history':
        if not isinstance(data,dict) or not isinstance(data.get('items'),list):raise ValueError('invalid history feed')
        date=dt.datetime.fromtimestamp(now,utc8).date()
        if (data.get('month'),data.get('day'))!=(date.month,date.day):raise ValueError('history feed date does not match')
        published=int(dt.datetime.combine(date,dt.time(),utc8).timestamp())
        rows=sorted(data['items'][:100],key=lambda r:0 if any(x in str(r.get('title','')) for x in ('科学','发现','发明','卫星','计算机','通讯','游戏','电影')) else 1)
        for row in rows:
            try:add(str(row['year'])+'年：'+row['title'],row['link'],'历史上的今天',published,published+86400,date.isoformat())
            except (ValueError,KeyError,TypeError):continue
    else:raise ValueError('unknown feed kind')
    return items[:20]


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self,*args,**kwargs):raise ValueError('feed redirects are not accepted')


def parse_payload(raw):
    if len(raw)>524288:raise ValueError('feed response too large')
    def members(pairs):
        result={}
        for key,value in pairs:
            if key in result:raise ValueError('duplicate feed object member')
            result[key]=value
        return result
    def constant(value):raise ValueError('non-finite feed number')
    try:
        value=json.loads(raw.decode('utf-8') if isinstance(raw,bytes) else raw,object_pairs_hook=members,parse_constant=constant)
        pending=[(value,0)];count=0
        while pending:
            item,depth=pending.pop();count+=1
            if depth>24 or count>40000:raise ValueError('feed structure exceeds limits')
            if isinstance(item,dict):pending.extend((child,depth+1) for child in item.values())
            elif isinstance(item,list):pending.extend((child,depth+1) for child in item)
        return value
    except (RecursionError,UnicodeError):raise ValueError('invalid bounded feed JSON') from None


def fetch(address):
    request=urllib.request.Request(address,headers={'User-Agent':'SDGO-Announcements/0.2 (+https://github.com/crabsatellite/SDGO-Mod-Framework)', 'Accept':'application/json'})
    with urllib.request.build_opener(NoRedirect()).open(request,timeout=5) as response:
        if response.status!=200:raise ValueError('feed response is not successful')
        chunks=[];length=0;deadline=time.monotonic()+5
        while True:
            if time.monotonic()>deadline:raise TimeoutError('feed response deadline')
            chunk=response.read1(min(8192,524289-length))
            if not chunk:break
            chunks.append(chunk);length+=len(chunk)
            if length>524288:raise ValueError('feed response too large')
        raw=b''.join(chunks)
    if len(raw)>524288:raise ValueError('feed response too large')
    return parse_payload(raw)


@contextmanager
def lease(file):
    file.parent.mkdir(parents=True,exist_ok=True)
    if os.name=='nt':
        import ctypes
        from ctypes import wintypes as w
        k=ctypes.WinDLL('kernel32',use_last_error=True)
        k.CreateFileW.argtypes=(w.LPCWSTR,w.DWORD,w.DWORD,ctypes.c_void_p,w.DWORD,w.DWORD,w.HANDLE);k.CreateFileW.restype=w.HANDLE
        k.CloseHandle.argtypes=(w.HANDLE,)
        handle=k.CreateFileW(str(file),0xC0000000,0,None,4,0x80,None)
        if handle==ctypes.c_void_p(-1).value:raise storage.Conflict('另一个客户端或 Manager 正在刷新资讯')
        try:yield
        finally:k.CloseHandle(handle)
    else:
        import fcntl
        with file.open('a+b') as stream:
            try:fcntl.flock(stream,fcntl.LOCK_EX|fcntl.LOCK_NB)
            except BlockingIOError:raise storage.Conflict('资讯正在刷新') from None
            yield


def cache_directory(directory):
    # Per-installation state never enters a shared Mod configuration package.
    return Path(directory).parent.parent/'local-state/announcements-news'


def read_cache(directory):
    try:
        raw=(cache_directory(directory)/'cache.json').read_bytes()
        if len(raw)>524288:raise ValueError()
        data=json.loads(raw)
        if data.get('schema_version')!=1 or not isinstance(data.get('feeds'),dict):raise ValueError()
        return data
    except (OSError,ValueError,TypeError):return dict(schema_version=1,attempted_at=0,feeds={},errors=[])


def refresh(directory,settings,*,now=None,fetcher=fetch):
    settings=validate(settings);now=int(time.time() if now is None else now)
    if not settings['enabled']:raise ValueError('请先启用动态资讯')
    cache=cache_directory(directory)
    with lease(cache/'.refresh.lock'):
        data=read_cache(directory);errors=[];deadline=time.monotonic()+45
        for kind in settings['categories']:
            success=False
            for base in settings['sources']:
                if time.monotonic()>deadline:break
                try:
                    items=normalized(kind,fetcher(base+PATHS[kind]),now,settings['max_age_hours']*3600)
                    if kind!='epic' and not items:raise ValueError('feed has no recent items')
                    data['feeds'][kind]=dict(fetched_at=now,source=base,items=items);success=True;break
                except (OSError,ValueError,TypeError,KeyError,TimeoutError):continue
            if not success:errors.append(kind)
        data['attempted_at']=now;data['errors']=errors
        storage.atomic(cache/'cache.json',storage.json_bytes(data))
    return public(directory,settings,now=now)


def public(directory,settings,*,now=None):
    now=int(time.time() if now is None else now);cache=read_cache(directory);items=[]
    for kind in settings['categories']:
        for item in cache['feeds'].get(kind,{}).get('items',[]):
            if (isinstance(item,dict) and type(item.get('expires_at')) is int and item['expires_at']>now
                    and type(item.get('published_at')) is int and 0<=now-item['published_at']<settings['max_age_hours']*3600):items.append(item)
    return {'settings':settings,'last_attempt':cache['attempted_at'],'failed_categories':cache.get('errors',[]),
            'items':items,'available_count':len(items)}


class NewsService:
    def __init__(self):
        self.thread=None;self.stop_event=threading.Event();self.wake=threading.Event()
        self.error='';self.last_refresh=0;self.next_refresh=0;self.lock=threading.Lock()
    def state(self):
        running=bool(self.thread and self.thread.is_alive())
        return dict(running=running,healthy=running,error=self.error,ownership='manager_process',
                    last_refresh=self.last_refresh,next_refresh=self.next_refresh)
    def request(self):self.wake.set()
    def start(self,provider):
        if self.thread and self.thread.is_alive():self.request();return self.state()
        if not provider.news_settings()[0]['enabled']:raise ValueError('请先启用动态资讯')
        self.stop_event.clear();self.wake.set()
        def run():
            while not self.stop_event.is_set():
                if self.wake.is_set() or time.time()>=self.next_refresh:
                    self.wake.clear()
                    try:
                        with provider.lock:
                            directory=provider.directory();settings=provider.news_settings()[0]
                        with self.lock:result=refresh(directory,settings)
                        self.error=('部分资讯来源暂不可用：'+ '、'.join(LABELS[x] for x in result['failed_categories'])) if result['failed_categories'] else ''
                        self.last_refresh=int(time.time());self.next_refresh=time.time()+(900 if self.error else settings['refresh_minutes']*60)
                    except Exception as error:
                        self.error=str(error) if isinstance(error,(ValueError,storage.Conflict)) else '资讯刷新未完成，稍后重试'
                        self.next_refresh=time.time()+900
                self.stop_event.wait(1)
        self.thread=threading.Thread(target=run,name='sdgo-announcement-news',daemon=True);self.thread.start();return self.state()
    def stop(self):
        self.stop_event.set()
        if self.thread:self.thread.join(timeout=60)
        return self.state()
