"""Coordinate existing owned native actors through gameplay business methods.

No desktop input, account creation, database writes or invented participants.
All supplied runs must already be prepared as quiet friends-only SDK sessions.
"""
from pathlib import Path
import argparse
import concurrent.futures
import json
import sys
import time

import game_test
from game_test_friends import FriendActor,assert_receipt
import game_test_server_report


def campaign(host:Path,peers:list[Path],pilots:list[int],events:list[Path],output:Path,*,timeout=1200):
    if not pilots:raise game_test.GameTestError('declare the existing human roster before launch')
    roster=sorted(game_test_server_report.human_roster(pilots[0],pilots))
    runs=[host.resolve(),*[p.resolve() for p in peers]]
    if len(runs)!=len(roster) or len(set(runs))!=len(runs):raise game_test.GameTestError('one distinct owned run per declared human is required')
    metadata=[game_test.validate_metadata(p) for p in runs]
    first=metadata[0]['plan']
    names=[]
    for m in metadata:
        plan=m['plan']
        if not plan.get('friends_only') or not plan.get('quiet') or plan.get('network_policy')!='direct':
            raise game_test.GameTestError('coordinated native actors require quiet direct friends-only SDK sessions')
        for key in ('target_host','login_port','mode_sequence','expected_ai'):
            if plan[key]!=first[key]:raise game_test.GameTestError('actor mode, endpoint or population differs')
        game_test.verify_hashes(m)
        profile=game_test.read_ini(Path(m['client']['root'])/'sdgo/gametest/private/account.ini')
        names.append(profile['account']['username'].casefold())
    if len(set(names))!=len(names):raise game_test.GameTestError('distinct existing accounts are required')
    if len(game_test.parse_mode_sequence(first['mode_sequence']))!=1:raise game_test.GameTestError('one native battle per coordinated session')
    receipt={'schema':1,'scope':'native_multiplayer_cohort','status':'running','host_run_id':metadata[0]['run_id'],
             'room_title':first['room_title'],'expected_human_pilot_ids':roster,'expected_ai':first['expected_ai'],
             'actors':[{'pilot':pilot,'run_id':m['run_id'],'run_dir':str(p)} for pilot,m,p in zip(pilots,metadata,runs)],'checks':[]}
    def save():game_test.atomic_write(output,json.dumps(receipt,indent=2)+'\n')
    def check(name,ok,**evidence):
        receipt['checks'].append({'name':name,'passed':bool(ok),**evidence});save()
        if not ok:raise game_test.GameTestError('native cohort assertion failed: '+name)
    save() # immutable identities and expectations are declared before any client starts
    actors=[];futures=[];pool=concurrent.futures.ThreadPoolExecutor(max_workers=len(runs))
    try:
        for pilot,run in zip(pilots,runs):
            future=pool.submit(game_test.run_prepared,run,timeout,60)
            actor=FriendActor(run,future);actors.append(actor);futures.append(future)
            state=actor.wait(lambda s:s.get('run',{}).get('ready')=='1',300)
            check('real authentication and native onboarding',int(state['state']['pilot'])==pilot,pilot=pilot)
        created=assert_receipt(actors[0].command('room_create',timeout=180))
        room=int(created['room']['actual_room_id'])
        check('host created a real native room',room>0 and created['room']['local_is_host']=='1')
        for pilot,actor in zip(pilots[1:],actors[1:]):
            joined=assert_receipt(actor.command('room_join',target_id=pilots[0],body=first['room_title'],human_count=len(pilots),timeout=180))
            p=joined['pvp']
            check('guest original join and actual room',p['join_original']=='1' and int(p['room'])==room and int(p['host'])==pilots[0],pilot=pilot)
            ready=assert_receipt(actor.command('room_ready',target_id=pilots[0],human_count=len(pilots),timeout=90))['pvp']
            check('guest original Ready acknowledgement',all(ready[k]=='1' for k in ('ready_reply','ready_original','ready_model')),pilot=pilot)
        state=actors[0].wait(lambda s:int(s.get('room',{}).get('player_count','0'))==len(pilots),60)
        check('all real humans occupy the native waiting room',int(state['room']['player_count'])==len(pilots))
        assert_receipt(actors[0].command('room_start',target_id=pilots[0],human_count=len(pilots),timeout=180))
        for pilot,actor in zip(pilots,actors):
            s=actor.wait(lambda s:s.get('pvp',{}).get('battle_seen')=='1',120)['pvp']
            check('real GAME_MAIN and correct AI authority',int(s['bots'])==first['expected_ai'] and
                  int(s['local_bots'] if pilot==pilots[0] else s['remote_bots'])==first['expected_ai'],pilot=pilot)
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(actors)) as waiting:
            ending=[waiting.submit(actor.command,'battle_finish',target_id=pilots[0],human_count=len(pilots),timeout=900) for actor in actors]
            for pilot,future in zip(pilots,ending):
                state=assert_receipt(future.result(timeout=930));p=state['pvp']
                check('native battle and settlement',p['settled']=='1' and int(p['end_opcode'])==0x0871 and
                      int(p['host']) in roster and int(p['movement_events'])>0,pilot=pilot)
        # Guests leave first so the original human host is still available.
        for actor in [*actors[1:],actors[0]]:assert_receipt(actor.command('room_leave',timeout=90))
        for actor in actors:assert_receipt(actor.command('finish',timeout=30))
        reports=[future.result(timeout=90) for future in futures]
        check('all actor launch/native reports pass',all(r['status']=='pass' for r in reports))
        server=game_test_server_report.evaluate(metadata[0],events,pilots[0],expected_humans=pilots)
        game_test.atomic_write(output.with_name(output.stem+'-server.json'),json.dumps(server,indent=2)+'\n')
        check('independent server lifecycle and exact human/AI teams',server['status']=='pass',errors=server['errors'])
        receipt['status']='pass';save();return receipt
    except BaseException as error:
        receipt['status']='fail';receipt['error']=str(error);save()
        for actor in actors:
            cfg=game_test.ini_parser();cfg['run']={'id':actor.metadata['run_id']}
            game_test.write_ini(actor.directory/'friend-abort.ini',cfg)
        for future in futures:
            try:future.result(timeout=90)
            except BaseException:pass
        raise
    finally:pool.shutdown(wait=True)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host-run',type=Path,required=True);p.add_argument('--peer-run',type=Path,action='append',required=True)
    p.add_argument('--human-pilots',required=True);p.add_argument('--server-events',type=Path,action='append',required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();pilots=[int(v) for v in a.human_pilots.split(',')]
    result=campaign(a.host_run,a.peer_run,pilots,a.server_events,a.output)
    print(json.dumps({'status':result['status'],'checks':len(result['checks'])}))

if __name__=='__main__':main()
