"""Exercise the real adapter in a NEW, synthetic-only SQL database.

Reads native procedure definitions from the selected host; never invokes them in
that host's game database. The fixture is retained with its receipt for review.
"""
import argparse
import configparser
import copy
import datetime
import json
from pathlib import Path
import re
import shutil
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from manager.engines.operators import model,runtime,server,presets
from manager.engines.operators.assets import Assets


def last_row(cur):
    found=None
    while True:
        if cur.description:
            values=cur.fetchall()
            if values:found=values[-1]
        if not cur.nextset():return found


def contained(root,path):
    root=Path(root).resolve(strict=True)
    path=Path(path).resolve(strict=False)
    return path==root or root in path.parents


def assert_sql_files_under(cur,test_root,database=None):
    query="""SELECT DB_NAME(database_id),type_desc,physical_name
             FROM sys.master_files"""
    parameters=()
    if database is not None:
        query+=" WHERE database_id=DB_ID(?)"
        parameters=(database,)
    query+=" ORDER BY database_id,file_id"
    rows=list(cur.execute(query,*parameters))
    if not rows:
        raise ValueError("SQL file containment check returned no rows")
    outside=[
        (str(row[0]),str(row[1]),str(row[2]))
        for row in rows if not contained(test_root,row[2])
    ]
    if outside:
        raise ValueError(
            "SQL instance contains files outside the isolated test root: "
            + "; ".join(f"{name}/{kind}: {path}" for name,kind,path in outside)
        )
    return rows


def package_server_root(package_root):
    root=Path(package_root).resolve(strict=True)
    direct=root/'server'
    if (direct/'sdgo').is_dir():return direct
    if (root/'sdgo').is_dir():return root
    raise ValueError("runtime package lacks a staged server tree")


def stage_isolated_runtime(output,test_root,battle_host_exe,package_root,port):
    test_root=Path(test_root).resolve(strict=True)
    source_exe=Path(battle_host_exe).resolve(strict=True)
    if not contained(test_root,source_exe):
        raise ValueError("fixture BattleServer.exe must come from the isolated test root")
    package_server=package_server_root(package_root)
    root=Path(output)/'runtime-server'
    if root.exists():
        raise ValueError(f"fixture runtime directory already exists: {root}")
    host=root/'Server'/'BattleServer1'
    (root/'ServerStartUp').mkdir(parents=True)
    (host/'sdgo').mkdir(parents=True)
    shutil.copy2(source_exe,host/'BattleServer.exe')
    shutil.copy2(package_server/'winmm.dll',host/'winmm.dll')
    shutil.copy2(package_server/'sdgo'/'core.dll',host/'sdgo'/'core.dll')
    (root/'ServerStartUp'/'ServerStartUp.xml').write_text(
        '<Processes><Process Run="True" '
        'Path=".\\Server\\BattleServer1\\BattleServer.exe" /></Processes>\n',
        encoding='utf-8',
    )
    (host/'sip.txt').write_text(
        f'LISTEN 127.0.0.1:{port} AGENT\n',encoding='ascii'
    )
    return root


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--server-ini',type=Path,required=True)
    p.add_argument('--client',type=Path,required=True)
    p.add_argument('--snapshot',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--test-root',type=Path,required=True)
    p.add_argument('--battle-host-exe',type=Path,required=True)
    p.add_argument('--runtime-package',type=Path,required=True)
    p.add_argument('--channel-port',type=int,default=49321)
    p.add_argument('--default-pack',action='store_true')
    args=p.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    if not 1<=args.channel_port<=65535:
        raise ValueError("fixture channel port is out of range")
    test_root=args.test_root.resolve(strict=True)
    stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d_%H%M%S_%f')
    db='sdgo_operator_fixture_'+stamp
    assert re.fullmatch(r'sdgo_operator_fixture_[0-9_]+',db)
    native=server.connect(args.server_ini)
    try:
        n=native.cursor()
        source_files=assert_sql_files_under(n,test_root)
        source_db=n.execute('SELECT DB_NAME()').fetchone()[0]
        assert source_db!=db
        names=['sp_OperatorDrop','sp_GetUserOperatorPage','sp_GetPilotOperator','sp_CreateUserOperator',
               'sp_CheckUserOperatorTimeLimit','sp_GeOperatorProbability','sp_CreateUserMixTreeItem','sp_SetUserOperatorName','sp_GetUserOperator',
               'sp_SetUserUnitCustomSkill','sp_GetOperatorSkill','sp_GetOperatorAddOn']
        definitions={name:server.definition(n,name) for name in names}
        if n.execute('SELECT OBJECT_ID(?)','dbo.sdgo_OperatorProcedureBackup').fetchone()[0] is not None:
            for name in server.WRAPPED:
                row=n.execute('SELECT OriginalDefinition FROM dbo.sdgo_OperatorProcedureBackup WHERE ProcedureName=?',name).fetchone()
                if row:definitions[name]=row[0]
        n.execute('CREATE DATABASE ['+db+']')
        fixture_files=assert_sql_files_under(n,test_root,db)
    finally:native.close()
    source_ini=server.read_ini(args.server_ini)
    ini=configparser.ConfigParser(interpolation=None)
    ini['DB Game']=dict(source_ini['DB Game']);ini['DB Game']['DBName']=db
    fixture_ini=args.output/'fixture-private.ini'
    with fixture_ini.open('w',encoding='utf-8') as f:ini.write(f)
    runtime_root=stage_isolated_runtime(
        args.output,test_root,args.battle_host_exe,
        args.runtime_package,args.channel_port,
    )
    cn=server.connect(fixture_ini);cur=cn.cursor()
    assert cur.execute('SELECT DB_NAME()').fetchone()[0]==db
    schema="""
    CREATE TABLE dbo.tbl_ChannelInfo(AddrPT INT PRIMARY KEY,ServerID INT);
    CREATE TABLE dbo.tbl_Users(AccountNumber INT PRIMARY KEY,GoUserOperatorNum BIGINT DEFAULT 0,UserLevelNumber TINYINT DEFAULT 18);
    CREATE TABLE dbo.tbl_OperatorData(OperatorID INT PRIMARY KEY,OperatorName VARCHAR(64),TeamWin INT,TeamLose INT,
      BattleResult INT,ChangeSp INT,ServiceType INT,AddOn1 INT,AddOn2 INT,AddOn3 INT,AddOn4 INT,DropType TINYINT,GParts INT);
    CREATE TABLE dbo.tbl_OperatorSkill(SkillNo INT PRIMARY KEY,SkillType INT,PointAdd INT,UnitExpAdd INT,EnergySave INT);
    CREATE TABLE dbo.tbl_OperatorTime(TimeID INT PRIMARY KEY,AddOnID INT UNIQUE,OperatorSkill INT,StartTime INT,CloseTime INT);
    CREATE TABLE dbo.tbl_ItemCategory(CatalogueID INT PRIMARY KEY,Name VARCHAR(100),RefIndex INT,TimeLimit TINYINT,Option1 INT,Option2 INT);
    CREATE TABLE dbo.tbl_UserOperators(UserOperatorNumber BIGINT IDENTITY PRIMARY KEY,AccountNumber INT,SlotNumber INT,
      OperatorNumber INT,OperatorNickname VARCHAR(16) DEFAULT '',DateGet DATETIME DEFAULT GETDATE(),BuyType INT,
      DateEnd DATETIME NULL,CategoryNumber INT,ShopOperator TINYINT DEFAULT 0,RunOut TINYINT DEFAULT 0,
      Forbidden TINYINT DEFAULT 0,OPType TINYINT DEFAULT 0,ShopItemId VARCHAR(6) DEFAULT '',ShopItemtblidx INT DEFAULT 0,
      SerialNum BIGINT DEFAULT 0,IsSetName TINYINT DEFAULT 0,RemainCount SMALLINT DEFAULT 0);
    CREATE TABLE dbo.tbl_UserOperatorSlotNumberList(UserOperatorSlotNumber INT PRIMARY KEY);
    CREATE TABLE dbo.tbl_UserDropItemBox(AccountNumber INT,ItemCategory INT,Amount INT,DropConditionID INT);
    CREATE TABLE dbo.tbls_AccountPocket(f_idx INT PRIMARY KEY,f_GC INT,f_GP INT);
    CREATE TABLE dbo.tbl_OperatorDropDice(Idx INT PRIMARY KEY,OperatorNumber INT,CategoryID INT,Prob INT,ItemName VARCHAR(100),Amount INT,Opt INT,SumProb INT);
    CREATE TABLE dbo.tbl_OperatorIndex(OperatorNumber INT,ReadIndex INT);
    CREATE TABLE dbo.tbl_DiceList_1000(Idx INT PRIMARY KEY,Probability INT);
    CREATE TABLE dbo.FixtureSerial(ID BIGINT IDENTITY PRIMARY KEY);
    CREATE TABLE dbo.FixtureRoll(Roll INT,Quantity INT);
    CREATE TABLE dbo.tbl_MixTreeFile(Idx INT PRIMARY KEY,OutUnitID INT);
    CREATE TABLE dbo.tbl_Units(UnitNumber INT PRIMARY KEY);
    CREATE TABLE dbo.tbl_PaintItem(PaintID INT PRIMARY KEY,Type INT);
    CREATE TABLE dbo.tbl_StickerItem(StickerID INT PRIMARY KEY);
    CREATE TABLE dbo.tbl_UserMixTreeItemsSlotNumberList(SlotNumber INT PRIMARY KEY);
    CREATE TABLE dbo.tbl_UserMixTreeItems(UserMixTreeItemNumber BIGINT IDENTITY PRIMARY KEY,AccountNumber INT,
        SlotNumber INT,MixTreeItemID INT,CategoryNumber INT,RemainCount INT,BuyType INT,ShopItemtblidx INT);
    CREATE TABLE dbo.tbl_UserUnits(UserUnitNumber BIGINT PRIMARY KEY,AccountNumber INT,UnitNumber INT,
        UnitLevelNumber TINYINT,TimeLimit TINYINT,Normal_MaxLv TINYINT,
        UnitSkill_1 SMALLINT,UnitSkill_2 SMALLINT,UnitSkill_3 SMALLINT,UnitSkill_4 SMALLINT);
    CREATE TABLE dbo.tbl_UnitCustomSkillRev(UnitSkillID SMALLINT PRIMARY KEY,Type_2 INT,UseUnitLv TINYINT,UseUserLv TINYINT);
    INSERT dbo.tbl_ChannelInfo VALUES(%d,1);
    INSERT dbo.FixtureRoll VALUES(0,0);
    """ % args.channel_port
    cur.execute(schema)
    cur.execute("CREATE PROCEDURE dbo.sp_CreateSerial @Account INT,@Serial BIGINT OUTPUT,@Ret INT OUTPUT AS BEGIN SET NOCOUNT ON;INSERT dbo.FixtureSerial DEFAULT VALUES;SET @Serial=SCOPE_IDENTITY();SET @Ret=0;END")
    catalog=model.import_catalog(args.snapshot,Assets(args.client),include_installed=False)
    default_catalog=presets.initialize(catalog,args.snapshot,Assets(args.client)) if args.default_pack else None
    data=catalog['native_server_data']
    cols=['OperatorID','OperatorName','TeamWin','TeamLose','BattleResult','ChangeSp','ServiceType','AddOn1','AddOn2','AddOn3','AddOn4','DropType','GParts']
    for source in data.values():cur.execute('INSERT dbo.tbl_OperatorData VALUES('+','.join('?' for _ in cols)+')',*[source[x] for x in cols])
    skill_path=args.snapshot/'GOnlineGame/dbo.tbl_OperatorSkill.json'
    if skill_path.exists():
        for source in model.load(skill_path):
            cur.execute('INSERT dbo.tbl_OperatorSkill VALUES(?,?,?,?,?)',
                        source['SkillNo'],source['SkillType'],source['PointAdd'],source['UnitExpAdd'],source['EnergySave'])
    time_path=args.snapshot/'GOnlineGame/dbo.tbl_OperatorTime.json'
    if time_path.exists():
        for source in model.load(time_path):
            cur.execute('INSERT dbo.tbl_OperatorTime VALUES(?,?,?,?,?)',
                        source['TimeID'],source['AddOnID'],source['OperatorSkill'],source['StartTime'],source['CloseTime'])
    cat_rows=model.load(args.snapshot/'GOnlineGame/dbo.tbl_ItemCategory.json')
    needed={1070001,2220010}
    needed.update(o['category_id'] for o in catalog['operators'] if any(c['CatalogueID']==o['category_id'] for c in cat_rows))
    dice=model.load(args.snapshot/'GOnlineGame/dbo.tbl_OperatorDropDice.json')
    needed.update(x['CategoryID'] for x in dice)
    if default_catalog:
        needed.update(r['category_id'] for o in default_catalog['operators'] if o['managed'] for r in o['drops'])
        needed.update(c for pool in default_catalog['drop_pools'].values() for c in pool['category_ids'])
    for source in cat_rows:
        if source['CatalogueID'] in needed:
            cur.execute('INSERT dbo.tbl_ItemCategory VALUES(?,?,?,?,?,?)',source['CatalogueID'],'fixture item',source['RefIndex'],source['TimeLimit'],source['Option1'],source['Option2'])
    for source in dice:
        cur.execute('INSERT dbo.tbl_OperatorDropDice VALUES(?,?,?,?,?,?,?,?)',source['Idx'],source['OperatorNumber'],source['CategoryID'],source['Prob'],'fixture',source['Amount'],source['Opt'],source['SumProb'])
    for i in range(100):cur.execute('INSERT dbo.tbl_UserOperatorSlotNumberList VALUES(?)',i+1)
    for i in range(1000):cur.execute('INSERT dbo.tbl_DiceList_1000 VALUES(?,?)',i,i)
    for i in range(1,121):cur.execute('INSERT dbo.tbl_UserMixTreeItemsSlotNumberList VALUES(?)',i)
    if default_catalog:
        for table,columns in [('tbl_MixTreeFile',['Idx','OutUnitID']),('tbl_Units',['UnitNumber']),
                              ('tbl_PaintItem',['PaintID','Type']),('tbl_StickerItem',['StickerID'])]:
            for r in model.load(args.snapshot/'GOnlineGame'/('dbo.'+table+'.json')):
                cur.execute('INSERT dbo.'+table+' VALUES('+','.join('?' for _ in columns)+')',*[r[c] for c in columns])
    native_contract_procedures={'sp_GetOperatorSkill','sp_GetOperatorAddOn'}
    for name,text in definitions.items():
        installed=text
        if name not in native_contract_procedures:
            installed=re.sub(
                r'\[GDStoreBilling\]',lambda _: '['+db+']',
                installed,flags=re.I,
            )
            assert 'GDStoreBilling' not in installed
        cur.execute(installed)
    native_fingerprints={
        name.casefold():server.source_hash(server.definition(cur,name))
        for name in native_contract_procedures
    }
    assert native_fingerprints==runtime.NATIVE_PROCEDURE_FINGERPRINTS
    runtime_receipts=[]
    def prepare_runtime(value):
        revision=model.revision(value)
        plan=runtime.plan_runtime_install(
            runtime_root,args.runtime_package,value,fixture_ini
        )
        if not plan['ready']:
            raise ValueError(
                "fixture runtime install plan is blocked: "
                + "; ".join(plan['blockers'])
            )
        installed=runtime.install_runtime(plan)
        readback_plan=runtime.plan_runtime_install(
            runtime_root,args.runtime_package,value,fixture_ini
        )
        if not readback_plan['ready']:
            raise ValueError(
                "installed fixture runtime is blocked: "
                + "; ".join(readback_plan['blockers'])
            )
        if readback_plan['actions']:
            raise ValueError("installed fixture runtime did not reach exact readback")
        evidence=runtime.installed_runtime_evidence(readback_plan)
        effects=(
            runtime_root/'Server'/'BattleServer1'/'sdgo'/'mods'
            /'operator_catalog_server'/'effects.ini'
        )
        assert effects.read_bytes()==runtime.compile_effects_ini(value)
        receipt={
            'revision':revision,
            'install_changed':bool(installed['changed']),
            'installed_files':len(installed['files']),
            'exact_readback':True,
            'fresh_installed_evidence':True,
            'database_result':'pending',
        }
        runtime_receipts.append(receipt)
        return evidence,receipt
    def trusted_apply(value):
        evidence,receipt=prepare_runtime(value)
        try:
            applied=server.apply_catalog(
                value,fixture_ini,runtime_evidence=evidence
            )
        except Exception:
            receipt['database_result']='rejected'
            raise
        receipt['database_result']='applied'
        return applied
    for account in [7770001,7770002,7770003]:
        cur.execute('INSERT dbo.tbl_Users(AccountNumber,GoUserOperatorNum) VALUES(?,0)',account)
        cur.execute('INSERT dbo.tbls_AccountPocket VALUES(?,100,200)',account)
    cur.execute("INSERT dbo.tbl_UserOperators(AccountNumber,SlotNumber,OperatorNumber,CategoryNumber,OperatorNickname,OPType,RunOut,DateEnd,IsSetName) VALUES(7770001,1,60003,1210002,'keep-personal',1,1,'20000101',1)")
    cur.execute('UPDATE dbo.tbl_Users SET GoUserOperatorNum=(SELECT UserOperatorNumber FROM dbo.tbl_UserOperators WHERE AccountNumber=7770001) WHERE AccountNumber=7770001')
    for slot in range(1,101):cur.execute('INSERT dbo.tbl_UserOperators(AccountNumber,SlotNumber,OperatorNumber,CategoryNumber,OPType) VALUES(7770002,?,60001,1210001,0)',slot)
    catalog=model.update_operator(catalog,60003,{'delivery':'all_players','drops':[{'category_id':2220010,'chance_bps':10000,'min_amount':2,'max_amount':5}]})
    catalog,new_id=model.create_operator(catalog,60003)
    catalog=model.update_operator(catalog,new_id,{'name':'registered only','delivery':'product'})
    first=trusted_apply(catalog)
    assert first['storefront_changes'] is False
    def page(account):
        return last_row(cur.execute('DECLARE @G BIGINT,@N INT,@R INT;EXEC @R=dbo.sp_GetUserOperatorPage ?,0,@G OUTPUT,@N OUTPUT;SELECT @R,@G,@N',account))
    page(7770001)
    row=cur.execute('SELECT OperatorNickname,OPType,RunOut,DateEnd,IsSetName FROM dbo.tbl_UserOperators WHERE AccountNumber=7770001 AND OperatorNumber=60003').fetchone()
    assert tuple(row)==('keep-personal',0,0,None,1)
    assert not cur.execute('SELECT 1 FROM dbo.tbl_UserOperators WHERE AccountNumber=7770001 AND OperatorNumber=?',new_id).fetchone()
    page(7770003);page(7770003)
    assert cur.execute('SELECT COUNT(*) FROM dbo.tbl_UserOperators WHERE AccountNumber=7770003 AND OperatorNumber=60003').fetchone()[0]==1
    page(7770002)
    assert tuple(cur.execute('SELECT COUNT(*),MIN(SlotNumber),MAX(SlotNumber) FROM dbo.tbl_UserOperators WHERE AccountNumber=7770002').fetchone())==(100,1,100)
    assert cur.execute('SELECT 1 FROM dbo.sdgo_OperatorGrantIssues WHERE AccountNumber=7770002 AND OperatorID=60003').fetchone()
    new_op=next(o for o in catalog['operators'] if o['id']==new_id)
    template_id=new_op['template_id']
    custom_abilities={
        'schema_version':1,'mode':'custom',
        'general':{'gp_percent':99,'unit_exp_percent':98,'energy_save_percent':97},
        'victory':{'gp_percent':96,'unit_exp_percent':95,'energy_save_percent':94},
        'defeat':{'gp_percent':93,'unit_exp_percent':92,'energy_save_percent':91},
        'timed':[{
            'start':'08:00','end':'09:00',
            'gp_percent':90,'unit_exp_percent':89,'energy_save_percent':88,
        }],
        'pilot_exp_percent':0,'mission_parts':True,'extensions':{},
    }
    catalog=model.update_operator(catalog,new_id,{'abilities':custom_abilities})
    trusted_apply(catalog)
    verified_capabilities={
        str(name):str(evidence)
        for name,evidence in cur.execute(
            """SELECT CapabilityName,Evidence
               FROM dbo.sdgo_OperatorNativeCapabilities
               WHERE Verified=1"""
        )
    }
    assert set(verified_capabilities)==runtime.TRUSTED_NATIVE_CAPABILITIES
    assert all(
        re.fullmatch(r'SDGO_RUNTIME_V1:[0-9a-f]{64}',evidence)
        for evidence in verified_capabilities.values()
    )
    owned_skills={r[0] for r in cur.execute('SELECT SkillNo FROM dbo.sdgo_OperatorSkillRegistry')}
    owned_add_ons={r[0] for r in cur.execute('SELECT AddOnID FROM dbo.sdgo_OperatorTimeRegistry')}
    custom_row=cur.execute(
        """SELECT TeamWin,TeamLose,BattleResult,AddOn1,AddOn2,AddOn3,AddOn4
           FROM dbo.tbl_OperatorData WHERE OperatorID=?""",
        new_id,
    ).fetchone()
    assert owned_skills and owned_add_ons
    assert any(value in owned_skills for value in custom_row[:3])
    assert any(value in owned_add_ons for value in custom_row[3:])
    cur.execute("INSERT dbo.tbl_UserOperators(AccountNumber,SlotNumber,OperatorNumber,CategoryNumber,OperatorNickname,OPType,IsSetName) VALUES(7770001,2,?,?,'my-custom',0,1)",new_id,model.native_category(new_id))
    purchased_number=cur.execute(
        "SELECT UserOperatorNumber FROM dbo.tbl_UserOperators WHERE AccountNumber=7770001 AND OperatorNumber=?",
        new_id,
    ).fetchone()[0]
    disabled_new=model.update_operator(catalog,new_id,{'enabled':False})
    trusted_apply(disabled_new)
    baseline_columns=server.OPERATOR_COLUMNS[2:11]+['GParts']
    retained=cur.execute(
        "SELECT "+",".join(baseline_columns)+",DropType FROM dbo.tbl_OperatorData WHERE OperatorID=?",
        new_id,
    ).fetchone()
    template=cur.execute(
        "SELECT "+",".join(baseline_columns)+" FROM dbo.tbl_OperatorData WHERE OperatorID=?",
        template_id,
    ).fetchone()
    assert tuple(retained[:-1])==tuple(template) and retained[-1]==0
    assert not cur.execute('SELECT 1 FROM dbo.sdgo_OperatorSkillRegistry').fetchone()
    assert not cur.execute('SELECT 1 FROM dbo.sdgo_OperatorTimeRegistry').fetchone()
    assert not cur.execute(
        "SELECT 1 FROM dbo.tbl_OperatorSkill WHERE SkillNo IN (" +
        ",".join(str(int(value)) for value in sorted(owned_skills)) + ")"
    ).fetchone()
    assert tuple(cur.execute(
        """SELECT UserOperatorNumber,SlotNumber,OperatorNickname,IsSetName
           FROM dbo.tbl_UserOperators WHERE AccountNumber=7770001 AND OperatorNumber=?""",
        new_id,
    ).fetchone())==(purchased_number,2,'my-custom',1)
    trusted_apply(disabled_new)
    assert cur.execute(
        "SELECT COUNT(*) FROM dbo.tbl_UserOperators WHERE AccountNumber=7770001 AND OperatorNumber=?",
        new_id,
    ).fetchone()[0]==1
    assert tuple(cur.execute(
        """SELECT UserOperatorNumber,SlotNumber,OperatorNickname,IsSetName
           FROM dbo.tbl_UserOperators WHERE AccountNumber=7770001 AND OperatorNumber=?""",
        new_id,
    ).fetchone())==(purchased_number,2,'my-custom',1)
    trusted_apply(catalog)
    user_op=cur.execute('SELECT UserOperatorNumber FROM dbo.tbl_UserOperators WHERE AccountNumber=7770001 AND OperatorNumber=60003').fetchone()[0]
    def drop(account=7770001,number=user_op):
        return last_row(cur.execute('DECLARE @C INT,@A INT,@R INT;EXEC dbo.sp_OperatorDrop ?,?,@C OUTPUT,@A OUTPUT,@R OUTPUT;SELECT @C,@A,@R',account,number))
    for _ in range(10):
        r=drop();assert r[0]==2220010 and 2<=r[1]<=5 and r[2]==0
    assert cur.execute('SELECT f_GC FROM dbo.tbls_AccountPocket WHERE f_idx=7770001').fetchone()[0]==10100
    count=cur.execute('SELECT COUNT(*) FROM dbo.tbl_UserDropItemBox').fetchone()[0]
    r=drop(7770003,user_op);assert r[2]!=0
    assert cur.execute('SELECT COUNT(*) FROM dbo.tbl_UserDropItemBox').fetchone()[0]==count
    catalog=model.update_operator(catalog,60003,{'drops':[]})
    trusted_apply(catalog);r=drop();assert tuple(r)==(1070001,1,0)
    # Test the SQL cumulative-boundary implementation with a fixture-only RNG.
    catalog=model.update_operator(catalog,60003,{'drops':[{'category_id':2220010,'chance_bps':2500,'min_amount':2,'max_amount':5}]})
    trusted_apply(catalog)
    cur.execute("ALTER PROCEDURE dbo.sdgo_OperatorUniform @Bound INT,@Value INT OUTPUT AS BEGIN SET NOCOUNT ON;SELECT @Value=CASE WHEN @Bound=10000 THEN Roll ELSE Quantity END FROM dbo.FixtureRoll;END")
    for roll,q,expected in [(0,0,(2220010,2,0)),(2499,3,(2220010,5,0)),(2500,0,(1070001,1,0)),(9999,0,(1070001,1,0))]:
        cur.execute('UPDATE dbo.FixtureRoll SET Roll=?,Quantity=?',roll,q);assert tuple(drop())==expected
    # Reinstall the production crypto RNG and verify repeatable module apply.
    trusted_apply(catalog)
    for bound in [1,4,10000,65535]:
        value=last_row(cur.execute('DECLARE @V INT;EXEC dbo.sdgo_OperatorUniform ?,@V OUTPUT;SELECT @V',bound))[0]
        assert 0<=value<bound
    original=server.definition(cur,'sp_GetPilotOperator')
    modify=server.alter(original) if re.search(r'(?im)^\s*CREATE\s+PROC',original) else original
    cur.execute(modify+'\n-- another module changed this procedure')
    def table_rows(statement,*parameters):
        return tuple(tuple(row) for row in cur.execute(statement,*parameters))
    conflict_before={
        'profile':table_rows(
            """SELECT OperatorID,DisplayName,Enabled,Revision
               FROM dbo.sdgo_OperatorProfiles ORDER BY OperatorID"""
        ),
        'operator_data':table_rows(
            """SELECT OperatorID,OperatorName,TeamWin,TeamLose,BattleResult,
                      ChangeSp,ServiceType,AddOn1,AddOn2,AddOn3,AddOn4,
                      DropType,GParts
               FROM dbo.tbl_OperatorData
               WHERE OperatorID IN (60003,?) ORDER BY OperatorID""",
            new_id,
        ),
        'skill_registry':table_rows(
            "SELECT * FROM dbo.sdgo_OperatorSkillRegistry ORDER BY SkillNo"
        ),
        'time_registry':table_rows(
            "SELECT * FROM dbo.sdgo_OperatorTimeRegistry ORDER BY TimeID"
        ),
        'capabilities':table_rows(
            """SELECT CapabilityName,Verified,Evidence
               FROM dbo.sdgo_OperatorNativeCapabilities
               ORDER BY CapabilityName"""
        ),
    }
    conflicting_procedure_hash=server.source_hash(
        server.definition(cur,'sp_GetPilotOperator')
    )
    conflict_catalog=model.update_operator(catalog,60003,{'name':'must rollback'})
    try:trusted_apply(conflict_catalog)
    except ValueError:pass
    else:raise AssertionError('Procedure conflict was overwritten')
    conflict_after={
        'profile':table_rows(
            """SELECT OperatorID,DisplayName,Enabled,Revision
               FROM dbo.sdgo_OperatorProfiles ORDER BY OperatorID"""
        ),
        'operator_data':table_rows(
            """SELECT OperatorID,OperatorName,TeamWin,TeamLose,BattleResult,
                      ChangeSp,ServiceType,AddOn1,AddOn2,AddOn3,AddOn4,
                      DropType,GParts
               FROM dbo.tbl_OperatorData
               WHERE OperatorID IN (60003,?) ORDER BY OperatorID""",
            new_id,
        ),
        'skill_registry':table_rows(
            "SELECT * FROM dbo.sdgo_OperatorSkillRegistry ORDER BY SkillNo"
        ),
        'time_registry':table_rows(
            "SELECT * FROM dbo.sdgo_OperatorTimeRegistry ORDER BY TimeID"
        ),
        'capabilities':table_rows(
            """SELECT CapabilityName,Verified,Evidence
               FROM dbo.sdgo_OperatorNativeCapabilities
               ORDER BY CapabilityName"""
        ),
    }
    assert conflict_after==conflict_before
    assert server.source_hash(
        server.definition(cur,'sp_GetPilotOperator')
    )==conflicting_procedure_hash
    cur.execute(modify)
    trusted_apply(catalog)
    source_hash=server.source_hash(server.definition(cur,'sp_GetPilotOperator'))
    assert cur.execute('SELECT InstalledHash FROM dbo.sdgo_OperatorProcedureBackup WHERE ProcedureName=?','sp_GetPilotOperator').fetchone()[0]==source_hash
    default_results={}
    if default_catalog:
        gparts_before={r[0]:r[1] for r in cur.execute('SELECT OperatorID,GParts FROM dbo.tbl_OperatorData')}
        cur.execute("INSERT dbo.tbl_UserOperators(AccountNumber,SlotNumber,OperatorNumber,CategoryNumber,OperatorNickname,OPType,RunOut,DateEnd,IsSetName) VALUES(7770003,2,60013,1210012,'Mizuki(EXP)',1,1,'20000101',1)")
        applied=trusted_apply(default_catalog)
        assert {r[0]:r[1] for r in cur.execute('SELECT OperatorID,GParts FROM dbo.tbl_OperatorData')
                if r[0] in presets.NAMES}=={identity:1 for identity in presets.NAMES}
        assert all(r[1]==gparts_before[r[0]] for r in cur.execute('SELECT OperatorID,GParts FROM dbo.tbl_OperatorData')
                   if r[0] not in presets.NAMES)
        paused=model.update_operator(default_catalog,60003,{'enabled':False})
        trusted_apply(paused)
        assert cur.execute('SELECT GParts FROM dbo.tbl_OperatorData WHERE OperatorID=60003').fetchone()[0]==gparts_before[60003]
        reapplied=trusted_apply(default_catalog)
        assert reapplied['default_mission_parts_enabled']==int(
            gparts_before[60003]!=1
        )
        assert cur.execute('SELECT GParts FROM dbo.tbl_OperatorData WHERE OperatorID=60003').fetchone()[0]==1
        assert {r[0]:r[1] for r in cur.execute('SELECT OperatorID,OriginalGParts FROM dbo.sdgo_OperatorMissionPartsBackup')}=={
            identity:gparts_before[identity] for identity in presets.NAMES}
        assert applied['legacy_script_aliases_cleared']==1
        assert applied['existing_default_entitlements_upgraded']==1
        assert tuple(cur.execute('SELECT OPType,RunOut,DateEnd FROM dbo.tbl_UserOperators WHERE AccountNumber=7770003 AND OperatorNumber=60013').fetchone())==(0,0,None)
        assert tuple(cur.execute('SELECT OperatorNickname,IsSetName FROM dbo.tbl_UserOperators WHERE AccountNumber=7770003 AND OperatorNumber=60013').fetchone())==('@SDGO:60013',1)
        assert cur.execute('SELECT OperatorNickname FROM dbo.tbl_UserOperators WHERE AccountNumber=7770001 AND OperatorNumber=60003').fetchone()[0]=='@SDGO:60003'
        assert tuple(cur.execute('SELECT OperatorNickname,IsSetName FROM dbo.tbl_UserOperators WHERE AccountNumber=7770001 AND OperatorNumber=?',new_id).fetchone())==('my-custom',1)
        page(7770003);page(7770003)
        assert cur.execute('SELECT COUNT(DISTINCT OperatorNumber) FROM dbo.tbl_UserOperators WHERE AccountNumber=7770003 AND OperatorNumber IN (60003,60013,60018,60025,60041)').fetchone()[0]==5
        assert not cur.execute('SELECT 1 FROM dbo.tbl_UserOperators WHERE AccountNumber=7770003 AND OperatorNumber IN (60003,60013,60018,60025,60041) GROUP BY OperatorNumber HAVING COUNT(*)>1').fetchone()
        assert cur.execute("SELECT COUNT(*) FROM dbo.tbl_UserOperators WHERE AccountNumber=7770003 AND OperatorNumber IN (60003,60013,60018,60025,60041) AND IsSetName=1 AND OperatorNickname='@SDGO:'+CONVERT(VARCHAR(5),OperatorNumber)").fetchone()[0]==5
        # Repair a permanent record before the early ownership CONTINUE, too.
        cur.execute('DISABLE TRIGGER dbo.sdgo_OperatorFixedNames ON dbo.tbl_UserOperators')
        cur.execute("UPDATE dbo.tbl_UserOperators SET OperatorNickname='',IsSetName=0 WHERE AccountNumber=7770003 AND OperatorNumber=60003")
        page(7770003)
        assert cur.execute('SELECT IsSetName FROM dbo.tbl_UserOperators WHERE AccountNumber=7770003 AND OperatorNumber=60003').fetchone()[0]==1
        cur.execute('ENABLE TRIGGER dbo.sdgo_OperatorFixedNames ON dbo.tbl_UserOperators')
        default_number=cur.execute('SELECT UserOperatorNumber FROM dbo.tbl_UserOperators WHERE AccountNumber=7770003 AND OperatorNumber=60003').fetchone()[0]
        assert last_row(cur.execute("DECLARE @R INT;EXEC @R=dbo.sp_SetUserOperatorName 7770003,?,'manual';SELECT @R",default_number))[0]==10
        # Other operator IDs retain the normal native naming flow.
        custom_number=cur.execute('SELECT UserOperatorNumber FROM dbo.tbl_UserOperators WHERE AccountNumber=7770001 AND OperatorNumber=?',new_id).fetchone()[0]
        cur.execute("UPDATE dbo.tbl_UserOperators SET OperatorNickname='',IsSetName=0 WHERE UserOperatorNumber=?",custom_number)
        assert last_row(cur.execute("DECLARE @R INT;EXEC @R=dbo.sp_SetUserOperatorName 7770001,?,'chosen';SELECT @R",custom_number))[0]==0
        # The immediate item receipt path also finalizes a fixed name, including
        # a purchased default after its delivery mode was changed to product.
        cur.execute("UPDATE dbo.sdgo_OperatorProfiles SET DeliveryMode='product' WHERE OperatorID=60013")
        cur.execute("UPDATE dbo.tbl_UserOperators SET OperatorNickname='',IsSetName=0 WHERE AccountNumber=7770003 AND OperatorNumber=60013")
        receipt_number=cur.execute('SELECT UserOperatorNumber FROM dbo.tbl_UserOperators WHERE AccountNumber=7770003 AND OperatorNumber=60013').fetchone()[0]
        cur.execute('EXEC dbo.sp_GetUserOperator 7770003,?',receipt_number)
        while cur.nextset():pass
        assert tuple(cur.execute('SELECT OperatorNickname,IsSetName FROM dbo.tbl_UserOperators WHERE UserOperatorNumber=?',receipt_number).fetchone())==('@SDGO:60013',1)
        trusted_apply(default_catalog)
        # Legacy Manager builds call the original creation procedure and do not
        # set a name afterward. The table guard finalizes only the fixed defaults
        # and does not disturb the native identity/output arguments.
        cur.execute('INSERT dbo.tbl_Users(AccountNumber,GoUserOperatorNum) VALUES(7770004,0)')
        def native_create(identity,category):
            return last_row(cur.execute("DECLARE @S INT,@U BIGINT,@Serial BIGINT,@R INT;EXEC @R=dbo.sp_CreateUserOperator 7770004,'',0,?,NULL,0,?,0,6,@S OUTPUT,@U OUTPUT,@Serial OUTPUT;SELECT @R,@S,@U",category,identity))
        created=native_create(60018,next(o['category_id'] for o in default_catalog['operators'] if o['id']==60018))
        assert created[0]==0 and created[1]>0 and created[2]>0
        assert tuple(cur.execute('SELECT OperatorNumber,OperatorNickname,IsSetName FROM dbo.tbl_UserOperators WHERE UserOperatorNumber=?',created[2]).fetchone())==(60018,'@SDGO:60018',1)
        custom_created=native_create(new_id,model.native_category(new_id))
        assert custom_created[0]==0
        assert tuple(cur.execute('SELECT OperatorNickname,IsSetName FROM dbo.tbl_UserOperators WHERE UserOperatorNumber=?',custom_created[2]).fetchone())==('',0)
        # Exercise the actual native Type_2=5 task-parts equipment gate for each
        # default, plus the original rejection on an unrelated stock operator.
        cur.execute('INSERT dbo.tbl_UserUnits VALUES(7779001,7770003,10001,18,0,4,0,0,0,0)')
        cur.execute('INSERT dbo.tbl_UnitCustomSkillRev VALUES(32001,5,1,1),(32002,1,1,1)')
        def equip_parts(skill):
            cur.execute('UPDATE dbo.tbl_UserUnits SET UnitSkill_1=0,UnitSkill_2=0,UnitSkill_3=0,UnitSkill_4=0 WHERE UserUnitNumber=7779001')
            return last_row(cur.execute('DECLARE @UL TINYINT,@L TINYINT,@Unit INT,@Max TINYINT,@R INT;EXEC dbo.sp_SetUserUnitCustomSkill 7770003,7779001,?,0,0,0,@UL OUTPUT,@L OUTPUT,@Unit OUTPUT,@Max OUTPUT,@R OUTPUT;SELECT @R',skill))[0]
        for identity in presets.NAMES:
            cur.execute('UPDATE dbo.tbl_Users SET GoUserOperatorNum=(SELECT UserOperatorNumber FROM dbo.tbl_UserOperators WHERE AccountNumber=7770003 AND OperatorNumber=?) WHERE AccountNumber=7770003',identity)
            assert equip_parts(32001)==0
            assert cur.execute('SELECT UnitSkill_1 FROM dbo.tbl_UserUnits WHERE UserUnitNumber=7779001').fetchone()[0]==32001
        cur.execute("INSERT dbo.tbl_UserOperators(AccountNumber,SlotNumber,OperatorNumber,CategoryNumber) VALUES(7770003,6,60001,1210001)")
        cur.execute('UPDATE dbo.tbl_Users SET GoUserOperatorNum=(SELECT UserOperatorNumber FROM dbo.tbl_UserOperators WHERE AccountNumber=7770003 AND OperatorNumber=60001) WHERE AccountNumber=7770003')
        assert equip_parts(32001)==2009 and equip_parts(32002)==0
        cur.execute("ALTER PROCEDURE dbo.sdgo_OperatorUniform @Bound INT,@Value INT OUTPUT AS BEGIN SET NOCOUNT ON;SELECT @Value=CASE WHEN @Bound=10000 THEN Roll ELSE Quantity END FROM dbo.FixtureRoll;SET @Value=@Value % @Bound;END")
        # Exercise the first and last member of every typed pool in SQL.
        for op in default_catalog['operators']:
            if not op['managed']:continue
            number=cur.execute('SELECT UserOperatorNumber FROM dbo.tbl_UserOperators WHERE AccountNumber=7770003 AND OperatorNumber=?',op['id']).fetchone()[0]
            offset=0
            for rule in op['drops']:
                members=default_catalog['drop_pools'][rule['pool']]['category_ids'] if 'pool' in rule else [rule['category_id']]
                for index in (0,len(members)-1):
                    cur.execute('UPDATE dbo.FixtureRoll SET Roll=?,Quantity=?',offset,index)
                    got=tuple(drop(7770003,number))
                    assert got[0]==members[index] and rule['min_amount']<=got[1]<=rule['max_amount'] and got[2]==0
                offset+=rule['chance_bps']
        # Every accepted design can be passed to the real native inventory proc.
        blueprints=default_catalog['drop_pools']['blueprints']['category_ids']
        for category in blueprints:
            # Test-only account can reuse empty synthetic blueprint slots.
            cur.execute('DELETE dbo.tbl_UserMixTreeItems WHERE AccountNumber=7770003')
            result=last_row(cur.execute('DECLARE @Count INT,@Number BIGINT,@R INT;EXEC dbo.sp_CreateUserMixTreeItem 7770003,0,?,1,11,@Count OUTPUT,@Number OUTPUT,@R OUTPUT;SELECT @R,@Count,@Number',category))
            assert result[0]==0 and result[1]==1 and result[2]>0
        trusted_apply(default_catalog)
        assert cur.execute('SELECT COUNT(*) FROM dbo.sdgo_OperatorLegacyEntitlementBackup').fetchone()[0]==1
        assert 'CRYPT_GEN_RANDOM' in server.definition(cur,'sdgo_OperatorUniform')
        default_results={'default_five_permanent_grants':True,'fixed_defaults_skip_native_naming':True,'other_operators_can_name_normally':True,
                         'all_five_allow_mission_parts':True,'mission_parts_disable_restores_original':True,
                         'native_task_parts_equipment_all_five':True,'other_parts_rules_preserved':True,
                         'legacy_creator_fixed_names_and_identity':True,
                         'existing_entitlements_migrated_in_place':True,
                         'typed_pool_sql_sampling':True,'native_blueprint_inventory_proc':len(blueprints)}
    final_runtime_catalog=default_catalog if default_catalog else catalog
    final_runtime_plan=runtime.plan_runtime_install(
        runtime_root,args.runtime_package,final_runtime_catalog,fixture_ini
    )
    assert final_runtime_plan['ready'] and not final_runtime_plan['actions']
    assert runtime.installed_runtime_evidence(final_runtime_plan)
    assert runtime_receipts and all(
        receipt['exact_readback']
        and receipt['fresh_installed_evidence']
        and receipt['database_result'] in {'applied','rejected'}
        for receipt in runtime_receipts
    )
    assert sum(
        receipt['database_result']=='rejected'
        for receipt in runtime_receipts
    )==1
    model.save(args.output/'catalog.json',catalog)
    model.save(args.output/'result.json',{'database':db,'synthetic_accounts_only':True,'source_game_database_written':False,
        'sql_master_files_under_test_root':len(source_files),
        'fixture_sql_files_under_test_root':len(fixture_files),
        'native_contract_procedure_fingerprints_verified':True,
        'trusted_runtime_apply_receipts':runtime_receipts,
        'all_catalog_applies_used_fresh_installed_runtime_evidence':True,
        'final_runtime_exact_readback':True,
        'native_procedures_exercised':True,'all_players_grant':True,'permanent_upgrade_preserves_nickname':True,
        'product_registration_without_storefront':True,'full_inventory_preserved':True,'cross_account_drop_rejected':True,
        'native_bonus_preserved_once':True,'zero_drop':True,'probability_boundaries':True,'quantity_bounds':True,
        'production_rng_restored':True,'idempotent_apply':True,
        'procedure_conflict_rolls_back':True,'database_transaction_atomic_rollback':True,
        'runtime_restored_after_rejected_apply':True,
        'purchased_disabled_operator_retained':True,'owned_ability_gc_safe':True,
        'retained_user_operator_idempotent':True,**default_results})
    print(json.dumps({'database':db,'result':'PASS','source_game_database_written':False}))
    cn.close()


if __name__=='__main__':main()
