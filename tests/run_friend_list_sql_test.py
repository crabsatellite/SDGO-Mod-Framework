"""Opt-in friend query checks in a fresh, owned SQL container; no player database.

--native-source is a private UTF-8 export of the selected native sp_FriendsList.
Only synthetic accounts and relationships are created. No game assets are copied.
"""
import argparse
import contextlib
import json
import os
from pathlib import Path
import subprocess
import threading
import uuid
import winreg
from run_hangar_capacity_sql_test import Harness, _redact
from run_new_account_sql_test import Bridge, Handler

class FriendHarness(Harness):
    def docker(self,args,**kwargs):
        kwargs['timeout']=max(60,kwargs.get('timeout',30))
        return super().docker(args,**kwargs)
    def sql(self,text,**kwargs):
        kwargs['timeout']=max(60,kwargs.get('timeout',30))
        return super().sql(text,**kwargs)

def run(args):
    repo=Path(__file__).resolve().parents[1]
    work=args.work_root.resolve()/('friend-list-'+uuid.uuid4().hex[:12])
    work.mkdir(parents=True)
    receipt={'status':'running','real_player_database_used':False,'checks':[]}
    artifacts=args.build_dir.resolve()/args.config
    harness=FriendHarness(repo,artifacts,args.image,work,receipt)
    harness.database='FriendList_'+uuid.uuid4().hex[:16]
    bridge=None
    private=work/'private-connection.ini'
    try:
        harness.create_container();harness.wait_ready()
        print('fixture: owned SQL Server ready',flush=True)
        source=args.native_source.resolve().read_text(encoding='utf-8-sig')
        harness.sql(f'CREATE DATABASE [{harness.database}];\nGO\nUSE [{harness.database}];\n'+SCHEMA+'\nGO\n'+source+'\nGO\n')
        if args.sql_only:
            emitted=work/'query.sql'
            subprocess.run([str(artifacts/'friend_list_sql_emit.exe'),str(args.native_source.resolve()),str(emitted)],check=True,timeout=30)
            harness.sql(f'USE [{harness.database}];\nGO\nCREATE SCHEMA sdgo_friend_list_fix AUTHORIZATION dbo;\nGO\n'+emitted.read_text(encoding='utf-8')+'\nGO\n')
            receipt['oledb_session']='not exercised (--sql-only)'
        else:
            bridge=Bridge(('127.0.0.1',0),Handler);bridge.container=harness.container_id
            thread=threading.Thread(target=bridge.serve_forever,daemon=True);thread.start()
            private.write_text('[fixture]\nserver=127.0.0.1,'+str(bridge.server_address[1])+'\ndatabase='+harness.database+'\npassword='+harness.password+'\n',encoding='utf-16')
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,r'SOFTWARE\Microsoft\Windows\CurrentVersion',0,winreg.KEY_READ|winreg.KEY_WOW64_64KEY) as key:
                common_x86=winreg.QueryValueEx(key,'CommonFilesDir (x86)')[0]
            if not (Path(common_x86)/'System/Ole DB/sqloledb.dll').is_file():
                raise RuntimeError('Registered 32-bit SQLOLEDB provider is unavailable')
            child_env=dict(os.environ,CommonProgramFiles=common_x86)
            result=subprocess.run([str(artifacts/'friend_list_db_test.exe'),str(private)],env=child_env,capture_output=True,text=True,timeout=180)
            if result.returncode:raise RuntimeError(_redact(result.stdout+result.stderr,harness.private_values))
            receipt['checks'].append(result.stdout.strip())
        harness.sql(f'USE [{harness.database}];\n'+CHECKS+'\nGO\n')
        print('fixture: SQL scenarios passed',flush=True)
        receipt['checks'].append('all-offline pagination, missing optional records, canonical rename, missing-profile fallback, online/logout transitions, empty list, clamped page, unchanged cached names and relationships')
        receipt['status']='passed'
    except Exception as error:
        receipt['status']='failed';receipt['error']=_redact(str(error),harness.private_values)
    finally:
        if bridge:bridge.shutdown();bridge.server_close()
        if private.exists():private.unlink()
        receipt['cleanup']=harness.cleanup()
        if not receipt['cleanup'].get('container_removed'):receipt['status']='failed'
        (work/'result.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(receipt,indent=2))
    return 0 if receipt['status']=='passed' else 1


SCHEMA=r"""
CREATE TABLE tbl_Friend(f_idx int NOT NULL,f_idx2 int NOT NULL,f_pilot varchar(20) NOT NULL,f_relation int NOT NULL,f_gender char(1),f_cutoff char(1),f_regdate datetime);
CREATE TABLE tbl_Account_Pilot(f_idx int NOT NULL PRIMARY KEY,f_pilot nvarchar(20) NOT NULL UNIQUE);
CREATE TABLE tblg_UserGameInfo(f_idx int NOT NULL PRIMARY KEY,f_level int,f_levelname varchar(10),f_levelstep int);
CREATE TABLE tblg_OnUser(f_idx int NOT NULL PRIMARY KEY,f_channel int NOT NULL,f_loc tinyint NOT NULL,f_room int,f_team int,f_agent int NOT NULL);
CREATE TABLE tblc_ClanMain(f_cIdx int PRIMARY KEY,f_cName varchar(20),f_mark1 int,f_mark2 int);
CREATE TABLE tblc_ClanMember(f_idx int PRIMARY KEY,f_cIdx int);
CREATE TABLE tblg_UserEmblem(f_idx int PRIMARY KEY,f_emblem int NOT NULL);
DECLARE @id int=2;
WHILE @id<=19 BEGIN
 INSERT tbl_Friend VALUES(1,@id,'Old'+RIGHT('0000'+CONVERT(varchar(4),@id),4),7,NULL,NULL,DATEADD(day,@id,CONVERT(datetime,0)));
 INSERT tbl_Account_Pilot VALUES(@id,N'Pilot'+RIGHT('0000'+CONVERT(varchar(4),@id),4));
 SET @id+=1;
END;
"""

CHECKS=r"""
SET NOCOUNT ON;
CREATE TABLE #rows(id int NOT NULL,name varchar(20) NOT NULL,relation int NOT NULL,gender char(1) NOT NULL,cutoff char(1) NOT NULL,level int NOT NULL,levelname varchar(10) NOT NULL,mark1 int NOT NULL,mark2 int NOT NULL,channel int NOT NULL,loc tinyint NOT NULL,room int NOT NULL,team int NOT NULL,agent int NOT NULL,regdate datetime NOT NULL,emblem int NOT NULL,levelstep int NOT NULL);
DECLARE @ret int,@total int,@page int=1;
CREATE TABLE #seen(id int PRIMARY KEY);
WHILE @page<=3 BEGIN
 TRUNCATE TABLE #rows;
 INSERT #rows EXEC sdgo_friend_list_fix.sp_FriendsList 1,@page,8,@ret OUTPUT,@total OUTPUT;
 IF @total<>18 OR (SELECT COUNT(*) FROM #rows)<>CASE WHEN @page=3 THEN 2 ELSE 8 END THROW 51000,'offline page count',1;
 IF EXISTS(SELECT 1 FROM #rows WHERE name NOT LIKE 'Pilot%' OR relation<>7 OR channel<>0 OR loc<>0 OR room<>0 OR team<>0 OR agent<>0 OR mark1<>0 OR mark2<>0 OR levelstep<>1 OR emblem<>0) THROW 51000,'offline scalar/name projection',1;
 INSERT #seen SELECT id FROM #rows;
 SET @page+=1;
END;
IF (SELECT COUNT(*) FROM #seen)<>18 THROW 51000,'page duplicate/omission',1;
TRUNCATE TABLE #rows;
INSERT #rows EXEC sdgo_friend_list_fix.sp_FriendsList 1,99,8,@ret OUTPUT,@total OUTPUT;
IF (SELECT COUNT(*) FROM #rows)<>2 THROW 51000,'clamped page',1;
INSERT tblg_OnUser VALUES(19,9,3,401,2,8);
TRUNCATE TABLE #rows;
INSERT #rows EXEC sdgo_friend_list_fix.sp_FriendsList 1,1,8,@ret OUTPUT,@total OUTPUT;
IF NOT EXISTS(SELECT 1 FROM #rows WHERE id=19 AND channel=9 AND loc=3 AND room=401 AND team=2 AND agent=8) THROW 51000,'online presence',1;
DELETE tblg_OnUser WHERE f_idx=19;
UPDATE tbl_Account_Pilot SET f_pilot=N'ChangedPilot' WHERE f_idx=2;
DELETE tbl_Account_Pilot WHERE f_idx=3;
TRUNCATE TABLE #rows;
INSERT #rows EXEC sdgo_friend_list_fix.sp_FriendsList 1,1,8,@ret OUTPUT,@total OUTPUT;
IF @total<>18 OR NOT EXISTS(SELECT 1 FROM #rows WHERE id=2 AND name='ChangedPilot' AND channel=0) OR NOT EXISTS(SELECT 1 FROM #rows WHERE id=3 AND name='Old0003') THROW 51000,'rename/fallback/logout',1;
IF (SELECT COUNT(*) FROM tbl_Friend)<>18 OR EXISTS(SELECT 1 FROM tbl_Friend WHERE f_pilot NOT LIKE 'Old%') THROW 51000,'friend rows modified',1;
TRUNCATE TABLE #rows;
INSERT #rows EXEC sdgo_friend_list_fix.sp_FriendsList 99,1,8,@ret OUTPUT,@total OUTPUT;
IF @total<>0 OR EXISTS(SELECT 1 FROM #rows) THROW 51000,'empty list',1;
"""

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir',type=Path,required=True)
    parser.add_argument('--config',default='Release')
    parser.add_argument('--work-root',type=Path,required=True)
    parser.add_argument('--native-source',type=Path,required=True)
    parser.add_argument('--image',required=True)
    parser.add_argument('--sql-only',action='store_true',help='exercise the transformed query without the Windows OLE DB session test')
    raise SystemExit(run(parser.parse_args()))
