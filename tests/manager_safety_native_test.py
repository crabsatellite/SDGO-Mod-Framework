"""Actual core/DLL loads and config acknowledgements in owned, non-game hosts."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
from manager import configuration as cfg,storage,runtime_state


def wait(predicate,process):
    end=time.monotonic()+20
    while time.monotonic()<end:
        value=predicate()
        if value:return value
        if process.poll()is not None:raise AssertionError('native fixture exited: '+str(process.returncode))
        time.sleep(.05)
    raise AssertionError('native fixture timed out')


def main():
    p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True);a=p.parse_args();release=a.build.resolve()/'Release'
    cases=[('legacy',{},True,True),('hard',{'required':[{'id':'provider','minimum':'1.0.0','capability':'bridge','capability_version':1}]},True,True),
           ('missing',{'required':[{'id':'absent'}]},True,False),
           ('optional',{'optional':[{'id':'absent'}]},True,True),
           ('incompatible',{'optional':[{'id':'provider','capability':'bridge','capability_version':2}]},True,False),
           ('failed-provider',{'required':[{'id':'provider'}]},False,False),
           ('conflict',{'conflicts':[{'id':'provider'}]},True,False),
           ('cycle',{'required':[{'id':'provider'}]},True,False)]
    for title,rules,provider_ok,expected in cases:
        with tempfile.TemporaryDirectory(prefix='sdgo-safety-native-')as directory:
            root=Path(directory);sdgo=root/'sdgo';sdgo.mkdir()
            shutil.copy2(release/'manager_safety_host.exe',root/'GOnline.exe');shutil.copy2(release/'core.dll',sdgo/'core.dll')
            for identity in ('consumer','provider'):
                path=sdgo/'mods'/identity;path.mkdir(parents=True)
                shutil.copy2(release/'manager_safety_fixture.dll',path/(identity+'.dll'))
                contract={'id':identity,'provides':{'bridge':1},'dependencies':rules if identity=='consumer'else({'required':[{'id':'consumer'}]}if title=='cycle'else{})}
                (path/'manager.json').write_bytes(storage.json_bytes(contract))
                # Consumer order deliberately precedes its required provider.
                raw=('[mod]\nid='+identity+'\nversion=1.0.0\napi=1\nhosts=*\norder='+('1'if identity=='consumer'else'100')+'\ndll='+identity+'.dll\n[settings]\nFailLoad='+('1'if identity=='provider'and not provider_ok else'0')+'\n').encode()
                (path/'mod.ini').write_bytes(raw if title=='legacy'else cfg.render(raw,contract))
            process=subprocess.Popen([str(root/'GOnline.exe'),str(root)],cwd=root,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            try:
                wait(lambda:(root/'ready').exists(),process)
                file=sdgo/'local-state/runtime'/(str(process.pid)+'.json')
                value=wait(lambda:storage.read_json(file)if file.exists()else None,process)
                assert value['mods']['consumer']['active']==expected,(title,value)
                assert value['pid']==process.pid and value['creation_ticks']
                if title=='hard':
                    manifest=sdgo/'mods/consumer/mod.ini';contract=storage.read_json(manifest.parent/'manager.json')
                    old=value['mods']['consumer']['configuration_sha256']
                    manifest.write_bytes(storage.patch_ini(manifest.read_bytes(),[('settings','Amount','42')]))
                    assert cfg.fingerprint(manifest.parent,cfg.files(storage.ini_values(manifest.read_bytes()),contract))!=old
                    (root/'reload').touch();wait(lambda:(root/'reload-result').exists(),process)
                    assert(root/'reload-result').read_text()=='1'
                    value=wait(lambda:(v if(v:=storage.read_json(file))['mods']['consumer']['configuration_sha256']!=old else None),process)
                    assert value['mods']['consumer']['configuration_sha256']==cfg.fingerprint(manifest.parent,cfg.files(storage.ini_values(manifest.read_bytes()),contract))
                    applied=value['mods']['consumer']['configuration_sha256'];(root/'reload-result').unlink()
                    manifest.write_bytes(storage.patch_ini(manifest.read_bytes(),[('settings','RejectReload','1')]))
                    (root/'reload').touch();wait(lambda:(root/'reload-result').exists(),process)
                    assert(root/'reload-result').read_text()=='0'
                    assert storage.read_json(file)['mods']['consumer']['configuration_sha256']==applied
                    (root/'reload-result').unlink()
                    manifest.write_bytes(storage.patch_ini(manifest.read_bytes(),[('settings','RejectReload','0'),('settings','ChangeDuringReload','1')]))
                    (root/'reload').touch();wait(lambda:(root/'reload-result').exists(),process)
                    assert(root/'reload-result').read_text()=='0'
                    assert storage.read_json(file)['mods']['consumer']['configuration_sha256']==applied
            finally:
                (root/'stop').touch()
                try:process.wait(timeout=15)
                except subprocess.TimeoutExpired:process.terminate();process.wait(timeout=30)
        print('PASS native core:',title,flush=True)


if __name__=='__main__':main()
