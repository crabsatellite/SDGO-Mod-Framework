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
