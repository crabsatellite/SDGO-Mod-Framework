"""Local Windows shortcuts for the server owner's existing Manager workspace."""
import ctypes
import json
import os
from pathlib import Path
import subprocess

DESKTOP_NAME = 'SDGO MOD 管理（服主专用）.lnk'
CLIENT_NAME = '0_MOD管理（服主专用）.lnk'
DESCRIPTION = '服主与维护人员工具。普通玩家请使用游戏启动器，勿自行修改 Mod；客户端与服务器配置不匹配可能导致断线或崩溃。'

SCRIPT = r'''
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$items=ConvertFrom-Json $env:SDGO_MANAGER_SHORTCUTS_JSON
$shell=New-Object -ComObject WScript.Shell
$result=@()
foreach($item in $items){
  $link=$shell.CreateShortcut($item.path)
  if((Test-Path -LiteralPath $item.path) -and $link.TargetPath -and (Test-Path -LiteralPath $link.TargetPath)){
    $result+=@{path=$item.path;status='kept'}
    continue
  }
  $link.TargetPath=$item.target
  $link.Arguments=$item.arguments
  $link.WorkingDirectory=$item.directory
  $link.Description=$item.description
  $link.IconLocation=$item.target+',0'
  $link.Save()
  $result+=@{path=$item.path;status='created'}
}
ConvertTo-Json -InputObject @($result) -Compress
'''


def desktop_directory():
    if os.name != 'nt': raise OSError('快捷方式仅支持 Windows')
    target=ctypes.create_unicode_buffer(32768)
    if ctypes.windll.shell32.SHGetFolderPathW(None,0x10,None,0,target):
        raise OSError('无法读取当前用户的桌面目录')
    return Path(target.value).resolve()


def create(workspace, executable, desktop=None):
    executable=Path(executable).resolve(strict=True)
    if executable.name!='SDGO Mod Manager.exe':raise ValueError('请选择正式的 Mod Manager 程序')
    desktop=Path(desktop).resolve(strict=True) if desktop is not None else desktop_directory()
    paths=[desktop/DESKTOP_NAME]
    for host in workspace.settings['hosts']:
        if host['role']!='client':continue
        root=Path(host['path']).resolve(strict=True)
        if (root/'GOnline.exe').is_file():paths.append(root/CLIENT_NAME)
    arguments='--workspace "'+str(workspace.root)+'"'
    items=[{'path':str(path),'target':str(executable),'arguments':arguments,
            'directory':str(executable.parent),'description':DESCRIPTION} for path in dict.fromkeys(paths)]
    process=subprocess.run(['powershell.exe','-NoLogo','-NoProfile','-NonInteractive','-Command',SCRIPT],
        env=dict(os.environ,SDGO_MANAGER_SHORTCUTS_JSON=json.dumps(items,ensure_ascii=False)),
        capture_output=True,text=True,encoding='utf-8',timeout=20,
        creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    if process.returncode:raise OSError('快捷方式创建失败，请检查桌面和客户端目录的写入权限')
    return json.loads(process.stdout)
