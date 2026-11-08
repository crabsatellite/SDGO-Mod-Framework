"""Finite startup intents using the normal client update transaction.

No game executable is patched or supervised. Requests and permits are local,
bounded, root-bound and single-use; a permit is issued only after remote recheck.
"""
from __future__ import annotations
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess
import time
import sys
from . import storage, updates
from .target_locks import lease

REQUEST = struct.Struct('<8sIIQII')
PERMIT = struct.Struct('<8sQ32s32sI')
PROTOCOL = b'SDGO_LAUNCH_GATE_V1'
TOKEN = re.compile('[0-9a-f]{32}')


def folder(root):return Path(root).resolve()/'sdgo/local-state/launch'


def item(root,token,suffix):
    if not isinstance(token,str)or not TOKEN.fullmatch(token):raise ValueError('启动请求标识无效')
    return updates.child(folder(root),token+'.'+suffix)


def creation(pid):
    """Exact Windows creation identity; PID reuse must not identify an origin."""
    if os.name!='nt':return None
    import ctypes as c
    from ctypes import wintypes as w
    k=c.WinDLL('kernel32',use_last_error=True)
    k.OpenProcess.argtypes=(w.DWORD,w.BOOL,w.DWORD);k.OpenProcess.restype=w.HANDLE
    k.GetProcessTimes.argtypes=(w.HANDLE,c.POINTER(w.FILETIME),c.POINTER(w.FILETIME),c.POINTER(w.FILETIME),c.POINTER(w.FILETIME))
    k.QueryFullProcessImageNameW.argtypes=(w.HANDLE,w.DWORD,w.LPWSTR,c.POINTER(w.DWORD))
    k.CloseHandle.argtypes=(w.HANDLE,)
    k.WaitForSingleObject.argtypes=(w.HANDLE,w.DWORD)
    process=k.OpenProcess(0x101000,False,pid)
    if not process:return None
    try:
        if k.WaitForSingleObject(process,0)==0:return None
        times=[w.FILETIME()for _ in range(4)];name=c.create_unicode_buffer(32768);length=w.DWORD(len(name))
        if not k.GetProcessTimes(process,*[c.byref(t)for t in times])or not k.QueryFullProcessImageNameW(process,0,name,c.byref(length)):
            if k.WaitForSingleObject(process,0)==0:return None
            raise ValueError('无法核对启动进程身份')
        return ((times[0].dwHighDateTime<<32)|times[0].dwLowDateTime,Path(name.value).resolve())
    finally:k.CloseHandle(process)


def read_request(root,token):
    root=Path(root).resolve();path=item(root,token,'request')
    if path.stat().st_size>140000:raise ValueError('启动请求过长')
    raw=path.read_bytes()
    if len(raw)<REQUEST.size:raise ValueError('启动请求不完整')
    magic,pid,reserved,created,n,m=REQUEST.unpack_from(raw)
    if magic!=b'SDGOLQ01'or reserved or not 0<n<32768 or not 0<m<32768 or len(raw)!=REQUEST.size+2*(n+m):raise ValueError('启动请求格式无效')
    requested=raw[REQUEST.size:REQUEST.size+2*n].decode('utf-16-le');command=raw[REQUEST.size+2*n:].decode('utf-16-le')
    if '\0'in requested or '\0'in command or Path(requested).resolve()!=root:raise ValueError('启动请求不属于这个客户端')
    if os.name=='nt':
        import ctypes as c
        from ctypes import wintypes as w
        shell=c.WinDLL('shell32');shell.CommandLineToArgvW.argtypes=(w.LPCWSTR,c.POINTER(c.c_int));shell.CommandLineToArgvW.restype=c.POINTER(w.LPWSTR)
        k=c.WinDLL('kernel32');k.LocalFree.argtypes=(w.HLOCAL,)
        count=c.c_int();args=shell.CommandLineToArgvW(command,c.byref(count))
        try:
            if not args or count.value<1:raise ValueError('启动命令不属于原版游戏入口')
            entry=Path(args[0]);entry=entry if entry.is_absolute()else root/entry
            if entry.resolve()!=root/'GOnline.exe':raise ValueError('启动命令不属于原版游戏入口')
        finally:
            if args:k.LocalFree(args)
    return {'token':token,'root':str(root),'pid':pid,'created':created,'command':command,'sha256':hashlib.sha256(raw).hexdigest()}


def accept(root,token):
    request=read_request(root,token);accepted=item(root,token,'accepted')
    if accepted.exists():
        if storage.read_bytes(accepted).decode('ascii')!=request['sha256']:raise ValueError('已接受的启动请求被修改')
    else:
        if creation(request['pid'])!=(request['created'],Path(root).resolve()/'GOnline.exe'):raise ValueError('原始启动进程已结束或身份不符，请重新打开游戏')
        storage.atomic(accepted,request['sha256'].encode('ascii'))
    return request


def state(root,token):return storage.read_json(item(root,token,'result.json'),{})


def save(root,token,**values):
    storage.atomic(item(root,token,'result.json'),storage.json_bytes(values));return values


def grant(root,token):
    from .client_runtime import RECEIPT,SOURCE
    root=Path(root).resolve();encoded=str(root).encode('utf-16-le')
    raw=PERMIT.pack(b'SDGOLP01',int((time.time()+90+11644473600)*10000000),
        hashlib.sha256((root/RECEIPT).read_bytes()).digest(),hashlib.sha256((root/SOURCE).read_bytes()).digest(),len(encoded)//2)+encoded
    path=item(root,token,'permit')
    # Never replace a permit that another launch may already be consuming.
    with path.open('xb')as stream:stream.write(raw)
    return path


def start_original(command,root,env):
    # The frozen Manager's private DLL search directory belongs to Python,
    # not the original game. Restore it for Manager after CreateProcess.
    kernel=None
    if os.name=='nt'and getattr(sys,'frozen',False):
        import ctypes as c
        from ctypes import wintypes as w
        kernel=c.WinDLL('kernel32',use_last_error=True)
        kernel.SetDllDirectoryW.argtypes=(w.LPCWSTR,)
        if not kernel.SetDllDirectoryW(None):raise c.WinError(c.get_last_error())
    try:return subprocess.Popen(command,executable=str(Path(root)/'GOnline.exe'),cwd=root,env=env,close_fds=True)
    finally:
        if kernel:kernel.SetDllDirectoryW(str(sys._MEIPASS))


def run(controller,host,token):
    from .client_runtime import installed_source,matches,RECEIPT
    from .owner_channel import mode
    root=Path(host['path']).resolve();request=read_request(root,token)
    def progress(phase,**values):controller.report_progress(phase=phase,**values)
    def wait(phase,message):
        progress(phase,message=message)
        if controller.operation_stop.is_set():raise InterruptedError('启动已取消')
        controller.cancel_event.wait(.25)
    while creation(request['pid'])==(request['created'],root/'GOnline.exe'):
        wait('handoff','等待原始启动进程退出，以安全更新文件')
    if mode(host)!='player':raise ValueError('客户端维护方式未通过核对，请在 Manager 检查服主绑定或 DIY 设置')
    initial=installed_source(root)
    if not initial:raise ValueError('客户端尚未导入服主配置')
    while True:
        if controller.operation_stop.is_set():raise InterruptedError('启动已取消')
        if installed_source(root)!=initial:raise storage.Conflict('启动期间更新源已改变，请重新打开游戏')
        if controller.unresolved(root):
            recovery=controller.recover(host)
            if recovery['status']=='waiting':wait('waiting',recovery['message']);continue
            if recovery['status']=='restart_required':
                value=storage.read_json(Path(recovery['handoff']));value['launch_request']=token
                storage.atomic(Path(recovery['handoff']),storage.json_bytes(value));return recovery
        result=controller.plan(host);plan=controller.plans[host['id']]
        receipt=storage.read_json(root/RECEIPT,{})
        if result['status']=='available'or receipt.get('commit')!=plan['commit']or receipt.get('effective_files')!=plan['effective']:
            result=controller.install(host,plan['digest'])
            if result['status']=='waiting':wait('waiting',result['message']);continue
            if result['status']=='restart_required':
                value=storage.read_json(Path(result['handoff']));value['launch_request']=token
                storage.atomic(Path(result['handoff']),storage.json_bytes(value));return result
            if result['status']!='installed':raise ValueError('客户端更新未完成，启动已停止')
        progress('recheck',message='重新核对远端配置修订')
        if initial['schema_version']==3:
            from .server_profiles import resolve
            profile,_,_=resolve(controller,initial)
            if updates.digest(storage.json_bytes(profile))!=plan['commit']:continue
        else:
            result=controller.plan(host)
            if result['status']!='current'or controller.plans[host['id']]['commit']!=plan['commit']:continue
        with lease('host:'+str(root).casefold()):
            if installed_source(root)!=initial or mode(host)!='player':raise storage.Conflict('启动前维护方式或更新源已变化')
            progress('verify',total_files=len(plan['effective']),verified_files=0)
            for number,(name,entry)in enumerate(plan['effective'].items(),1):
                if controller.operation_stop.is_set():raise InterruptedError('启动已取消')
                if not matches(updates.child(root,name),entry):raise storage.Conflict('启动前文件已变化，请重试：'+name)
                controller.report_progress(verified_files=number,file=name)
            if plan.get('owner_programme'):
                from .owner_programmes import unknown_files
                if unknown_files(root,plan['effective']):raise storage.Conflict('启动前出现未登记的 Mod，请重新校验并隔离后启动')
            # A loader without the protocol would silently skip the next check.
            # Fail here instead of claiming an automatic workflow was installed.
            if b'SdgoLaunchGateProtocol'not in(root/'d3d9.dll').read_bytes():raise ValueError('服主选定的框架尚不支持启动前校验，请服主发布配套框架')
            if controller.operation_stop.is_set():raise InterruptedError('启动已取消')
            progress('launch',message='文件校验通过，正在启动游戏')
            permit=grant(root,token);env=dict(os.environ,SDGO_LAUNCH_PERMIT=token)
            try:process=start_original(request['command'],root,env)
            except BaseException:permit.unlink(missing_ok=True);raise
            deadline=time.monotonic()+30
            while not item(root,token,'ack').is_file():
                if process.poll()is not None or time.monotonic()>=deadline:
                    permit.unlink(missing_ok=True);raise ValueError('游戏未确认启动许可；请查看客户端提示后重试')
                time.sleep(.05)
            if item(root,token,'ack').read_bytes()!=PROTOCOL:raise ValueError('游戏启动确认无效')
            result=save(root,token,status='launched',pid=process.pid,origin_pid=request['pid'],origin_created=request['created'],
                        request_sha256=request['sha256'],commit=plan['commit'],message='校验通过，游戏已启动')
            for suffix in('request','accepted','ack'):
                try:storage.remove(item(root,token,suffix))
                except OSError:result.setdefault('cleanup_pending',[]).append(suffix)
            if result.get('cleanup_pending'):
                try:save(root,token,**result)
                except OSError:pass  # The persisted terminal result is already valid.
            return result
