"""Native local-SQL calendar gift contracts in a task-owned synthetic database.

This is a database fixture, not native player/UI acceptance. The selected test
copy and every online SQL file are verified before creating any fixture data.
"""
from __future__ import annotations
import argparse
import concurrent.futures
import copy
import json
from pathlib import Path
import sys
import uuid

REPO=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(REPO));sys.path.insert(0,str(REPO/'tools'))
import game_test
from game_test_friend_gifts import InventoryReadback
from owner.calendar_gifts.repository import Repository
from owner.calendar_gifts import model


def fixture(test_root,readback_ini):
    import pyodbc
    guard=InventoryReadback(test_root,readback_ini)
    native=json.loads((test_root/'database/native-sql.json').read_text(encoding='utf-8'))
    credential=game_test.read_ini(readback_ini)['DB Member']
    quote=lambda v:'{'+str(v).replace('}','}}')+'}'
    name='SDGOCalendarGiftFixture_'+uuid.uuid4().hex[:10]
    def connect():
        return pyodbc.connect('DRIVER={SQL Server};SERVER=127.0.0.1,'+str(native['port'])+';DATABASE='+quote(name)+
            ';UID='+quote(credential.get('ID',credential.get('id')))+';PWD='+quote(credential.get('PWD',credential.get('pwd')))+';',
            autocommit=True,timeout=10)
    c=guard.connection.cursor();connections=[];created=False;result={'schema':1,'passed':False,'checks':{}}
    def check(label,condition):
        result['checks'][label]=bool(condition)
        if not condition:raise AssertionError('calendar native SQL contract failed: '+label)
    try:
        data=Path(c.execute("SELECT physical_name FROM sys.master_files WHERE database_id=DB_ID() AND type=0").fetchval()).parent.resolve()
        if not data.is_relative_to(test_root.resolve()):raise ValueError('fixture data path is outside the selected native copy')
        mdf=data/(name+'.mdf');ldf=data/(name+'.ldf')
        if mdf.exists() or ldf.exists():raise ValueError('fixture file path is already occupied')
        q=lambda s:"N'"+str(s).replace("'","''")+"'"
        c.execute('CREATE DATABASE ['+name+'] ON PRIMARY(NAME='+q(name)+',FILENAME='+q(mdf)+',SIZE=8MB) LOG ON(NAME='+q(name+'_log')+',FILENAME='+q(ldf)+',SIZE=8MB)')
        created=True;connection=connect();connections.append(connection);db=connection.cursor()
        db.execute('''CREATE TABLE dbo.tbl_Users(AccountNumber int PRIMARY KEY,PilotName varchar(16));
          CREATE TABLE dbo.tbl_Units(UnitNumber int PRIMARY KEY,UnitGradeNumber tinyint,UnitGradeNumber2 tinyint,UnitModelName nvarchar(64));
          CREATE TABLE dbo.tbl_ItemCategory(CatalogueID int PRIMARY KEY,Name nvarchar(64),RefIndex int,TimeLimit tinyint,Option1 int,Option2 int);
          CREATE TABLE dbo.tbl_UserUnits(UserUnitNumber bigint IDENTITY PRIMARY KEY,AccountNumber int,UnitNumber int,TimeLimit tinyint DEFAULT 0,RunOut tinyint DEFAULT 0);
          CREATE TABLE dbo.tbl_UserDropItemBox(UserDropItemIdx bigint IDENTITY PRIMARY KEY,AccountNumber int,ItemCategory int,Amount int,DropConditionID int);
          INSERT dbo.tbl_Users VALUES(1,'FixtureOne'),(2,'FixtureTwo'),(3,'FixtureThree'),(4,'FixtureFour'),(5,'FixtureFive');''')
        units=[]
        for n,grade in enumerate(model.GRADES):
            for variant in range(2):
                uid=1000+n*2+variant;units.append(uid)
                db.execute('INSERT dbo.tbl_Units VALUES(?,?,?,?)',uid,*grade,'Fixture '+str(uid))
                db.execute('INSERT dbo.tbl_ItemCategory VALUES(?,?,?,0,1,2)',1100000+uid,'Fixture '+str(uid),uid)
        db.execute("INSERT dbo.tbl_ItemCategory VALUES(2900001,N'Fixture item',1,0,0,0),(2900002,N'Rollback item',2,0,0,0)")
        db.execute('INSERT dbo.tbl_UserUnits(AccountNumber,UnitNumber) VALUES(1,?)',units[0])
        repository=Repository(connection)
        occurrence={'key':'birthday:2026','date':'2026-11-03','rewards':model.default_rewards(True)}
        before=db.execute('SELECT COUNT(*) FROM dbo.tbl_UserDropItemBox').fetchval()
        preview=repository.deliver(occurrence,1,preview=True)
        check('fresh_preview_is_readonly_without_install',preview['delivery_count']==12 and
              db.execute("SELECT SCHEMA_ID(N'sdgo_calendar_gifts')").fetchval() is None and
              db.execute('SELECT COUNT(*) FROM dbo.tbl_UserDropItemBox').fetchval()==before)
        repository.install();repository.install()
        first=repository.deliver(occurrence,1)
        check('twelve_grades_enter_native_storage',first['delivery_count']==12 and db.execute('SELECT COUNT(*) FROM sdgo_calendar_gifts.Delivery WHERE AccountNumber=1').fetchval()==12)
        ids=[r[0] for r in db.execute('SELECT UnitNumber FROM sdgo_calendar_gifts.Delivery WHERE AccountNumber=1')]
        check('native_owned_and_same_bundle_duplicates_excluded',len(set(ids))==12 and units[0] not in ids)
        changed=copy.deepcopy(occurrence);changed['date']='2026-11-04';changed['rewards']['items']=[{'category_id':2900001,'amount':3}]
        check('date_and_rule_edit_never_reissues_same_annual_gift',repository.deliver(changed,1)['status']=='already_recorded' and
              db.execute('SELECT COUNT(*) FROM dbo.tbl_UserDropItemBox WHERE AccountNumber=1').fetchval()==12)
        holiday={'key':'new_year:2026','date':'2026-01-01','rewards':model.default_rewards()}
        repository.deliver(holiday,1)
        new_unit=db.execute("SELECT UnitNumber FROM sdgo_calendar_gifts.Delivery WHERE EventKey='new_year:2026' AND AccountNumber=1").fetchval()
        check('unclaimed_native_gifts_are_not_awarded_again',new_unit not in ids and new_unit!=units[0])
        # Native drop-box triggers remain enabled, including failure rollback.
        db.execute("CREATE TRIGGER dbo.fixture_drop_failure ON dbo.tbl_UserDropItemBox AFTER INSERT AS IF EXISTS(SELECT 1 FROM inserted WHERE ItemCategory=2900002) RAISERROR('fixture atomic failure',16,1)")
        failing=copy.deepcopy(occurrence);failing['rewards']['items']=[{'category_id':2900002,'amount':1}]
        rejected=False
        try:repository.deliver(failing,2)
        except Exception:rejected=True
        check('later_native_failure_rolls_back_all_units_and_receipt',rejected and db.execute('SELECT COUNT(*) FROM dbo.tbl_UserDropItemBox WHERE AccountNumber=2').fetchval()==0 and
              db.execute('SELECT COUNT(*) FROM sdgo_calendar_gifts.Receipt WHERE AccountNumber=2').fetchval()==0)
        imported=repository.import_legacy([{'account_id':2,'date':'2026-03-04'},{'account_id':1,'date':'2026-01-01'}])
        check('legacy_completed_birthdays_block_resend_without_inventing_rewards',imported==1 and repository.deliver(occurrence,2)['status']=='already_recorded' and
              db.execute('SELECT COUNT(*) FROM dbo.tbl_UserDropItemBox WHERE AccountNumber=2').fetchval()==0 and
              db.execute('SELECT Status FROM sdgo_calendar_gifts.Receipt WHERE AccountNumber=1').fetchval()=='delivered')
        def concurrent_delivery():
            own=connect()
            try:return Repository(own).deliver(occurrence,3)
            finally:own.close()
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            outcomes=list(pool.map(lambda _:concurrent_delivery(),range(2)))
        check('two_workers_serialize_one_annual_bundle',sorted(r['status'] for r in outcomes)==['already_recorded','delivered'] and
              db.execute('SELECT COUNT(*) FROM dbo.tbl_UserDropItemBox WHERE AccountNumber=3').fetchval()==12)
        class LostCommit:
            autocommit=True
            def cursor(self):
                class Cursor:
                    def __init__(self):self.inner=connection.cursor()
                    def execute(self,sql,*args):
                        self.inner.execute(sql,*args)
                        if sql=='COMMIT TRANSACTION;':raise RuntimeError('fixture lost commit acknowledgement')
                        return self
                    def __iter__(self):return iter(self.inner)
                    def __getattr__(self,name):return getattr(self.inner,name)
                return Cursor()
        unknown=False
        try:Repository(LostCommit()).deliver(occurrence,4)
        except RuntimeError:unknown=True
        check('lost_commit_ack_recovers_original_receipt_without_resend',unknown and repository.deliver(occurrence,4)['status']=='already_recorded' and
              db.execute('SELECT COUNT(*) FROM dbo.tbl_UserDropItemBox WHERE AccountNumber=4').fetchval()==12)
        # Give the last synthetic account all registered types.
        for uid in units:db.execute('INSERT dbo.tbl_UserUnits(AccountNumber,UnitNumber) VALUES(5,?)',uid)
        check('fully_owned_catalog_records_exhaustion_without_duplicates',repository.deliver(occurrence,5)['status']=='exhausted' and
              db.execute('SELECT COUNT(*) FROM dbo.tbl_UserDropItemBox WHERE AccountNumber=5').fetchval()==0)
        item_only=copy.deepcopy(holiday);item_only['key']='items:2026';item_only['rewards']['mode']='none';item_only['rewards']['items']=[{'category_id':2900001,'amount':7}]
        check('configurable_native_items_keep_their_exact_quantity',repository.deliver(item_only,5)['delivery_count']==1 and
              db.execute('SELECT Amount FROM dbo.tbl_UserDropItemBox WHERE AccountNumber=5').fetchval()==7)
        for account in range(6,206):db.execute('INSERT dbo.tbl_Users VALUES(?,?)',account,'BulkFixture')
        for account in range(1,201):db.execute("INSERT sdgo_calendar_gifts.Receipt VALUES('bulk:2026',?,?,?,'exhausted',SYSUTCDATETIME())",account,'0'*64,'2026-11-03')
        bulk={**holiday,'key':'bulk:2026','group':'bulk','account_id':None}
        waiting=repository.accounts(bulk,{'groups':{'bulk':list(range(1,207))}})
        check('completed_private_audiences_do_not_starve_later_batches',waiting==list(range(201,206)) and
              repository.accounts({**occurrence,'account_id':1},{'groups':{}})==[])
        check('caller_session_has_no_leaked_transaction',db.execute('SELECT @@TRANCOUNT').fetchval()==0)
        result['passed']=True
    finally:
        for connection in connections:connection.close()
        if created:
            c.execute('ALTER DATABASE ['+name+'] SET SINGLE_USER WITH ROLLBACK IMMEDIATE');c.execute('DROP DATABASE ['+name+']')
            result['cleanup']=c.execute('SELECT DB_ID(?)',name).fetchval() is None and not mdf.exists() and not ldf.exists()
        guard.close()
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--test-root',type=Path,required=True)
    parser.add_argument('--sql-readback-ini',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();result=fixture(args.test_root.resolve(),args.sql_readback_ini)
    args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,indent=2));return 0 if result['passed'] and result.get('cleanup') else 1


if __name__=='__main__':raise SystemExit(main())
