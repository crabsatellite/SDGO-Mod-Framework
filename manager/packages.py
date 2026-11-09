"""Import allowlisted client overlays without executing package scripts."""
from __future__ import annotations
import base64
import io
import json
from pathlib import Path
import re
import os
import uuid
import zipfile
from . import distribution, storage

ALLOWED = {'.ini','.json','.bin','.dll','.spr','.txr','.png','.lua','.tsv','.html'}


def _running(executable):
    if __import__('os').name != 'nt': return False
    # Read-only process inventory. Never terminate a game to import a package.
    from .windows import process_image_paths
    values=process_image_paths('GOnline.exe')
    return any(Path(value).resolve()==executable.resolve() for value in values if value)


def import_client(workspace, host_id, encoded):
    host=workspace.host(host_id)
    if host['role']!='client': raise ValueError('玩家配置包只能安装到客户端')
    root=Path(host['path']).resolve()
    if _running(root/'GOnline.exe'): raise ValueError('请先关闭这个客户端，再导入配置包')
    if not isinstance(encoded,str) or len(encoded)>352*1024*1024:raise ValueError('客户端配置包过大')
    raw=base64.b64decode(encoded,validate=True)
    with zipfile.ZipFile(io.BytesIO(raw)) as archive:
        if 'manifest.json' in archive.namelist():
            if archive.getinfo('manifest.json').file_size > 2*1024*1024:raise ValueError('发布清单过大')
            manifest=json.loads(archive.read('manifest.json'))
            if manifest.get('module')=='unit_catalog':
                if len(encoded)>224*1024*1024:raise ValueError('客户端配置包过大')
                from owner.units.deployment import install_package
                return {**install_package(root,raw),'state':workspace.public()}
            if manifest.get('module')=='map_catalog':
                from owner.maps.deployment import install_package as install_maps
                return {**install_maps(root,raw),'state':workspace.public()}
            if manifest.get('module')=='operator_catalog':
                from .engines.operators.install_package import install_client
                return install_client(workspace,host,raw)
    if len(encoded)>96*1024*1024:raise ValueError('客户端配置包过大')
    pending={};package_files={};total=0
    with zipfile.ZipFile(io.BytesIO(raw)) as archive:
        if len(archive.infolist())>4096:raise ValueError('配置包条目过多')
        for entry in archive.infolist():
            if entry.is_dir():continue
            name=entry.filename.replace('\\','/')
            package_name=name
            if package_name.startswith('client/'):package_name=package_name[7:]
            if not package_name.startswith('sdgo/'):
                # A client package may carry readable notes and its fingerprint manifest.
                if '/' not in package_name and (package_name.lower().startswith('readme') or package_name in ('manifest.json','registrations.json','symbols.fragment.ini','Install-Operator.ps1','install_ai_catalog_client.ps1')):continue
                raise ValueError('配置包包含客户端以外的文件：'+package_name)
            target=storage.safe_child(root,package_name)
            parts=package_name.split('/')
            if len(parts)<4 or parts[1] not in ('mods','owner-mods','local-mods') or not re.fullmatch(r'[a-z][a-z0-9_]{0,63}',parts[2]):
                raise ValueError('此配置包路径未登记：'+package_name)
            if target.suffix.lower() not in ALLOWED or entry.file_size>32*1024*1024:raise ValueError('此配置包文件类型或大小不受支持：'+package_name)
            total+=entry.file_size
            if total>96*1024*1024:raise ValueError('配置包解压总量过大')
            if target in pending:raise ValueError('配置包文件重复')
            data=archive.read(entry)
            package_files[package_name]=data
            if target.name=='mod.ini':
                ini=storage.ini_values(data)
                if ini.get('mod','id',fallback='')!=parts[2] or ini.get('mod','dll',fallback=parts[2]+'.dll')!=parts[2]+'.dll':raise ValueError('Mod 身份或 DLL 路径无效')
                if any(not h.startswith('GOnline-') for h in ini.get('mod','hosts',fallback='').split(',')):raise ValueError('包内 Mod 不属于客户端')
            if target.suffix.lower()=='.dll' and target.name!=parts[2]+'.dll':raise ValueError('Mod DLL 名称无效')
            pending[target]=data
        mod_roots={name.split('/',3)[0]+'/'+name.split('/',3)[1]+'/'+name.split('/',3)[2]
                   for name in package_files}
        addon_roots=[]
        for base in mod_roots:
            mod_raw=package_files.get(base+'/mod.ini')
            if mod_raw is None:raise ValueError('配置包中的 Mod 缺少 mod.ini：'+base)
            scope=distribution.resolve(
                mod_raw,
                package_files.get(base+'/manager.json'),
                allow_legacy_public=True,
            )
            marked=any(distribution.NONREDISTRIBUTABLE_MARKER in data
                       for name,data in package_files.items() if name.startswith(base+'/'))
            if scope==distribution.LOCAL_ONLY and not marked:
                raise ValueError('仅本地 Mod 缺少禁止分发标记：'+base)
            if scope==distribution.PUBLIC and marked:
                raise ValueError('公共 Mod 包含禁止分发标记：'+base)
            if scope==distribution.PUBLIC:
                for name,data in package_files.items():
                    if name.startswith(base+'/'):distribution.require_redistributable(name,data)
            if scope==distribution.OWNER:
                for name,data in package_files.items():
                    if name.startswith(base+'/'):distribution.require_owner_payload(name,data)
            if base.startswith('sdgo/owner-mods/')and scope!=distribution.OWNER:
                raise ValueError('本服目录中的配置包必须声明 owner：'+base)
            if base.startswith('sdgo/local-mods/') and scope!=distribution.LOCAL_ONLY:
                raise ValueError('本地目录中的配置包必须声明 local_only：'+base)
            if scope in(distribution.LOCAL_ONLY,distribution.OWNER):
                destination='sdgo/'+('local-mods/'if scope==distribution.LOCAL_ONLY else'owner-mods/')+base.split('/')[2]
                addon_roots.append(destination)
                for path in list(pending):
                    relative=path.relative_to(root).as_posix()
                    if relative.startswith(base+'/'):
                        replacement=storage.safe_child(root,destination+relative[len(base):])
                        if replacement!=path and replacement in pending:raise ValueError('本地安装目标重复')
                        data=pending.pop(path)
                        pending[replacement]=data
        if not pending:raise ValueError('配置包没有客户端 Mod 文件')
        # Merge known symbol fragments as data, never run supplied setup scripts.
        if 'symbols.fragment.ini' in archive.namelist():
            if addon_roots:
                if len(addon_roots)!=1 or len(mod_roots)!=1:
                    raise ValueError('仅本地配置包的符号片段必须属于一个明确的 Mod')
                path=storage.safe_child(root,addon_roots[0]+'/symbols.fragment.ini')
                raw_fragment=archive.read('symbols.fragment.ini')
                if path in pending and pending[path]!=raw_fragment:raise ValueError('本地符号片段重复且不一致')
                pending[path]=raw_fragment
            else:
                fragment=storage.ini_values(archive.read('symbols.fragment.ini'))
                stamp=int(fragment.get('host','timestamp'),0);size=int(fragment.get('host','size_of_image'),0)
                name=f'GOnline-{stamp:08X}-{size:08X}.ini';path=root/'sdgo/symbols'/name
                original=path.read_bytes() if path.exists() else b'';merged=storage.ini_values(original)
                for section in fragment.sections():
                    if merged.has_section(section):
                        if dict(merged[section])!=dict(fragment[section]):raise ValueError('符号定义存在冲突：'+section)
                    else:merged[section]=dict(fragment[section])
                text=io.StringIO();merged.write(text,space_around_delimiters=False);pending[path]=text.getvalue().encode('ascii')
    from .target_locks import lease
    with lease('host:'+str(root).casefold()),storage.LOCK:
        previous={p:(p.read_bytes() if p.exists() else None) for p in pending};written=[]
        migrated=[];recovery=root/'sdgo/local-state/scope-migrations'/uuid.uuid4().hex
        try:
            for path,data in pending.items():storage.atomic(path,data);written.append(path)
            # Importing a new declared scope replaces that identity, while
            # preserving its previous directory outside every loading root.
            for destination in addon_roots:
                identity=destination.split('/')[-1]
                for tree in('mods','owner-mods','local-mods'):
                    old=storage.safe_child(root,'sdgo/'+tree+'/'+identity)
                    if old==root/destination or not old.exists():continue
                    if tree=='mods':
                        cfg=storage.ini_values((old/'mod.ini').read_bytes())if(old/'mod.ini').is_file()else storage.ini_values(b'')
                        if cfg.get('mod','distribution',fallback='public')!='local_only':raise ValueError('附加模块不能替换同名框架 Mod；请使用独立标识')
                    if not old.is_dir()or old.is_symlink()or(hasattr(old,'is_junction')and old.is_junction()):raise ValueError('旧 Mod 目录不是可安全迁移的普通目录')
                    saved=storage.safe_child(root,recovery.relative_to(root).as_posix()+'/'+tree+'/'+identity)
                    if not old.resolve().is_relative_to(root)or not saved.resolve().is_relative_to(root):raise ValueError('Mod 迁移超出客户端目录')
                    saved.parent.mkdir(parents=True,exist_ok=True);os.replace(old,saved);migrated.append((old,saved))
        except (OSError,ValueError):
            for old,saved in reversed(migrated):os.replace(saved,old)
            for path in reversed(written):
                if previous[path] is None:path.unlink(missing_ok=True)
                else:storage.atomic(path,previous[path])
            raise
    return {'installed_files':len(pending),'activation':'restart_required','scope_recovery':str(recovery)if migrated else None,'state':workspace.public()}
