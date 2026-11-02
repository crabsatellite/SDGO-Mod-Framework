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
from game_test_friends import Campaign, assert_receipt


def text_hex(value: str) -> str:
    raw=value.encode('utf-16-le')
    return ''.join(f'{unit:04X}' for unit in struct.unpack('<'+'H'*(len(raw)//2),raw))


def from_text_hex(value: str) -> str:
    if len(value)%4:raise game_test.GameTestError('malformed native UTF-16 receipt')
    return b''.join(int(value[i:i+4],16).to_bytes(2,'little') for i in range(0,len(value),4)).decode('utf-16-le')


def provenance_timestamp(value: dt.datetime) -> str:
    # SQL records UTC datetime2 values; preserve that meaning for naive rows.
    if value.tzinfo is not None:
        value=value.astimezone(dt.timezone.utc)
    return value.strftime('%Y-%m-%d %H:%M UTC')


class InventoryReadback:
    def __init__(self,test_root:Path):
        import pyodbc
        self.root=test_root.resolve()
        if not (self.root/'TEST-ONLY.txt').is_file():
            raise game_test.GameTestError('gift verification requires a marked local test copy')
        native=json.loads((self.root/'database/native-sql.json').read_text(encoding='utf-8'))
        if Path(native['root']).resolve()!=self.root:
            raise game_test.GameTestError('native SQL belongs to another test copy')
        settings=game_test.read_ini(self.root/'server/Server/WorldServer/WorldServer.ini')['DB Game']
        quote=lambda value:'{'+str(value).replace('}','}}')+'}'
        self.connection=pyodbc.connect(
            'DRIVER={SQL Server};SERVER=127.0.0.1,'+str(int(native['port']))+
            ';DATABASE='+quote(settings['DBName'])+';UID='+quote(settings.get('ID',settings.get('id')))+
            ';PWD='+quote(settings.get('PWD',settings.get('pwd')))+';',
            timeout=10,autocommit=True)
        cursor=self.connection.cursor()
        if int(cursor.execute("SELECT CAST(SERVERPROPERTY('ProcessID') AS int)").fetchval())!=int(native['pid']):
            raise game_test.GameTestError('SQL process identity does not match the selected native test')
        paths=cursor.execute('SELECT f.physical_name FROM sys.master_files f JOIN sys.databases d ON d.database_id=f.database_id WHERE d.state=0').fetchall()
        if not all(Path(row[0]).resolve().is_relative_to(self.root) for row in paths):
            raise game_test.GameTestError('an online SQL file lies outside the selected test copy')

    def unit(self,uid:int):
        cursor=self.connection.cursor()
        rows=cursor.execute('SELECT * FROM dbo.tbl_UserUnits WHERE UserUnitNumber=?',uid).fetchall()
        columns=[column[0] for column in cursor.description]
        if len(rows)>1:raise game_test.GameTestError('native unit UID is ambiguous')
        return dict(zip(columns,rows[0])) if rows else None

    def escrow(self,uid:int):
        cursor=self.connection.cursor()
        rows=cursor.execute('SELECT * FROM sdgo_friend_unit_gift.EscrowUnit WHERE UserUnitNumber=?',uid).fetchall()
        columns=[column[0] for column in cursor.description]
        if len(rows)>1:raise game_test.GameTestError('gift escrow UID is ambiguous')
        return dict(zip(columns,rows[0])) if rows else None

    def counts_valid(self,account:int):
        row=self.connection.cursor().execute(
            'SELECT u.UserUnitTotalCnt,u.UserUnitKindCnt,'
            '(SELECT COUNT(*) FROM dbo.tbl_UserUnits x WHERE x.AccountNumber=u.AccountNumber),'
            '(SELECT COUNT(DISTINCT x.UnitNumber) FROM dbo.tbl_UserUnits x WHERE x.AccountNumber=u.AccountNumber) '
            'FROM dbo.tbl_Users u WHERE u.AccountNumber=?',account).fetchone()
        return bool(row) and int(row[0])==int(row[2]) and int(row[1])==int(row[3])

    def gift_record(self,gift:int):
        cursor=self.connection.cursor()
        rows=cursor.execute(
            'SELECT UserUnitNumber,SenderAccount,RecipientAccount,State,OfferedAt,AcceptedAt '
            'FROM sdgo_friend_unit_gift.GiftOffer WHERE GiftId=?',gift).fetchall()
        if len(rows)!=1:raise game_test.GameTestError('authoritative gift record is missing or ambiguous')
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
            if destination.exists() and destination.read_bytes()!=source.read_bytes():
                raise game_test.GameTestError('gift actor profile identity changed')
            if not destination.exists():shutil.copy2(source,destination)
        self.native=Campaign(args.client_template,args.stage,self.root,args.target_host,args.asset_baseline,
                             extra_mods=('friend_list_client_fix','friend_unit_gift_client'),peer_labels=())
        self.db=InventoryReadback(args.test_root)
        self.receipt={'schema':1,'status':'running','checks':[],'actors':[]}
        self.save()

    def save(self):
        game_test.atomic_write(self.root/'gift-campaign.json',json.dumps(self.receipt,ensure_ascii=True,indent=2))

    def check(self,name,condition,**details):
        self.receipt['checks'].append({'name':name,'passed':bool(condition),**details});self.save()
        if not condition:raise game_test.GameTestError('native unit gift check failed: '+name)

    @staticmethod
    def view(actor):
        state=actor.state()
        if not state or state.get('unit_gift',{}).get('valid')!='1':
            raise game_test.GameTestError('no correlated product gift observer snapshot')
        return state['unit_gift']

    @staticmethod
    def rows(state):return [row for key,row in state.items() if key.startswith('unit_gift_row')]

    def command(self,actor,name,**fields):
        return assert_receipt(actor.command(name,**fields))

    def wait_reply(self,actor,command,baseline,*,gift=None,unit=None):
        def matches(state):
            view=state.get('unit_gift',{})
            return (view.get('valid')=='1' and int(view.get('reply_sequence','0'))>baseline and
                    int(view.get('last_reply_command','0'))==command and
                    int(view.get('last_reply_status','-1'))==0 and
                    (gift is None or int(view.get('last_reply_gift_id','0'))==gift) and
                    (unit is None or int(view.get('last_reply_unit_uid','0'))==unit))
        return actor.wait(matches,120)

    def open_draft(self,sender,recipient,note):
        self.command(sender,'gift_context',target_id=recipient)
        sender.wait(lambda state:int(state.get('unit_gift',{}).get('control_flags','0'))&2,60)
        self.command(sender,'gift_open',target_id=recipient)
        sender.wait(lambda state:state.get('unit_gift',{}).get('composer_phase')=='1' and
                    int(state['unit_gift'].get('control_flags','0'))&8,60)
        self.command(sender,'gift_select_unit',unit_uid=self.uid)
        sender.wait(lambda state:int(state.get('unit_gift',{}).get('selected_unit_uid','0'))==self.uid,60)
        self.command(sender,'gift_note',note_utf16_hex=text_hex(note))
        baseline=int(self.view(sender)['reply_sequence'])
        self.command(sender,'gift_preview')
        self.wait_reply(sender,10,baseline,unit=self.uid)
        sender.wait(lambda state:state.get('unit_gift',{}).get('composer_phase')=='3',60)

    def offer(self,sender,recipient,note):
        self.open_draft(sender,recipient,note)
        baseline=int(self.view(sender)['reply_sequence'])
        self.command(sender,'gift_confirm')
        received=self.wait_reply(sender,2,baseline,unit=self.uid)
        gift=int(received['unit_gift']['last_reply_gift_id'])
        self.check('confirmed native gift receives a nonzero authoritative gift ID',gift>0)
        return gift

    def select_inbox(self,recipient,gift):
        self.command(recipient,'gift_storage')
        baseline=int(self.view(recipient)['reply_sequence'])
        self.command(recipient,'gift_inbox')
        state=self.wait_reply(recipient,3,baseline)
        found=any(int(row['gift_id'])==gift and int(row['unit_uid'])==self.uid for row in self.rows(state))
        self.check('receiver sees the exact offered instance in the product Storage view',found)
        self.command(recipient,'gift_select',gift_id=gift)
        recipient.wait(lambda current:int(current.get('unit_gift',{}).get('selected_gift_id','0'))==gift,60)

    def enabled(self):
        sender,aid,_=self.native.start('a');recipient,bid,_=self.native.start('b')
        self.receipt['actors']=list(self.native.receipt['actors']);self.save()
        for actor in (sender,recipient):
            actor.wait(lambda state:state.get('unit_gift',{}).get('valid')=='1' and
                       state['unit_gift'].get('ready')=='1' and
                       int(state['unit_gift'].get('capabilities','0'))&4,120)
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
        self.native.state_friend(sender,bid,presence=0)
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
        baseline=int(self.view(recipient)['reply_sequence'])
        self.command(recipient,'gift_return',gift_id=returned)
        self.wait_reply(recipient,7,baseline,gift=returned,unit=self.uid)
        self.check('native return restores the same full payload and original acquisition date',same_payload(before,self.db.unit(self.uid)))
        self.check('return leaves no duplicate escrow row',self.db.escrow(self.uid) is None)

        accepted_note='出击顺利！🎁\n赠礼来源 '+uuid.uuid4().hex[:8]+'\n'
        accepted_note+='愿'*(200-len(accepted_note.encode('utf-16-le'))//2)
        accepted=self.offer(sender,bid,accepted_note)
        self.select_inbox(recipient,accepted)
        baseline=int(self.view(recipient)['reply_sequence'])
        self.command(recipient,'gift_accept',gift_id=accepted)
        self.wait_reply(recipient,6,baseline,gift=accepted,unit=self.uid)
        after=self.db.unit(self.uid)
        self.check('native accept transfers exactly one unchanged instance to the receiver',
                   after and int(after['AccountNumber'])==bid and
                   same_payload(before,after,allow_owner_slot=True) and self.db.escrow(self.uid) is None)
        self.check('both inventory counters match authoritative instance rows',self.db.counts_valid(aid) and self.db.counts_valid(bid))
        baseline=int(self.view(recipient)['reply_sequence'])
        self.command(recipient,'gift_provenance',unit_uid=self.uid)
        self.wait_reply(recipient,9,baseline,unit=self.uid)
        state=recipient.wait(lambda current:current.get('unit_gift',{}).get('provenance_present')=='1',60)
        text=from_text_hex(state['unit_gift']['provenance_utf16_hex'])
        self.check('product provenance shows the original sender and complete Unicode note',
                   accepted_note in text and self.binding['sender']['nickname'] in text)
        record=self.db.gift_record(accepted)
        self.check('product provenance shows the authoritative offer and acceptance times for this instance',
                   int(record['UserUnitNumber'])==self.uid and int(record['SenderAccount'])==aid and
                   int(record['RecipientAccount'])==bid and int(record['State'])==1 and
                   record['AcceptedAt'] is not None and
                   '赠送时间：'+provenance_timestamp(record['OfferedAt']) in text and
                   '接收时间：'+provenance_timestamp(record['AcceptedAt']) in text)
        self.native.stop('b');self.native.stop('a')
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
    parser.add_argument('--pair',choices=('primary_friend_pair','all_payload_groups_pair'),default='all_payload_groups_pair')
    parser.add_argument('--asset-baseline',type=Path)
    parser.add_argument('--target-host',required=True)
    args=parser.parse_args();campaign=GiftCampaign(args)
    try:campaign.enabled()
    except Exception as error:
        campaign.receipt.update(status='fail',error_type=type(error).__name__,error=str(error));campaign.save();raise
    finally:campaign.close()
    print(json.dumps(campaign.receipt,ensure_ascii=True,indent=2))


if __name__=='__main__':main()
