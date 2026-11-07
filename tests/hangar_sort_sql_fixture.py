"""Explicit opt-in SQL fixture in two newly-created, disposable databases.

Never invokes sorting against the selected server's real game/member tables.
"""
from pathlib import Path
import argparse,configparser,io,sys,tempfile,uuid
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from manager import storage
from manager.database import connect
from manager.engines.hangar.server import Server,HEADER,PARAMETERS,column_type,ident


def run(ini):
    assert column_type(('Text','nvarchar',80,0,0,'Thai_CI_AS',True,False,False))=='nvarchar(40) COLLATE Thai_CI_AS'
    rejected_collation=False
    try:column_type(('Text','nvarchar',80,0,0,'Thai_CI_AS;DROP',True,False,False))
    except ValueError:rejected_collation=True
    assert rejected_collation
    tag=uuid.uuid4().hex[:12]
    game='SDGO_Hangar_TestGame_'+tag;common='SDGO_Hangar_TestCommon_'+tag;billing='SDGO_Hangar_TestBilling_'+tag
    names=[game,common,billing]
    cn=connect(ini,'DB Member',autocommit=True)
    original=storage.ini_values(Path(ini).read_bytes())
    cur=cn.cursor()
    try:
        for name in names:cur.execute('CREATE DATABASE '+ident(name))
        cur.execute('USE '+ident(game))
        cur.execute('CREATE TABLE dbo.tbl_Users(AccountNumber int PRIMARY KEY,GoUserUnitNum bigint); CREATE TABLE dbo.tbl_Units(UnitNumber int PRIMARY KEY,UnitGradeNumber int,UnitGradeNumber2 int); CREATE TABLE dbo.tbl_UserUnits(UserUnitNumber bigint PRIMARY KEY,AccountNumber int,UserUnitSlotNumber int,UnitNumber int,DateGet datetime,LockState tinyint,BattleShipCount int,UnitLevelNumber tinyint,Paint int,CONSTRAINT UniqueSlots UNIQUE(AccountNumber,UserUnitSlotNumber)); CREATE TABLE dbo.Untouched(GB bigint,DropItems int); INSERT dbo.Untouched VALUES(12345,9); INSERT dbo.tbl_Users VALUES(1,1001),(2,2001); INSERT dbo.tbl_Units VALUES(10,1,2),(20,4,2),(30,3,4); INSERT dbo.tbl_UserUnits VALUES(1001,1,1,10,\'20260101\',1,1,9,77),(1002,1,3,20,\'20260102\',0,0,1,12),(1003,1,4,30,\'20260103\',0,0,1,13),(2001,2,8,20,\'20260104\',1,1,8,88)')
        cur.execute("""ALTER TABLE dbo.tbl_Users ADD ExpandSlotCount int NOT NULL DEFAULT 0;
            ALTER TABLE dbo.tbl_UserUnits ADD
                TimeLimit tinyint NOT NULL DEFAULT 0,
                RunOut tinyint NOT NULL DEFAULT 0,
                Forbidden tinyint NOT NULL DEFAULT 0,
                AnsiNote varchar(24) COLLATE Thai_CI_AS NULL,
                FixedCode char(4) COLLATE Thai_CI_AS NULL,
                UnicodeNote nvarchar(40) COLLATE Thai_CI_AS NULL;
            CREATE TABLE dbo.tbl_UnitGradeInfo(UnitGradeNumber int,UnitGradeNumber2 int,DeleteUnitPoint int,PRIMARY KEY(UnitGradeNumber,UnitGradeNumber2));
            INSERT dbo.tbl_UnitGradeInfo VALUES(1,2,400),(4,2,1600),(3,4,1200);
            CREATE TABLE dbo.NativeDeleted(UserUnitNumber bigint)""")
        cur.execute('CREATE PROCEDURE dbo.sp_DeleteUserUnit @a int,@u bigint,@t int,@d tinyint,@ot int OUTPUT,@ti tinyint OUTPUT,@sh tinyint OUTPUT,@r tinyint OUTPUT,@s bigint OUTPUT,@c int OUTPUT,@sl int OUTPUT,@ret int OUTPUT AS BEGIN SET NOCOUNT ON; DELETE dbo.tbl_UserUnits WHERE AccountNumber=@a AND UserUnitNumber=@u AND UnitNumber=@t; SET @ret=CASE WHEN @@ROWCOUNT=1 THEN 0 ELSE 1 END; IF @ret=0 INSERT dbo.NativeDeleted VALUES(@u); END')
        cur.execute('USE '+ident(billing));cur.execute('CREATE TABLE dbo.tbls_AccountPocket(f_idx int PRIMARY KEY,f_GP int); INSERT dbo.tbls_AccountPocket VALUES(1,10000),(2,777)')
        cur.execute('USE '+ident(common))
        cur.execute("CREATE TABLE dbo.tbl_Account_Pilot(f_idx int PRIMARY KEY,f_pilot nvarchar(20)); INSERT dbo.tbl_Account_Pilot VALUES(1,N'FixtureA'),(2,N'FixtureB')")
        cur.execute('CREATE PROCEDURE dbo.sp_SendMsgGame '+HEADER+' AS SET @retVal=77; SET @v_outIdx=42; RETURN;')
        # A short-lived INI stays local and is removed by TemporaryDirectory.
        root=Path(__file__).resolve().parents[1]/'build/delivery'
        root.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='hangar-fixture-',dir=root) as temp:
            temp=Path(temp).resolve();assert temp.is_relative_to(root.resolve())
            for section,name in [('DB Game',game),('DB Member',common),('DB StoreBilling',billing)]:original[section]['DBName']=name
            out=io.StringIO();original.write(out)
            fixture_ini=temp/'server.ini';fixture_ini.write_text(out.getvalue(),encoding='utf-8')
            server=Server(fixture_ini)
            assert not server.status()['installed']
            installed=server.install()
            assert installed['installed'] and installed['delete_guard_installed'] and installed['delete_audit_rows']==0
            cur.execute('USE '+ident(game))
            cur.execute("""SELECT name,is_nullable,is_identity,is_computed,collation_name
                FROM sys.columns WHERE object_id=OBJECT_ID('dbo.sdgo_mod_HangarUnitDeleteAudit')
                AND name IN('AuditId','DeletedUtc','TimeLimit','AnsiNote','FixedCode','UnicodeNote')""")
            audit_shape={row[0]:tuple(row[1:]) for row in cur.fetchall()}
            assert audit_shape['AuditId'][:3]==(False,True,False)
            assert audit_shape['DeletedUtc'][:3]==(False,False,False)
            assert audit_shape['TimeLimit'][:3]==(True,False,False)
            for name in ('AnsiNote','FixedCode','UnicodeNote'):
                assert audit_shape[name]==(True,False,False,'Thai_CI_AS')
            installed=server.install()
            assert installed['installed'] and installed['delete_guard_installed'] and installed['delete_audit_rows']==0
            cur.execute('USE '+ident(game))
            cur.execute('DISABLE TRIGGER dbo.sdgo_mod_InventoryDeleteGuard ON dbo.tbl_UserUnits')
            assert not server.status()['delete_guard_installed']
            installed=server.install()
            assert installed['installed'] and installed['delete_guard_installed']
            token=str(uuid.uuid4())
            def mail(sender=1,pilot='FixtureA',recipient='SDGOHangar',body=None):
                body=body or '!sdgo-sort-v1 '+token
                cur.execute('USE '+ident(common))
                cur.execute("DECLARE @out int,@r int; EXEC dbo.sp_SendMsgGame ?,?,0,?,?, '1',0,0,0,0,0,0,0,0,0,0,@out OUTPUT,@r OUTPUT; SELECT @r,@out",sender,pilot,recipient,body)
                while not cur.description:assert cur.nextset()
                return tuple(cur.fetchone())
            assert mail(pilot='wrong')==(5,0)
            assert mail(body='!sdgo-sort-v1 invalid')==(5,0)
            assert mail(recipient='OrdinaryPilot')==(77,42)
            assert mail()==(0,1)
            cur.execute('SELECT UserUnitNumber,UserUnitSlotNumber FROM '+ident(game)+'.dbo.tbl_UserUnits WHERE AccountNumber=1 ORDER BY UserUnitSlotNumber')
            assert [tuple(x) for x in cur.fetchall()]==[(1002,1),(1003,2),(1001,3)]
            cur.execute('SELECT UserUnitSlotNumber,LockState,BattleShipCount,UnitLevelNumber,Paint FROM '+ident(game)+'.dbo.tbl_UserUnits WHERE UserUnitNumber=1001')
            assert tuple(cur.fetchone())==(3,1,1,9,77)
            cur.execute('SELECT GoUserUnitNum FROM '+ident(game)+'.dbo.tbl_Users WHERE AccountNumber=1')
            assert cur.fetchone()[0]==1001
            cur.execute('SELECT UserUnitSlotNumber FROM '+ident(game)+'.dbo.tbl_UserUnits WHERE AccountNumber=2')
            assert cur.fetchone()[0]==8
            # A statement that selects the same slot for two owners must be
            # rejected atomically. Single-owner multi-row deletes remain valid
            # and retain every original column plus bounded connection metadata.
            cur.execute('USE '+ident(game))
            cur.execute("""INSERT dbo.tbl_UserUnits
                (UserUnitNumber,AccountNumber,UserUnitSlotNumber,UnitNumber,DateGet,LockState,
                 BattleShipCount,UnitLevelNumber,Paint,TimeLimit,RunOut,Forbidden,
                 AnsiNote,FixedCode,UnicodeNote)
                VALUES(2002,2,1,10,'20260105',0,0,7,321,0,0,0,
                       'ansi-2002','A2',N'汉字中文-2002'),
                      (2003,2,2,20,'20260106',0,0,4,654,0,0,0,
                       NULL,'B3',N'中文汉字-2003'),
                      (2004,2,3,30,'20260107',0,0,6,987,0,0,0,
                       'rollback','C4',N'回滚审计')""")
            cur.execute('SELECT f_idx,f_GP FROM '+ident(billing)+'.dbo.tbls_AccountPocket ORDER BY f_idx')
            wallets=[tuple(row) for row in cur.fetchall()]
            cur.execute('SELECT COUNT_BIG(*) FROM dbo.sdgo_mod_HangarUnitDeleteAudit')
            audit_before=cur.fetchone()[0]
            refused=False
            try:cur.execute('DELETE dbo.tbl_UserUnits WHERE UserUnitSlotNumber=1')
            except Exception:refused=True
            assert refused
            cur.execute('SELECT UserUnitNumber FROM dbo.tbl_UserUnits WHERE UserUnitNumber IN(1002,2002) ORDER BY UserUnitNumber')
            assert [row[0] for row in cur.fetchall()]==[1002,2002]
            cur.execute('SELECT COUNT_BIG(*) FROM dbo.sdgo_mod_HangarUnitDeleteAudit')
            assert cur.fetchone()[0]==audit_before
            cur.execute('SELECT f_idx,f_GP FROM '+ident(billing)+'.dbo.tbls_AccountPocket ORDER BY f_idx')
            assert [tuple(row) for row in cur.fetchall()]==wallets
            cur.execute("""SELECT UserUnitNumber,AccountNumber,UserUnitSlotNumber,UnitNumber,DateGet,
                LockState,BattleShipCount,UnitLevelNumber,Paint,TimeLimit,RunOut,Forbidden,
                AnsiNote,FixedCode,UnicodeNote
                FROM dbo.tbl_UserUnits WHERE UserUnitNumber IN(2002,2003) ORDER BY UserUnitNumber""")
            deleted_rows=[tuple(row) for row in cur.fetchall()]
            cur.execute('DELETE dbo.tbl_UserUnits WHERE AccountNumber=2 AND UserUnitNumber IN(2002,2003)')
            cur.execute("""SELECT UserUnitNumber,AccountNumber,UserUnitSlotNumber,UnitNumber,DateGet,
                LockState,BattleShipCount,UnitLevelNumber,Paint,TimeLimit,RunOut,Forbidden,
                AnsiNote,FixedCode,UnicodeNote
                FROM dbo.sdgo_mod_HangarUnitDeleteAudit WHERE UserUnitNumber IN(2002,2003)
                ORDER BY UserUnitNumber""")
            assert [tuple(row) for row in cur.fetchall()]==deleted_rows
            assert deleted_rows[0][-1]=='汉字中文-2002'
            assert deleted_rows[1][-3] is None and deleted_rows[1][-1]=='中文汉字-2003'
            cur.execute("""SELECT COUNT(*),MIN(SessionId),MIN(LEN(ClientApplication)),
                MIN(LEN(ClientHost)) FROM dbo.sdgo_mod_HangarUnitDeleteAudit
                WHERE UserUnitNumber IN(2002,2003)""")
            metadata=tuple(cur.fetchone())
            assert metadata[0]==2 and metadata[1]>0 and metadata[2]>0 and metadata[3]>0
            cur.execute('SELECT COUNT_BIG(*) FROM dbo.sdgo_mod_HangarUnitDeleteAudit')
            committed_audit=cur.fetchone()[0]
            cur.execute('BEGIN TRANSACTION; DELETE dbo.tbl_UserUnits WHERE UserUnitNumber=2004; ROLLBACK TRANSACTION')
            cur.execute('SELECT COUNT(*) FROM dbo.tbl_UserUnits WHERE UserUnitNumber=2004')
            assert cur.fetchone()[0]==1
            cur.execute('SELECT COUNT_BIG(*) FROM dbo.sdgo_mod_HangarUnitDeleteAudit')
            assert cur.fetchone()[0]==committed_audit
            assert mail()==(0,1)
            cur.execute('SELECT COUNT(*) FROM '+ident(game)+'.dbo.sdgo_mod_HangarSortReceipt');assert cur.fetchone()[0]==1
            cur.execute('SELECT GB,DropItems FROM '+ident(game)+'.dbo.Untouched');assert tuple(cur.fetchone())==(12345,9)
            from hangar_gb_sql_cases import run_gb_cases
            run_gb_cases(cur,game,common,billing,mail)
            cur.execute('USE '+ident(game))
            cur.execute("CREATE TRIGGER dbo.FixtureBadSideEffect ON dbo.tbl_UserUnits AFTER UPDATE AS UPDATE dbo.tbl_UserUnits SET Paint=Paint+1 WHERE AccountNumber=1")
            cur.execute('UPDATE dbo.tbl_UserUnits SET UserUnitSlotNumber=UserUnitSlotNumber+10 WHERE AccountNumber=1')
            cur.execute('SELECT UserUnitNumber,UserUnitSlotNumber,Paint FROM dbo.tbl_UserUnits ORDER BY UserUnitNumber');before=[tuple(x) for x in cur.fetchall()]
            token=str(uuid.uuid4());assert mail()==(5,0)
            cur.execute('SELECT UserUnitNumber,UserUnitSlotNumber,Paint FROM '+ident(game)+'.dbo.tbl_UserUnits ORDER BY UserUnitNumber')
            assert [tuple(x) for x in cur.fetchall()]==before
            status=server.status()
            assert status['delete_guard_installed'] and status['delete_audit_rows']>=committed_audit
            cur.execute('USE '+ident(game))
            cur.execute('SELECT COUNT_BIG(*) FROM dbo.sdgo_mod_HangarUnitDeleteAudit')
            audit_before_remove=cur.fetchone()[0]
            removed=server.remove()
            assert removed['installed'] is False and removed['delete_audit_preserved']
            assert mail()==(77,42)
            cur.execute('USE '+ident(game))
            cur.execute("SELECT OBJECT_ID('dbo.sdgo_mod_InventoryDeleteGuard','TR'),COUNT_BIG(*) FROM dbo.sdgo_mod_HangarUnitDeleteAudit")
            after_remove=cur.fetchone()
            assert after_remove[0] is None and after_remove[1]==audit_before_remove
            cur.execute("CREATE TRIGGER dbo.sdgo_mod_InventoryDeleteGuard ON dbo.tbl_UserUnits AFTER DELETE AS RETURN")
            foreign=cur.execute("SELECT OBJECT_DEFINITION(OBJECT_ID('dbo.sdgo_mod_InventoryDeleteGuard'))").fetchone()[0]
            rejected=False
            try:server.install()
            except ValueError:rejected=True
            assert rejected
            cur.execute('USE '+ident(game))
            assert cur.execute("SELECT OBJECT_DEFINITION(OBJECT_ID('dbo.sdgo_mod_InventoryDeleteGuard'))").fetchone()[0]==foreign
            assert mail()==(77,42)
            cur.execute('USE '+ident(game))
            cur.execute('DROP TRIGGER dbo.sdgo_mod_InventoryDeleteGuard')
            reinstalled=server.install()
            assert reinstalled['installed'] and reinstalled['delete_guard_installed']
            assert reinstalled['delete_audit_rows']==audit_before_remove
            assert server.remove()['installed'] is False
            cur.execute('USE '+ident(game))
            assert cur.execute('SELECT COUNT_BIG(*) FROM dbo.sdgo_mod_HangarUnitDeleteAudit').fetchone()[0]==audit_before_remove
            removed_status=server.status()
            assert not removed_status['installed'] and not removed_status['delete_guard_installed']
            assert removed_status['delete_audit_rows']==audit_before_remove
        print('PASS: isolated SQL install/readback/idempotence, authenticated ownership, durable sorting, locks/ships/active UID preserved, other owner/GB/dropbox unchanged, rollback, original mail restoration')
    finally:
        cur.execute('USE master')
        for name in reversed(names):
            assert name.startswith(('SDGO_Hangar_TestGame_','SDGO_Hangar_TestCommon_','SDGO_Hangar_TestBilling_')) and name.endswith(tag)
            cur.execute('IF DB_ID(?) IS NOT NULL BEGIN ALTER DATABASE '+ident(name)+' SET SINGLE_USER WITH ROLLBACK IMMEDIATE; DROP DATABASE '+ident(name)+'; END',name)
        cn.close()


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--server-ini',type=Path,required=True)
    run(parser.parse_args().server_ini)
