"""Prepare a private client and reuse the game's existing presentation paths.

No server definitions, shop products, inventories or machine economics are applied.
The normal native UI must report both presentations before Manager accepts approval.
"""
from __future__ import annotations
import configparser
import copy
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import struct
import subprocess
import uuid
import zipfile

from .compiler import HOST, HOST_SHA256, ROOT, publish
from .model import content_revision
from .workspace import save_json

RENDERER_CONTRACT='native-unit-presentations-4-normal-shop-index-capsule-frames-9'


class StartupEvent:
    """The original launcher's named gate, owned/released by Manager."""
    def __init__(self):
        import ctypes
        from ctypes import wintypes
        self.kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        self.kernel.CreateEventW.argtypes=[wintypes.LPVOID,wintypes.BOOL,wintypes.BOOL,wintypes.LPCWSTR]
        self.kernel.CreateEventW.restype=wintypes.HANDLE
        self.kernel.CloseHandle.argtypes=[wintypes.HANDLE]
        self.handle=self.kernel.CreateEventW(None,False,False,'9YOU SDGO')
        if not self.handle:raise OSError(ctypes.get_last_error(),'Cannot create original launcher startup gate')
    def close(self):
        if self.handle:self.kernel.CloseHandle(self.handle);self.handle=None


def digest_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _inside(engine, path):
    path=Path(path).resolve()
    if not path.is_relative_to((engine.root/'native-previews').resolve()):
        raise ValueError('原生预览路径不属于当前机体工作区')
    return path


def _index(engine):
    path=engine.root/'native-previews/index.json'
    return json.loads(path.read_bytes()) if path.exists() else {}


def _discard(engine,directory):
    from manager.packages import _running
    directory=_inside(engine,directory)
    base=(engine.root/'native-previews').resolve()
    marker=directory/'manager-preview-owner.json'
    if directory.parent!=base or len(directory.name)!=32 or not marker.is_file():return False
    try:owner=json.loads(marker.read_bytes())
    except (OSError,ValueError):return False
    if owner!={'provider':'unit_import','token':directory.name}:return False
    if (directory/'client/GOnline.exe').is_file() and _running(directory/'client/GOnline.exe'):return False
    # Resolved, marker-owned, stopped child of this workspace only. Hardlinks
    # to base archives lose only this directory entry, never the source data.
    shutil.rmtree(directory)
    return True


def cleanup(engine):
    with engine.lock:
        drafts=engine.read()['drafts'];index=_index(engine);current={}
        for identity,entry in index.items():
            draft=drafts.get(identity)
            if not draft or draft['status']=='disabled' or entry.get('content_revision')!=content_revision(draft):continue
            try:
                proof=status(engine,identity)
                if proof['prepared'] and not proof.get('error'):current[identity]=entry
            except (OSError,ValueError,KeyError,TypeError):continue
        if current!=index:save_json(engine.root/'native-previews/index.json',current)
        keep={Path(r['client']).resolve().parent for r in current.values()}
        base=engine.root/'native-previews';removed=0
        if base.is_dir():
            for directory in base.iterdir():
                if directory.is_dir() and directory.resolve() not in keep and _discard(engine,directory):removed+=1
        return {'removed_copies':removed,'current_previews_retained':len(keep),'running_clients_stopped':False}


def status(engine, identity):
    with engine.lock:
        draft=engine._get(engine.read(),identity)
        entry=_index(engine).get(identity)
        value={'prepared':False,'ready':False,'shop':False,'capsule':False,
               'evidence':'pending_native_ui','content_revision':content_revision(draft)}
        if not entry or entry['content_revision']!=value['content_revision']:return value
        if entry.get('renderer_contract')!=RENDERER_CONTRACT:
            value['error']='卡面生成逻辑已更新，请重新准备原生预览';return value
        client=_inside(engine,entry['client'])
        if not client.is_dir():return value
        from manager.engines.capsules.capsule_acquisition import display_number
        value.update(prepared=True,client=str(client),machine_id=entry['machine_id'],machine_kind=entry.get('machine_kind','fixed'),slot=1,
                     machine_number=display_number(entry['machine_id'],entry.get('machine_kind','fixed')),
                     instructions='启动此独立预览客户端并正常登录。进入商城打开机体详情，原生机体视图会展示候选；'+
                     ('进入自定义扭蛋，选择预览机台并点击候选机体，查看原生等级框和 unitview。' if entry.get('machine_kind')=='custom' else
                      '进入固定扭蛋，选择预览机台并点击第一个机体槽，查看实际卡面和原生 unitview。')+
                     '购买、赠送和抽取在此副本中禁用。')
        runtime=client/'sdgo/mods/unit_catalog'
        if (not (runtime/'unit_catalog.dll').is_file() or not (runtime/'catalog.bin').is_file() or
            digest_file(runtime/'unit_catalog.dll')!=entry['module_sha256'] or digest_file(runtime/'catalog.bin')!=entry['catalog_sha256']):
            value['error']='预览组件或候选目录已变化，请重新准备';return value
        for relative,expected in entry.get('files',{}).items():
            path=(client/relative).resolve()
            if not path.is_relative_to(client) or not path.is_file() or digest_file(path)!=expected:
                value['error']='原生预览资源已变化，请重新准备';return value
        archive=client/'GPackData.zpk'
        if not archive.is_file() or [archive.stat().st_size,archive.stat().st_mtime_ns]!=entry.get('base_archive'):
            value['error']='客户端基础资源已变化，请重新准备';return value
        for role in ('shop','capsule'):
            receipt=runtime/f'native-preview-{role}.json'
            if not receipt.is_file():continue
            try:
                if receipt.stat().st_size>4096:continue
                row=json.loads(receipt.read_bytes())
            except (OSError,ValueError):continue
            valid=(row.get('schema_version')==1 and row.get('role')==role and
                   row.get('nonce')==entry['nonce'] and row.get('content_revision')==value['content_revision'] and
                   row.get('unit_id')==draft['root_unit_id'] and row.get('category_id')==draft['category_id'] and
                   row.get('native_logic') is True)
            value[role]=valid
            value[role+'_detail']=str(row.get('detail',''))[:300]
        value['ready']=value['shop'] and value['capsule']
        if value['ready']:value['evidence']='native_function_calls_pending_manual_visual_review'
        return value


def require_seen(engine, identity):
    value=status(engine,identity)
    if not value['ready']:
        raise ValueError('请先在独立预览客户端调用实际商城和扭蛋界面，再确认本版本；资源缩略图不能替代原生 UI')
    return value


def _clone_base(source, target):
    from .assets import _is_reparse
    from manager.storage import patch_ini,ini_values
    def copy_tree(origin,destination):
        if _is_reparse(origin) or any(_is_reparse(p) for p in origin.rglob('*')):
            raise ValueError('预览基础目录不能包含重解析路径')
        shutil.copytree(origin,destination)
    if digest_file(source/'GOnline.exe')!=HOST_SHA256:raise ValueError('原生预览仅支持已验证的客户端版本')
    target.mkdir()
    # Immutable resource archive is read by the stock game and compilers. No
    # installer or updater is ever allowed to write these shared hardlinks.
    for name in ('GPackData.zpk','GPackData.zdx'):
        try:os.link(source/name,target/name)
        except OSError:shutil.copyfile(source/name,target/name)
    for file in source.iterdir():
        if not file.is_file() or file.name in ('GPackData.zpk','GPackData.zdx'):continue
        if file.name=='GOnline.exe' or file.suffix.lower() in ('.dll','.ini','.cfg','.xml','.ttf','.mpg','.des'):
            if file.is_symlink():raise ValueError('客户端基础文件不能使用符号链接')
            shutil.copyfile(file,target/file.name)
    for name in ('mss','GameGuard'):
        if (source/name).is_dir():
            copy_tree(source/name,target/name)
    sdgo=target/'sdgo';sdgo.mkdir(exist_ok=True)
    shutil.copyfile(source/'sdgo/core.dll',sdgo/'core.dll')
    copy_tree(source/'sdgo/symbols',sdgo/'symbols')
    # Keep the owner's connection routing and ordinary automatic fixes. Other
    # gameplay/developer mods do not participate in the candidate client.
    for folder in (source/'sdgo/mods').iterdir():
        manifest=folder/'mod.ini'
        if not manifest.is_file():continue
        cfg=ini_values(manifest.read_bytes())
        if cfg.get('mod','category',fallback='') in ('fixes','network'):
            copy_tree(folder,sdgo/'mods'/folder.name)
    global_ini=source/'sdgo/sdgo.ini'
    raw=global_ini.read_bytes() if global_ini.is_file() else b''
    changes=[('categories','fixes','1'),('categories','gameplay','1'),('categories','development','0'),
             ('features','unit-import','1'),('features','capsule_catalog','1'),('mods','unit_catalog','1'),('mods','capsule_catalog','1')]
    (sdgo/'sdgo.ini').write_bytes(patch_ini(raw,changes))


def _capsule_catalog(engine, family, client, output):
    from manager.engines.capsules.capsule_catalog import ClientData,gdt_records,grade,GRADE_SETS
    from manager.engines.capsules.capsule_client_compile import compile_catalog
    from .presentation import capsule_unit
    reader=ClientData(client)
    _,rows=gdt_records(reader.read_resource('gdrs/0033.gdt'))
    source=None
    for row in rows:
        ids=list(struct.unpack_from('<6I',row,27))
        if row[4]==0 and len(set(ids))==6 and all(n in engine.repository.catalog['units'] for n in ids):
            source=row;break
    if source is None:raise ValueError('原生机台中没有可复用的六槽预览模板')
    native_ids=list(struct.unpack_from('<6I',source,27))
    root=family['root_unit_id']
    root_form=next(f for f in family['forms'] if f['id']==root)
    custom=root_form['server']['UnitGradeNumber']==4
    ids=native_ids if custom else [root]+native_ids[1:]
    units={uid:{'id':uid,'name':record['UnitModelName'],'rank':record['UnitGradeNumber'],
        'subgrade':record['UnitGradeNumber2'],'grade':grade(record),'eligible':True,'transform_to':0,'capsule_sprites':[]}
        for uid,record in engine.repository.catalog['units'].items()
        if record['UnitGradeNumber'] in (1,2,3,4) and record['UnitGradeNumber2'] in (1,2,3,4,5)}
    units[root]=capsule_unit(engine,family)
    for index,uid in enumerate(native_ids):
        # An earlier preview can already contain this root. Its resident
        # card must not override the author's newly selected source image.
        if uid==root:continue
        units[uid]['capsule_sprites']=[source[59+(index*2+j)*60:59+(index*2+j+1)*60].decode('utf-16-le').split('\0')[0] for j in (0,1)]
    machine_id=struct.unpack_from('<I',source,5)[0]
    catalog={'schema_version':1,'units':list(units.values()),'custom_machines':[],'fixed_machines':[
        {'id':machine_id,'template_id':machine_id,'name':'机体导入预览 · '+family['name'],'enabled':True,
         'owner':'mod','currency':'GP','price':struct.unpack_from('<I',source,15)[0],
         'draw_policy':'independent_weighted','slots':[{'unit_id':uid,'weight':weight} for uid,weight in zip(ids,[20,80,250,250,250,150])]}]}
    kind='fixed'
    if custom:
        size,custom_rows=gdt_records(reader.read_resource('gdrs/0154.gdt'))
        if size!=231 or not custom_rows:raise ValueError('客户端缺少可复用的原生自定义机台模板')
        row=next((r for r in custom_rows if r[4]==0),None)
        if row is None:raise ValueError('没有已启用的原生自定义机台')
        machine_id=struct.unpack_from('<I',row,5)[0];kind='custom'
        catalog['custom_machines']=[{'id':machine_id,'name':'机体导入预览 · '+family['name'],'enabled':True,'owner':'mod',
            'currency_type':struct.unpack_from('<H',row,9)[0],'price':struct.unpack_from('<I',row,11)[0],
            'draw_policy':'native_mileage','slot_rules':[{'slot':i+1,'rank_max':row[115+i*2],
                'subgrades':GRADE_SETS[row[116+i*2]],'extension':i>=6} for i in range(8)],
            'candidates':[{'unit_id':root,'basic_weight':100,'mileage':0.0,'extra_price':0,'reset_mileage':False,'featured':True}]}]
    lookup=output.parent/'lookup.json';lookup.write_text('{}')
    compile_catalog(catalog,client,lookup,output)
    return {'id':machine_id,'kind':kind}


def prepare(engine, identity):
    with engine.lock:
        state=engine.read();draft=engine._get(state,identity)
        if draft['status']=='disabled':raise ValueError('已停用的草稿不能预览')
        blockers=[r for r in engine.requirements(draft,state) if r['level']!='warning']
        if blockers:raise ValueError('请先补齐或明确处理内容缺项：'+blockers[0]['message'])
        source=engine.baseline.root
        if source is None:raise ValueError('原生 UI 预览需要完整的配套客户端')
        directory=engine.root/'native-previews'/uuid.uuid4().hex;directory.mkdir(parents=True)
        save_json(directory/'manager-preview-owner.json',{'provider':'unit_import','token':directory.name})
        try:
            return _prepare_private(engine,identity,state,draft,source,directory)
        except BaseException:
            try:_discard(engine,directory)
            except (OSError,ValueError):pass
            raise


def _prepare_private(engine,identity,state,draft,source,directory):
    from .deployment import install_package
    client=directory/'client';_clone_base(source,client)
    candidate=copy.deepcopy(state)
    family={k:copy.deepcopy(v) for k,v in draft.items() if k not in ('published','release','server_result','published_server_result')}
    candidate['drafts'][identity]['published']=family
    release=directory/'release';release.mkdir()
    publication=publish(engine,candidate,identity,release)
    install_package(client,Path(publication['client_zip']).read_bytes())
    capsule=client/'sdgo/mods/capsule_catalog'
    selection=_capsule_catalog(engine,family,client,capsule)
    machine_id=selection['id']
    binary=next((p for p in (ROOT/'unit-import/capsule_catalog.dll',ROOT/'build/Release/capsule_catalog.dll',
                source/'sdgo/mods/capsule_catalog/capsule_catalog.dll') if p.is_file()),None)
    if binary is None:raise ValueError('缺少配套的原生扭蛋目录组件，不能声称已调用原生界面')
    shutil.copyfile(binary,capsule/'capsule_catalog.dll')
    shutil.copyfile(ROOT/'support/capsule_registry/runtime-mod.ini',capsule/'mod.ini')
    fragment=configparser.ConfigParser(interpolation=None)
    fragment.read(ROOT/'mods/unit_catalog/preview.symbols.fragment.ini',encoding='utf-8-sig')
    symbols=client/'sdgo/symbols'/(HOST+'.ini')
    from manager.storage import ini_values
    combined=ini_values(symbols.read_bytes())
    for section in fragment.sections():
        if section=='host':continue
        if combined.has_section(section) and dict(combined[section])!=dict(fragment[section]):
            raise ValueError('原生预览符号与当前框架冲突：'+section)
        combined[section]=dict(fragment[section])
    with symbols.open('w',encoding='ascii') as stream:combined.write(stream,space_around_delimiters=False)
    unit_manifest=client/'sdgo/mods/unit_catalog/mod.ini'
    unit_config=ini_values(unit_manifest.read_bytes())
    required=unit_config['mod']['requires'].split(',')
    required.extend(section for section in fragment.sections() if section!='host' and section not in required)
    unit_config['mod']['requires']=','.join(required)
    with unit_manifest.open('w',encoding='utf-8') as stream:unit_config.write(stream,space_around_delimiters=False)
    nonce=secrets.token_hex(32);revision=content_revision(draft)
    runtime=client/'sdgo/mods/unit_catalog'
    (runtime/'native-preview.ini').write_text('[preview]\nunit_id='+str(draft['root_unit_id'])+'\ncategory_id='+str(draft['category_id'])+
        '\ncontent_revision='+revision+'\nnonce='+nonce+'\n',encoding='ascii')
    with zipfile.ZipFile(publication['client_zip']) as archive:
        manifest=json.loads(archive.read('manifest.json'))
    files={name[7:]:digest for name,digest in manifest['files'].items() if name.startswith('client/')}
    files['sdgo/mods/unit_catalog/mod.ini']=digest_file(unit_manifest)
    compiled=json.loads((capsule/'manifest.json').read_bytes())
    files.update({'sdgo/mods/capsule_catalog/'+name:digest for name,digest in compiled['files'].items()})
    for path in (runtime/'native-preview.ini',capsule/'runtime.ini',capsule/'mod.ini',capsule/'capsule_catalog.dll',symbols,
                 client/'GOnline.exe',client/'GPackData.zdx'):
        files[path.relative_to(client).as_posix()]=digest_file(path)
    base_archive=client/'GPackData.zpk'
    index=_index(engine);index[identity]={'client':str(client),'content_revision':revision,'nonce':nonce,'machine_id':machine_id,
        'renderer_contract':RENDERER_CONTRACT,
        'machine_kind':selection['kind'],
        'module_sha256':digest_file(runtime/'unit_catalog.dll'),'catalog_sha256':digest_file(runtime/'catalog.bin'),
        'files':files,'base_archive':[base_archive.stat().st_size,base_archive.stat().st_mtime_ns],
        'game_server_changed':False,'source_client_changed':False}
    save_json(engine.root/'native-previews/index.json',index)
    return status(engine,identity)

def launch(engine, identity):
    entry=status(engine,identity)
    if not entry['prepared'] or entry.get('error'):raise ValueError('请先为当前内容版本重新准备有效的原生预览')
    client=_inside(engine,entry['client'])
    if digest_file(client/'GOnline.exe')!=HOST_SHA256:raise ValueError('预览客户端版本已改变')
    from manager.packages import _running
    if _running(client/'GOnline.exe'):raise ValueError('此预览副本已运行，请切换到它的游戏窗口')
    from manager.storage import ini_values
    settings=client/'配置文件.ini'
    if not settings.is_file():raise ValueError('预览副本缺少原启动器的配置文件.ini，无法确定登录地址和端口')
    cfg=ini_values(settings.read_bytes())
    section=next((cfg[s] for s in cfg.sections() if '地址' in cfg[s] and '端口' in cfg[s]),None)
    if section is None:raise ValueError('原启动器配置中没有登录地址和端口')
    import ipaddress
    host=str(ipaddress.IPv4Address(section['地址'].strip()));port=int(section['端口'])
    if not 1<=port<=65535:raise ValueError('原启动器登录端口无效')
    env=dict(os.environ);env['__COMPAT_LAYER']='RunAsInvoker'
    process=subprocess.Popen([str(client/'GOnline.exe'),host,str(port)],cwd=client,env=env,
                             creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    return {**entry,'launched_pid':process.pid}
