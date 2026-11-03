"""Manager-owned stable release updates. Packages contain framework files only."""
from __future__ import annotations
import configparser
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import stat
import sys
import threading
import time
import urllib.request
import zipfile
from . import storage

REPOSITORY = 'crabsatellite/SDGO-Mod-Framework'
LATEST = 'https://api.github.com/repos/' + REPOSITORY + '/releases/latest'
RELEASES = 'https://api.github.com/repos/' + REPOSITORY + '/releases?per_page=30'
MANAGER = 'SDGO Mod Manager.exe'
SERVICE = 'manager.updates'
MAX_ARCHIVE = 128 * 1024 * 1024
MAX_EXPANDED = 512 * 1024 * 1024
EXES = {'client': 'GOnline.exe', 'battle': 'BattleServer.exe', 'agent': 'AgentServer.exe', 'world': 'WorldServer.exe'}


def version(value):
    if not isinstance(value, str) or not re.fullmatch(r'v?\d+\.\d+\.\d+(?:-beta\.\d+)?', value):
        raise ValueError('更新版本号无效')
    core,_,beta=value.lstrip('v').partition('-beta.')
    return tuple(int(n) for n in core.split('.')) + ((0,int(beta)) if beta else (1,0))


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def safe_name(name):
    if (not isinstance(name, str) or '\\' in name or not name or
            any(c in name for c in ':*?"<>|\x00') or any(ord(c)<32 for c in name) or
            any(p in ('', '.', '..') or p.endswith((' ', '.')) for p in name.split('/'))):
        raise ValueError('更新包路径无效')
    return name


def child(root, name):
    root = Path(root).resolve()
    path = storage.safe_child(root, safe_name(name))
    walk = path
    while True:
        if walk.is_symlink() or (walk.exists() and getattr(walk.lstat(), 'st_file_attributes', 0) & 0x400):
            raise ValueError('更新目标不能是目录链接')
        if walk == root: break
        if walk == walk.parent:raise ValueError('更新路径不属于目标目录')
        walk = walk.parent
    return path


def read_ini(raw):
    result = configparser.ConfigParser(interpolation=None, strict=True)
    result.optionxform = str
    result.read_string(storage.decode_ini(raw)[0])
    return result


def serialize_ini(value):
    stream = io.StringIO(); value.write(stream, space_around_delimiters=False)
    return stream.getvalue().encode('utf-8-sig')


def fetch(url, limit, stop=None):
    request = urllib.request.Request(url, headers={'User-Agent': 'SDGO-Mod-Manager', 'Accept': 'application/vnd.github+json'})
    with urllib.request.urlopen(request, timeout=20) as response:
        if int(response.headers.get('Content-Length', '0')) > limit:
            raise ValueError('更新下载超过大小限制')
        chunks=[]; size=0
        while True:
            if stop and stop.is_set(): raise InterruptedError('Manager 已停止检查更新')
            block=response.read(256*1024)
            if not block: break
            size+=len(block)
            if size>limit: raise ValueError('更新下载超过大小限制')
            chunks.append(block)
        return b''.join(chunks)


def release_asset(data, channel='stable'):
    if data.get('draft') or (data.get('prerelease') and channel!='beta'): raise ValueError('当前通道不接收测试 release')
    tag=data.get('tag_name'); version(tag)
    label=tag.lstrip('v'); name='SDGO-ModFramework-'+label+'.zip'
    if bool(data.get('prerelease'))!=('-beta.' in label):raise ValueError('release 通道标记不一致')
    assets=[a for a in data.get('assets',[]) if a.get('name')==name and a.get('state')=='uploaded']
    if len(assets)!=1: raise ValueError('此 release 没有可验证的 Manager 更新包')
    asset=assets[0]; expected='https://github.com/'+REPOSITORY+'/releases/download/'+tag+'/'+name
    if asset.get('browser_download_url')!=expected or not re.fullmatch(r'sha256:[0-9a-f]{64}', asset.get('digest','')):
        raise ValueError('release 下载来源或 SHA-256 无效')
    size=asset.get('size')
    if type(size) is not int or not 0<size<=MAX_ARCHIVE: raise ValueError('release 大小无效')
    return {'version':label,'url':expected,'sha256':asset['digest'][7:],'bytes':size,
            'channel':'beta' if data.get('prerelease') else 'stable',
            'notes':str(data.get('body',''))[:16000], 'page':'https://github.com/'+REPOSITORY+'/releases/tag/'+tag}


def unpack(raw, asset, destination):
    if len(raw)!=asset['bytes'] or digest(raw)!=asset['sha256']:
        raise ValueError('下载文件校验失败，未安装任何文件')
    with zipfile.ZipFile(io.BytesIO(raw)) as archive:
        entries=archive.infolist()
        if len(entries)>10000 or sum(e.file_size for e in entries)>MAX_EXPANDED:
            raise ValueError('更新包解压量无效')
        names=set(); folded=set()
        for entry in entries:
            name=safe_name(entry.filename)
            if entry.is_dir() or stat.S_ISLNK(entry.external_attr>>16): raise ValueError('更新包不能含链接或目录条目')
            if name.casefold() in folded or entry.file_size>64*1024*1024: raise ValueError('更新包文件重复或过大')
            names.add(name);folded.add(name.casefold())
        manifest=json.loads(archive.read('release.json'))
        version(manifest.get('version'))
        if (manifest.get('schema_version')!=1 or manifest.get('repository')!=REPOSITORY or manifest.get('version')!=asset['version'] or
                manifest.get('channel')!=asset['channel']):
            raise ValueError('更新包身份与 release 不一致')
        if not re.fullmatch('[0-9a-f]{40}',manifest.get('source_commit','')): raise ValueError('更新包缺少源码提交')
        files=manifest.get('files')
        if not isinstance(files,dict) or names!=set(files)|{'release.json'} or MANAGER not in files:
            raise ValueError('更新包文件清单不完整')
        payload={}
        for name, expected in files.items():
            if name!=MANAGER and not name.startswith(('client/','server/')): raise ValueError('更新包含框架以外的文件')
            if any(x.lower() in ('gonline.exe','battleserver.exe','agentserver.exe','worldserver.exe') or
                   x.lower().endswith(('.mdf','.ldf','.bak','.zpk','.zdx')) for x in name.split('/')):
                raise ValueError('更新包不能包含原始游戏或数据库')
            data=archive.read(name)
            if len(data)!=expected.get('bytes') or digest(data)!=expected.get('sha256'):
                raise ValueError('更新包内文件校验失败：'+name)
            payload[name]=data
        for tree in ('client','server'):
            catalog=json.loads(payload[tree+'/sdgo/catalog.json'])
            channels={'stable','beta'} if manifest['channel']=='beta' else {'stable'}
            if any(m.get('channel') not in channels or m.get('category')=='development' for m in catalog['mods']):
                raise ValueError('更新包包含当前通道以外的 Mod')
            for mod in catalog['mods']:
                if mod['target']!=tree: continue
                base=tree+'/sdgo/mods/'+mod['id']+'/'
                for name in (mod['id']+'.dll','mod.ini','manager.json',*mod['files'],*mod.get('runtime_files',[])):
                    if base+name not in payload:raise ValueError('更新包缺少 Mod 文件')
        allowed={MANAGER}
        for tree,loader in (('client','d3d9.dll'),('server','winmm.dll')):
            allowed.update(tree+'/'+name for name in (loader,'sdgo/core.dll','sdgo/sdgo.ini','sdgo/catalog.json'))
            allowed.update(n for n in payload if re.fullmatch(tree+r'/sdgo/symbols/[A-Za-z]+-[0-9A-F]{8}-[0-9A-F]{8}\.ini',n))
            for mod in json.loads(payload[tree+'/sdgo/catalog.json'])['mods']:
                if mod['target']!=tree:continue
                if not re.fullmatch(r'[a-z][a-z0-9_]{0,63}',mod['id']):raise ValueError('更新包 Mod 标识无效')
                base=tree+'/sdgo/mods/'+mod['id']+'/'
                allowed.update(base+safe_name(n) for n in (mod['id']+'.dll','mod.ini','manager.json',*mod['files'],*mod.get('runtime_files',[])))
        if set(payload)!=allowed:raise ValueError('更新包包含未登记文件，或缺少框架文件')
        destination.mkdir(parents=True,exist_ok=True)
        for name,data in payload.items(): storage.atomic(child(destination,name),data)
        storage.atomic(destination/'release.json',storage.json_bytes(manifest))
        return manifest


def running(executable):
    """Read only. An unreadable process with the same name defers installation."""
    if os.name!='nt': return False
    import ctypes
    from ctypes import wintypes as w
    class Entry(ctypes.Structure):
        _fields_=[('size',w.DWORD),('usage',w.DWORD),('pid',w.DWORD),('heap',ctypes.c_size_t),
                  ('module',w.DWORD),('threads',w.DWORD),('parent',w.DWORD),('priority',w.LONG),
                  ('flags',w.DWORD),('exe',w.WCHAR*260)]
    k=ctypes.WinDLL('kernel32',use_last_error=True)
    k.CreateToolhelp32Snapshot.restype=w.HANDLE
    k.Process32FirstW.argtypes=(w.HANDLE,ctypes.POINTER(Entry));k.Process32NextW.argtypes=k.Process32FirstW.argtypes
    k.OpenProcess.argtypes=(w.DWORD,w.BOOL,w.DWORD);k.OpenProcess.restype=w.HANDLE
    k.QueryFullProcessImageNameW.argtypes=(w.HANDLE,w.DWORD,w.LPWSTR,ctypes.POINTER(w.DWORD))
    k.CloseHandle.argtypes=(w.HANDLE,)
    snapshot=k.CreateToolhelp32Snapshot(2,0)
    if snapshot==ctypes.c_void_p(-1).value: raise ValueError('无法核对游戏进程，稍后重试')
    try:
        entry=Entry();entry.size=ctypes.sizeof(entry);found=k.Process32FirstW(snapshot,ctypes.byref(entry))
        while found:
            if entry.exe.casefold()==executable.name.casefold():
                process=k.OpenProcess(0x1000,False,entry.pid)
                if not process: return True
                try:
                    size=w.DWORD(32768);buffer=ctypes.create_unicode_buffer(size.value)
                    if not k.QueryFullProcessImageNameW(process,0,buffer,ctypes.byref(size)): return True
                    if Path(buffer.value).resolve()==executable.resolve(): return True
                finally:k.CloseHandle(process)
            found=k.Process32NextW(snapshot,ctypes.byref(entry))
    finally:k.CloseHandle(snapshot)
    return False


def host_key(executable):
    import struct
    with executable.open('rb') as stream:
        stream.seek(0x3c); offset=struct.unpack('<I',stream.read(4))[0]
        stream.seek(offset)
        if stream.read(4)!=b'PE\0\0':raise ValueError('目标游戏程序格式无效')
        stream.seek(offset+8);stamp=struct.unpack('<I',stream.read(4))[0]
        stream.seek(offset+24+56);size=struct.unpack('<I',stream.read(4))[0]
    return f'{executable.stem}-{stamp:08X}-{size:08X}'


def install_host(host, bundle, manifest, cache, is_running=None):
    is_running=is_running or running
    root=Path(host['path']).resolve();role=host['role'];exe=root/EXES[role]
    marker=root/'sdgo/release.json';prior=storage.read_json(marker,{})
    if prior.get('version') and prior.get('channel')==manifest['channel'] and version(prior['version'])>version(manifest['version']):
        return {'id':host['id'],'name':host['name'],'status':'newer','version':prior['version']}
    if is_running(exe):return {'id':host['id'],'name':host['name'],'status':'waiting','message':'关闭游戏或对应服务器后自动安装'}
    tree='client' if role=='client' else 'server';source=bundle/tree
    key=host_key(exe);symbols='sdgo/symbols/'+key+'.ini'
    if not (source/symbols).is_file():raise ValueError('此游戏程序版本暂不支持自动更新')
    catalog=json.loads((source/'sdgo/catalog.json').read_text(encoding='utf-8'))
    supported=[m for m in catalog['mods'] if m['target']==tree and key in m['hosts']]
    selected=[];retained=[];protected_symbols=set()
    for mod in supported:
        ini=child(root,'sdgo/mods/'+mod['id']+'/mod.ini')
        old=read_ini(ini.read_bytes()) if ini.exists() else None
        try:newer=old is not None and version(old['mod']['version'])>version(mod['version'])
        except (KeyError,ValueError):newer=False
        if newer:
            retained.append({**mod,'version':old['mod']['version'],'channel':old['mod'].get('channel',mod['channel'])})
            protected_symbols.update(n.strip() for n in old['mod'].get('requires','').split(',') if n.strip())
        else:selected.append(mod)
    loader='d3d9.dll' if role=='client' else 'winmm.dll'
    if (root/loader).exists() and not (root/'sdgo/core.dll').exists():
        raise ValueError('这个目录有其他图形/音频扩展，请先用框架安装器完成首次安装')
    names=[loader,'sdgo/core.dll','sdgo/sdgo.ini',symbols]
    preserve=set()
    for mod in selected:
        base='sdgo/mods/'+mod['id']+'/'
        names.extend(base+n for n in (mod['id']+'.dll','mod.ini',*mod['files'],*mod.get('runtime_files',[])))
        preserve.update(base+n for n in mod['config_files'])
    changes={}
    with storage.LOCK:
        for name in dict.fromkeys(names):
            path=child(root,name)
            if path.exists() and (name=='sdgo/sdgo.ini' or name in preserve):continue
            data=(source/name).read_bytes()
            if path.exists() and name.endswith('/mod.ini'):
                old=read_ini(path.read_bytes());new=read_ini(data)
                patches=[]
                for section in old.sections():
                    for k,v in old[section].items():
                        if section.lower()=='settings' or (section.lower()=='mod' and k.lower()=='enabled'):
                            patches.append((section,k,v))
                data=storage.patch_ini(data,patches)
            if path.exists() and name==symbols:
                old=read_ini(path.read_bytes());new=read_ini(data)
                for section in old.sections():
                    if not new.has_section(section) or section in protected_symbols:new[section]=dict(old[section])
                data=serialize_ini(new)
            if not path.exists() or path.read_bytes()!=data:changes[path]=data
        catpath=child(root,'sdgo/catalog.json');current=storage.read_json(catpath,{'schema_version':1,'mods':[],'features':{},'categories':{}})
        ids={m['id'] for m in selected+retained};features={m['feature'] for m in selected}
        existing={m['id']:m for m in current['mods']}
        kept=[{**existing.get(m['id'],m),'version':m['version'],'channel':m['channel']} for m in retained]
        current['mods']=[m for m in current['mods'] if m['id'] not in ids]+selected+kept
        current.setdefault('features',{}).update({k:v for k,v in catalog['features'].items() if k in features})
        current.setdefault('categories',{}).update(catalog['categories'])
        catbytes=storage.json_bytes(current)
        if not catpath.exists() or catpath.read_bytes()!=catbytes:changes[catpath]=catbytes
        installed={'schema_version':1,'version':manifest['version'],'channel':manifest['channel'],'source_commit':manifest['source_commit'],
                   'mods':{m['id']:m['version'] for m in selected+retained}}
        mbytes=storage.json_bytes(installed)
        if not marker.exists() or marker.read_bytes()!=mbytes:changes[marker]=mbytes
        # Recheck immediately before touching runtime files. Locked files also
        # cause a transaction rollback; no game process is ever stopped.
        if is_running(exe):return {'id':host['id'],'name':host['name'],'status':'waiting','message':'等待对应游戏或服务器关闭'}
        previous={p:p.read_bytes() if p.exists() else None for p in changes};written=[]
        backup=cache/'backups'/manifest['version']/digest(str(root).encode())[:16]
        try:
            for path,data in changes.items():
                if previous[path] is not None:
                    saved=child(backup,path.relative_to(root).as_posix())
                    if not saved.exists():storage.atomic(saved,previous[path])
                storage.atomic(path,data);written.append(path)
        except OSError:
            for path in reversed(written):
                if previous[path] is None:path.unlink(missing_ok=True)
                else:storage.atomic(path,previous[path])
            raise
    return {'id':host['id'],'name':host['name'],'status':'installed','version':manifest['version'],'changed_files':len(changes),
            'preserved_newer_mods':[m['id'] for m in retained]}


class Updates:
    def __init__(self,workspace,distribution,manager_exe=None,fetcher=fetch):
        self.workspace=workspace;self.distribution=Path(distribution).resolve()
        self.manager_exe=Path(manager_exe).resolve() if manager_exe else None
        self.fetcher=fetcher;self.cache=workspace.root/'updates';self.lock=threading.RLock()
        self.busy=threading.Lock();self.stop_event=threading.Event();self.wake=threading.Event();self.thread=None
        info=json.loads((Path(__file__).with_name('repository.json')).read_text(encoding='utf-8'))
        self.current=info['manager_version'];self.latest=None;self.bundle=None;self.manifest=None
        self.status={'phase':'idle','message':'等待检查更新','hosts':[],'error':'','checked_at':None,'manager_restart':False}
        self.install_requested=False

    def state(self):
        with self.lock:
            return {**self.status,'running':bool(self.thread and self.thread.is_alive()),'healthy':True,
                    'automatic':self.workspace.services.desired(SERVICE),'current_version':self.current,
                    'channel':self.workspace.settings.get('updates',{}).get('channel','stable'),
                    'latest_version':self.latest['version'] if self.latest else None,
                    'release_url':self.latest['page'] if self.latest else None,
                    'notes':self.latest['notes'] if self.latest else ''}

    def change(self,**values):
        with self.lock:self.status.update(values)

    def channel(self,value):
        if value not in ('stable','beta'):raise ValueError('更新通道无效')
        if not self.busy.acquire(blocking=False):raise storage.Conflict('正在处理更新，请完成后切换通道')
        try:
            with storage.LOCK:
                self.workspace.settings.setdefault('updates',{})['channel']=value;self.workspace.save_workspace()
            self.latest=None;self.manifest=None;self.bundle=None
            self.change(phase='idle',message='已切换到 '+value.title(),hosts=[],manager_restart=False,error='')
        finally:self.busy.release()
        return self.request()

    def start(self):
        if self.thread and self.thread.is_alive():return
        self.stop_event.clear()
        def loop():
            next_check=0
            while not self.stop_event.is_set():
                try:
                    if self.wake.is_set() or time.monotonic()>=next_check:
                        self.wake.clear();self.cycle(self.workspace.services.desired(SERVICE) or self.install_requested)
                        self.install_requested=False;next_check=time.monotonic()+3600
                    elif self.manifest and self.workspace.services.desired(SERVICE):
                        host_ids={h['id'] for h in self.workspace.settings['hosts']}
                        if self.status['phase']=='waiting' or host_ids!={h['id'] for h in self.status['hosts']}:self.apply()
                except InterruptedError:break
                except Exception as error:
                    self.change(phase='error',error=str(error),message='更新未完成，可稍后重试')
                    next_check=time.monotonic()+60
                self.stop_event.wait(5)
        self.thread=threading.Thread(target=loop,name='sdgo-release-updates',daemon=True);self.thread.start()

    def stop(self):
        self.stop_event.set()
        if self.thread:self.thread.join(timeout=25)

    def request(self,install=False):
        self.install_requested=install;self.wake.set()
        self.start()
        return self.state()

    def cycle(self,install=False):
        if not self.busy.acquire(blocking=False):return
        try:
            self.change(phase='checking',message='正在检查更新',error='')
            channel=self.workspace.settings.get('updates',{}).get('channel','stable')
            data=json.loads(self.fetcher(RELEASES if channel=='beta' else LATEST,2*1024*1024,self.stop_event))
            if channel=='beta':
                choices=[]
                for row in data:
                    try:choices.append(release_asset(row,channel))
                    except (ValueError,TypeError,KeyError):continue
                if not choices:raise ValueError('尚无可用的 Beta 或 Stable 更新包')
                # Beta packages include additional Mods, so prefer that payload
                # over Stable for the same core version. A newer core wins.
                latest=max(choices,key=lambda r:(version(r['version'])[:3],r['channel']=='beta',version(r['version'])[-1]))
            else:latest=release_asset(data,channel)
            self.latest=latest;self.change(checked_at=time.time())
            if version(latest['version'])[:3]<version(self.current)[:3]:
                self.change(phase='current',message='当前版本已是最新');return
            folder=self.cache/'releases'/(latest['version']+'-'+latest['sha256'][:12])
            if self.manifest is None or self.manifest['version']!=latest['version'] or self.bundle!=folder:
                self.change(phase='downloading',message='正在下载并校验正式版本')
                raw=self.fetcher(latest['url'],MAX_ARCHIVE,self.stop_event)
                self.manifest=unpack(raw,latest,folder);self.bundle=folder
            self.change(phase='available',message='发现新版本，可安装')
            if install:self._apply()
        finally:self.busy.release()

    def apply(self):
        if not self.busy.acquire(blocking=False):return
        try:self._apply()
        finally:self.busy.release()

    def _apply(self):
        if not self.manifest:return
        rows=[]
        # Always verify cached input again before an installation/retry.
        for name,expected in self.manifest['files'].items():
            raw=child(self.bundle,name).read_bytes()
            if digest(raw)!=expected['sha256'] or len(raw)!=expected['bytes']:raise ValueError('已下载的更新文件发生变化，请重新检查')
        self.change(phase='installing',message='正在安装已关闭目录中的更新')
        hosts=list(self.workspace.settings['hosts'])
        for host in hosts:
            try:rows.append(install_host(host,self.bundle,self.manifest,self.cache))
            except Exception as error:rows.append({'id':host['id'],'name':host['name'],'status':'error','message':str(error)})
        if self.manager_exe:
            target=self.manager_exe.parent/'framework'
            # The local distribution is a cache, not the user's writable host.
            # Keep private/custom additions and replace only declared payloads.
            for name,expected in self.manifest['files'].items():
                if name==MANAGER:continue
                dest=child(target,name);raw=child(self.bundle,name).read_bytes()
                if not dest.exists() or digest(dest.read_bytes())!=expected['sha256']:storage.atomic(dest,raw)
        restart=bool(self.manager_exe and digest(self.manager_exe.read_bytes())!=self.manifest['files'][MANAGER]['sha256'])
        waiting=any(r['status']=='waiting' for r in rows);errors=any(r['status']=='error' for r in rows)
        phase='error' if errors else 'waiting' if waiting else 'ready' if restart else 'current'
        self.change(phase=phase,hosts=rows,manager_restart=restart,
                    message='部分目录暂未安装，查看下方原因' if errors else '等待游戏或服务器关闭后自动安装' if waiting else
                            'Mod 已更新，重启 Manager 完成管理器更新' if restart else '已是最新正式版本')


def attach(workspace,distribution,manager_exe=None):
    controller=Updates(workspace,distribution,manager_exe)
    workspace.updates=controller
    workspace.services.register(SERVICE,'正式版本自动更新',controller,controller.start)
    with storage.LOCK:
        if SERVICE not in workspace.settings.get('services',{}):
            workspace.settings.setdefault('services',{})[SERVICE]={'enabled':True};workspace.save_workspace()
    return controller
