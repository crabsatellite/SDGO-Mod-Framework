"""Verify actual frozen Manager handoff against an existing isolated native SQL.

No gameplay is invented and no wallet jobs/accounts/balances are created. The
SQL scope and receipt count are read before and after two real Manager starts.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.parse
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from manager.database import connect
from manager.update_restart import finish
from manager.updates import MANAGER
from manager.storage import ini_values


def run(args):
    test=args.test_root.resolve();ini=args.server_ini.resolve();work=args.work.resolve()
    if not (test/'TEST-ONLY.txt').is_file() or not ini.is_relative_to(test):
        raise RuntimeError('Use an existing marked isolated test copy and its own native INI')
    if work== (ROOT/'build').resolve() or not work.is_relative_to((ROOT/'build').resolve()):
        raise RuntimeError('Keep lifecycle output in this checkout owned build directory')
    if work.exists():raise RuntimeError('Select a new owned output directory')
    connection=connect(ini,'DB StoreBilling',autocommit=True)
    try:
        cursor=connection.cursor()
        files=[Path(r[0]).resolve() for r in cursor.execute('SELECT physical_name FROM sys.database_files')]
        if not files or any(not p.is_relative_to(test) for p in files):
            raise RuntimeError('The native SQL files are outside the isolated test root')
        scope=str(cursor.execute('SELECT ScopeId FROM dbo.SdgoBattleRewardWalletMeta WHERE Singleton=1').fetchone()[0]).lower()
        initial_count=cursor.execute('SELECT COUNT(*) FROM dbo.SdgoBattleRewardMbReceipt').fetchone()[0]
        work.mkdir(parents=True);workspace=work/'workspace';workspace.mkdir()
        target=work/MANAGER;shutil.copyfile(args.exe.resolve(),target)
        settings={'schema_version':1,'hosts':[],'modules':{'rewards':{'workspace':str(work/'rewards'),
                  'runtime':str(work/'rewards/runtime'),'server_ini':str(ini)}},
                  'services':{'rewards.wallet':{'enabled':True},'manager.updates':{'enabled':False},
                              'calendar.gifts':{'enabled':False}}}
        (workspace/'manager.json').write_text(json.dumps(settings),encoding='utf-8')
        processes=[];base='';token=''
        def call(path,body=None):
            request=urllib.request.Request(base+path,None if body is None else json.dumps(body).encode(),
                {'Content-Type':'application/json','Origin':base,'X-ModManager-Token':token})
            with urllib.request.urlopen(request,timeout=15) as response:return json.load(response)
        def start(previous=0):
            nonlocal base,token
            process=subprocess.Popen([str(target),'--workspace',str(workspace),'--no-browser'],cwd=work,
                creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0),stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            processes.append(process);deadline=time.monotonic()+40
            while time.monotonic()<deadline:
                try:
                    session=json.loads((workspace/'manager-session.json').read_text())
                    if session['pid']==previous:time.sleep(.05);continue
                    parsed=urllib.parse.urlsplit(session['url']);base=parsed.scheme+'://'+parsed.netloc;token=''
                    state=call('/api/state');token=state['session_token']
                    status=call('/api/services/rewards.wallet')
                    if status['healthy']:return session
                except (OSError,ValueError):pass
                time.sleep(.05)
            raise RuntimeError('The real Manager did not restore a healthy native wallet')
        try:
            first=start();runtime=work/'rewards/runtime';binding=(runtime/'wallet-binding.ini').read_bytes()
            if ini_values(binding).get('binding','ScopeId')!=scope:raise RuntimeError('Frozen Manager bound another native ledger')
            source=workspace/'updates/releases/verified-fixture'/MANAGER;source.parent.mkdir(parents=True)
            shutil.copyfile(target,source);digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
            job=workspace/'updates/restart.json'
            value={'schema_version':1,'source':str(source),'target':str(target),'workspace':str(workspace),
                   'parent_pid':first['pid'],'source_sha256':digest(source),'previous_sha256':digest(target),
                   'backup':str(workspace/'updates/backups/manager/fixture'/MANAGER)}
            job.write_text(json.dumps(value),encoding='utf-8')
            call('/api/exit',{});finish(job,launch=False)
            handoff=ini_values((runtime/'wallet-status.ini').read_bytes())
            if handoff.get('wallet','Healthy')!='1':raise RuntimeError('A verified restart published a false stopped heartbeat')
            # The timestamp is the actual last observation; no future lease is forged.
            original_heartbeat=handoff.getint('wallet','HeartbeatUnix')
            second=start(first['pid'])
            if (runtime/'wallet-binding.ini').read_bytes()!=binding:raise RuntimeError('Native wallet identity changed during handoff')
            if not call('/api/services/rewards.wallet')['healthy']:raise RuntimeError('Successor wallet is not healthy')
            call('/api/services/rewards.wallet',{'action':'stop'})
            if ini_values((runtime/'wallet-status.ini').read_bytes()).get('wallet','Healthy')!='0':
                raise RuntimeError('An explicit service stop did not close its readiness lease')
            if any(list((runtime/name).glob('*.ini')) for name in ['outbox','receipts','jobs-completed']):
                raise RuntimeError('Lifecycle-only checks unexpectedly created a native currency job')
            other_activity=cursor.execute('SELECT COUNT(*) FROM dbo.SdgoBattleRewardMbReceipt').fetchone()[0]-initial_count
            result={'status':'pass','actual_frozen_manager':True,'native_test_sql_verified':True,
                    'planned_restart_kept_binding':True,'successor_restored':True,'explicit_stop_closed_lease':True,
                    'owned_currency_jobs':0,'other_test_receipt_delta':other_activity,
                    'last_true_heartbeat_age_seconds':max(0,int(time.time())-original_heartbeat)}
            (work/'result.json').write_text(json.dumps(result,indent=2)+'\n')
            print(json.dumps(result),flush=True)
        finally:
            try:call('/api/exit',{})
            except OSError:pass
            for process in processes:
                try:process.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    process.terminate();process.wait(timeout=5)
    finally:connection.close()


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--exe',type=Path,required=True)
    p.add_argument('--test-root',type=Path,required=True);p.add_argument('--server-ini',type=Path,required=True)
    p.add_argument('--work',type=Path,required=True);run(p.parse_args())


if __name__=='__main__':main()
