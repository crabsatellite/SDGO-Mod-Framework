"""Exercise the frozen Manager and shipped loader on synthetic native hosts.

The fixture Git transport is explicitly remapped to a local repository. This
proves process/update boundaries, not a public download or gameplay acceptance.
"""
from __future__ import annotations
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import struct
import sys
import time
import urllib.request
import zipfile
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from manager import storage,updates,launch,owner_programmes,distribution
from tests.test_manager_updates import bundle
from tools.game_test_windows import WindowsBackgroundProcess
from tools.game_test import process_snapshot


def wait(check,seconds=90):
    deadline=time.monotonic()+seconds
    while time.monotonic()<deadline:
        try:value=check()
        except(FileNotFoundError,PermissionError):value=None
        if value:return value
        time.sleep(.1)
    raise AssertionError('native startup condition timed out')


def api(root,path,body=None):
    session=storage.read_json(root/'sdgo/local-state/manager/manager-session.json',{})
    url=session.get('url')
    if not url:return None
    if body is None:
        with urllib.request.urlopen(url+path,timeout=3)as response:return json.load(response)
    with urllib.request.urlopen(url+'api/identity',timeout=3)as response:secret=json.load(response)['session_token']
    request=urllib.request.Request(url+path,data=storage.json_bytes(body),headers={'Origin':url.rstrip('/'),'Content-Type':'application/json','X-ModManager-Token':secret})
    with urllib.request.urlopen(request,timeout=3)as response:return json.load(response)


def close_error(process):
    user=c.WinDLL('user32');user.GetWindowThreadProcessId.argtypes=(w.HWND,c.POINTER(w.DWORD));user.PostMessageW.argtypes=(w.HWND,w.UINT,w.WPARAM,w.LPARAM)
    found=[]
    @c.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
    def window(hwnd,data):
        value=w.DWORD();user.GetWindowThreadProcessId(hwnd,c.byref(value))
        name=c.create_unicode_buffer(256);user.GetClassNameW(hwnd,name,256)
        if value.value==process.pid and name.value=='#32770':found.append(hwnd);user.PostMessageW(hwnd,0x111,1,0)
        return True
    user.EnumDesktopWindows.argtypes=(w.HANDLE,type(window),w.LPARAM)
    user.EnumDesktopWindows(process._desktop_handle,window,0);return bool(found)


def prepare(base,build,with_owner=False,manager_handoff=False):
    game=base/'client';game.mkdir(parents=True)
    shutil.copyfile(build/'Release/launch_gate_host.exe',game/'GOnline.exe')
    shutil.copyfile(build/'Release/d3d9.dll',game/'d3d9.dll')
    shutil.copyfile(build/'manager/SDGO Mod Manager.exe',game/'SDGO Mod Manager.exe')
    raw,asset,_=bundle(release='1.14.0');payload={}
    with zipfile.ZipFile(io.BytesIO(raw))as z:
        manifest=json.loads(z.read('release.json'))
        payload={n:z.read(n)for n in manifest['files']}
    payload['client/d3d9.dll']=(game/'d3d9.dll').read_bytes()
    if with_owner:payload[updates.MANAGER]=(game/updates.MANAGER).read_bytes()
    manifest['capabilities']={'runtime_profiles':1,'runtime_safety':1,'launch_gate':1}
    if with_owner:manifest['capabilities']['owner_programmes']=1
    manifest['files']={n:{'bytes':len(raw),'sha256':updates.digest(raw)}for n,raw in payload.items()}
    archive=io.BytesIO()
    with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_DEFLATED)as z:
        z.writestr('release.json',storage.json_bytes(manifest))
        for name,raw in payload.items():z.writestr(name,raw)
    raw=archive.getvalue();repo=base/'framework';repo.mkdir()
    name='SDGO-ModFramework-1.14.0.zip';storage.atomic(repo/'releases'/name,raw)
    source_id=updates.digest(('sdgo-framework-runtime:'+updates.REPOSITORY).encode())[:32]
    storage.atomic(repo/'framework-feed.json',storage.json_bytes({'schema_version':1,'kind':'sdgo-framework-runtime','source_id':source_id,
        'repository':updates.REPOSITORY,'version':'1.14.0','channel':'stable','source_commit':'d'*40,
        'asset':{'path':'releases/'+name,'bytes':len(raw),'sha256':updates.digest(raw)}}))
    def git(*args):return subprocess.check_output(['git','-C',str(repo),*args],stderr=subprocess.DEVNULL).decode().strip()
    git('init','-b','runtime/stable');git('config','user.name','Fixture');git('config','user.email','fixture@example.invalid');git('add','.');git('commit','-m','Native startup fixture')
    commit=git('rev-parse','HEAD')
    profile_dir=base/'profile';profile_dir.mkdir()
    shared={'schema_version':1,'patches':{'sdgo/mods/example/mod.ini':[['settings','Amount','8']]},'personal_settings':[]}
    content={'sdgo/shared-settings.json':storage.json_bytes(shared),'sprs/fixture.spr':b'native startup content revision 1'}
    profile={'schema_version':1,'kind':'sdgo-server-profile','source_id':'a'*32,'label':'Native startup fixture','revision':1,
        'base':{'executable':'GOnline.exe','host_key':updates.host_key(game/'GOnline.exe')},
        'framework':{'version':'1.14.0','sha256':updates.digest(raw),'commit':commit},'mods':{'example':'2.0.0'},
        'shared_files':[],'files':{n:{'bytes':len(raw),'sha256':updates.digest(raw)}for n,raw in content.items()}}
    for n,raw in content.items():storage.atomic(profile_dir/'content'/n,raw)
    if with_owner:
        editor=base/'owner-editor';mod=editor/'sdgo/owner-mods/fixture_owner';mod.mkdir(parents=True)
        storage.atomic(mod/'fixture_owner.dll',b'owner programme revision 1 '+distribution.OWNER_MARKER)
        storage.atomic(mod/'mod.ini',b'[mod]\nid=fixture_owner\nname=Owner fixture\nversion=0.1.0\ndll=fixture_owner.dll\ndistribution=owner\nenabled=1\n')
        with(mod/'mod.ini').open('ab')as stream:stream.write(('hosts='+profile['base']['host_key']+'\n').encode())
        storage.atomic(mod/'manager.json',storage.json_bytes({'schema_version':1,'id':'fixture_owner','title':'Owner fixture','distribution':'owner','hosts':['client'],'settings':[]}))
        owner_dir=base/'owner-programme'
        programme=owner_programmes.publish({'path':str(editor)},{'source_id':profile['source_id'],'channel':'stable',
            'feeds':['local+'+(owner_dir/owner_programmes.MANIFEST).as_uri()]},profile['framework'],owner_dir,['fixture_owner'])
        profile.update(schema_version=2,channel='stable',owner_programme=programme)
        storage.atomic(game/'sdgo/local-mods/unknown/mod.ini',b'[mod]\nid=unknown\n')
        storage.atomic(game/'sdgo/local-mods/unknown/unknown.dll',b'personal addon bytes')
    storage.atomic(profile_dir/'server-profile.json',storage.json_bytes(profile))
    binding={'schema_version':3,'kind':'sdgo-client-runtime','source_id':'a'*32,'label':profile['label'],
        'feeds':['local+'+(profile_dir/'server-profile.json').as_uri()],
        'framework':{'schema_version':1,'kind':'sdgo-framework-runtime','source_id':source_id,'repository':updates.REPOSITORY,'channel':'stable',
            'feeds':['git+https://startup-fixture.invalid/framework.git#runtime/stable']}}
    if with_owner:binding.update(channel='stable',manager_policy='owner')
    storage.atomic(game/'sdgo/client-source.json',storage.json_bytes(binding))
    if manager_handoff:
        # Only the fixture's initial EXE timestamp differs. Its frozen code is
        # unchanged; the approved programme contains the exact original build.
        manager=game/updates.MANAGER;changed=bytearray(manager.read_bytes())
        pe=struct.unpack_from('<I',changed,0x3c)[0]
        stamp=struct.unpack_from('<I',changed,pe+8)[0]
        struct.pack_into('<I',changed,pe+8,stamp^1);manager.write_bytes(changed)
    # Only this test process tree receives the fixture transport mapping.
    env=dict(os.environ,GIT_CONFIG_COUNT='2',GIT_CONFIG_KEY_0='url.'+repo.as_uri()+'.insteadOf',GIT_CONFIG_VALUE_0='https://startup-fixture.invalid/framework.git',
        GIT_CONFIG_KEY_1='protocol.file.allow',GIT_CONFIG_VALUE_1='always',SDGO_MANAGER_SHUTDOWN_TRACE='1')
    return game,profile_dir,profile,env


def run_sealed(base,build):
    """A matching sealed run keeps candidates; other IDs still enter the gate."""
    game=base/'sealed-client';game.mkdir(parents=True)
    for source,name in [('launch_gate_host.exe','GOnline.exe'),('d3d9.dll','d3d9.dll')]:
        shutil.copyfile(build/'Release'/source,game/name)
    storage.atomic(game/'sdgo/client-source.json',b'{}')
    storage.atomic(game/'sdgo/gametest/run.ini',b'[run]\nid=sealed-native-fixture\n')
    checks=[]
    for name,run_id,forced,allowed in [
        ('matching_sealed_run_preserves_candidates','sealed-native-fixture','',True),
        ('different_run_cannot_bypass_manager','different-native-fixture','',False),
        ('forced_startup_test_enters_manager','sealed-native-fixture','sealed-native-fixture',False)]:
        env={**os.environ,'SDGO_GAME_TEST_RUN':run_id,'SDGO_GAME_TEST_LAUNCH_GATE':forced}
        env.pop('SDGO_LAUNCH_PERMIT',None)
        process=WindowsBackgroundProcess([str(game/'GOnline.exe')],cwd=game,env=env,
            stdout=None,stderr=None,run_id='launch-sealed-'+uuid.uuid4().hex,allow_named_desktop_fallback=True)
        try:
            if allowed:
                assert process.wait(10)==0
                assert(game/'native-entry.txt').is_file()
                (game/'native-entry.txt').unlink()
            else:
                wait(lambda:close_error(process),10)
                if process.poll()is None:process.kill()
                assert process.wait(10)!=0
                assert not(game/'native-entry.txt').exists()
            checks.append(name)
        finally:
            if process.poll()is None:process.kill();process.wait(10)
            process.close()
    return {'status':'pass','checks':checks}


def run(base,build,with_owner=False,manager_handoff=False):
    sealed=run_sealed(base,build)
    game,profile_dir,profile,env=prepare(base,build,with_owner,manager_handoff);checks=[];owned=[]
    checks.extend(sealed['checks'])
    def start(**extra):
        process=WindowsBackgroundProcess([str(game/'GOnline.exe'),'--example','中文 参数'],cwd=game,env={**env,**extra},
            stdout=None,stderr=None,run_id='launch-gate-'+uuid.uuid4().hex,allow_named_desktop_fallback=True)
        owned.append(process);return process
    try:
        before=hashlib.sha256((game/'GOnline.exe').read_bytes()).hexdigest()
        first=start();second=start()
        wait(lambda:(game/'native-entry.txt').is_file(),120)
        results=wait(lambda:[storage.read_json(p,{})for p in launch.folder(game).glob('*.result.json')if storage.read_json(p,{}).get('status')=='launched'])
        assert len(results)==1,results
        assert before==hashlib.sha256((game/'GOnline.exe').read_bytes()).hexdigest()
        assert '中文 参数'in(game/'native-entry.txt').read_bytes().decode('utf-16-le')
        assert (game/'sprs/fixture.spr').read_bytes()==b'native startup content revision 1'
        assert first.wait(10)==0 and second.wait(10)==0
        checks+=['frozen_manager_native_handoff','one_launch_for_double_click','original_executable_and_arguments_preserved','content_installed_before_native_entry']
        if with_owner:
            assert(game/'sdgo/owner-mods/fixture_owner/fixture_owner.dll').is_file()
            receipt=storage.read_json(game/'sdgo/local-state/client-runtime.json')
            assert receipt['owner_programme']['revision']==1
            assert not(game/'sdgo/local-mods/unknown/mod.ini').exists()
            assert(Path(receipt['backup'])/'sdgo/local-mods/unknown/unknown.dll').read_bytes()==b'personal addon bytes'
            checks+=['owner_programme_installed_before_entry','unknown_addon_preserved_outside_load_path']
        if manager_handoff:
            assert(game/updates.MANAGER).read_bytes()==(build/'manager'/updates.MANAGER).read_bytes()
            jobs=game/'sdgo/local-state/manager/client-updates/runs'
            assert any(storage.read_json(p,{}).get('status')=='installed'for p in jobs.glob('*/result.json'))
            checks+=['approved_manager_replaces_changed_initial_exe','self_update_handoff_resumes_original_launch']
        wait(lambda:not(launch.folder(game)/'ui.json').exists(),15)
        (game/'native-entry.txt').unlink()
        # Native rejection must not hand back to Manager or reach main.
        bad=start(SDGO_LAUNCH_PERMIT='f'*32)
        wait(lambda:close_error(bad),10)
        # A modal error on an inactive desktop cannot always receive a button
        # click. The negative assertion is no native entry or recursive handoff;
        # terminate only this held rejected fixture after observing its dialog.
        bad.kill();assert bad.wait(10)!=0
        assert not(game/'native-entry.txt').exists();checks.append('invalid_permit_does_not_launch_or_recurse')
        # A valid root/receipt/source-bound permit is consumed exactly once.
        token='e'*32;launch.grant(game,token)
        allowed=start(SDGO_LAUNCH_PERMIT=token);assert allowed.wait(10)==0
        assert (game/'native-entry.txt').is_file()and not launch.item(game,token,'permit').exists()
        (game/'native-entry.txt').unlink();replay=start(SDGO_LAUNCH_PERMIT=token)
        wait(lambda:close_error(replay),10);replay.kill();assert replay.wait(10)!=0
        assert not(game/'native-entry.txt').exists();checks.append('native_permit_is_single_use')
        if with_owner:
            revised=b'native startup content revision 2';profile['revision']+=1
            profile['files']['sprs/fixture.spr']={'bytes':len(revised),'sha256':updates.digest(revised)}
            storage.atomic(profile_dir/'content/sprs/fixture.spr',revised)
            storage.atomic(profile_dir/'server-profile.json',storage.json_bytes(profile))
            changed=start();assert changed.wait(40)==0
            wait(lambda:(game/'native-entry.txt').exists(),90)
            assert(game/'sprs/fixture.spr').read_bytes()==revised
            assert storage.read_json(game/'sdgo/local-state/client-runtime.json')['owner_programme']['revision']==1
            (game/'native-entry.txt').unlink();checks.append('next_launch_updates_content_without_changing_programme')
            mod=base/'owner-editor/sdgo/owner-mods/fixture_owner'
            storage.atomic(mod/'fixture_owner.dll',b'owner programme revision 2 '+distribution.OWNER_MARKER)
            storage.atomic(mod/'mod.ini',(mod/'mod.ini').read_bytes().replace(b'version=0.1.0',b'version=0.1.1'))
            programme=owner_programmes.publish({'path':str(base/'owner-editor')},{'source_id':profile['source_id'],'channel':'stable',
                'feeds':['local+'+(base/'owner-programme'/owner_programmes.MANIFEST).as_uri()]},profile['framework'],base/'owner-programme',['fixture_owner'])
            profile.update(revision=profile['revision']+1,owner_programme=programme)
            storage.atomic(profile_dir/'server-profile.json',storage.json_bytes(profile))
            changed=start();assert changed.wait(40)==0
            wait(lambda:(game/'native-entry.txt').exists(),90)
            assert(game/'sdgo/owner-mods/fixture_owner/fixture_owner.dll').read_bytes()==b'owner programme revision 2 '+distribution.OWNER_MARKER
            assert storage.read_json(game/'sdgo/local-state/client-runtime.json')['owner_programme']['revision']==2
            (game/'native-entry.txt').unlink();checks.append('next_launch_updates_locked_owner_programme')
        # Unavailable content never reaches native main; cancellation closes UI.
        previous_content=(game/'sprs/fixture.spr').read_bytes()
        profile['revision']+=1;profile['files']['sprs/fixture.spr']={'bytes':5,'sha256':updates.digest(b'wrong')}
        storage.atomic(profile_dir/'server-profile.json',storage.json_bytes(profile))
        failed=start();assert failed.wait(20)==0
        def error():
            state=api(game,'api/client-updates')
            return next((h for h in state['hosts']if h['result'].get('status')=='error'),None)if state else None
        host=wait(error,60);assert not(game/'native-entry.txt').exists()
        api(game,'api/client-updates',{'action':'cancel','host':host['id']})
        wait(lambda:any(storage.read_json(p,{}).get('status')=='cancelled'for p in launch.folder(game).glob('*.result.json')),15)
        assert (game/'sprs/fixture.spr').read_bytes()==previous_content
        checks+=['failed_content_blocks_game','cancel_preserves_installed_content']
        return {'status':'pass','checks':checks}
    finally:
        try:
            state=api(game,'api/client-updates')
            for host in(state or{}).get('hosts',[]):api(game,'api/client-updates',{'action':'cancel','host':host['id']})
            time.sleep(1)
        except(OSError,ValueError):pass
        try:api(game,'api/exit',{})
        except(OSError,ValueError):pass
        for process in owned:
            if process.poll()is None:
                close_error(process)
                try:process.wait(5)
                except subprocess.TimeoutExpired:process.kill();process.wait(10)
            process.close()
        wait(lambda:not any(row.get('image_path')and Path(row['image_path']).resolve().is_relative_to(game)for row in process_snapshot()),90)


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--build',type=Path,default=ROOT/'build');parser.add_argument('--work',type=Path,required=True)
    parser.add_argument('--sealed-only',action='store_true');parser.add_argument('--owner-programmes',action='store_true');parser.add_argument('--manager-handoff',action='store_true');args=parser.parse_args()
    args.work.mkdir(parents=True,exist_ok=False)
    result=run_sealed(args.work.resolve(),args.build.resolve())if args.sealed_only else run(args.work.resolve(),args.build.resolve(),args.owner_programmes,args.manager_handoff)
    storage.atomic(args.work/'native-result.json',storage.json_bytes(result));print(json.dumps(result))
