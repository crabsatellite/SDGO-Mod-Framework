"""The same Manager executable finishes its own staged update after exit."""
from pathlib import Path
import ctypes
import json
import os
import subprocess
import sys
import time
from . import storage
from .updates import MANAGER, child, digest


def pending(workspace, pid=None):
    """Recognize only an intact handoff staged for this exact Manager process."""
    root=Path(workspace).resolve()
    try:
        value=json.loads((root/'updates/restart.json').read_text(encoding='utf-8'))
        if not isinstance(value,dict):return False
        source=Path(value['source']).resolve();target=Path(value['target']).resolve()
        return (value.get('schema_version')==1 and value.get('parent_pid')==(os.getpid() if pid is None else pid)
                and Path(value['workspace']).resolve()==root and source.name==MANAGER and target.name==MANAGER
                and source.is_relative_to(root/'updates/releases')
                and digest(source.read_bytes())==value['source_sha256']
                and digest(target.read_bytes())==value['previous_sha256'])
    except (OSError,ValueError,KeyError,TypeError):return False


def schedule(controller):
    if not controller.manager_exe or not controller.state()['manager_restart']:
        raise ValueError('没有等待重启的管理器更新')
    source=controller.bundle/MANAGER;target=controller.manager_exe
    job=controller.cache/'restart.json'
    value={'schema_version':1,'source':str(source),'target':str(target),'parent_pid':os.getpid(),
           'source_sha256':controller.manifest['files'][MANAGER]['sha256'],
           'previous_sha256':digest(target.read_bytes()),'workspace':str(controller.workspace.root),
           'backup':str(controller.cache/'backups'/'manager'/controller.current/MANAGER)}
    if digest(source.read_bytes())!=value['source_sha256']:raise ValueError('管理器更新文件校验失败')
    storage.atomic(job,storage.json_bytes(value))
    subprocess.Popen([str(source),'--finish-update',str(job)],cwd=source.parent,
        creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0),close_fds=True)


def finish(job, launch=True):
    job=Path(job).resolve();value=json.loads(job.read_text(encoding='utf-8'))
    workspace=Path(value['workspace']).resolve();source=Path(value['source']).resolve();target=Path(value['target']).resolve()
    if value.get('schema_version')!=1 or job!=workspace/'updates/restart.json' or target.name!=MANAGER:
        raise ValueError('管理器更新交接无效')
    if not source.is_relative_to(workspace/'updates/releases') or source.name!=MANAGER:
        raise ValueError('管理器更新不属于本地已校验缓存')
    child(target.parent,target.name);child(workspace,source.relative_to(workspace).as_posix())
    raw=source.read_bytes()
    if digest(raw)!=value['source_sha256']:raise ValueError('管理器更新校验失败')
    # Keep the original process handle, not a recycled PID. It is never killed.
    if os.name=='nt':
        from ctypes import wintypes as w
        kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        kernel.OpenProcess.argtypes=(w.DWORD,w.BOOL,w.DWORD);kernel.OpenProcess.restype=w.HANDLE
        kernel.WaitForSingleObject.argtypes=(w.HANDLE,w.DWORD);kernel.CloseHandle.argtypes=(w.HANDLE,)
        process=kernel.OpenProcess(0x100000,False,int(value['parent_pid']))
        if process:
            try:
                if kernel.WaitForSingleObject(process,120000)!=0:raise ValueError('请先正常退出旧管理器')
            finally:kernel.CloseHandle(process)
    if digest(target.read_bytes())!=value['previous_sha256']:raise ValueError('管理器已被其他更新修改，请重新检查')
    backup=child(workspace,Path(value['backup']).resolve().relative_to(workspace).as_posix())
    storage.atomic(backup,target.read_bytes())
    for attempt in range(100):
        try:storage.atomic(target,raw);break
        except PermissionError:
            if attempt==99:raise
            time.sleep(.1)
    assert digest(target.read_bytes())==value['source_sha256']
    job.unlink()
    if launch:
        subprocess.Popen([str(target),'--workspace',str(workspace)],cwd=target.parent,
            creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0),close_fds=True)
    return target
