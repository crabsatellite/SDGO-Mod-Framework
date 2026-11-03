"""Coordinate real native friend sessions in owned GameTest clients.

Commands are scoped to an authenticated --friends-only run and are executed by
the GameTest Mod on the native main thread. This tool never edits game state,
friendship tables, server replies or production accounts.
"""
from __future__ import annotations
import argparse
import concurrent.futures
import json
import io
from pathlib import Path
import time
import uuid
import game_test


class FriendActor:
    def __init__(self,run_dir:Path,completion=None):
        self.run_dir=run_dir.resolve()
        self.metadata=game_test.validate_metadata(self.run_dir)
        if not self.metadata['plan'].get('friends_only'):
            raise game_test.GameTestError('friend commands require a friends-only native session')
        self.directory=Path(self.metadata['client']['root'])/'sdgo/gametest'
        self.sequence=0
        self.completion=completion

    def native_events(self):
        path=Path(self.metadata['client']['events'])
        if not path.exists():return []
        rows=[]
        for line in path.read_text(encoding='utf-8').splitlines(keepends=True):
            if not line.endswith('\n'):break
            try:row=json.loads(line)
            except json.JSONDecodeError as error:
                raise game_test.GameTestError('native friend event is malformed') from error
            if row.get('run_id')!=self.metadata['run_id']:
                raise game_test.GameTestError('native friend event belongs to another session')
            rows.append(row)
        return rows

    def native_error_after(self,baseline,code,name):
        expected=f'native error code={code:04X} name={name}'
        return any(row.get('case')=='friends.session' and row.get('event')=='observation' and
                   row.get('message')==expected for row in self.native_events()[baseline:])

    def state(self):
        path=self.directory/'friend-result.ini'
        if not path.exists():return None
        try:
            state=game_test.read_ini(path)
            if state.get('run','id',fallback='')!=self.metadata['run_id']:
                raise game_test.GameTestError('friend receipt belongs to another native session')
            return {section:dict(state[section]) for section in state.sections()}
        except game_test.GameTestError as error:
            cause=error.__cause__
            if isinstance(cause,(FileNotFoundError,PermissionError)) or getattr(cause,'winerror',None) in (32,33):
                return None
            raise
        except (OSError,ValueError):return None

    def wait(self,predicate,timeout=90):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            state=self.state()
            if state and predicate(state):return state
            if self.completion is not None and self.completion.done():
                final_state=self.state()
                if final_state and predicate(final_state):return final_state
                report=self.completion.result()
                raise game_test.GameTestError('native actor exited before its friend condition: '+report.get('status','unknown'))
            events=Path(self.metadata['client']['events'])
            if events.exists() and game_test.has_run_end(events):
                final_state=self.state()
                if final_state and predicate(final_state):return final_state
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
        # GetPrivateProfileStringW reads UTF-16 INIs independently of the
        # Windows ANSI code page. Non-ASCII pilot names must survive IPC.
        text=io.StringIO();cfg.write(text,space_around_delimiters=False)
        path=self.directory/'friend-command.ini';temporary=path.with_name(path.name+'.tmp')
        temporary.write_bytes(b'\xff\xfe'+text.getvalue().encode('utf-16-le'))
        deadline=time.monotonic()+2
        while True:
            try:
                temporary.replace(path)
                break
            except PermissionError as error:
                # The native INI reader briefly opens the previous command
                # without delete sharing. Publish the same sequence once it
                # releases that handle; other failures remain errors.
                if getattr(error,'winerror',None) not in (5,32,33) or time.monotonic()>=deadline:
                    raise
                time.sleep(.025)
        return self.wait(lambda s:int(s.get('command',{}).get('sequence','0'))==self.sequence and
                         s.get('command',{}).get('done')=='1',timeout)


def prepare_actor(template,stage,run_dir,profile,host,*,baseline=None,extra_mods=(),rebind_existing_account=False):
    if not profile.is_file():
        raise game_test.GameTestError('friend tests require an existing sealed-template account profile')
    options=dict(target_host=host,login_port=5001,account_profile=profile,
                 friends_only=True,asset_baseline=baseline,extra_mods=tuple(extra_mods))
    if run_dir.exists():
        metadata=game_test.validate_metadata(run_dir)
        evidence=run_dir.parent/'evidence';evidence.mkdir(exist_ok=True)
        events=Path(metadata['client']['events']);report=run_dir/'receipts/report.json'
        game_test.atomic_write(evidence/(metadata['run_id']+'.json'),json.dumps({
            'run_id':metadata['run_id'],'run_dir':str(run_dir),'source':metadata.get('source'),
            'immutable_hashes':metadata['client']['immutable_hashes'],
            'events':events.read_text(encoding='utf-8') if events.exists() else '',
            'report':game_test.read_json(report) if report.exists() else None},indent=2))
        return game_test.reuse_prepared(template,stage,run_dir,
                                        rebind_existing_account=rebind_existing_account,**options)
    return game_test.prepare(template,stage,run_dir,**options)


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
    result=int(command.get('result','0'))
    if result in (0x109,0x10B,0x10C):
        raise game_test.GameTestError('native friend command faulted, malformed its cache or is unsupported')
    if error is None and result not in (4,5,6,7):
        raise game_test.GameTestError('native friend command did not complete successfully: '+str(result))
    if sent is not None and command.get('request_sent')!=str(int(sent)):
        raise game_test.GameTestError('native friend transport observation differs from the expected action')
    if error is not None and (int(command.get('reply_status','0'))!=error or command.get('reply_consumed')!='1'):
        raise game_test.GameTestError('native friend server result differs from the expected status')
    return state


def native_rejection(state):
    command=state.get('command',{})
    return command.get('local_validation_rejected')=='1' or (
        command.get('reply_consumed')=='1' and int(command.get('reply_status','0'))!=0)


class Campaign:
    def __init__(self,template:Path,stage:Path,root:Path,host:str,baseline:Path|None=None,*,extra_mods=(),on_process=None,on_poll=None,peer_labels=None):
        self.template=template;self.stage=stage;self.root=root.resolve();self.host=host;self.baseline=baseline
        self.extra_mods=tuple(extra_mods);self.on_process=on_process;self.on_poll=on_poll
        self.peer_labels=tuple(peer_labels) if peer_labels is not None else tuple(
            path.stem for path in sorted((self.root/'profiles').glob('page-peer-*.ini'))
            if path.stem.removeprefix('page-peer-').isdigit())
        self.pool=concurrent.futures.ThreadPoolExecutor(max_workers=3)
        self.running={};self.counts={};self.receipt={'schema':1,'status':'running','checks':[],'actors':[]}
        self.root.mkdir(parents=True,exist_ok=True)
        previous=self.root/'campaign.json'
        if previous.exists():
            history=self.root/'evidence';history.mkdir(exist_ok=True)
            game_test.atomic_write(history/('campaign-'+uuid.uuid4().hex+'.json'),previous.read_text(encoding='utf-8'))
        self.save()
    def save(self):
        game_test.atomic_write(self.root/'campaign.json',json.dumps(self.receipt,ensure_ascii=True,indent=2))
    def check(self,name,condition,**evidence):
        row={'name':name,'passed':bool(condition),**evidence};self.receipt['checks'].append(row);self.save()
        if not condition:raise game_test.GameTestError('friend campaign failed: '+name)
    def start(self,label,_startup_attempt=0):
        number=self.counts.get(label,0)+1;self.counts[label]=number
        # Pagination visits these existing identities serially; reuse one
        # capsule rather than making another full client for every account.
        run=self.root/('page-peer-01' if label.startswith('page-peer-') else label+'-01');profile=self.root/'profiles'/(label+'.ini')
        metadata=prepare_actor(self.template,self.stage,run,profile,self.host,baseline=self.baseline,
                               extra_mods=self.extra_mods,rebind_existing_account=label.startswith('page-peer-'))
        future=self.pool.submit(game_test.run_prepared,run,9000,60,on_process=self.on_process,on_poll=self.on_poll)
        actor=FriendActor(run,future);self.running[label]=(actor,future,profile)
        try:
            state=actor.wait(lambda s:s.get('run',{}).get('ready')=='1',900)
        except game_test.GameTestError:
            events=Path(metadata['client']['events'])
            rows=[json.loads(line) for line in events.read_text(encoding='utf-8').splitlines() if line] if events.exists() else []
            graphics_only=any('Failed to create Direct3D object.' in row.get('message','') for row in rows) and not any(
                row.get('case') in ('flow.login','flow.onboarding','friends.session') for row in rows)
            if graphics_only and _startup_attempt<2:
                report=future.result(timeout=30)
                self.receipt.setdefault('startup_retries',[]).append({'label':label,'run_id':metadata['run_id'],
                    'reason':'native Direct3D creation failed before login','status':report['status'],'accepted_as_pass':False})
                self.save();self.running.pop(label)
                return self.start(label,_startup_attempt+1)
            raise
        promote_profile(run,profile)
        account=state['state'].get('own_name','')
        if not account:raise game_test.GameTestError('native friend actor has no observed pilot nickname')
        self.receipt['actors'].append({'label':label,'run_dir':str(run),'run_id':metadata['run_id']})
        self.save()
        return actor,int(state['state']['pilot']),account
    def stop(self,label):
        actor,future,_=self.running[label]
        actor.command('finish',timeout=30)
        report=future.result(timeout=90)
        self.check(label+' clean native completion',report['status']=='pass',run_dir=str(actor.run_dir))
        self.running.pop(label)
    def state_friend(self,actor,pilot,presence=None,timeout=90):
        def found(state):
            row=find_friend(state,pilot)
            return bool(row) and row.get('name') and row.get('relation')=='7' and (presence is None or int(row['presence'])==presence)
        return actor.wait(found,timeout)
    def accept_pair(self,a,a_id,a_name,b,b_id,b_name):
        baseline=int(b.wait(lambda s:True)['pending'].get('sequence','0'))
        state=a.command('add',target_name=b_name,target_id=b_id)
        assert_receipt(state,sent=True)
        b.wait(lambda s:s.get('pending',{}).get('available')=='1' and int(s['pending']['sequence'])>baseline and int(s['pending']['sender_id'])==a_id and int(s['pending']['recipient_id'])==b_id)
        assert_receipt(b.command('accept',target_name=a_name,target_id=a_id),sent=True)
        self.state_friend(a,b_id);self.state_friend(b,a_id)
    def smoke(self):
        a,aid,aname=self.start('a');b,bid,bname=self.start('b')
        a.command('snapshot');b.command('snapshot')
        baseline=int(b.wait(lambda s:True)['pending'].get('sequence','0'))
        assert_receipt(a.command('add',target_name=bname,target_id=bid),sent=True)
        b.wait(lambda s:s.get('pending',{}).get('available')=='1' and int(s['pending']['sequence'])>baseline and int(s['pending']['sender_id'])==aid and int(s['pending']['recipient_id'])==bid)
        assert_receipt(b.command('reject',target_name=aname,target_id=aid),sent=True)
        assert_receipt(a.command('refresh'),sent=True);assert_receipt(b.command('refresh'),sent=True)
        self.check('rejected request does not create an accepted friend',not find_friend(a.state(),bid) and not find_friend(b.state(),aid))
        self.accept_pair(a,aid,aname,b,bid,bname)
        self.check('mutual native acceptance retains names',find_friend(a.state(),bid)['name']==bname and find_friend(b.state(),aid)['name']==aname)
        assert_receipt(a.command('open',target_name=bname,target_id=bid))
        self.check('online native conversation opens',find_friend(a.state(),bid).get('conversation_open')=='1')
        close=a.command('close',target_name=bname,target_id=bid)
        self.check('native conversation close removes its tab only',close.get('extra',{}).get('selected_target_matched')=='1' and
                   int(close['extra']['conversation_count_after'])==int(close['extra']['conversation_count_before'])-1)
        assert_receipt(a.command('open',target_name=bname,target_id=bid))
        self.check('closed conversation can reopen without losing friendship',bool(find_friend(a.state(),bid)) and find_friend(a.state(),bid)['conversation_open']=='1')
        body='A'+uuid.uuid4().hex[:6]
        assert_receipt(a.command('send',target_name=bname,target_id=bid,body=body),sent=True)
        b.wait(lambda s:s.get('state',{}).get('last_message')==body and s['state'].get('message_applied')=='1' and int(s['state'].get('message_sender_pilot','0'))==aid)
        self.check('online whisper reaches peer original handler',b.state()['state'].get('last_sender')==aname)
        assert_receipt(b.command('open',target_name=aname,target_id=aid))
        reply='B'+uuid.uuid4().hex[:6]
        assert_receipt(b.command('send',target_name=aname,target_id=aid,body=reply),sent=True)
        a.wait(lambda s:s.get('state',{}).get('last_message')==reply and s['state'].get('message_applied')=='1' and int(s['state'].get('message_sender_pilot','0'))==bid)
        self.check('reverse whisper reaches original handler',a.state()['state'].get('last_sender')==bname)
        chinese='好友'+uuid.uuid4().hex[:4]
        assert_receipt(a.command('send',target_name=bname,target_id=bid,body=chinese),sent=True)
        b.wait(lambda s:s.get('state',{}).get('last_message')==chinese and s['state'].get('message_applied')=='1' and int(s['state'].get('message_sender_pilot','0'))==aid)
        self.check('Chinese whisper survives original UI serialization and peer consumption',b.state()['state'].get('last_sender')==aname)
        self.stop('b')
        assert_receipt(a.command('refresh'),sent=True);self.state_friend(a,bid,0)
        assert_receipt(a.command('close',target_name=bname,target_id=bid))
        self.check('offline conversation tab closes before the reopen test',find_friend(a.state(),bid).get('conversation_open')=='0')
        assert_receipt(a.command('open',target_name=bname,target_id=bid))
        self.check('offline friend retains identity/name/conversation',find_friend(a.state(),bid)['name']==bname and find_friend(a.state(),bid).get('conversation_open')=='1')
        assert_receipt(a.command('send',target_name=bname,target_id=bid,body='OfflineProbe'),sent=True)
        rejected=a.wait(lambda s:s.get('command',{}).get('reply_consumed')=='1' and int(s['command'].get('reply_status','0'))==0x1407)
        assert_receipt(rejected,sent=True,error=0x1407)
        self.check('offline send has native rejection',int(a.state()['command']['reply_status'])==0x1407)
        self.stop('a')
        self.persistence(aid,bid,bname)

    def persistence(self,aid=None,bid=None,bname=None):
        if aid is None:
            previous=FriendActor(self.root/'a-01').state()
            peer=FriendActor(self.root/'b-01')
            report=game_test.read_json(peer.run_dir/'receipts/report.json')
            if report.get('status')!='pass' or report.get('run_id')!=peer.metadata['run_id']:
                raise game_test.GameTestError('persistence resume requires a passed native peer session')
            peer_state=peer.state()
            if not previous or not peer_state:
                raise game_test.GameTestError('persistence resume requires correlated native identities')
            aid=int(previous['state']['pilot'])
            bid=int(peer_state['state']['pilot']);bname=peer_state['state']['own_name']
        a,aid2,_=self.start('a');self.state_friend(a,bid,0)
        self.check('relogin preserves offline friendship',aid2==aid and find_friend(a.state(),bid)['name']==bname)
        profile=a.command('profile',target_id=bid,target_name=bname)
        assert_receipt(profile)
        observed=profile.get('extra',{})
        self.check('selected offline friend profile consumes native data in the managed popup',
                   all(observed.get(key)=='1' for key in ('profile_clone_created','profile_clone_identity_matched',
                       'profile_initialized','profile_request_started','profile_reply_consumed','profile_reply_target_matched')) and
                   int(observed.get('profile_reply_opcode','0'))==0x084B and int(observed.get('profile_reply_target_id','0'))==bid)
        self.stop('a')
        self.receipt['status']='primary_flow_passed';self.save()

    def edges(self):
        a,aid,aname=self.start('a');b,bid,bname=self.start('b')
        if not find_friend(a.state(),bid) and not find_friend(b.state(),aid):
            self.accept_pair(a,aid,aname,b,bid,bname)
        self.state_friend(a,bid);self.state_friend(b,aid)
        before=int(a.state()['state']['friend_count'])
        duplicate=a.command('add',target_id=bid,target_name=bname)
        self.check('duplicate friend is rejected without adding another row',
                   native_rejection(duplicate) and
                   int(duplicate['state']['friend_count'])==before)
        own=a.command('add',target_id=aid,target_name=aname)
        self.check('self friendship is rejected',native_rejection(own))
        error_baseline=len(a.native_events())
        unknown=a.command('add',target_name='NoUser'+uuid.uuid4().hex[:8])
        rejected=native_rejection(unknown)
        if not rejected:
            assert_receipt(unknown,sent=True)
            # Add-name failure is consumed by the original global error path,
            # rather than a Messenger reply ACK. Require this run's fresh,
            # exact native error after the verified request.
            a.wait(lambda state:a.native_error_after(error_baseline,0x0818,'ERR_MAKE_FRIEND_MSN_NOT_EXIST'))
            rejected=True
        self.check('unknown pilot receives a native rejection',rejected and
                   int(a.state()['state']['friend_count'])==before)
        removed=a.command('remove',target_id=bid,target_name=bname)
        assert_receipt(removed,sent=True)
        assert_receipt(a.command('refresh'),sent=True)
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
        if not self.peer_labels:
            raise game_test.GameTestError('pagination requires additional existing fixture account profiles')
        a,aid,aname=self.start('a')
        b,bid,bname=self.start('b')
        if not find_friend(a.state(),bid):self.accept_pair(a,aid,aname,b,bid,bname)
        expected={bid:bname}
        assert_receipt(b.command('snapshot'))
        self.stop('b')
        for label in self.peer_labels:
            peer,pid,name=self.start(label)
            if not find_friend(a.state(),pid):
                self.accept_pair(a,aid,aname,peer,pid,name)
            self.state_friend(a,pid);self.state_friend(peer,aid)
            expected[pid]=name
            assert_receipt(peer.command('snapshot'))
            self.stop(label)
        self.check('all declared existing fixture peers became native friends',int(a.state()['state']['friend_count'])>=len(expected))
        self.stop('a')
        a,aid2,_=self.start('a')
        state=assert_receipt(a.command('page',page=2),sent=True);rows=friendship_rows(state)
        self.check('native page request preserves the complete fixture friend cache',int(state['state']['list_page_replies'])>=2 and int(state['state']['friend_count'])>=len(expected))
        self.check('all additional offline friends retain their exact names after relogin',
                   aid2==aid and all(find_friend(state,pilot) and find_friend(state,pilot)['name']==name and
                                     find_friend(state,pilot)['presence']=='0' for pilot,name in expected.items()))
        self.check('paged friend identities are unique',len({row['id'] for row in rows})==len(rows))
        self.stop('a')
        self.receipt['status']='passed';self.save()

    def invitations(self):
        for decision in ('reject','accept'):
            a,aid,aname=self.start('a');b,bid,bname=self.start('b')
            self.state_friend(a,bid);self.state_friend(b,aid)
            # Each decision starts with fresh authenticated clients and a new
            # genuine room. Native post-refusal state is never bypassed.
            for actor,peer in ((a,bid),(b,aid)):
                assert_receipt(actor.command('refresh'),sent=True)
                actor.wait(lambda state:bool(find_friend(state,peer)) and int(find_friend(state,peer)['presence'])!=0,90)
            self.check('real friend refresh observes both authenticated peers online for '+decision,True)
            assert_receipt(a.command('open',target_name=bname,target_id=bid))
            created=assert_receipt(a.command('room_create',timeout=180))
            room=created['room'];room_id=int(room['actual_room_id']);channel=int(room['actual_channel_id'])
            self.check('friend invitation uses a real newly created host room for '+decision,room_id>0 and channel>0 and room['model_present']=='1' and room['local_is_host']=='1')
            baseline=int(room['player_count'])
            identity=dict(target_id=aid,target_name=aname,channel_id=channel,room_id=room_id)
            assert_receipt(b.command('invite_arm_'+decision,**identity))
            sent=assert_receipt(a.command('invite_send',target_id=bid,target_name=bname,channel_id=channel,room_id=room_id),sent=True)
            self.check('native room invite transport '+decision,int(sent['invite_sender']['request_opcode'])==0x077E and sent['invite_sender']['recipient_correlated']=='1')
            b.wait(lambda s:s.get('invite_receiver',{}).get('controls_ready')=='1' and s['invite_receiver'].get('incoming_packet_matched')=='1')
            result=assert_receipt(b.command('invite_decide',timeout=180,**identity))
            received=result['invite_receiver']
            if decision=='reject':
                self.check('native invitation rejection closes its real prompt without joining',received['prompt_destroyed']=='1' and received['decision_sent']=='1' and int(received['decision_opcode'])==0x07A9 and received['join_request_sent']=='0')
            else:
                self.check('native invitation acceptance joins the exact room as a guest',received['decision_sent']=='1' and int(received['decision_opcode'])==0x04A8 and received['room_model_matched']=='1' and received['local_guest']=='1' and received['wait_room_owner_matched']=='1' and received['manager_vtable_matched']=='1' and int(received['model_room_id'])==room_id)
                a.wait(lambda s:int(s.get('room',{}).get('player_count','0'))>=baseline+1)
                assert_receipt(b.command('room_leave',channel_id=channel,room_id=room_id,timeout=90))
            assert_receipt(a.command('room_leave',channel_id=channel,room_id=room_id,timeout=90))
            self.check('both invitation participants return to the real lobby after '+decision,a.state()['state']['app']=='6' and b.state()['state']['app']=='6')
            self.stop('b');self.stop('a')
        self.receipt['status']='invitations_passed';self.save()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    subs=parser.add_subparsers(dest='action',required=True)
    campaign=subs.add_parser('campaign')
    campaign.add_argument('--client-template',type=Path,required=True)
    campaign.add_argument('--stage',type=Path,required=True)
    campaign.add_argument('--work-root',type=Path,required=True)
    campaign.add_argument('--asset-baseline',type=Path)
    campaign.add_argument('--include-mod',action='append',default=['friend_list_client_fix'])
    campaign.add_argument('--peer-label',action='append',help='existing profile label; defaults to declared page-peer profiles')
    campaign.add_argument('--target-host',required=True,help='the verified unique loopback IP of this campaign backend')
    campaign.add_argument('--phase',choices=('all','smoke','persistence','remaining','edges','pagination','invitations'),default='all')
    state=subs.add_parser('state');state.add_argument('--run-dir',type=Path,required=True)
    command=subs.add_parser('command');command.add_argument('--run-dir',type=Path,required=True)
    command.add_argument('--operation',required=True)
    command.add_argument('--target-id',type=int,default=0);command.add_argument('--target-name',default='')
    command.add_argument('--body',default='');command.add_argument('--page',type=int,default=1)
    command.add_argument('--decision',type=int,default=0);command.add_argument('--timeout',type=float,default=90)
    args=parser.parse_args()
    if args.action=='campaign':
        run=Campaign(args.client_template,args.stage,args.work_root,args.target_host,args.asset_baseline,
                     extra_mods=args.include_mod,peer_labels=args.peer_label)
        try:
            if args.phase in ('all','smoke'):run.smoke()
            if args.phase=='persistence':run.persistence()
            if args.phase in ('all','remaining','edges'):run.edges()
            if args.phase in ('all','remaining','invitations'):run.invitations()
            if args.phase in ('all','remaining','pagination'):run.pagination()
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
