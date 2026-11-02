"""Coordinate real native friend sessions in owned GameTest clients.

Commands are scoped to an authenticated --friends-only run and are executed by
the GameTest Mod on the native main thread. This tool never edits game state,
friendship tables, server replies or production accounts.
"""
from __future__ import annotations
import argparse
import concurrent.futures
import json
from pathlib import Path
import time
import uuid
import game_test


class FriendActor:
    def __init__(self,run_dir:Path):
        self.run_dir=run_dir.resolve()
        self.metadata=game_test.validate_metadata(self.run_dir)
        if not self.metadata['plan'].get('friends_only'):
            raise game_test.GameTestError('friend commands require a friends-only native session')
        self.directory=Path(self.metadata['client']['root'])/'sdgo/gametest'
        self.sequence=0

    def state(self):
        path=self.directory/'friend-result.ini'
        if not path.exists():return None
        try:
            state=game_test.read_ini(path)
            if state.get('run','id',fallback='')!=self.metadata['run_id']:
                raise game_test.GameTestError('friend receipt belongs to another native session')
            return {section:dict(state[section]) for section in state.sections()}
        except (OSError,ValueError):return None

    def wait(self,predicate,timeout=90):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            state=self.state()
            if state and predicate(state):return state
            events=Path(self.metadata['client']['events'])
            if events.exists() and game_test.has_run_end(events):
                raise game_test.GameTestError('native actor ended before its friend condition')
            time.sleep(.15)
        raise game_test.GameTestError('native friend observation timed out')

    def command(self,operation:str,*,target_id=0,target_name='',body='',page=1,decision=0,timeout=90,**fields):
        self.sequence+=1
        cfg=game_test.ini_parser()
        cfg['run']={'id':self.metadata['run_id']}
        cfg['command']={'sequence':str(self.sequence),'operation':operation,'target_id':str(target_id),
                        'target_name':target_name,'body':body,'page':str(page),'decision':str(decision),
                        **{key:str(value) for key,value in fields.items()}}
        for value in cfg['command'].values():
            if any(c in value for c in '\r\n\0'):raise game_test.GameTestError('friend command contains an invalid delimiter')
        game_test.write_ini(self.directory/'friend-command.ini',cfg)
        return self.wait(lambda s:int(s.get('command',{}).get('sequence','0'))==self.sequence and
                         s.get('command',{}).get('done')=='1',timeout)


def prepare_actor(template,stage,run_dir,profile,host,*,baseline=None):
    return game_test.prepare(template,stage,run_dir,target_host=host,login_port=5001,
                             account_profile=profile,create_test_account=not profile.exists(),
                             friends_only=True,asset_baseline=baseline)


def promote_profile(run_dir:Path,profile:Path):
    """Promote only this actor's passed native onboarding identity."""
    metadata=game_test.validate_metadata(run_dir)
    native_path=Path(metadata['client']['events'])
    rows=[json.loads(line) for line in native_path.read_text(encoding='utf-8').splitlines() if line]
    if not any(row.get('event')=='case_end' and row.get('case')=='flow.onboarding' and
               row.get('status')=='pass' and row.get('run_id')==metadata['run_id'] for row in rows):
        raise game_test.GameTestError('cannot promote an identity without passed native onboarding')
    saved=game_test.read_ini(profile)
    copied=game_test.read_ini(Path(metadata['client']['root'])/'sdgo/gametest/private/account.ini')
    if dict(saved['account'])!=dict(copied['account']):raise game_test.GameTestError('profile identity mismatch')
    if not saved.has_section('registration'):saved.add_section('registration')
    saved['registration']['state']='existing_account'
    game_test.write_ini(profile,saved)


def friendship_rows(state):
    return [row for section,row in state.items() if section.startswith('friend') and section[6:].isdigit()]


def find_friend(state,pilot):
    return next((row for row in friendship_rows(state) if int(row['id'])==int(pilot)),None)


def assert_receipt(state,*,sent=None,error=None):
    command=state.get('command',{})
    if command.get('done')!='1':raise game_test.GameTestError('native command has not finished')
    if int(command.get('result','0')) in (0x109,0x10B,0x10C):
        raise game_test.GameTestError('native friend command faulted, malformed its cache or is unsupported')
    if sent is not None and command.get('request_sent')!=str(int(sent)):
        raise game_test.GameTestError('native friend transport observation differs from the expected action')
    if error is not None and int(command.get('reply_status','0'))!=error:
        raise game_test.GameTestError('native friend server result differs from the expected status')
    return state


class Campaign:
    def __init__(self,template:Path,stage:Path,root:Path,host:str,baseline:Path):
        self.template=template;self.stage=stage;self.root=root.resolve();self.host=host;self.baseline=baseline
        self.pool=concurrent.futures.ThreadPoolExecutor(max_workers=3)
        self.running={};self.counts={};self.receipt={'schema':1,'status':'running','checks':[],'actors':[]}
        self.root.mkdir(parents=True,exist_ok=True)
    def save(self):
        game_test.atomic_write(self.root/'campaign.json',json.dumps(self.receipt,ensure_ascii=True,indent=2))
    def check(self,name,condition,**evidence):
        row={'name':name,'passed':bool(condition),**evidence};self.receipt['checks'].append(row);self.save()
        if not condition:raise game_test.GameTestError('friend campaign failed: '+name)
    def start(self,label):
        number=self.counts.get(label,0)+1;self.counts[label]=number
        run=self.root/(label+'-'+str(number).zfill(2));profile=self.root/'profiles'/(label+'.ini')
        metadata=prepare_actor(self.template,self.stage,run,profile,self.host,baseline=self.baseline)
        future=self.pool.submit(game_test.run_prepared,run,3900,60)
        actor=FriendActor(run);self.running[label]=(actor,future,profile)
        state=actor.wait(lambda s:s.get('run',{}).get('ready')=='1',360)
        promote_profile(run,profile)
        account=game_test.read_ini(profile)['account']['username']
        self.receipt['actors'].append({'label':label,'run_dir':str(run),'run_id':metadata['run_id']})
        self.save()
        return actor,int(state['state']['pilot']),account
    def stop(self,label):
        actor,future,_=self.running.pop(label)
        actor.command('finish',timeout=30)
        report=future.result(timeout=90)
        self.check(label+' clean native completion',report['status']=='pass',run_dir=str(actor.run_dir))
    def state_friend(self,actor,pilot,presence=None,timeout=90):
        def found(state):
            row=find_friend(state,pilot)
            return bool(row) and row.get('name') and row.get('relation')=='7' and (presence is None or int(row['presence'])==presence)
        return actor.wait(found,timeout)
    def accept_pair(self,a,a_id,a_name,b,b_id,b_name):
        state=a.command('add',target_name=b_name,target_id=b_id)
        assert_receipt(state,sent=True)
        b.wait(lambda s:s.get('pending',{}).get('available')=='1' and int(s['pending']['sender_id'])==a_id)
        assert_receipt(b.command('accept',target_name=a_name,target_id=a_id),sent=True)
        self.state_friend(a,b_id);self.state_friend(b,a_id)
    def smoke(self):
        a,aid,aname=self.start('a');b,bid,bname=self.start('b')
        a.command('snapshot');b.command('snapshot')
        assert_receipt(a.command('add',target_name=bname,target_id=bid),sent=True)
        b.wait(lambda s:s.get('pending',{}).get('available')=='1' and int(s['pending']['sender_id'])==aid)
        assert_receipt(b.command('reject',target_name=aname,target_id=aid),sent=True)
        a.command('refresh');b.command('refresh')
        self.check('rejected request does not create an accepted friend',not find_friend(a.state(),bid) and not find_friend(b.state(),aid))
        self.accept_pair(a,aid,aname,b,bid,bname)
        self.check('mutual native acceptance retains names',find_friend(a.state(),bid)['name']==bname and find_friend(b.state(),aid)['name']==aname)
        assert_receipt(a.command('open',target_name=bname,target_id=bid))
        self.check('online native conversation opens',find_friend(a.state(),bid).get('conversation_open')=='1')
        body='FriendTestA'+uuid.uuid4().hex[:12]
        assert_receipt(a.command('send',target_name=bname,target_id=bid,body=body),sent=True)
        b.wait(lambda s:s.get('state',{}).get('last_message')==body)
        self.check('online whisper reaches peer original handler',b.state()['state'].get('last_sender')==aname)
        assert_receipt(b.command('open',target_name=aname,target_id=aid))
        reply='FriendTestB'+uuid.uuid4().hex[:12]
        assert_receipt(b.command('send',target_name=aname,target_id=aid,body=reply),sent=True)
        a.wait(lambda s:s.get('state',{}).get('last_message')==reply)
        self.check('reverse whisper reaches original handler',a.state()['state'].get('last_sender')==bname)
        self.stop('b')
        a.command('refresh');self.state_friend(a,bid,0)
        assert_receipt(a.command('open',target_name=bname,target_id=bid))
        self.check('offline friend retains identity/name/conversation',find_friend(a.state(),bid)['name']==bname and find_friend(a.state(),bid).get('conversation_open')=='1')
        assert_receipt(a.command('send',target_name=bname,target_id=bid,body='OfflineProbe'),sent=True,error=0x1407)
        self.check('offline send has native rejection',int(a.state()['command']['reply_status'])==0x1407)
        self.stop('a')
        a,aid2,_=self.start('a');self.state_friend(a,bid,0)
        self.check('relogin preserves offline friendship',aid2==aid and find_friend(a.state(),bid)['name']==bname)
        self.stop('a')
        self.receipt['status']='primary_flow_passed';self.save()

    def edges(self):
        a,aid,aname=self.start('a');b,bid,bname=self.start('b')
        self.state_friend(a,bid);self.state_friend(b,aid)
        before=int(a.state()['state']['friend_count'])
        duplicate=a.command('add',target_id=bid,target_name=bname)
        self.check('duplicate friend is rejected without adding another row',
                   (int(duplicate['command']['result'])>=0x100 or int(duplicate['command']['reply_status'])!=0) and
                   int(duplicate['state']['friend_count'])==before)
        own=a.command('add',target_id=aid,target_name=aname)
        self.check('self friendship is rejected',int(own['command']['result'])>=0x100 or int(own['command']['reply_status'])!=0)
        unknown=a.command('add',target_name='NoUser'+uuid.uuid4().hex[:8])
        self.check('unknown pilot receives a native rejection',int(unknown['command']['result'])>=0x100 or int(unknown['command']['reply_status'])!=0)
        removed=a.command('remove',target_id=bid,target_name=bname)
        assert_receipt(removed,sent=True)
        a.command('refresh')
        self.check('native deletion removes the selected friendship',find_friend(a.state(),bid) is None)
        self.stop('b');self.stop('a')
        a,aid2,_=self.start('a')
        self.check('deleted friendship remains absent after relogin',aid2==aid and find_friend(a.state(),bid) is None)
        b,bid2,_=self.start('b')
        self.accept_pair(a,aid,aname,b,bid,bname)
        self.check('deleted friend can be added and accepted again',bid2==bid and bool(find_friend(a.state(),bid)))
        self.stop('b');self.stop('a')
        self.receipt['status']='edge_flow_passed';self.save()

    def pagination(self):
        a,aid,aname=self.start('a')
        expected={}
        for index in range(8):
            label='page-peer-'+str(index)
            peer,pid,name=self.start(label)
            self.accept_pair(a,aid,aname,peer,pid,name)
            expected[pid]=name
            self.stop(label)
        self.check('nine accepted native friendships exist before relogin',int(a.state()['state']['friend_count'])>=9)
        self.stop('a')
        a,aid2,_=self.start('a')
        state=a.state();rows=friendship_rows(state)
        self.check('initial native loader consumes multiple friend-list pages',int(state['state']['list_page_replies'])>=2 and int(state['state']['friend_count'])>=9)
        self.check('all additional offline friends retain their exact names after relogin',
                   aid2==aid and all(find_friend(state,pilot) and find_friend(state,pilot)['name']==name and
                                     find_friend(state,pilot)['presence']=='0' for pilot,name in expected.items()))
        self.check('paged friend identities are unique',len({row['id'] for row in rows})==len(rows))
        self.stop('a')
        self.receipt['status']='passed';self.save()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    subs=parser.add_subparsers(dest='action',required=True)
    campaign=subs.add_parser('campaign')
    campaign.add_argument('--client-template',type=Path,required=True)
    campaign.add_argument('--stage',type=Path,required=True)
    campaign.add_argument('--work-root',type=Path,required=True)
    campaign.add_argument('--asset-baseline',type=Path,required=True)
    campaign.add_argument('--target-host',default='127.92.0.1')
    state=subs.add_parser('state');state.add_argument('--run-dir',type=Path,required=True)
    command=subs.add_parser('command');command.add_argument('--run-dir',type=Path,required=True)
    command.add_argument('--operation',required=True)
    command.add_argument('--target-id',type=int,default=0);command.add_argument('--target-name',default='')
    command.add_argument('--body',default='');command.add_argument('--page',type=int,default=1)
    command.add_argument('--decision',type=int,default=0);command.add_argument('--timeout',type=float,default=90)
    args=parser.parse_args()
    if args.action=='campaign':
        run=Campaign(args.client_template,args.stage,args.work_root,args.target_host,args.asset_baseline)
        try:run.smoke()
        except Exception as error:
            run.receipt['status']='failed';run.receipt['error_type']=type(error).__name__;run.receipt['error']=str(error);run.save()
            for label in list(run.running):
                actor,future,_=run.running.pop(label)
                abort=game_test.ini_parser();abort['run']={'id':actor.metadata['run_id']}
                game_test.write_ini(actor.directory/'friend-abort.ini',abort)
                try:future.result(timeout=120)
                except Exception:pass
            raise
        finally:run.pool.shutdown(wait=False,cancel_futures=False)
        print(json.dumps(run.receipt,indent=2));return
    actor=FriendActor(args.run_dir)
    if args.action=='state':result=actor.state()
    else:
        state=actor.state()
        actor.sequence=int(state.get('command',{}).get('sequence','0')) if state else 0
        result=actor.command(args.operation,target_id=args.target_id,target_name=args.target_name,
                             body=args.body,page=args.page,decision=args.decision,timeout=args.timeout)
    print(json.dumps(result,ensure_ascii=True,indent=2))


if __name__=='__main__':main()
