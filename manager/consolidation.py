"""Declarative, transactional consolidation of public Mod identities.

Only Manager installation transactions consume this contract. Saved game data
is outside the migration: it copies settings and archives superseded programs.
"""
from pathlib import Path
import json
import re
from . import storage


def validate(contract):
    rows=contract.get('consolidates',[])
    if not isinstance(rows,list)or len(rows)>16:raise ValueError('Mod 合并声明无效')
    fields={f['id']:f for f in contract.get('settings',[])}
    seen=set();targets=set()
    conflicts={r['id']for r in contract.get('dependencies',{}).get('conflicts',[])}
    for row in rows:
        if not isinstance(row,dict)or set(row)!={'id','setting'}:raise ValueError('Mod 合并字段无效')
        identity=row['id'];field=fields.get(row['setting'],{})
        if not isinstance(identity,str)or not re.fullmatch(r'[a-z][a-z0-9_]{0,63}',identity)or identity==contract['id']or identity in seen or identity not in conflicts:raise ValueError('Mod 合并身份必须唯一且声明冲突')
        if row['setting']in targets or row['setting']==contract.get('runtime_enabled_setting')or field.get('type')!='boolean'or field.get('file')!='mod.ini'or field.get('section','settings')!='settings':raise ValueError('Mod 合并目标必须是独立开关')
        seen.add(identity);targets.add(row['setting'])
    if rows and(contract.get('distribution')!='public'or contract.get('runtime_enabled_setting')not in fields):raise ValueError('Mod 合并需要公共模块与运行开关')


def _on(cfg,section,key,default=True):
    value=cfg.get(section,key,fallback=None)
    return default if value is None else str(value).strip().lower()in('1','true')


def prepare(root,pending,*,preserve_existing=True):
    """Patch incoming configuration; return files to retire in the same transaction."""
    from .updates import child
    root=Path(root).resolve();retired={}
    global_ini=child(root,'sdgo/sdgo.ini')
    global_cfg=storage.ini_values(global_ini.read_bytes()if global_ini.is_file()else b'')
    for path,data in list(pending.items()):
        if path.name!='manager.json':continue
        contract=json.loads(data.decode('utf-8-sig'));validate(contract)
        if not contract.get('consolidates'):continue
        identity=contract['id'];base=child(root,'sdgo/mods/'+identity)
        if path!=base/'manager.json':raise ValueError('Mod 合并仅适用于公共程序目录')
        manifest=base/'mod.ini'
        if manifest not in pending:raise ValueError('合并包缺少新 Mod 清单')
        cfg=storage.ini_values(pending[manifest])
        if cfg.get('mod','id',fallback='')!=identity or cfg.get('mod','distribution',fallback='')!='public':raise ValueError('合并清单身份不一致')
        fields={f['id']:f for f in contract['settings']};values=[];found=False
        for row in contract['consolidates']:
            old=child(root,'sdgo/mods/'+row['id']);ini=child(root,'sdgo/mods/'+row['id']+'/mod.ini')
            active=False
            if old.exists() and (ini.exists() or any(p.is_file() for p in old.rglob('*'))):
                if not ini.is_file():raise ValueError('旧 Mod 缺少身份清单：'+row['id'])
                previous=storage.ini_values(ini.read_bytes())
                if previous.get('mod','id',fallback='')!=row['id']or previous.get('mod','distribution',fallback='')!='public':raise ValueError('旧 Mod 身份或分发范围不匹配')
                found=True
                active=all((_on(previous,'mod','enabled'),_on(previous,'settings','Enabled',False),
                    _on(global_cfg,'mods',row['id']),_on(global_cfg,'features',previous.get('mod','feature',fallback=row['id'])),
                    _on(global_cfg,'categories',previous.get('mod','category',fallback='gameplay'))))
                for entry in old.rglob('*'):
                    checked=child(root,entry.relative_to(root).as_posix())
                    if checked.is_file():
                        if checked in pending:raise ValueError('合并包同时包含旧 Mod')
                        retired[checked]=checked.read_bytes()
            values.append((fields[row['setting']]['key'],active))
        patches=[]
        if manifest.is_file() and preserve_existing:
            installed=storage.ini_values(manifest.read_bytes())
            for section in installed.sections():
                for key,value in installed[section].items():
                    if section.lower()=='settings'or(section.lower()=='mod'and key.lower()=='enabled'):patches.append((section,key,value))
        elif found and not manifest.is_file():
            patches=[('settings',key,'1'if active else'0')for key,active in values]
            patches.append(('settings',fields[contract['runtime_enabled_setting']]['key'],'1'if any(active for _,active in values)else'0'))
        pending[manifest]=storage.patch_ini(pending[manifest],patches)
    return retired
