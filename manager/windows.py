"""Launch Windows PowerShell with its own modules, even from a pwsh 7 parent."""
import os
from pathlib import Path


def powershell_environment():
    env=dict(os.environ)
    system=Path(env.get('SystemRoot',r'C:\Windows'))
    paths=[system/'System32/WindowsPowerShell/v1.0/Modules']
    if env.get('ProgramFiles'):paths.append(Path(env['ProgramFiles'])/'WindowsPowerShell/Modules')
    if env.get('USERPROFILE'):paths.append(Path(env['USERPROFILE'])/'Documents/WindowsPowerShell/Modules')
    # Python does not receive pwsh's special Windows PowerShell compatibility
    # environment rewrite. Inheriting its .NET 9 modules breaks Get-FileHash.
    env['PSModulePath']=os.pathsep.join(str(p) for p in paths)
    return env


def powershell_executable():
    path=Path(os.environ.get('SystemRoot',r'C:\Windows'))/'System32/WindowsPowerShell/v1.0/powershell.exe'
    return str(path) if path.is_file() else 'powershell.exe'


def product_name(path):
    """Read a PE's version resource without executing it or starting a shell."""
    if os.name!='nt':return None
    import ctypes
    from ctypes import wintypes
    path=Path(path).resolve(strict=True)
    version=ctypes.WinDLL('version',use_last_error=True)
    size_fn=version.GetFileVersionInfoSizeW
    size_fn.argtypes=(wintypes.LPCWSTR,ctypes.POINTER(wintypes.DWORD));size_fn.restype=wintypes.DWORD
    read=version.GetFileVersionInfoW
    read.argtypes=(wintypes.LPCWSTR,wintypes.DWORD,wintypes.DWORD,wintypes.LPVOID);read.restype=wintypes.BOOL
    query=version.VerQueryValueW
    query.argtypes=(wintypes.LPCVOID,wintypes.LPCWSTR,ctypes.POINTER(wintypes.LPVOID),ctypes.POINTER(wintypes.UINT));query.restype=wintypes.BOOL
    unused=wintypes.DWORD();size=size_fn(str(path),ctypes.byref(unused))
    if not size:return None
    if size>16*1024*1024:raise ValueError('Version resource exceeds supported size')
    buffer=ctypes.create_string_buffer(size)
    if not read(str(path),0,size,buffer):raise ctypes.WinError(ctypes.get_last_error())
    pointer=wintypes.LPVOID();length=wintypes.UINT()
    if not query(buffer,r'\VarFileInfo\Translation',ctypes.byref(pointer),ctypes.byref(length)):return None
    if not pointer.value or length.value%4 or length.value>4096:raise ValueError('Invalid version translation table')
    translations=ctypes.string_at(pointer,length.value)
    import struct
    for offset in range(0,len(translations),4):
        language,codepage=struct.unpack_from('<HH',translations,offset)
        key=rf'\StringFileInfo\{language:04x}{codepage:04x}\ProductName'
        if not query(buffer,key,ctypes.byref(pointer),ctypes.byref(length)):continue
        if not pointer.value or not 0<length.value<=32768:raise ValueError('Invalid product-name version string')
        return ctypes.wstring_at(pointer,length.value).split('\0',1)[0].strip() or None
    return None


def process_image_paths(image_name):
    """Enumerate exact Windows images, failing closed on inaccessible matches."""
    if os.name!='nt':return []
    if Path(image_name).name!=image_name or not image_name:raise ValueError('Expected an executable basename')
    import ctypes
    from ctypes import wintypes as w
    class Entry(ctypes.Structure):
        _fields_=[('dwSize',w.DWORD),('cntUsage',w.DWORD),('pid',w.DWORD),
                  ('heap',ctypes.c_size_t),('module',w.DWORD),('threads',w.DWORD),
                  ('parent',w.DWORD),('priority',w.LONG),('flags',w.DWORD),('name',w.WCHAR*260)]
    k=ctypes.WinDLL('kernel32',use_last_error=True)
    k.CreateToolhelp32Snapshot.argtypes=(w.DWORD,w.DWORD);k.CreateToolhelp32Snapshot.restype=w.HANDLE
    k.Process32FirstW.argtypes=(w.HANDLE,ctypes.POINTER(Entry));k.Process32FirstW.restype=w.BOOL
    k.Process32NextW.argtypes=(w.HANDLE,ctypes.POINTER(Entry));k.Process32NextW.restype=w.BOOL
    k.OpenProcess.argtypes=(w.DWORD,w.BOOL,w.DWORD);k.OpenProcess.restype=w.HANDLE
    k.QueryFullProcessImageNameW.argtypes=(w.HANDLE,w.DWORD,w.LPWSTR,ctypes.POINTER(w.DWORD));k.QueryFullProcessImageNameW.restype=w.BOOL
    k.CloseHandle.argtypes=(w.HANDLE,);k.CloseHandle.restype=w.BOOL
    snapshot=k.CreateToolhelp32Snapshot(2,0)
    if snapshot==ctypes.c_void_p(-1).value:raise ctypes.WinError(ctypes.get_last_error())
    result=[]
    try:
        entry=Entry();entry.dwSize=ctypes.sizeof(Entry)
        available=k.Process32FirstW(snapshot,ctypes.byref(entry))
        if not available and ctypes.get_last_error()!=18:raise ctypes.WinError(ctypes.get_last_error())
        while available:
            if entry.name.casefold()==image_name.casefold():
                process=k.OpenProcess(0x1000,False,entry.pid)
                if not process:
                    error=ctypes.get_last_error()
                    if error not in (87,1168):raise ctypes.WinError(error)
                else:
                    try:
                        buffer=ctypes.create_unicode_buffer(32768);size=w.DWORD(len(buffer))
                        if not k.QueryFullProcessImageNameW(process,0,buffer,ctypes.byref(size)):
                            raise ctypes.WinError(ctypes.get_last_error())
                        if Path(buffer.value).name.casefold()==image_name.casefold():result.append(buffer.value)
                    finally:k.CloseHandle(process)
            available=k.Process32NextW(snapshot,ctypes.byref(entry))
        if ctypes.get_last_error()!=18:raise ctypes.WinError(ctypes.get_last_error())
    finally:k.CloseHandle(snapshot)
    return result
