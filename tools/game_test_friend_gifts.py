"""Verify real friend-unit gift controls and exact instance preservation.

Uses existing accounts in a marked local test copy. Mutations are performed
only by the published client/server Mods through their native UI path; SQL
access below is read-only verification.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
from pathlib import Path
import shutil
import struct
import sys
import uuid

import game_test
from game_test_friends import Campaign, assert_receipt, find_friend


def text_hex(value: str) -> str:
    raw=value.encode('utf-16-le')
    return ''.join(f'{unit:04X}' for unit in struct.unpack('<'+'H'*(len(raw)//2),raw))


def from_text_hex(value: str) -> str:
    if len(value)%4:raise game_test.GameTestError('malformed native UTF-16 receipt')
    return b''.join(int(value[i:i+4],16).to_bytes(2,'little') for i in range(0,len(value),4)).decode('utf-16-le')


def provenance_timestamp(value: dt.datetime | str) -> str:
    # SQL records UTC datetime2 values; preserve that meaning for naive rows.
    if isinstance(value,str):
        value=dt.datetime.fromisoformat(value.strip().removesuffix('Z')+('+00:00' if value.strip().endswith('Z') else ''))
    if value.tzinfo is not None:
        value=value.astimezone(dt.timezone.utc)
    return value.strftime('%Y-%m-%d %H:%M UTC')


def native_paths_owned(root: Path, paths) -> bool:
    paths=list(paths)
    return bool(paths) and all(Path(path).resolve().is_relative_to(root.resolve()) for path in paths)


def same_profile_settings(left: Path, right: Path) -> bool:
    def settings(path):
        cfg=game_test.read_ini(path)
        return dict(cfg.defaults()),{name:dict(cfg[name]) for name in cfg.sections()}
    return settings(left)==settings(right)


class InventoryReadback:
    def __init__(self,test_root:Path,sql_readback_ini:Path|None=None):
        import pyodbc
        self.root=test_root.resolve()
        if not (self.root/'TEST-ONLY.txt').is_file():
            raise game_test.GameTestError('gift verification requires a marked local test copy')
        native=json.loads((self.root/'database/native-sql.json').read_text(encoding='utf-8'))
        if Path(native['root']).resolve()!=self.root:
            raise game_test.GameTestError('native SQL belongs to another test copy')
        settings=game_test.read_ini(self.root/'server/Server/WorldServer/WorldServer.ini')['DB Game']
        credentials=(game_test.read_ini(sql_readback_ini)['DB Member']
                     if sql_readback_ini else settings)
        quote=lambda value:'{'+str(value).replace('}','}}')+'}'
        self.connection=pyodbc.connect(
            'DRIVER={SQL Server};SERVER=127.0.0.1,'+str(int(native['port']))+
            ';DATABASE='+quote(settings['DBName'])+';UID='+quote(credentials.get('ID',credentials.get('id')))+
            ';PWD='+quote(credentials.get('PWD',credentials.get('pwd')))+';',
            timeout=10,autocommit=True)
        cursor=self.connection.cursor()
        if int(cursor.execute("SELECT CAST(SERVERPROPERTY('ProcessID') AS int)").fetchval())!=int(native['pid']):
            raise game_test.GameTestError('SQL process identity does not match the selected native test')
        paths=cursor.execute('SELECT f.physical_name FROM sys.master_files f JOIN sys.databases d ON d.database_id=f.database_id WHERE d.state=0').fetchall()
        if not native_paths_owned(self.root,(row[0] for row in paths)):
            raise game_test.GameTestError('SQL file ownership is unavailable or outside this test copy; use a local metadata readback credential INI')
        cursor.execute('SELECT TOP (0) * FROM dbo.tbl_UserUnits')
        self.native_columns=tuple(column[0] for column in cursor.description)
        if len(self.native_columns)!=50:
            raise game_test.GameTestError('the fixture native instance contract is not fifty columns')

    def unit(self,uid:int):
        cursor=self.connection.cursor()
        rows=cursor.execute('SELECT * FROM dbo.tbl_UserUnits WHERE UserUnitNumber=?',uid).fetchall()
        columns=[column[0] for column in cursor.description]
        if len(rows)>1:raise game_test.GameTestError('native unit UID is ambiguous')
        return dict(zip(columns,rows[0])) if rows else None

    def nickname(self,account:int):
        value=self.connection.cursor().execute(
            'SELECT PilotName FROM dbo.tbl_Users WHERE AccountNumber=?',account).fetchval()
        if not isinstance(value,str) or not value:
            raise game_test.GameTestError('authoritative native pilot name is unavailable')
        return value

    def escrow(self,uid:int):
        cursor=self.connection.cursor()
        rows=cursor.execute('SELECT * FROM sdgo_friend_unit_gift.EscrowUnit WHERE UserUnitNumber=?',uid).fetchall()
        columns=[column[0] for column in cursor.description]
        if len(rows)>1:raise game_test.GameTestError('gift escrow UID is ambiguous')
        if not rows:return None
        value=dict(zip(columns,rows[0]))
        if any(name not in value for name in self.native_columns):
            raise game_test.GameTestError('escrow is missing a native instance column')
        return {name:value[name] for name in self.native_columns}

    def counts_valid(self,account:int):
        row=self.connection.cursor().execute(
            'SELECT u.UserUnitTotalCnt,u.UserUnitKindCnt,'
            '(SELECT COUNT(*) FROM dbo.tbl_UserUnits x WHERE x.AccountNumber=u.AccountNumber),'
            '(SELECT COUNT(DISTINCT x.UnitNumber) FROM dbo.tbl_UserUnits x WHERE x.AccountNumber=u.AccountNumber) '
            'FROM dbo.tbl_Users u WHERE u.AccountNumber=?',account).fetchone()
        return bool(row) and int(row[0])==int(row[2]) and int(row[1])==int(row[3])

    def counter_values(self,account:int):
        row=self.connection.cursor().execute(
            'SELECT UserUnitTotalCnt,UserUnitKindCnt FROM dbo.tbl_Users WHERE AccountNumber=?',account).fetchone()
        if row is None:raise game_test.GameTestError('native inventory counter owner is unavailable')
        return tuple(row)

    def gift_record(self,gift:int):
        cursor=self.connection.cursor()
        rows=cursor.execute(
            'SELECT GiftId,UserUnitNumber,SenderAccount,RecipientAccount,State,OfferedAt,AcceptedAt,Note '
            'FROM sdgo_friend_unit_gift.GiftOffer WHERE GiftId=?',gift).fetchall()
        if len(rows)!=1:raise game_test.GameTestError('authoritative gift record is missing or ambiguous')
        return dict(zip((column[0] for column in cursor.description),rows[0]))

    def pending_gift(self,uid:int):
        cursor=self.connection.cursor()
        rows=cursor.execute('SELECT GiftId,UserUnitNumber,SenderAccount,RecipientAccount,State,Note '
                            'FROM sdgo_friend_unit_gift.GiftOffer WHERE UserUnitNumber=? AND State=0',uid).fetchall()
        if len(rows)!=1:raise game_test.GameTestError('the fixture has no unique pending gift to resume')
        return dict(zip((column[0] for column in cursor.description),rows[0]))

    def close(self):self.connection.close()


def same_payload(before,after,*,allow_owner_slot=False):
    if before is None or after is None:return False
    skip={'AccountNumber','UserUnitSlotNumber'} if allow_owner_slot else set()
    return all(name in after and before[name]==after[name] for name in before if name not in skip)


class GiftCampaign:
    def __init__(self,args):
        self.args=args;self.root=args.work_root.resolve()
        binding=json.loads(args.fixture_binding.read_text(encoding='utf-8'))[args.pair]
        self.binding=binding;self.uid=int(binding['unit']['user_unit_number'])
        profiles=self.root/'profiles';profiles.mkdir(parents=True,exist_ok=True)
        for label,role in (('a','sender'),('b','recipient')):
            source=Path(binding[role]['profile_path']);destination=profiles/(label+'.ini')
            if destination.exists() and not same_profile_settings(destination,source):
                raise game_test.GameTestError('gift actor profile identity changed')
            if not destination.exists():shutil.copy2(source,destination)
        self.native=Campaign(args.client_template,args.stage,self.root,args.target_host,args.asset_baseline,
                             extra_mods=('friend_list_client_fix','storage_claim','friend_unit_gift_client'),peer_labels=())
        self.db=InventoryReadback(args.test_root,getattr(args,'sql_readback_ini',None))
        self.receipt={'schema':1,'status':'running','checks':[],'actors':[]}
        self.save()

    def save(self):
        game_test.atomic_write(self.root/'gift-campaign.json',json.dumps(self.receipt,ensure_ascii=True,indent=2))

    def check(self,name,condition,**details):
        self.receipt['checks'].append({'name':name,'passed':bool(condition),**details});self.save()
        if not condition:raise game_test.GameTestError('native unit gift check failed: '+name)

    @staticmethod
    def view(actor):
        # The native observer file is atomically replaced on UI ticks. A
        # concurrent read can briefly have no snapshot; wait for actual data.
        state=actor.wait(lambda current:current.get('unit_gift',{}).get('valid')=='1',30)
        return state['unit_gift']

    @staticmethod
    def rows(state):return [row for key,row in state.items() if key.startswith('unit_gift_row')]

    def command(self,actor,name,**fields):
        return assert_receipt(actor.command(name,**fields))

    def capture(self,actor,label):
        self.command(actor,'capture',label=label)
        self.check('genuine native frame saved: '+label,(actor.directory/('capture-'+label+'.bmp')).is_file())

    def wait_reply(self,actor,command,baseline,*,gift=None,unit=None):
        def matches(state):
            view=state.get('unit_gift',{})
            received=(view.get('valid')=='1' and int(view.get('reply_sequence','0'))>baseline and
                      int(view.get('last_reply_command','0'))==command)
            if received and int(view.get('last_reply_status','-1'))!=0:
                raise game_test.GameTestError('native gift command '+str(command)+' rejected with status '+view['last_reply_status'])
            return (received and (gift is None or int(view.get('last_reply_gift_id','0'))==gift) and
                    (unit is None or int(view.get('last_reply_unit_uid','0'))==unit))
        return actor.wait(matches,120)

    def open_draft(self,sender,recipient,note):
        surface=getattr(self.args,'friend_surface','home')
        self.command(sender,'gift_home_context' if surface=='home' else 'gift_context',
                     target_id=recipient,cursor_x=480,cursor_y=263)
        placement=sender.wait(lambda state:(int(state.get('unit_gift',{}).get('control_flags','0'))&2) and
            (surface!='home' or (state.get('home_friends',{}).get('context_target')==str(recipient) and
             int(state.get('unit_gift',{}).get('cursor_input_calls','0'))>0)),60)['unit_gift']
        self.check('native friend popup follows the supplied friend-row mouse input',
                   int(placement.get('cursor_input_calls','0'))>0 and
                   (placement.get('friend_context_x'),placement.get('friend_context_y'))==('480','263'))
        if surface=='home':
            home=sender.state().get('home_friends',{})
            self.check('home USER LIST opens the exact selected friend with Chinese context captions',
                       home.get('open')=='1' and home.get('mode')=='2' and
                       home.get('context_target')==str(recipient) and home.get('captions_chinese')=='1')
        self.capture(sender,'gift-context-'+str(sender.sequence))
        self.command(sender,'gift_open',target_id=recipient)
        sender.wait(lambda state:state.get('unit_gift',{}).get('composer_phase')=='1' and
                    int(state['unit_gift'].get('control_flags','0'))&8,60)
        self.command(sender,'gift_select_unit',unit_uid=self.uid)
        sender.wait(lambda state:int(state.get('unit_gift',{}).get('selected_unit_uid','0'))==self.uid and
                    state['unit_gift'].get('inventory_sync_busy')=='0' and
                    int(state['unit_gift'].get('control_flags','0'))&32,120)
        self.command(sender,'gift_note',note_utf16_hex=text_hex(note))
        self.capture(sender,'gift-draft-'+str(sender.sequence))
        baseline=int(self.view(sender)['reply_sequence'])
        self.command(sender,'gift_preview')
        self.wait_reply(sender,10,baseline,unit=self.uid)
        sender.wait(lambda state:state.get('unit_gift',{}).get('composer_phase')=='3',60)
        self.capture(sender,'gift-preview-'+str(sender.sequence))

    def offer(self,sender,recipient,note):
        self.open_draft(sender,recipient,note)
        baseline=int(self.view(sender)['reply_sequence'])
        self.command(sender,'gift_confirm')
        received=self.wait_reply(sender,2,baseline,unit=self.uid)
        sender.wait(lambda state:int(state.get('unit_gift',{}).get('inventory_last_removed_uid','0'))==self.uid and
                    state['unit_gift'].get('inventory_sync_busy')=='0',120)
        gift=int(received['unit_gift']['last_reply_gift_id'])
        self.check('confirmed native gift receives a nonzero authoritative gift ID',gift>0)
        self.capture(sender,'gift-offered-'+str(sender.sequence))
        self.command(sender,'gift_cancel')
        sender.wait(lambda state:state.get('unit_gift',{}).get('composer_phase')=='0',60)
        return gift

    def select_inbox(self,recipient,gift):
        self.command(recipient,'storage_open',timeout=150)
        self.command(recipient,'gift_storage')
        baseline=int(self.view(recipient)['reply_sequence'])
        self.command(recipient,'gift_inbox')
        state=self.wait_reply(recipient,3,baseline)
        found=any(int(row['gift_id'])==gift and int(row['unit_uid'])==self.uid for row in self.rows(state))
        self.check('receiver sees the exact offered instance in the product Storage view',found)
        recipient.wait(lambda current:current.get('unit_gift',{}).get('pending_command')=='0' and
                       int(current['unit_gift'].get('control_flags','0')) & 512,120)
        self.command(recipient,'gift_select',gift_id=gift)
        recipient.wait(lambda current:int(current.get('unit_gift',{}).get('selected_gift_id','0'))==gift and
                       current['unit_gift'].get('pending_command')=='0' and
                       int(current['unit_gift'].get('control_flags','0')) & 1024,60)
        self.capture(recipient,'gift-inbox-'+str(recipient.sequence))

    def open_gift_information(self,actor,gift):
        baseline=int(self.view(actor)['reply_sequence'])
        self.command(actor,'gift_provenance',gift_id=gift)
        self.wait_reply(actor,5,baseline,gift=gift,unit=self.uid)
        actor.wait(lambda current:current.get('unit_gift',{}).get('composer_phase')=='1' and
                   int(current['unit_gift'].get('control_flags','0')) & 32,60)
        self.capture(actor,'gift-native-unit-information-'+str(actor.sequence))

    def enabled(self):
        existing=self.db.unit(self.uid)
        self.check('the existing offered instance has not moved before the native campaign',existing is not None)
        expected_sender=int(existing['AccountNumber'])
        expected_sender_name=self.db.nickname(expected_sender)
        recipient,bid,bname=self.native.start('b')
        initial=recipient.wait(lambda state:True)
        offline=find_friend(initial,expected_sender)
        if offline and offline['presence']=='0':
            self.command(recipient,'refresh')
            initial=self.native.state_friend(recipient,expected_sender,presence=0)
            offline=find_friend(initial,expected_sender)
            self.command(recipient,'open',target_id=expected_sender,target_name=offline['name'])
            self.capture(recipient,'gift-offline-friend-name')
            self.check('an initially offline friend retains the canonical sender name',
                       offline['name']==expected_sender_name)
            self.command(recipient,'close',target_id=expected_sender,target_name=offline['name'])
        sender,aid,aname=self.native.start('a')
        self.receipt['actors']=list(self.native.receipt['actors']);self.save()
        for actor in (sender,recipient):
            actor.wait(lambda state:state.get('unit_gift',{}).get('valid')=='1' and
                       state['unit_gift'].get('ready')=='1' and
                       int(state['unit_gift'].get('capabilities','0'))&4,150)
            self.check('gift controls share the published Storage extension broker',
                       self.view(actor).get('storage_broker')=='1' and
                       self.view(actor).get('storage_fallback')=='0')
        if not find_friend(sender.state(),bid):
            self.native.accept_pair(sender,aid,aname,recipient,bid,bname)
        sender_state=self.native.state_friend(sender,bid)
        recipient_state=recipient.wait(lambda state:
            (row:=find_friend(state,aid)) is not None and int(row['presence'])>0,90)
        self.check('gift participants are genuine mutual native friends',
                   find_friend(sender_state,bid)['name']==bname and
                   find_friend(recipient_state,aid)['name']==aname)
        before=self.db.unit(self.uid)
        self.check('fixture is the sender exact existing native instance',
                   before and int(before['AccountNumber'])==aid and
                   int(before['UserUnitNumber'])==self.uid)
        cancelled='取消验收 '+uuid.uuid4().hex[:8]
        self.open_draft(sender,bid,cancelled)
        self.command(sender,'gift_cancel')
        sender.wait(lambda state:state.get('unit_gift',{}).get('composer_phase')=='0',60)
        self.check('preview and cancellation never move or change the original unit',same_payload(before,self.db.unit(self.uid)))

        self.native.stop('b')
        offline_state=self.native.state_friend(sender,bid,presence=0)
        self.check('the friend name remains canonical after the native logout update',
                   find_friend(offline_state,bid)['name']==bname)
        returned_note='归还验收\n保留养成 '+uuid.uuid4().hex[:8]
        returned=self.offer(sender,bid,returned_note)
        self.check('an offline confirmed friend can receive the offered instance',
                   'b' not in self.native.running)
        self.check('offer escrows the full exact original instance',self.db.unit(self.uid) is None and same_payload(before,self.db.escrow(self.uid)))
        recipient,returning_bid,_=self.native.start('b')
        self.check('the same existing recipient logs in to resolve the offline gift',returning_bid==bid)
        recipient.wait(lambda state:state.get('unit_gift',{}).get('valid')=='1' and
                       state['unit_gift'].get('ready')=='1',120)
        self.select_inbox(recipient,returned)
        self.open_gift_information(recipient,returned)
        baseline=int(self.view(recipient)['reply_sequence'])
        self.command(recipient,'gift_return',gift_id=returned)
        self.wait_reply(recipient,7,baseline,gift=returned,unit=self.uid)
        self.check('native return restores the same full payload and original acquisition date',same_payload(before,self.db.unit(self.uid)))
        self.check('return leaves no duplicate escrow row',self.db.escrow(self.uid) is None)

        accepted_note='出击顺利！🎁\n赠礼来源 '+uuid.uuid4().hex[:8]+'\n'
        accepted_note+='愿'*(200-len(accepted_note.encode('utf-16-le'))//2)
        accepted=self.offer(sender,bid,accepted_note)
        self.accept_and_prove(sender,aid,aname,recipient,bid,bname,before,accepted,accepted_note)
        self.native.stop('b');self.native.stop('a')
        self.receipt['status']='pass';self.save()

    def accept_and_prove(self,sender,aid,aname,recipient,bid,bname,before,accepted,accepted_note):
        self.select_inbox(recipient,accepted)
        self.command(recipient,'gift_storage')
        self.check('opening an already active gift page retains the selected native gift',
                   int(self.view(recipient).get('selected_gift_id','0'))==accepted)
        self.open_gift_information(recipient,accepted)
        baseline=int(self.view(recipient)['reply_sequence'])
        self.command(recipient,'gift_accept',gift_id=accepted)
        self.wait_reply(recipient,6,baseline,gift=accepted,unit=self.uid)
        recipient.wait(lambda state:int(state.get('unit_gift',{}).get('inventory_last_added_uid','0'))==self.uid and
                       state['unit_gift'].get('inventory_sync_busy')=='0',120)
        after=self.db.unit(self.uid)
        self.check('native accept transfers exactly one unchanged instance to the receiver',
                   after and int(after['AccountNumber'])==bid and
                   same_payload(before,after,allow_owner_slot=True) and self.db.escrow(self.uid) is None)
        self.check('both inventory counters match authoritative instance rows',self.db.counts_valid(aid) and self.db.counts_valid(bid))
        self.prove_accepted(sender,aid,aname,recipient,bid,bname,before,accepted,accepted_note)

    def prove_accepted(self,sender,aid,aname,recipient,bid,bname,before,accepted,accepted_note):
        recipient.wait(lambda state:state.get('unit_gift',{}).get('composer_phase')=='0' and
                       state['unit_gift'].get('pending_command')=='0',120)
        self.command(recipient,'storage_hangar',timeout=180)
        recipient.wait(lambda state:state.get('unit_gift',{}).get('pending_command')=='0' and
                       state['unit_gift'].get('composer_phase')=='0',120)
        baseline=int(self.view(recipient)['reply_sequence'])
        self.command(recipient,'gift_provenance',unit_uid=self.uid)
        self.wait_reply(recipient,9,baseline,unit=self.uid)
        state=recipient.wait(lambda current:current.get('unit_gift',{}).get('provenance_present')=='1',60)
        text=from_text_hex(state['unit_gift']['provenance_utf16_hex'])
        self.check('product provenance shows the original sender and complete Unicode note',
                   accepted_note in text and aname in text)
        record=self.db.gift_record(accepted)
        self.check('product provenance shows the authoritative offer and acceptance times for this instance',
                   int(record['UserUnitNumber'])==self.uid and int(record['SenderAccount'])==aid and
                   int(record['RecipientAccount'])==bid and int(record['State'])==1 and
                   record['AcceptedAt'] is not None and
                   '赠送时间：'+provenance_timestamp(record['OfferedAt']) in text and
                   '接收时间：'+provenance_timestamp(record['AcceptedAt']) in text)
        self.capture(recipient,'gift-provenance')
        if getattr(self.args,'prepare_off_resolution',False):
            self.native.stop('b')
            recipient,relogged_bid,_=self.native.start('b')
            self.check('the accepted native instance survives the recipient relogin',relogged_bid==bid and same_payload(before,self.db.unit(self.uid),allow_owner_slot=True))
            recipient.wait(lambda state:state.get('unit_gift',{}).get('ready')=='1' and int(state['unit_gift'].get('capabilities','0'))&4,120)
            returning_payload=self.db.unit(self.uid)
            returned=self.offer(recipient,aid,'原生机体信息退还验收')
            self.return_and_prepare_off(sender,aid,recipient,bid,returning_payload,returned)

    def return_and_prepare_off(self,recipient,recipient_id,sender,sender_id,payload,gift):
        recipient_counters=self.db.counter_values(recipient_id)
        self.command(sender,'storage_hangar',timeout=180)
        self.check('the open native sender inventory excludes the escrowed instance',
                   not any(k.startswith('storage_inventory_unit') and int(v.get('uid','0'))==self.uid
                           for k,v in sender.state().items()))
        self.select_inbox(recipient,gift)
        self.open_gift_information(recipient,gift)
        baseline=int(self.view(recipient)['reply_sequence'])
        self.command(recipient,'gift_return',gift_id=gift)
        self.wait_reply(recipient,7,baseline,gift=gift,unit=self.uid)
        self.check('the native unit-information return button restores all fifty original sender fields',
                   same_payload(payload,self.db.unit(self.uid)) and self.db.escrow(self.uid) is None)
        self.check('native return restores sender counters and leaves the unaccepted recipient counters unchanged',
                   self.db.counter_values(recipient_id)==recipient_counters and self.db.counts_valid(sender_id))
        sender.wait(lambda current:int(current.get('unit_gift',{}).get('inventory_last_added_uid','0'))==self.uid and
                    current['unit_gift'].get('inventory_sync_busy')=='0',120)
        self.check('the existing open native sender inventory receives the returned instance without relogin',
                   int(self.view(sender)['inventory_last_added_uid'])==self.uid)
        self.capture(recipient,'gift-returned-from-native-information')
        if not getattr(self.args,'prepare_off_resolution',False):return
        pending=self.offer(sender,recipient_id,'关闭新赠送后仍可接受已有赠礼')
        handoff={'gift_id':pending,'unit_uid':self.uid,'sender_id':sender_id,'recipient_id':recipient_id}
        game_test.atomic_write(self.root/'gift-off-handoff.json',json.dumps(handoff,indent=2))
        self.check('an authoritative pending gift is retained for disabled-feature resolution',
                   self.db.unit(self.uid) is None and same_payload(payload,self.db.escrow(self.uid)))

    def return_pending(self):
        record=self.db.gift_record(self.args.resume_gift_id);payload=self.db.escrow(self.uid)
        self.check('return resumes the exact reverse gift without resetting an accepted instance',
                   payload and len(payload)==50 and int(record['State'])==0 and
                   int(record['UserUnitNumber'])==self.uid and self.db.unit(self.uid) is None and
                   int(record['SenderAccount'])==int(self.binding['recipient']['account_id']) and
                   int(record['RecipientAccount'])==int(self.binding['sender']['account_id']) and
                   int(payload['AccountNumber'])==int(record['SenderAccount']))
        recipient,aid,_=self.native.start('a');sender,bid,_=self.native.start('b')
        self.receipt['actors']=list(self.native.receipt['actors']);self.save()
        self.check('return participants match the authoritative pending gift',
                   aid==int(record['RecipientAccount']) and bid==int(record['SenderAccount']))
        for actor in (recipient,sender):
            actor.wait(lambda current:current.get('unit_gift',{}).get('ready')=='1',150)
        self.return_and_prepare_off(recipient,aid,sender,bid,payload,int(record['GiftId']))
        self.native.stop('b');self.native.stop('a');self.receipt['status']='pass';self.save()

    def prepare_off_resolution(self):
        record=self.db.gift_record(self.args.resume_gift_id);payload=self.db.unit(self.uid)
        self.check('disabled handoff resumes the returned instance and preserves its fifty columns',
                   payload and len(payload)==50 and int(record['State'])==3 and
                   int(record['UserUnitNumber'])==self.uid and self.db.escrow(self.uid) is None and
                   int(record['SenderAccount'])==int(self.binding['recipient']['account_id']) and
                   int(record['RecipientAccount'])==int(self.binding['sender']['account_id']) and
                   int(payload['AccountNumber'])==int(record['SenderAccount']))
        recipient,aid,_=self.native.start('a');sender,bid,_=self.native.start('b')
        self.receipt['actors']=list(self.native.receipt['actors']);self.save()
        self.check('disabled handoff actors match the returned gift participants',
                   aid==int(record['RecipientAccount']) and bid==int(record['SenderAccount']))
        self.command(recipient,'refresh')
        for actor in (recipient,sender):
            actor.wait(lambda current:current.get('unit_gift',{}).get('ready')=='1',150)
        pending=self.offer(sender,aid,'关闭新赠送后仍可接受已有赠礼')
        handoff={'gift_id':pending,'unit_uid':self.uid,'sender_id':bid,'recipient_id':aid}
        game_test.atomic_write(self.root/'gift-off-handoff.json',json.dumps(handoff,indent=2))
        self.check('an authoritative pending gift is retained for disabled-feature resolution',
                   self.db.unit(self.uid) is None and same_payload(payload,self.db.escrow(self.uid)))
        self.native.stop('b');self.native.stop('a');self.receipt['status']='pass';self.save()

    def verify_pending_handoff(self):
        handoff=json.loads((self.root/'gift-off-handoff.json').read_text(encoding='utf-8'))
        record=self.db.gift_record(int(handoff['gift_id']));payload=self.db.escrow(self.uid)
        self.check('handoff readback preserves the pending exact fifty-column instance',
                   payload and len(payload)==50 and int(record['State'])==0 and
                   self.db.unit(self.uid) is None and int(record['UserUnitNumber'])==self.uid and
                   int(record['SenderAccount'])==int(handoff['sender_id']) and
                   int(record['RecipientAccount'])==int(handoff['recipient_id']))
        recipient,aid,_=self.native.start('a')
        self.receipt['actors']=list(self.native.receipt['actors']);self.save()
        self.check('handoff readback uses the authenticated pending recipient',aid==int(record['RecipientAccount']))
        recipient.wait(lambda current:current.get('unit_gift',{}).get('ready')=='1',150)
        self.check('new offers are enabled before the deliberate OFF restart',int(self.view(recipient)['capabilities'])&4!=0)
        self.select_inbox(recipient,int(record['GiftId']))
        self.open_gift_information(recipient,int(record['GiftId']))
        self.command(recipient,'gift_cancel')
        self.check('reading and closing pending gift information does not resolve it',
                   int(self.db.gift_record(int(record['GiftId']))['State'])==0 and same_payload(payload,self.db.escrow(self.uid)))
        self.native.stop('a');self.receipt['status']='pass';self.save()

    def accept_pending(self):
        pending=self.db.pending_gift(self.uid)
        before=self.db.escrow(self.uid)
        self.check('resume uses the exact native pending instance and original fixture participants',
                   before and len(before)==50 and int(pending['UserUnitNumber'])==self.uid and
                   int(pending['SenderAccount'])==int(self.binding['sender']['account_id']) and
                   int(pending['RecipientAccount'])==int(self.binding['recipient']['account_id']) and
                   int(before['AccountNumber'])==int(pending['SenderAccount']))
        sender,aid,aname=self.native.start('a')
        recipient,bid,bname=self.native.start('b')
        self.receipt['actors']=list(self.native.receipt['actors']);self.save()
        self.check('resumed native actors match the original pending sender and recipient',
                   aid==int(pending['SenderAccount']) and bid==int(pending['RecipientAccount']))
        for actor in (sender,recipient):
            actor.wait(lambda state:state.get('unit_gift',{}).get('ready')=='1',150)
        self.accept_and_prove(sender,aid,aname,recipient,bid,bname,before,int(pending['GiftId']),pending['Note'])
        self.native.stop('b');self.native.stop('a');self.receipt['status']='pass';self.save()

    def verify_accepted(self):
        record=self.db.gift_record(self.args.resume_gift_id)
        before=self.db.unit(self.uid)
        self.check('resume verifies the exact already accepted instance and original gift participants',
                   before and len(before)==50 and int(record['UserUnitNumber'])==self.uid and
                   int(record['State'])==1 and self.db.escrow(self.uid) is None and
                   int(record['SenderAccount'])==int(self.binding['sender']['account_id']) and
                   int(record['RecipientAccount'])==int(self.binding['recipient']['account_id']) and
                   int(before['AccountNumber'])==int(record['RecipientAccount']))
        sender,aid,aname=self.native.start('a')
        recipient,bid,bname=self.native.start('b')
        self.receipt['actors']=list(self.native.receipt['actors']);self.save()
        self.check('accepted-instance actors match the original gift participants',
                   aid==int(record['SenderAccount']) and bid==int(record['RecipientAccount']))
        for actor in (sender,recipient):
            actor.wait(lambda state:state.get('unit_gift',{}).get('ready')=='1',150)
        self.prove_accepted(sender,aid,aname,recipient,bid,bname,before,int(record['GiftId']),record['Note'])
        self.native.stop('b');self.native.stop('a');self.receipt['status']='pass';self.save()

    def disabled_resolution(self):
        handoff=json.loads((self.root/'gift-off-handoff.json').read_text(encoding='utf-8'))
        self.check('disabled resolution uses the exact previously offered instance',int(handoff['unit_uid'])==self.uid)
        recipient,aid,_=self.native.start('a');sender,bid,_=self.native.start('b')
        self.check('existing authenticated participants match the original pending gift',aid==int(handoff['recipient_id']) and bid==int(handoff['sender_id']))
        for actor in (recipient,sender):
            actor.wait(lambda state:state.get('unit_gift',{}).get('ready')=='1',120)
            view=self.view(actor)
            self.check('new offers stay disabled while existing gift controls remain available',int(view['capabilities'])&4==0)
        before=self.db.escrow(self.uid);self.check('the same complete fifty-column instance remains in escrow',before and len(before)==50)
        gift=int(handoff['gift_id']);self.select_inbox(recipient,gift)
        self.open_gift_information(recipient,gift)
        baseline=int(self.view(recipient)['reply_sequence'])
        self.command(recipient,'gift_accept',gift_id=gift);self.wait_reply(recipient,6,baseline,gift=gift,unit=self.uid)
        recipient.wait(lambda state:int(state.get('unit_gift',{}).get('inventory_last_added_uid','0'))==self.uid and state['unit_gift'].get('inventory_sync_busy')=='0',120)
        after=self.db.unit(self.uid)
        self.check('disabled-feature native accept preserves the full instance and removes escrow',after and int(after['AccountNumber'])==aid and same_payload(before,after,allow_owner_slot=True) and self.db.escrow(self.uid) is None)
        self.check('disabled-feature native accept retains consistent inventory counters',self.db.counts_valid(aid) and self.db.counts_valid(bid))
        self.capture(recipient,'gift-disabled-accepted')
        self.command(sender,'storage_hangar',timeout=180)
        self.capture(sender,'gift-disabled-sender-hangar')
        self.native.stop('b');self.native.stop('a');self.receipt['status']='pass';self.save()

    def verify_disabled_accepted(self):
        handoff=json.loads((self.root/'gift-off-handoff.json').read_text(encoding='utf-8'))
        gift=int(self.args.resume_gift_id);record=self.db.gift_record(gift);payload=self.db.unit(self.uid)
        self.check('readback verifies the exact already accepted disabled-feature gift without accepting again',
                   gift==int(handoff['gift_id']) and self.uid==int(handoff['unit_uid']) and payload and len(payload)==50 and
                   int(record['State'])==1 and int(record['UserUnitNumber'])==self.uid and self.db.escrow(self.uid) is None and
                   int(record['SenderAccount'])==int(handoff['sender_id']) and
                   int(record['RecipientAccount'])==int(handoff['recipient_id']) and
                   int(payload['AccountNumber'])==int(handoff['recipient_id']))
        recipient,aid,aname=self.native.start('a');sender,bid,bname=self.native.start('b')
        self.receipt['actors']=list(self.native.receipt['actors']);self.save()
        self.check('readback authenticates both original disabled-feature gift participants',
                   aid==int(handoff['recipient_id']) and bid==int(handoff['sender_id']))
        for actor in (recipient,sender):
            actor.wait(lambda state:state.get('unit_gift',{}).get('ready')=='1',120)
            self.check('readback leaves new offers disabled for both native clients',int(self.view(actor)['capabilities'])&4==0)
        self.command(sender,'storage_hangar',timeout=180)
        self.capture(sender,'gift-disabled-sender-hangar')
        self.prove_accepted(sender,bid,bname,recipient,aid,aname,payload,gift,record['Note'])
        self.check('disabled-feature readback preserves the accepted native instance and both counters',
                   same_payload(payload,self.db.unit(self.uid)) and self.db.counts_valid(aid) and self.db.counts_valid(bid))
        self.native.stop('b');self.native.stop('a');self.receipt['status']='pass';self.save()

    def picker_pagination(self):
        sender,account,_=self.native.start('a')
        recipient=int(self.binding['recipient']['account_id'])
        self.check('picker actor is the bound existing sender',account==int(self.binding['sender']['account_id']))
        before=self.db.unit(self.uid)
        self.check('picker fixture is a genuine owned instance beyond the first page',
                   before and int(before['AccountNumber'])==account and int(before['UserUnitSlotNumber'])>36)
        expected={int(row[0]):int(row[1]) for row in self.db.connection.cursor().execute(
            'SELECT UserUnitSlotNumber,UserUnitNumber FROM dbo.tbl_UserUnits WHERE AccountNumber=?',account)}

        def ready(state):
            p=state.get('gift_picker',{})
            return p.get('open')=='1' and p.get('selecting')=='1' and int(p.get('page_count','0'))>0 and p.get('pointer_phase')=='0'
        def open_picker():
            self.command(sender,'gift_home_context' if self.args.friend_surface=='home' else 'gift_context',
                         target_id=recipient,cursor_x=480,cursor_y=263)
            sender.wait(lambda s:int(s.get('unit_gift',{}).get('control_flags','0'))&2,60)
            self.command(sender,'gift_open',target_id=recipient)
            return sender.wait(ready,60)['gift_picker']
        def click(value,page,label):
            sender.wait(ready,30)
            self.command(sender,'gift_picker_page',target_id=value)
            p=sender.wait(lambda s:ready(s) and s['gift_picker'].get('grid_page')==str(page),20)['gift_picker']
            visible={int(p['slot'+str(i)]):int(p['uid'+str(i)]) for i in range(36) if int(p['slot'+str(i)])}
            self.check(label, p['pager_page']==str(page) and
                       all(page*36<slot<=(page+1)*36 and uid==expected.get(slot,0) for slot,uid in visible.items()) and
                       int(p['pointer_position_reads'])>0 and int(p['pointer_state_reads'])>0)
            return p
        p=open_picker();count=int(p['page_count'])
        self.check('native picker exposes enough real pages for group-boundary coverage',count>=12)
        def go(page):
            for _ in range(count+2):
                current=int(sender.wait(ready,30)['gift_picker']['grid_page'])
                if current==page:return
                if current//10==page//10:click(page+1,page,'native page circle '+str(page+1));return
                if current//10<page//10:
                    edge=min(current-current%10+9,count-1)
                    if current!=edge:click(edge+1,edge,'native last circle before next group')
                    else:click(4294967295,current+1,'native next arrow across page group')
                else:
                    edge=current-current%10
                    if current!=edge:click(edge+1,edge,'native first circle before previous group')
                    else:click(4294967294,current-1,'native previous arrow across page group')
            raise game_test.GameTestError('native picker could not reach its requested page')
        go(0)
        click(4294967294,0,'previous arrow at first page is inert')
        click(4294967295,1,'next arrow visits the adjacent second page')
        self.capture(sender,'gift-picker-second-page')
        click(4294967294,0,'previous arrow returns to the adjacent first page')
        for page in (3,8,9):go(page)
        click(4294967295,10,'next arrow crosses from page ten to eleven')
        self.capture(sender,'gift-picker-eleventh-page')
        click(4294967294,9,'previous arrow crosses from page eleven to ten')
        for page in (11,count-1):go(page)
        click(4294967295,count-1,'next arrow at last page is inert')
        self.capture(sender,'gift-picker-last-page')
        go(0)
        target=(int(before['UserUnitSlotNumber'])-1)//36
        for attempt in range(2):
            if attempt:open_picker()
            go(target)
            p=sender.wait(ready,30)['gift_picker']
            self.check('chosen instance is visible before the native double-click',str(self.uid) in [p['uid'+str(i)] for i in range(36)])
            self.command(sender,'gift_picker_select',unit_uid=self.uid)
            sender.wait(lambda s:s.get('unit_gift',{}).get('selected_unit_uid')==str(self.uid) and
                        int(s['unit_gift'].get('control_flags','0'))&32,120)
            self.capture(sender,'gift-picker-selected-'+str(attempt))
            self.command(sender,'gift_cancel')
            sender.wait(lambda s:s.get('unit_gift',{}).get('composer_phase')=='0' and
                        s.get('gift_picker',{}).get('pointer_phase')=='0',30)
            self.check('selection cancellation preserves every native instance field',same_payload(before,self.db.unit(self.uid)))
        self.native.stop('a');self.receipt['actors']=list(self.native.receipt['actors'])
        self.receipt['status']='pass';self.save()

    def close(self):
        for label in list(self.native.running):
            actor,future,_=self.native.running[label]
            abort=game_test.ini_parser();abort['run']={'id':actor.metadata['run_id']}
            game_test.write_ini(actor.directory/'friend-abort.ini',abort)
            try:future.result(timeout=180)
            except Exception:pass
        self.native.pool.shutdown(wait=True);self.db.close()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--test-root',type=Path,required=True)
    parser.add_argument('--client-template',type=Path,required=True)
    parser.add_argument('--stage',type=Path,required=True)
    parser.add_argument('--work-root',type=Path,required=True)
    parser.add_argument('--fixture-binding',type=Path,required=True)
    parser.add_argument('--sql-readback-ini',type=Path,
                        help='existing private DB Member credential INI for read-only SQL metadata verification')
    parser.add_argument('--pair',choices=('primary_friend_pair','all_payload_groups_pair'),default='all_payload_groups_pair')
    parser.add_argument('--asset-baseline',type=Path)
    parser.add_argument('--target-host',required=True)
    parser.add_argument('--friend-surface',choices=('home','messenger'),default='home',
                        help='use the top-right USER LIST or the older messenger context')
    parser.add_argument('--phase',choices=('enabled','accept-pending','verify-accepted','return-pending','prepare-off-resolution','verify-pending-handoff','disabled-resolution','verify-disabled-accepted','picker-pagination'),default='enabled')
    parser.add_argument('--prepare-off-resolution',action='store_true',help='retain one genuine pending gift for a later disabled-resolution phase')
    parser.add_argument('--resume-gift-id',type=int,help='accepted gift to verify for source information and restart persistence')
    args=parser.parse_args()
    if args.phase in ('verify-accepted','return-pending','prepare-off-resolution','verify-disabled-accepted') and (args.resume_gift_id is None or args.resume_gift_id<=0):
        parser.error(args.phase+' requires a positive --resume-gift-id')
    campaign=GiftCampaign(args)
    try:
        if args.phase=='enabled':campaign.enabled()
        elif args.phase=='picker-pagination':campaign.picker_pagination()
        elif args.phase=='accept-pending':campaign.accept_pending()
        elif args.phase=='verify-accepted':campaign.verify_accepted()
        elif args.phase=='return-pending':campaign.return_pending()
        elif args.phase=='prepare-off-resolution':campaign.prepare_off_resolution()
        elif args.phase=='verify-pending-handoff':campaign.verify_pending_handoff()
        elif args.phase=='verify-disabled-accepted':campaign.verify_disabled_accepted()
        else:campaign.disabled_resolution()
    except Exception as error:
        campaign.receipt.update(status='fail',error_type=type(error).__name__,error=str(error));campaign.save()
        for actor,future,_ in list(campaign.native.running.values()):
            if future.done():continue
            try:campaign.command(actor,'capture',label='gift-failure',timeout=15)
            except Exception:pass
        raise
    finally:campaign.close()
    print(json.dumps(campaign.receipt,ensure_ascii=True,indent=2))


if __name__=='__main__':main()
