"""Pinned owner programmes, separate from public releases and owner content."""
from pathlib import Path
import json
import re
import subprocess
from . import distribution, storage, updates

KIND = 'sdgo-owner-programme'
MANIFEST = 'owner-programme.json'
PREFIX = 'programmes/'
ROOT = 'sdgo/owner-mods/'
ID = re.compile('[a-z][a-z0-9_]{0,63}')
HASH = re.compile('[0-9a-f]{64}')
OID = re.compile('[0-9a-f]{40}')


def binding(value, publisher):
    from .server_profiles import feed
    if not isinstance(value, dict) or value.get('schema_version') != 1 or value.get('kind') != KIND:
        raise ValueError('本服程序源格式无效')
    if set(value)-{'schema_version','kind','source_id','channel','feeds','revision','sha256','commit','mods'}:
        raise ValueError('本服程序来源包含未登记字段')
    if value.get('source_id') != publisher:
        raise ValueError('本服程序与内容必须属于同一发布者')
    if value.get('channel') not in ('stable', 'beta'):
        raise ValueError('本服程序通道无效')
    feeds = value.get('feeds')
    if not isinstance(feeds, list) or not 1 <= len(feeds) <= 3 or len(set(feeds)) != len(feeds):
        raise ValueError('配置一至三个本服程序镜像')
    if type(value.get('revision')) is not int or value['revision'] < 1 or not HASH.fullmatch(str(value.get('sha256', ''))):
        raise ValueError('本服程序须锁定修订与摘要')
    if value.get('commit') and not OID.fullmatch(str(value['commit'])):
        raise ValueError('本服程序锁定提交无效')
    return {**value, 'feeds': [feed(url) for url in feeds]}


def name(value):
    updates.safe_name(value)
    parts = value.split('/')
    if len(parts) != 4 or '/'.join(parts[:2]) != 'sdgo/owner-mods' or not ID.fullmatch(parts[2]):
        raise ValueError('本服程序只能安装到独立 Mod 目录：' + value)
    if parts[3] not in (parts[2] + '.dll', 'mod.ini', 'manager.json', 'symbols.fragment.ini'):
        raise ValueError('本服程序包只能包含声明的程序与元数据：' + value)
    return value


def validate(value, source, framework):
    if not isinstance(value, dict) or value.get('schema_version') != 1 or value.get('kind') != KIND:
        raise ValueError('本服程序清单无效')
    if set(value)-{'schema_version','kind','source_id','channel','revision','framework','files','mods'}:
        raise ValueError('本服程序清单包含未登记字段')
    for key in ('source_id', 'channel', 'revision'):
        if value.get(key) != source[key]:
            raise ValueError('本服程序发布身份、通道或修订不匹配')
    if value.get('framework') != framework:
        raise ValueError('本服程序锁定的官方基线与内容档不同')
    files, mods = value.get('files'), value.get('mods')
    if not isinstance(mods, dict) or not isinstance(files, dict) or len(files) > 4096:
        raise ValueError('本服程序文件清单无效')
    if source.get('mods',mods)!=mods:raise ValueError('本服 Mod 选择与锁定清单不一致')
    case = set()
    for path, entry in files.items():
        name(path)
        if path.casefold() in case or path.split('/')[2] not in mods:
            raise ValueError('本服程序路径重复或未声明')
        case.add(path.casefold())
        if not isinstance(entry, dict) or type(entry.get('bytes')) is not int or not 0 <= entry['bytes'] <= 64 * 1024**2 or not HASH.fullmatch(str(entry.get('sha256', ''))):
            raise ValueError('本服程序文件摘要无效')
    if sum(e['bytes']for e in files.values())>256*1024**2:raise ValueError('本服程序包总量超过限制')
    for identity, version in mods.items():
        if not ID.fullmatch(identity):
            raise ValueError('本服 Mod 标识无效')
        updates.version(version)
        for leaf in (identity + '.dll', 'mod.ini', 'manager.json'):
            if ROOT + identity + '/' + leaf not in files:
                raise ValueError('本服 Mod 程序或元数据不完整：' + identity)
    return value


def _read(controller, url, path, limit, commit=None):
    from . import server_profiles, git_source
    if url.startswith(('git+ssh://', 'git+https://')) and commit:
        remote, _ = git_source.parse(url)
        repo = controller.cache/'git'/updates.digest(url.encode())
        repo.mkdir(parents=True, exist_ok=True)
        if not (repo/'HEAD').is_file():
            controller.git(repo, ['init', '--bare'])
            controller.git(repo, ['remote', 'add', 'origin', remote])
        if controller.git(repo, ['config', '--get', 'remote.origin.url']).decode().strip() != remote:
            raise ValueError('本服程序缓存身份不一致')
        controller.git(repo, ['fetch', '--filter=blob:none', '--no-tags', '--depth=1', 'origin', commit])
        raw = controller.git(repo, ['show', commit + ':' + path])
        if len(raw) > limit:
            raise ValueError('本服程序文件超过限制')
        return raw
    # read() treats its own manifest name specially for HTTPS/offline sources.
    if url.startswith('local+file:'):
        manifest = server_profiles.local_path(url)
        path = manifest if path == MANIFEST else updates.child(manifest.parent, path)
        if not path.is_file() or path.stat().st_size > limit:
            raise ValueError('离线本服程序文件缺失或过大')
        return path.read_bytes()
    if url.startswith('https:'):
        from urllib.parse import urljoin
        endpoint = url if path == MANIFEST else urljoin(url, path)
        return updates.fetch(endpoint, limit, controller.operation_stop)
    raw, transport = server_profiles.read(controller, url, path, limit)
    return raw


def module(folder,identity,version):
    from .server_profiles import check_secrets,SECRET_KEYS
    cfg=distribution.mod_ini((folder/'mod.ini').read_bytes())
    registration=json.loads((folder/'manager.json').read_bytes())
    if distribution.resolve((folder/'mod.ini').read_bytes(),(folder/'manager.json').read_bytes())!=distribution.OWNER:
        raise ValueError('本服包只能分发明确声明 owner 的 Mod')
    if cfg.get('mod','id')!=identity or cfg.get('mod','version')!=version or cfg.get('mod','dll')!=identity+'.dll':
        raise ValueError('本服 Mod 身份与文件清单不一致')
    if registration.get('id')!=identity or registration.get('hosts')!=['client']:
        raise ValueError('玩家程序包只能包含客户端 Mod')
    hosts=cfg.get('mod','hosts',fallback='').split(',')
    if not hosts or any(not re.fullmatch(r'GOnline-[0-9A-F]{8}-[0-9A-F]{8}',h.strip())for h in hosts):
        raise ValueError('本服程序须声明实际支持的客户端宿主')
    check_secrets({section:dict(cfg[section])for section in cfg.sections()})
    check_secrets(registration)
    for field in registration.get('settings',[]):
        if re.sub('[^a-z]','',str(field.get('key','')).casefold())in SECRET_KEYS and field.get('default')not in(None,'',[],{}):
            raise ValueError('私人凭据不能作为本服 Mod 默认配置发布')
    from . import registry
    registry.load(folder.parent)


def load(controller, source, framework):
    source = binding(source, source.get('source_id'))
    folder = controller.cache/'owner-programmes'/source['sha256']
    folder.mkdir(parents=True, exist_ok=True)
    errors = []
    for url in source['feeds']:
        try:
            raw = _read(controller, url, MANIFEST, 2 * 1024**2, source.get('commit'))
            value = validate(json.loads(raw), source, framework)
            if updates.digest(storage.json_bytes(value)) != source['sha256']:
                raise ValueError('本服程序镜像不是同一锁定清单')
            for path, entry in value['files'].items():
                target = updates.child(folder, path)
                from .client_runtime import matches
                if not matches(target, entry):
                    controller.report_progress(phase='owner_programme', file=path, total_bytes=entry['bytes'], current_bytes=0)
                    if url.startswith('https:'):
                        from urllib.parse import urljoin
                        from . import transfers
                        transfers.download(urljoin(url,PREFIX+path),target,entry,controller.operation_stop,
                            lambda count:controller.report_progress(current_bytes=count))
                        data=target.read_bytes()
                    else:data = _read(controller, url, PREFIX + path, entry['bytes'] + 1, source.get('commit'))
                    if len(data) != entry['bytes'] or updates.digest(data) != entry['sha256']:
                        raise ValueError('本服程序文件校验失败：' + path)
                    distribution.require_owner_payload(path, data)
                    storage.atomic(target, data)
                distribution.require_owner_payload(path, target.read_bytes())
            for identity, version in value['mods'].items():
                base = folder/ROOT/identity
                module(base,identity,version)
            return {'manifest': value, 'bundle': str(folder), 'source': source}
        except InterruptedError:
            raise
        except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
            errors.append(str(error))
    raise ValueError('本服程序在同一发布者的镜像均不可用；保留当前安装：' + '；'.join(errors))


def publish(host, source, framework, directory, selected, previous=None):
    """Publish only explicitly selected, owner-distributable installed modules."""
    from .server_profiles import feed
    publisher = source['source_id']
    if not re.fullmatch('[0-9a-f]{32}', publisher) or source.get('channel') not in ('stable', 'beta'):
        raise ValueError('本服程序发布身份或通道无效')
    if not isinstance(selected, list) or len(set(selected)) != len(selected):
        raise ValueError('显式选择要分发的本服 Mod')
    directory = Path(directory).resolve(); client = Path(host['path']).resolve()
    if directory.is_relative_to(client) or client.is_relative_to(directory):
        raise ValueError('程序发布目录须与编辑客户端分开')
    old = previous or storage.read_json(directory/MANIFEST, {})
    if old and old.get('source_id') != publisher:
        raise ValueError('程序发布目录属于其他发布者')
    data = {}; mods = {}
    for identity in selected:
        if not ID.fullmatch(identity):
            raise ValueError('本服 Mod 标识无效')
        folder = client/ROOT/identity
        for leaf in (identity + '.dll', 'mod.ini', 'manager.json'):
            if not (folder/leaf).is_file():
                raise ValueError('本服 Mod 文件不完整：' + identity)
        if distribution.resolve((folder/'mod.ini').read_bytes(), (folder/'manager.json').read_bytes()) != distribution.OWNER:
            raise ValueError('仅分发 owner Mod；仅本地插件不可发布')
        mods[identity] = distribution.mod_ini((folder/'mod.ini').read_bytes()).get('mod', 'version')
        module(folder,identity,mods[identity])
        for leaf in (identity + '.dll', 'mod.ini', 'manager.json', 'symbols.fragment.ini'):
            path = folder/leaf
            if path.is_file():
                raw = path.read_bytes(); distribution.require_owner_payload(ROOT+identity+'/'+leaf, raw)
                data[ROOT+identity+'/'+leaf] = raw
    value = {'schema_version': 1, 'kind': KIND, 'source_id': publisher, 'channel': source['channel'],
             'revision': 1, 'framework': framework, 'mods': mods,
             'files': {p: {'bytes': len(raw), 'sha256': updates.digest(raw)} for p, raw in sorted(data.items())}}
    changed = {k:v for k,v in old.items() if k != 'revision'} != {k:v for k,v in value.items() if k != 'revision'}
    value['revision'] = old.get('revision', 0) + (changed or not old)
    result = {'schema_version': 1, 'kind': KIND, 'source_id': publisher, 'channel': source['channel'],
              'revision': value['revision'], 'sha256': updates.digest(storage.json_bytes(value)),
              'feeds': [feed(url) for url in source['feeds']], 'mods': mods}
    validate(value, result, framework)
    binding(result,publisher)
    # Git consumers pin a commit. HTTPS/offline consumers pin an immutable
    # revision directory so advancing the channel never destroys an old lock.
    revision_dir=directory/'revisions'/str(value['revision'])
    for path,raw in {MANIFEST:storage.json_bytes(value),**{PREFIX+p:b for p,b in data.items()}}.items():
        target=revision_dir/path
        if target.exists()and target.read_bytes()!=raw:raise ValueError('已发布的本服程序修订不可覆盖')
        if not target.exists():storage.atomic(target,raw)
    pinned=[]
    for url in result['feeds']:
        if url.startswith('https:'):
            from urllib.parse import urljoin
            url=urljoin(url,'revisions/'+str(value['revision'])+'/'+MANIFEST)
        elif url.startswith('local+file:'):
            url='local+'+(revision_dir/MANIFEST).as_uri()
        pinned.append(url)
    result['feeds']=pinned
    for path, raw in data.items(): storage.atomic(directory/PREFIX/path, raw)
    storage.atomic(directory/MANIFEST, storage.json_bytes(value))
    return result


def unknown_files(root, expected):
    """Only load-capable Mod paths; personal data is backed up locally."""
    root = Path(root); result = []; expected={name.casefold()for name in expected}
    for tree in ('mods', 'owner-mods', 'local-mods'):
        base = root/'sdgo'/tree
        if not base.is_dir(): continue
        for folder in base.iterdir():
            if folder.is_symlink() or (hasattr(folder,'is_junction')and folder.is_junction()): raise ValueError('Mod 目录包含链接，无法安全隔离')
            if folder.name.startswith('.') or not folder.is_dir() or not (folder/'mod.ini').is_file(): continue
            declared = (folder/'mod.ini').relative_to(root).as_posix().casefold() in expected
            for path in folder.iterdir():
                relative = path.relative_to(root).as_posix()
                if path.is_symlink(): raise ValueError('Mod 文件包含链接，无法安全隔离')
                if path.is_file() and relative.casefold() not in expected and (not declared or path.suffix.casefold() in ('.dll', '.asi')):
                    result.append(relative)
    return sorted(result)
