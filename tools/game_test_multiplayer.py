"""Coordinate existing owned native actors through gameplay business methods.

No desktop input, account creation, database writes or invented participants.
All supplied runs must already be prepared as quiet friends-only SDK sessions.
"""
from pathlib import Path
import argparse
import concurrent.futures
import json
import time
from contextlib import contextmanager

import game_test
from game_test_friends import FriendActor,assert_receipt
import game_test_server_report


def owned_actor_process(actor):
    """Resolve the authenticated capsule's exact native image before a delay."""
    import psutil
    rows=actor.native_events()
    pilots={int(row['pid']) for row in rows if row.get('pid')}
    if len(pilots)!=1:raise game_test.GameTestError('delayed actor must have one observed native process')
    expected=Path(actor.metadata['client']['game']).resolve()
    process=psutil.Process(next(iter(pilots)))
    if Path(process.exe()).resolve()!=expected:
        raise game_test.GameTestError('refuse to delay a process outside its owned native capsule')
    return process,expected,process.create_time()


@contextmanager
def pause_owned_actor(actor):
    """Suspend scheduling, never game state, and always release the owned PID."""
    import psutil
    process,expected,created=owned_actor_process(actor)
    process.suspend()
    proof={'pid':process.pid,'method':'owned_native_process_suspend','started':time.monotonic()}
    try:yield proof
    finally:
        try:
            current=psutil.Process(process.pid)
            if current.create_time()!=created or Path(current.exe()).resolve()!=expected:
                raise game_test.GameTestError('delayed native process identity changed; foreign process preserved')
            current.resume();proof['resumed']=True
        except psutil.NoSuchProcess:proof['resumed']=False
        proof['elapsed_seconds']=time.monotonic()-proof['started']


def campaign(host:Path,peers:list[Path],pilots:list[int],events:list[Path],output:Path,*,timeout=1200,rounds=1,
             slow_peer_pilot=0,slow_peer_seconds=0):
    if type(rounds) is not int or rounds not in (1,2):
        raise game_test.GameTestError('coordinated multiplayer rounds must be 1 or 2')
    if not pilots:raise game_test.GameTestError('declare the existing human roster before launch')
    roster=sorted(game_test_server_report.human_roster(pilots[0],pilots))
    if len(roster)<2:raise game_test.GameTestError('coordinated multiplayer requires at least two real humans')
    if (bool(slow_peer_pilot)!=bool(slow_peer_seconds) or
            (slow_peer_pilot and (slow_peer_pilot not in pilots[1:] or type(slow_peer_seconds) is not int or not 1<=slow_peer_seconds<=25))):
        raise game_test.GameTestError('a delayed native guest must be a declared peer with a 1..25 second bounded pause')
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
    if not 1<=first['expected_ai']<=7 or len(roster)+first['expected_ai']>8:
        raise game_test.GameTestError('the declared humans and AI exceed the supported native room capacity')
    receipt={'schema':1,'scope':'native_multiplayer_cohort','status':'running','host_run_id':metadata[0]['run_id'],
             'room_title':first['room_title'],'expected_human_pilot_ids':roster,'expected_ai':first['expected_ai'],
             'expected_rounds':rounds,
             'slow_peer':{'pilot':slow_peer_pilot,'seconds':slow_peer_seconds,'round':1} if slow_peer_pilot else None,
             'actors':[{'pilot':pilot,'run_id':m['run_id'],'run_dir':str(p)} for pilot,m,p in zip(pilots,metadata,runs)],'checks':[]}
    def save():game_test.atomic_write(output,json.dumps(receipt,indent=2)+'\n')
    def check(name,ok,**evidence):
        receipt['checks'].append({'name':name,'passed':bool(ok),**evidence});save()
        if not ok:raise game_test.GameTestError('native cohort assertion failed: '+name)
    save() # immutable identities and expectations are declared before any client starts
    actors=[];futures=[];pool=concurrent.futures.ThreadPoolExecutor(max_workers=len(runs))
    try:
        def start_actor(pilot,run):
            future=pool.submit(game_test.run_prepared,run,timeout,60)
            actor=FriendActor(run,future);actors.append(actor);futures.append(future)
            state=actor.wait(lambda s:s.get('run',{}).get('ready')=='1',300)
            check('real authentication and native onboarding',int(state['state']['pilot'])==pilot,pilot=pilot)
            return actor
        start_actor(pilots[0],runs[0])
        created=assert_receipt(actors[0].command('room_create',timeout=180))
        room=int(created['room']['actual_room_id'])
        channel=int(created['pvp']['channel_id']);group=int(created['pvp']['channel_group'])
        check('host created a real native room',room>0 and created['room']['local_is_host']=='1')
        for pilot,run in zip(pilots[1:],runs[1:]):
            actor=start_actor(pilot,run)
            joined=assert_receipt(actor.command('room_join',target_id=pilots[0],body=first['room_title'],human_count=len(pilots),channel_id=channel,channel_group=group,timeout=180))
            p=joined['pvp']
            check('guest original join and actual room',p['join_original']=='1' and int(p['room'])==room and int(p['host'])==pilots[0],pilot=pilot)

        def clean_waiting_room(state):
            p=state.get('pvp',{})
            return (int(p.get('room','0'))==room and int(p.get('host','0'))==pilots[0] and
                    int(p.get('humans','0'))==len(pilots) and
                    int(p.get('room_occupied','0'))==len(pilots) and int(p.get('room_ai','-1'))==0)

        previous_start=[0]*len(actors);previous_end=[0]*len(actors)
        for round_number in range(1,rounds+1):
            for pilot,actor in zip(pilots,actors):
                state=actor.wait(clean_waiting_room,60)
                check('same clean human-owned waiting room',clean_waiting_room(state),
                      round=round_number,pilot=pilot,room=room,host=pilots[0])
            for pilot,actor in zip(pilots[1:],actors[1:]):
                ready=assert_receipt(actor.command('room_ready',target_id=pilots[0],human_count=len(pilots),timeout=90))['pvp']
                check('guest original Ready acknowledgement',
                      all(ready[k]=='1' for k in ('ready_reply','ready_original','ready_model')) and
                      int(ready['round'])==round_number,round=round_number,pilot=pilot)
            if round_number==1 and slow_peer_pilot:
                delayed=actors[pilots.index(slow_peer_pilot)]
                with concurrent.futures.ThreadPoolExecutor(max_workers=1) as starting:
                    with pause_owned_actor(delayed) as delay:
                        future=starting.submit(actors[0].command,'room_start',target_id=pilots[0],human_count=len(pilots),timeout=180)
                        deadline=time.monotonic()+slow_peer_seconds
                        while time.monotonic()<deadline:
                            if future.done():assert_receipt(future.result())
                            time.sleep(min(.1,max(0,deadline-time.monotonic())))
                    receipt['slow_peer']['observation']=delay;save()
                    check('delayed guest resumed with the same native identity',delay['resumed'] and
                          delay['elapsed_seconds']>=slow_peer_seconds,round=round_number,pilot=slow_peer_pilot)
                    started=assert_receipt(future.result(timeout=190))['pvp']
            else:
                started=assert_receipt(actors[0].command(
                    'room_start',target_id=pilots[0],human_count=len(pilots),timeout=180))['pvp']
            check('human host started the same native room',
                  int(started['room'])==room and int(started['host'])==pilots[0] and
                  int(started['round'])==round_number,round=round_number,pilot=pilots[0])
            for index,(pilot,actor) in enumerate(zip(pilots,actors)):
                state=actor.wait(
                    lambda s,index=index:int(s.get('pvp',{}).get('start_sequence','0'))>previous_start[index] and
                                         s.get('pvp',{}).get('battle_seen')=='1',120)
                p=state['pvp'];start_sequence=int(p['start_sequence'])
                check('fresh native battle generation and correct AI authority',
                      start_sequence>previous_start[index] and int(p['room'])==room and int(p['host'])==pilots[0] and
                      int(p['round'])==round_number and int(p['bots'])==first['expected_ai'] and
                      int(p['local_bots'] if pilot==pilots[0] else p['remote_bots'])==first['expected_ai'],
                      round=round_number,pilot=pilot,start_sequence=start_sequence)
                previous_start[index]=start_sequence
            with concurrent.futures.ThreadPoolExecutor(max_workers=len(actors)) as waiting:
                ending=[waiting.submit(actor.command,'battle_finish',target_id=pilots[0],
                                       human_count=len(pilots),timeout=900) for actor in actors]
                for index,(pilot,future) in enumerate(zip(pilots,ending)):
                    state=assert_receipt(future.result(timeout=930));p=state['pvp'];end_sequence=int(p['end_sequence'])
                    check('native battle and settlement',
                          p['settled']=='1' and int(p['end_opcode'])==0x0871 and
                          int(p['room'])==room and int(p['host'])==pilots[0] and
                          int(p['round'])==round_number and end_sequence>previous_end[index] and
                          int(p['movement_events'])>0,round=round_number,pilot=pilot,end_sequence=end_sequence)
                    previous_end[index]=end_sequence
            for pilot,actor in zip(pilots,actors):
                state=actor.wait(clean_waiting_room,60)
                check('settlement removed every AI room slot',clean_waiting_room(state),
                      round=round_number,pilot=pilot,room=room)
        # Guests leave first so the original human host is still available.
        for actor in [*actors[1:],actors[0]]:
            assert_receipt(actor.command('room_leave',channel_id=channel,room_id=room,timeout=90))
        for actor in actors:assert_receipt(actor.command('finish',timeout=30))
        reports=[future.result(timeout=90) for future in futures]
        check('all actor launch/native reports pass',all(r['status']=='pass' for r in reports))
        server=game_test_server_report.evaluate(
            metadata[0],events,pilots[0],expected_humans=pilots,expected_rounds=rounds)
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
    p.add_argument('--rounds',type=int,choices=(1,2),default=1)
    p.add_argument('--slow-peer-pilot',type=int,default=0)
    p.add_argument('--slow-peer-seconds',type=int,default=0)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();pilots=[int(v) for v in a.human_pilots.split(',')]
    result=campaign(a.host_run,a.peer_run,pilots,a.server_events,a.output,rounds=a.rounds,
                    slow_peer_pilot=a.slow_peer_pilot,slow_peer_seconds=a.slow_peer_seconds)
    print(json.dumps({'status':result['status'],'checks':len(result['checks'])}))

if __name__=='__main__':main()
