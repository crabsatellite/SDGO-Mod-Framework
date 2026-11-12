"""Launch the real self-contained EXE outside the checkout, without real game data."""
import argparse
import configparser
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
from frozen_process import stop_manager


def check_unit_import_resources(executable):
    """Inspect the shipped inputs, not a caller-patched extraction directory."""
    from PyInstaller.archive.readers import CArchiveReader
    archive=CArchiveReader(str(executable.resolve()))
    def resource(name):
        member=next((value for value in archive.toc if value.replace('\\','/')==name),None)
        assert member is not None, 'Frozen Manager lacks '+name
        return archive.extract(member)
    script=resource('tools/install_unit_catalog.ps1').decode('utf-8-sig')
    allowed=script.split('$AllowedModuleFiles =',1)[1].split('$AllowedResourceRoots',1)[0]
    for leaf in ('blueprint-availability.json','blueprint-availability.bin'):
        assert 'sdgo\\mods\\unit_catalog\\'+leaf in allowed, 'Frozen installer rejects '+leaf
    template=configparser.ConfigParser(interpolation=None)
    template.read_string(resource('mods/unit_catalog/mod.ini').decode('utf-8-sig'))
    assert template['framework.dependencies']['schema']=='1'
    names=set(template['framework.configuration']['files'].split(','))
    assert {'mod.ini','catalog.bin','definitions.json','blueprint-availability.json','blueprint-availability.bin'}<=names
    assert 'resources.zpk' not in names and 'unit_catalog.dll' not in names


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--exe',type=Path,required=True)
    parser.add_argument('--version',help='Expected embedded version for a versioned release')
    args=parser.parse_args()
    check_unit_import_resources(args.exe)
    repo=Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='sdgo-manager-exe-') as temporary:
        root=Path(temporary);state=root/'state';state.mkdir();client=root/'client';client.mkdir();(client/'GOnline.exe').write_bytes(b'fixture')
        for name in ('findwindow_fix','mission_ai'):
            folder=client/'sdgo/mods'/name;folder.mkdir(parents=True);shutil.copyfile(repo/'mods'/name/'mod.ini',folder/'mod.ini')
        (state/'manager.json').write_text(json.dumps({'schema_version':1,'hosts':[{'id':'client','name':'test client','path':str(client),'role':'client'}],
             'modules':{'shop':{'workspace':str(root/'shop'),'demo':'1'},'rewards':{'workspace':str(root/'rewards')},
                        'calendar_gifts':{'workspace':str(root/'calendar')}}}),encoding='utf-8')
        environment=dict(os.environ);environment.pop('PYTHONPATH',None)
        process=subprocess.Popen([str(args.exe.resolve()),'--workspace',str(state),'--no-browser'],cwd=root,env=environment)
        base=token=None
        try:
            for _ in range(200):
                if process.poll() is not None:raise RuntimeError('Frozen Manager exited before publishing its session')
                session=state/'manager-session.json'
                if session.exists():
                    try:
                        base=json.loads(session.read_text(encoding='utf-8'))['url'].rstrip('/')
                        break
                    except (OSError, json.JSONDecodeError):
                        # Atomic publication can briefly hold a Windows sharing
                        # lock. Existence alone does not mean it is readable yet.
                        pass
                time.sleep(.1)
            if not base:raise RuntimeError('Frozen Manager did not start')
            def call(path,body=None,headers=None):
                values={'Content-Type':'application/json','Origin':base,'X-ModManager-Token':token or ''};values.update(headers or {})
                request=urllib.request.Request(base+path,None if body is None else json.dumps(body).encode(),values)
                with urllib.request.urlopen(request,timeout=30) as response:return json.load(response)
            info=call('/api/state');token=info['session_token']
            if args.version:
                update=call('/api/updates')
                assert update['current_version']==args.version,(update['current_version'],args.version)
                # The workspace chooses its update channel independently of
                # the executable's version. Fresh installations use Stable.
                assert update['channel']=='stable'
            with urllib.request.urlopen(base+'/',timeout=10) as response:html=response.read().decode('utf-8')
            assert '客户端更新与服主管理' in html and '玩家可以在此更新完整客户端' in html and 'owner-notice' in html
            assert 'href="/brand.svg"' in html
            with urllib.request.urlopen(base+'/favicon.ico',timeout=10) as response:
                assert response.read()==(repo/'manager/static/brand.ico').read_bytes()
            assert info['mode']=='standard'
            assert call('/api/safety')['changes']==[]
            runtime=call('/api/client-updates');assert runtime['hosts'][0]['id']=='client'
            assert runtime['hosts'][0]['source'] is None
            call('/api/client-updates',{'action':'check','host':'client'})
            for _ in range(100):
                result=call('/api/client-updates')['hosts'][0]['result']
                if result.get('status')=='error':break
                time.sleep(.05)
            assert result['status']=='error' and '未配置完整运行更新源' in result['message']
            assert [mod['id'] for mod in info['hosts'][0]['mods']]==['mission_ai']
            assert [mod['id'] for mod in info['hosts'][0]['archive']]==['findwindow_fix']
            personal=client/'sdgo/mods/personal_fixture';personal.mkdir(parents=True)
            (personal/'mod.ini').write_text('[mod]\nid=personal_fixture\ndistribution=local_only\ncategory=gameplay\nfeature=personal_fixture\n[settings]\nstrength=20\n',encoding='utf-8')
            (personal/'manager.json').write_text(json.dumps({'schema_version':1,'id':'personal_fixture','distribution':'local_only',
                'title':'Local configuration fixture','category':'gameplay','hosts':['client'],'settings':[
                    {'id':'strength','label':'Strength','type':'integer','minimum':0,'maximum':40,'default':20,
                     'audience':'user','file':'mod.ini','section':'settings','key':'strength'}]}),encoding='utf-8')
            local=next(mod for mod in call('/api/state')['hosts'][0]['mods'] if mod['id']=='personal_fixture')
            assert local['distribution']=='local_only' and not local['redistributable']
            settings=call('/api/settings?host=client&mod=personal_fixture')
            updated=call('/api/settings',{'host':'client','mod':'personal_fixture','values':{'strength':25},'revisions':settings['revisions']})
            assert updated['saved'] and updated['values']['strength']==25
            assert {p['id'] for p in info['panels']} >= {'shop','rewards','collection_capsule','calendar_gifts'}
            calendar=call('/api/providers/calendar_gifts/api/calendar-gifts')
            assert not calendar['policy']['enabled'] and not calendar['service']['running']
            assert len(calendar['policy']['events'])==14 and len(calendar['policy']['birthday']['rewards']['grades'])==12
            assert any(e['value']=='christmas' and e['month_day']=='12-25' for e in calendar['festivals'])
            people={'schema':1,'birthdays':[{'account_id':73491,'month_day':'11-03','enabled':True}],'groups':{}}
            call('/api/providers/calendar_gifts/api/calendar-gifts/private',{'value':people,'revision':calendar['private_revision']})
            rules=call('/api/providers/calendar_gifts/api/calendar-gifts/export')
            assert '73491' not in json.dumps(rules) and 'birthdays' not in rules
            policy=calendar['policy'];policy['catchup_days']=3
            saved=call('/api/providers/calendar_gifts/api/calendar-gifts/policy',{'value':policy,'revision':calendar['policy_revision']})
            assert saved['policy']['catchup_days']==3
            with urllib.request.urlopen(base+'/panels/calendar_gifts.js',timeout=10) as response:
                assert '导入旧生日发放记录' in response.read().decode('utf-8')
            collection=call('/api/providers/collection_capsule/api/state')
            assert collection['price_gp']==100000 and not collection['generated']
            assert collection['rules']['draw_limit']==15 and collection['rules']['limit_period']=='week'
            custom_rules={**collection['rules'],'price_gp':76543,'draw_limit':4,'limit_period':'day'}
            collection=call('/api/providers/collection_capsule/api/rules',{'value':custom_rules,'revision':collection['revision']})
            assert collection['rules']==custom_rules and collection['price_gp']==76543
            context=call('/api/ai-context',{'provider':'shop','kind':'bug'})
            assert 'SDGO-Mod-Framework' in context['text'] and 'AI models:' in context['text']
            catalog=call('/api/providers/shop/api/shop/catalog');assert catalog['demo'] and catalog['items']
            with urllib.request.urlopen(base+'/panels/shop.js',timeout=10) as response:
                panel=response.read().decode('utf-8')
            assert '新商品中文名称' in panel and '导出玩家更新包' in panel and 'hasChinese' in panel
            try:
                call('/api/providers/shop/api/shop/create',{'category_id':catalog['items'][0]['Id'],'name':'English only'},{'X-Shop-Revision':catalog['revision']})
                raise AssertionError('Frozen Manager accepted an English-only shop title')
            except urllib.error.HTTPError as error:
                assert error.code==400
            created=call('/api/providers/shop/api/shop/create',{'category_id':catalog['items'][0]['Id'],'name':'冻结包测试商品'},{'X-Shop-Revision':catalog['revision']})
            assert len(created['catalog']['products'])==1
            rewards=call('/api/providers/rewards/api/rewards')
            assert not rewards['progression']['config']['enabled']
            rewards=call('/api/providers/rewards/api/rewards/solo-preset',{}, {'X-Reward-Revision':rewards['revision']})
            assert rewards['policy']['progression']['daily_mb']==400 and rewards['policy']['progression']['enabled']
            assert (root/'rewards/runtime/policies'/(rewards['revision']+'.json')).is_file()
            assert not rewards['gathering']['config']['enabled'] and not rewards['gathering']['config']['rules']
            social={'human_multipliers':['1','1.1']+['1.5']*10,'gathering':{
                **rewards['gathering']['config'],'enabled':True,'rules':[{
                    'id':'weekly_meetup','title':'Weekly gathering','enabled':True,'minimum_humans':3,
                    'period':'week','modes':['pvp','pve'],'gb':12345,'mb':678}]}}
            rewards=call('/api/providers/rewards/api/rewards/social-rules',{'rules':social},{'X-Reward-Revision':rewards['revision']})
            assert rewards['human_multipliers']==social['human_multipliers']
            assert rewards['gathering']['config']==social['gathering']
            assert rewards['policy']['progression']['daily_mb']==400
            with urllib.request.urlopen(base+'/panels/reward_social.js',timeout=10) as response:
                assert '保存人数与聚会规则' in response.read().decode('utf-8')
            stop_manager(process,state/'manager-session.json',lambda:call('/api/exit',{}))
            assert process.returncode==0
            print('PASS frozen EXE: standalone launch, local-only module discovery/configuration, private calendar gift policies/export, daily mode, fix archive, collection provider, AI/PR guidance, C# shop CRUD and solo MB preset')
        finally:
            if process.poll() is None:
                if base and token:
                    try:call('/api/exit',{})
                    except Exception:pass
                try:process.wait(timeout=10)
                except subprocess.TimeoutExpired:process.terminate();process.wait(timeout=10)


if __name__=='__main__':main()
