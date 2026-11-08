"""Observed native activation, separate from saved configuration and SQL commits."""
from __future__ import annotations
import hashlib
import os
from pathlib import Path
from . import storage


def process_state(executable):
    from .windows import native_processes
    expected=Path(executable).resolve()
    rows=native_processes(expected.name)
    if any(not p.get('path')for p in rows):raise ValueError('存在无法核实路径的原生进程')
    return [p for p in rows if Path(p['path']).resolve()==expected]


def receipt(root,process):
    path=Path(root)/'sdgo/local-state/runtime'/(str(process['pid'])+'.json')
    try:
        if path.stat().st_size>2*1024*1024:return None
        value=storage.read_json(path)
        if(value.get('schema_version')!=1 or value.get('pid')!=process['pid'] or
           str(value.get('creation_ticks'))!=str(process['creation_ticks']) or
           Path(value['executable']).resolve()!=Path(process['path']).resolve()):return None
        return value
    except (OSError,ValueError,KeyError,TypeError):return None


def mod_state(host,identity,enabled):
    return host_mod_states(host,[{'id':identity,'enabled':enabled}])[identity]


def host_mod_states(host,mods):
    def all_states(status,message):return {m['id']:{'status':status,'message':message}for m in mods}
    try:
        processes=process_state(Path(host['path'])/{'client':'GOnline.exe','battle':'BattleServer.exe','agent':'AgentServer.exe','world':'WorldServer.exe'}[host['role']])
        if not processes:return all_states('stopped','目标程序未运行；下次启动读取已保存配置')
        values=[receipt(host['path'],p)for p in processes]
        if any(v is None for v in values):return all_states('unknown','运行中的旧版程序没有生效回执；配置已保留')
        result={}
        for mod in mods:
            identity=mod['id'];enabled=mod.get('runtime_enabled',mod['enabled']);raw=Path(host['path'])/'sdgo/mods'/identity/'mod.ini'
            if not raw.exists():raw=Path(host['path'])/'sdgo/owner-mods'/identity/'mod.ini'
            if not raw.exists():raw=Path(host['path'])/'sdgo/local-mods'/identity/'mod.ini'
            from .configuration import files,fingerprint
            contract=storage.read_json(raw.parent/'manager.json',{})
            names=files(storage.ini_values(raw.read_bytes()),contract)
            digest=fingerprint(raw.parent,names)
            rows=[v.get('mods',{}).get(identity,{})for v in values]
            matching=all(bool(r.get('active'))==enabled and(not enabled or(r.get('configuration_sha256')==digest and sorted(r.get('configuration_files',[]))==names))for r in rows)
            result[identity]={'status':('active'if enabled else'disabled')if matching else'pending_restart',
                              'message':'运行状态已确认'if matching else'已保存；运行状态尚未与配置一致'}
            if enabled and any(r.get('requested')and not r.get('active')for r in rows):
                result[identity]={'status':'failed','message':'运行中的程序未能加载此 Mod，请查看原生日志'}
        return result
    except (OSError,ValueError,KeyError):return all_states('unknown','尚不能核实运行状态；保留当前程序')


def native_activation(executable,change):
    result={'node':executable.parent.name,'ready':False}
    try:
        processes=process_state(executable)
        result.update(running=bool(processes),ports=sorted({port for p in processes for port in p.get('ports',[])}))
        if not processes or any(p['started_ns']<=change['committed_ns']for p in processes):
            return {**result,'reason':'尚未完成变更后的重启'}
        if not all(p['listening']for p in processes):return {**result,'reason':'原生监听尚未就绪'}
        if executable.name!='BattleServer.exe'and not all(p.get('tcp_listening',p['listening'])for p in processes):
            return {**result,'reason':'原生业务监听尚未就绪'}
        if executable.name=='BattleServer.exe':
            found=False
            for log in(executable.parent/'LogData').glob('*.txt'):
                with log.open('rb')as stream:
                    offset=change.get('log_offsets',{}).get(str(log),0)
                    stream.seek(offset if log.stat().st_size>=offset else 0)
                    found=found or b'Table Download OK.. Time Procedure Ready..'in stream.read(32*1024*1024)
            if not found:return {**result,'reason':'尚未观察到本次原生表加载完成'}
        # StoreServer has no proxy ABI. Its fresh process and native listener
        # participate in the barrier without inventing a Mod load receipt.
        if executable.name!='StoreServer.exe'and any(receipt(executable.parent,p)is None for p in processes):
            return {**result,'reason':'等待新进程的框架加载回执'}
        return {**result,'ready':True,'reason':'新进程与原生就绪检查通过'}
    except (OSError,ValueError):return {**result,'reason':'无法核实原生服务，保持待生效'}
