"""Local Windows shortcuts for the server owner's existing Manager workspace."""
import ctypes
import json
import os
from pathlib import Path
import subprocess

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


def create(workspace, executable):
    executable=Path(executable).resolve(strict=True)
    if executable.name!='SDGO Mod Manager.exe':raise ValueError('请选择正式的 Mod Manager 程序')
    paths=[]
    for host in workspace.settings['hosts']:
        if host['role']!='client':continue
        root=Path(host['path']).resolve(strict=True)
        if (root/'GOnline.exe').is_file():paths.append(root/CLIENT_NAME)
    if not paths:return []
    arguments='--workspace "'+str(workspace.root)+'"'
    items=[{'path':str(path),'target':str(executable),'arguments':arguments,
            'directory':str(executable.parent),'description':DESCRIPTION} for path in dict.fromkeys(paths)]
    process=subprocess.run(['powershell.exe','-NoLogo','-NoProfile','-NonInteractive','-Command',SCRIPT],
        env=dict(os.environ,SDGO_MANAGER_SHORTCUTS_JSON=json.dumps(items,ensure_ascii=True)),
        capture_output=True,text=True,encoding='utf-8',timeout=20,
        creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    if process.returncode:raise OSError('目录入口创建失败：'+process.stderr.strip()[-800:])
    result=json.loads(process.stdout)
    if os.name=='nt':
        for path in paths:ctypes.windll.shell32.SHChangeNotify(0x2000,5,str(path),None)
    return result
