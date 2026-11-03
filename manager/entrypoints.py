"""Unicode Windows Shell links in registered client folders; no desktop entries."""
import ctypes as c
import os
from pathlib import Path
import uuid

CLIENT_NAME='0_MOD管理（服主专用）.lnk'
DESCRIPTION='服主与维护人员工具。普通玩家请使用游戏启动器，勿自行修改 Mod；客户端与服务器配置不匹配可能导致断线或崩溃。'

class GUID(c.Structure):
    _fields_=[('a',c.c_uint32),('b',c.c_uint16),('d',c.c_uint16),('tail',c.c_ubyte*8)]
    def __init__(self,value):super().__init__();c.memmove(c.byref(self),uuid.UUID(value).bytes_le,16)

def check(result):
    if result<0:raise OSError('Windows 目录入口创建失败：0x%08X'%(result & 0xffffffff))

def method(pointer,index,*types):
    address=c.cast(pointer,c.POINTER(c.POINTER(c.c_void_p))).contents[index]
    return c.WINFUNCTYPE(c.c_long,c.c_void_p,*types)(address)

class ShellLink:
    """IShellLinkW and IPersistFile, using the Windows SDK vtable contract."""
    def __enter__(self):
        self.shell=c.c_void_p();self.file=c.c_void_p();self.ole=c.WinDLL('ole32')
        self.ole.CoInitializeEx.argtypes=[c.c_void_p,c.c_uint32]
        status=self.ole.CoInitializeEx(None,2)
        self.initialized=status in (0,1)
        if status not in (0,1,-2147417850):check(status)
        self.ole.CoCreateInstance.argtypes=[c.POINTER(GUID),c.c_void_p,c.c_uint32,c.POINTER(GUID),c.POINTER(c.c_void_p)]
        try:
            check(self.ole.CoCreateInstance(c.byref(GUID('00021401-0000-0000-c000-000000000046')),None,1,
                c.byref(GUID('000214f9-0000-0000-c000-000000000046')),c.byref(self.shell)))
            check(method(self.shell,0,c.POINTER(GUID),c.POINTER(c.c_void_p))(self.shell,
                c.byref(GUID('0000010b-0000-0000-c000-000000000046')),c.byref(self.file)))
            return self
        except BaseException:self.__exit__(None,None,None);raise
    def __exit__(self,*_):
        for pointer in (self.file,self.shell):
            if pointer:method(pointer,2)(pointer)
        if self.initialized:self.ole.CoUninitialize()
    def load(self,path):check(method(self.file,5,c.c_wchar_p,c.c_uint32)(self.file,str(path),0))
    def target(self):
        buffer=c.create_unicode_buffer(32768)
        check(method(self.shell,3,c.c_wchar_p,c.c_int,c.c_void_p,c.c_uint32)(self.shell,buffer,len(buffer),None,0))
        return buffer.value
    def set(self,index,value):check(method(self.shell,index,c.c_wchar_p)(self.shell,str(value)))
    def save(self,path):check(method(self.file,6,c.c_wchar_p,c.c_int)(self.file,str(path),1))

def create(workspace,executable):
    if os.name!='nt':raise OSError('目录快捷入口仅支持 Windows')
    executable=Path(executable).resolve(strict=True)
    if executable.name!='SDGO Mod Manager.exe':raise ValueError('请选择正式的 Mod Manager 程序')
    paths=[]
    for host in workspace.settings['hosts']:
        if host['role']!='client':continue
        root=Path(host['path']).resolve(strict=True)
        if (root/'GOnline.exe').is_file():paths.append(root/CLIENT_NAME)
    rows=[]
    for path in dict.fromkeys(paths):
        with ShellLink() as link:
            if path.exists():
                link.load(path);target=link.target()
                if target and Path(target).exists():
                    rows.append({'path':str(path),'status':'kept'});continue
            link.set(20,executable);link.set(9,executable.parent)
            link.set(11,'--workspace "'+str(workspace.root)+'"');link.set(7,DESCRIPTION)
            check(method(link.shell,17,c.c_wchar_p,c.c_int)(link.shell,str(executable),0))
            link.save(path);rows.append({'path':str(path),'status':'created'})
    for path in (*paths,executable):c.windll.shell32.SHChangeNotify(0x2000,5,str(path),None)
    return rows
