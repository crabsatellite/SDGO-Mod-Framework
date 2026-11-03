"""Install an existing Manager publication before a native run is sealed."""
from __future__ import annotations
import hashlib
import json
from pathlib import Path
import subprocess

PROGRAMS=('d3d9.dll','sdgo/core.dll','sdgo/mods/capsule_catalog/capsule_catalog.dll')

def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def validate(source,stage):
    from manager.engines.capsules import capsule_deployment as deploy
    source=Path(source).resolve();stage=Path(stage).resolve()
    manifest=deploy._read_json(source/'manifest.json')
    revision=manifest.get('catalog_revision')
    if not isinstance(revision,str) or len(revision)!=64 or any(c not in '0123456789abcdef' for c in revision):
        raise ValueError('capsule publication revision is invalid')
    deploy._validate_package(source,revision)
    for name in PROGRAMS:
        if sha(source/'player/client'/name)!=sha(stage/'client'/name):
            raise ValueError('capsule publication program differs from the selected candidate stage: '+name)
    return {'path':str(source),'stage':str(stage),'revision':revision,
        'manifest_sha256':sha(source/'manifest.json')}

def install(binding,client):
    from manager.windows import powershell_environment,powershell_executable
    if validate(binding['path'],binding['stage'])!=binding:
        raise ValueError('capsule publication changed before installation')
    source=Path(binding['path']);client=Path(client).resolve();player=source/'player'
    # Execute framework-owned installer code, never the package's script.
    script=Path(__file__).resolve().parents[1]/'manager/engines/capsules/install_capsule_client.ps1'
    result=subprocess.run([powershell_executable(),'-NoProfile','-NonInteractive',
        '-ExecutionPolicy','Bypass','-File',str(script),'-GameDir',str(client),
        '-SourceClient',str(player/'client'),'-SymbolFragment',str(player/'symbol-fragment.ini'),
        '-PackageManifest',str(player/'package-manifest.json')],capture_output=True,text=True,
        encoding='utf-8',errors='replace',env=powershell_environment(),timeout=180,
        creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    if result.returncode:raise ValueError('capsule candidate installation failed: '+(result.stderr or result.stdout)[-3000:])
    if validate(binding['path'],binding['stage'])!=binding:
        raise ValueError('capsule publication changed during installation')
    manifest=json.loads((player/'package-manifest.json').read_text(encoding='utf-8-sig'))
    for name,expected in manifest['files'].items():
        if (name in PROGRAMS or name.startswith('sdgo/mods/capsule_catalog/')) and name!='sdgo/mods/capsule_catalog/mod.ini':
            if sha(client/name)!=expected:raise ValueError('installed capsule candidate differs: '+name)
    return {key:value for key,value in binding.items() if key!='stage'}
