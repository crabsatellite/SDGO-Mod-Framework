"""Native startup progress attached to the existing Manager session/API."""
from __future__ import annotations
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from . import launch
from .instance import Lease

class IncompatibleSession(ValueError):pass


def command():
    return [sys.executable]if getattr(sys,'frozen',False)else[sys.executable,str(Path(__file__).resolve().parents[1]/'tools/mod_manager.py')]


def cache_manager(target,raw):
    """Serialize writers before a second click can map the shared helper EXE."""
    from . import storage
    target=Path(target)
    k=c.WinDLL('kernel32',use_last_error=True)
    k.CreateMutexW.argtypes=(c.c_void_p,w.BOOL,w.LPCWSTR);k.CreateMutexW.restype=w.HANDLE
    k.WaitForSingleObject.argtypes=(w.HANDLE,w.DWORD);k.ReleaseMutex.argtypes=(w.HANDLE,);k.CloseHandle.argtypes=(w.HANDLE,)
    mutex=k.CreateMutexW(None,False,'Local\\SDGO.HelperCopy.'+hashlib.sha256(str(target.resolve()).casefold().encode()).hexdigest())
    if not mutex:raise c.WinError(c.get_last_error())
    held=False
    try:
        if k.WaitForSingleObject(mutex,0xffffffff)not in(0,0x80):raise c.WinError(c.get_last_error())
        held=True
        target.parent.mkdir(parents=True,exist_ok=True)
        if not target.is_file()or hashlib.sha256(storage.read_bytes(target)).digest()!=hashlib.sha256(raw).digest():
            storage.atomic(target,raw)
    finally:
        if held:k.ReleaseMutex(mutex)
        k.CloseHandle(mutex)


def detach(root,token):
    """Run the same distributed Manager from local state to unlock its EXE."""
    from . import storage
    source=Path(sys.executable);raw=storage.read_bytes(source)
    target=launch.folder(root)/'manager'/hashlib.sha256(raw).hexdigest()/source.name
    cache_manager(target,raw)
    subprocess.Popen([str(target),'--launch-game',str(root),'--launch-request',token,'--launch-worker'],cwd=root,
                     env=dict(os.environ,PYINSTALLER_RESET_ENVIRONMENT='1'),
                     creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0),close_fds=True)


class Session:
    def __init__(self,root,token):
        self.root=Path(root).resolve();self.token=token;self.workspace=self.root/'sdgo/local-state/manager'
        self.result={'status':'queued','message':'正在连接 Manager'};self.lock=threading.Lock();self.stop=threading.Event()
        self.retry=threading.Event();self.cancel=threading.Event();self.url=None;self.secret=None;self.host=None

    def api(self,path,body=None):
        request=urllib.request.Request(self.url+path)
        if body is not None:
            request.data=json.dumps(body).encode();request.add_header('Content-Type','application/json')
            request.add_header('Origin',self.url.rstrip('/'));request.add_header('X-ModManager-Token',self.secret)
        try:
            with urllib.request.urlopen(request,timeout=5)as response:return json.load(response)
        except urllib.error.HTTPError as error:
            try:message=json.load(error).get('error',str(error))
            except(ValueError,OSError):message=str(error)
            raise ValueError(message)from error

    def connect(self):
        lease=Lease(self.workspace)
        if lease.file:
            lease.close()
            subprocess.Popen([*command(),'--workspace',str(self.workspace),'--no-browser','--startup-client',str(self.root)],cwd=self.root,
                             env=dict(os.environ,PYINSTALLER_RESET_ENVIRONMENT='1'),
                             creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0),close_fds=True)
        lease.close()
        deadline=time.monotonic()+90
        while not self.stop.is_set():
            try:
                self.url=lease.existing_url();identity=self.api('api/identity')
                if identity.get('launch_gate')!=1:raise IncompatibleSession('当前 Manager 会话版本过旧，请正常退出旧 Manager 后重试')
                self.secret=identity['session_token'];return
            except IncompatibleSession:raise
            except(OSError,ValueError):
                if time.monotonic()>=deadline:raise ValueError('无法连接配套 Manager，请打开 Manager 检查后重试')
                self.stop.wait(.25)

    def work(self):
        submitted=False;restarting=False
        while not self.stop.is_set():
            try:
                if not self.url:self.connect();submitted=False
                if self.cancel.is_set():
                    if self.host:self.api('api/client-updates',{'action':'cancel','host':self.host})
                    else:
                        launch.save(self.root,self.token,status='cancelled',message='启动已取消')
                        with self.lock:self.result={'status':'cancelled'}
                        return
                    self.cancel.clear()
                    # Wait for rollback/worker acknowledgement before closing.
                if not submitted or self.retry.is_set():
                    self.retry.clear();response=self.api('api/launch',{'root':str(self.root),'token':self.token})
                    self.token=response.get('token',self.token);self.host=response.get('host',self.host);submitted=True
                result=self.api('api/launch?'+urllib.parse.urlencode({'root':str(self.root),'token':self.token}))
                self.host=result.get('host',self.host)
                with self.lock:self.result=result
                if result.get('status')=='restart_required'and not restarting:
                    restarting=True
                    self.api('api/client-updates',{'action':'restart','host':self.host})
                    self.url=None;self.stop.wait(1);continue
                if result.get('status')in('launched','cancelled'):return
                self.stop.wait(.3)
            except(OSError,ValueError)as error:
                try:completed=launch.state(self.root,self.token)
                except(OSError,ValueError):completed={}
                if completed.get('status')in('launched','cancelled'):
                    with self.lock:self.result=completed
                    return
                if restarting:
                    self.url=None;restarting=False;continue
                with self.lock:self.result={'status':'error','message':str(error)}
                while not self.stop.is_set()and not self.retry.wait(.25):
                    if self.cancel.is_set():
                        with self.lock:self.result={'status':'cancelled'}
                        return
                self.url=None;submitted=False


def progress_text(result):
    p=result.get('progress',{});phase=p.get('phase');status=result.get('status')
    names={'manifest':'检查服主配置','reuse':'校验可复用内容','mirror':'核对同一修订的备用源','content':'下载服主内容',
        'content_archive':'下载服主内容包','framework':'下载并校验官方模组','owner_programme':'下载并校验本服补丁','staging':'暂存文件','install':'安装已校验文件',
        'handoff':'准备安全更新','waiting':'等待客户端关闭','recheck':'核对最新远端修订','verify':'启动前完整校验','launch':'正在启动游戏'}
    title='更新未完成'if status=='error'else'正在重启 Manager'if status=='restart_required'else names.get(phase,'准备游戏')
    text=result.get('message')or p.get('message')or'正在同步模组程序与服主配置，请稍候。'
    total=p.get('total_bytes');done=p.get('verified_bytes',p.get('current_bytes',0));fraction=None
    if total:
        fraction=min(1,done/total);text+=f'\n{done/1048576:.1f} / {total/1048576:.1f} MiB'
    elif p.get('total_files')and p.get('verified_files')is not None:
        total=p['total_files'];done=p['verified_files'];fraction=min(1,done/total);text+=f'\n{done} / {total} 个文件'
    elif p.get('wire_bytes'):text+=f"\n已接收 {p['wire_bytes']/1048576:.1f} MiB（完成后校验）"
    if p.get('file'):text+='\n'+p['file'].split('/')[-1]
    if status=='error':text+='\n已校验下载保留。可以重试，或取消本次启动。'
    return title,text,fraction


def dialog(session):
    callback_type=c.WINFUNCTYPE(c.c_long,w.HWND,w.UINT,w.WPARAM,w.LPARAM,c.c_ssize_t)
    class Config(c.Structure):
        _pack_=1
        _fields_=[('size',w.UINT),('parent',w.HWND),('instance',w.HINSTANCE),('flags',c.c_int),('common',c.c_int),
            ('title',w.LPCWSTR),('icon',w.HICON),('instruction',w.LPCWSTR),('content',w.LPCWSTR),('buttons',w.UINT),('button_rows',c.c_void_p),
            ('default',c.c_int),('radios',w.UINT),('radio_rows',c.c_void_p),('default_radio',c.c_int),('verification',w.LPCWSTR),
            ('expanded',w.LPCWSTR),('expand_text',w.LPCWSTR),('collapse_text',w.LPCWSTR),('footer_icon',w.HICON),('footer',w.LPCWSTR),
            ('callback',callback_type),('data',c.c_ssize_t),('width',w.UINT)]
    user=c.WinDLL('user32',use_last_error=True);user.SendMessageW.argtypes=(w.HWND,w.UINT,w.WPARAM,w.LPARAM);user.SendMessageW.restype=w.LPARAM
    previous=None;closing=False
    @callback_type
    def callback(hwnd,msg,wp,lp,data):
        nonlocal previous,closing
        try:
            if msg in(0,4):
                with session.lock:result=dict(session.result)
                status=result.get('status')
                if status in('launched','cancelled'):
                    closing=True;user.SendMessageW(hwnd,1024+102,2,0);return 0
                title,text,fraction=progress_text(result);snapshot=(title,text,fraction,status)
                if previous!=snapshot:
                    for element,value in((3,title),(0,text)):
                        buffer=c.create_unicode_buffer(value);user.SendMessageW(hwnd,1024+108,element,c.addressof(buffer))
                    user.SendMessageW(hwnd,1024+111,4,int(status=='error'))
                    user.SendMessageW(hwnd,1024+103,int(fraction is None),0)
                    if fraction is None:user.SendMessageW(hwnd,1024+107,1,40)
                    else:user.SendMessageW(hwnd,1024+106,int(fraction*100),0)
                    previous=snapshot
            elif msg==2:
                if closing:return 0
                if wp==4:session.retry.set()
                else:session.cancel.set()
                return 1  # Keep the dialog until cancellation/rollback finishes.
        except Exception as error:
            with session.lock:session.result={'status':'error','message':str(error)}
        return 0
    cfg=Config();cfg.size=c.sizeof(cfg);cfg.flags=0x200|0x800|0x8000|0x10000;cfg.common=0x8|0x10
    cfg.title='SDGO Mod Manager';cfg.instruction='启动前更新';cfg.content='正在连接 Manager…';cfg.callback=callback;cfg.width=340
    controls=c.WinDLL('comctl32');controls.TaskDialogIndirect.argtypes=(c.POINTER(Config),c.c_void_p,c.c_void_p,c.c_void_p);controls.TaskDialogIndirect.restype=c.c_long
    result=controls.TaskDialogIndirect(c.byref(cfg),None,None,None)
    if result<0:raise OSError('无法显示启动进度窗口：'+hex(result&0xffffffff))


def run(root,token):
    if os.name!='nt':raise ValueError('游戏启动入口仅适用于 Windows')
    # Coalesce repeated clicks in one local UI, as well as in the worker queue.
    k=c.WinDLL('kernel32',use_last_error=True);k.CreateMutexW.argtypes=(c.c_void_p,w.BOOL,w.LPCWSTR);k.CreateMutexW.restype=w.HANDLE;k.CloseHandle.argtypes=(w.HANDLE,)
    k.WaitForSingleObject.argtypes=(w.HANDLE,w.DWORD);k.ReleaseMutex.argtypes=(w.HANDLE,)
    mutex=k.CreateMutexW(None,True,'Local\\SDGO.LaunchUI.'+hashlib.sha256(str(Path(root).resolve()).casefold().encode()).hexdigest())
    if not mutex:raise OSError(c.get_last_error(),'无法创建启动窗口锁')
    if c.get_last_error()==183:
        from . import storage
        current=storage.read_json(launch.folder(root)/'ui.json',{})
        pending=launch.state(root,current['token'])if current.get('token')else{}
        if pending.get('status')not in('launched','cancelled'):
            k.CloseHandle(mutex);return
        # The previous launch is complete but its dialog is still unwinding.
        # Wait for that finite UI to close instead of dropping the next click.
        if k.WaitForSingleObject(mutex,15000)not in(0,0x80):
            k.CloseHandle(mutex);raise ValueError('上一次启动窗口仍在关闭，请稍后重试')
    from . import storage
    storage.atomic(launch.folder(root)/'ui.json',storage.json_bytes({'token':token,'pid':os.getpid()}))
    session=Session(root,token);worker=threading.Thread(target=session.work,name='startup-progress',daemon=True);worker.start()
    try:dialog(session)
    finally:
        session.stop.set();worker.join(timeout=6)
        current=storage.read_json(launch.folder(root)/'ui.json',{})
        if current.get('pid')==os.getpid():storage.remove(launch.folder(root)/'ui.json')
        k.ReleaseMutex(mutex);k.CloseHandle(mutex)
