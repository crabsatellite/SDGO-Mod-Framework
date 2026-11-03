"""Build a generic, verified Stable or Beta Manager release (no game data)."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import sys
import zipfile

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from tools.mod_catalog import read_catalog, validate_stage
from manager.updates import REPOSITORY, MANAGER, version, unpack


def catalog_for(channel):
    full=read_catalog(ROOT)
    channels={'stable','beta'} if channel=='beta' else {'stable'}
    mods=[m for m in full['mods'] if m['channel'] in channels and m['category']!='development']
    features={m['feature'] for m in mods}
    return {**full,'profile':channel,'mods':mods,'features':{k:v for k,v in full['features'].items() if k in features}}


def build(channel, release_version, stage, manager, output, commit=None):
    version(release_version)
    if (channel=='beta')!=('-beta.' in release_version):raise ValueError('Version must match the release channel')
    catalog=catalog_for(channel);validate_stage(ROOT,stage,catalog)
    payload={MANAGER:manager.read_bytes()}
    for tree,loader in (('client','d3d9.dll'),('server','winmm.dll')):
        for name in (loader,'sdgo/core.dll','sdgo/sdgo.ini'):
            payload[tree+'/'+name]=(stage/tree/name).read_bytes()
        for symbol in (stage/tree/'sdgo/symbols').glob('*.ini'):
            payload[tree+'/sdgo/symbols/'+symbol.name]=symbol.read_bytes()
        payload[tree+'/sdgo/catalog.json']=(json.dumps(catalog,ensure_ascii=False,indent=2)+'\n').encode('utf-8')
    for mod in catalog['mods']:
        base=mod['target']+'/sdgo/mods/'+mod['id']+'/'
        for name in (mod['id']+'.dll','mod.ini',*mod['files'],*mod.get('runtime_files',[])):
            payload[base+name]=(stage/base/name).read_bytes()
    commit=commit or subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    manifest={'schema_version':1,'repository':REPOSITORY,'version':release_version,'channel':channel,
              'source_commit':commit,'files':{n:{'bytes':len(b),'sha256':hashlib.sha256(b).hexdigest()} for n,b in sorted(payload.items())}}
    output.mkdir(parents=True,exist_ok=True)
    path=output/('SDGO-ModFramework-'+release_version+'.zip')
    if path.exists():raise ValueError('Release asset already exists; never overwrite published versions')
    with zipfile.ZipFile(path,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as archive:
        archive.writestr('release.json',json.dumps(manifest,ensure_ascii=False,indent=2)+'\n')
        for name,data in sorted(payload.items()):archive.writestr(name,data)
    raw=path.read_bytes();digest=hashlib.sha256(raw).hexdigest()
    # Validate the exact output using the Manager's production package reader.
    check=output/('verified-'+release_version)
    unpack(raw,{'bytes':len(raw),'sha256':digest,'version':release_version,'channel':channel},check)
    checksums=path.with_suffix('.sha256');checksums.write_text(digest+'  '+path.name+'\n',encoding='ascii')
    print(json.dumps({'asset':str(path),'sha256':digest,'bytes':len(raw),'channel':channel,
                      'mods':len(catalog['mods']),'source_commit':commit}),flush=True)
    return path


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--channel',choices=('stable','beta'),required=True)
    p.add_argument('--version',required=True)
    p.add_argument('--build-dir',type=Path,default=ROOT/'build')
    p.add_argument('--output',type=Path,default=ROOT/'dist')
    p.add_argument('--build',action='store_true')
    args=p.parse_args();directory=args.build_dir.resolve()
    if args.build:
        subprocess.run(['cmake','-S',str(ROOT),'-B',str(directory),'-A','Win32','-DBUILD_TESTING=ON'],check=True)
        # The authoring tool embeds its optional importer binary; Stable still
        # excludes that Beta Mod from the installable client catalog/payload.
        subprocess.run(['cmake','--build',str(directory),'--config','Release','--target','release_'+args.channel+'_payload','unit_catalog','sdgo_data','--parallel','3'],check=True)
        subprocess.run(['powershell.exe','-NoProfile','-File',str(ROOT/'tools/build_mod_manager.ps1'),'-BuildDir',str(directory)],
            check=True,env=dict(os.environ,SDGO_RELEASE_VERSION=args.version))
    build(args.channel,args.version,directory/'stage/Release',directory/'manager'/MANAGER,args.output.resolve())


if __name__=='__main__':main()
