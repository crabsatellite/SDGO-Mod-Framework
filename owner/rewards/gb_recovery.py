"""Average-based GB compensation through the unchanged native wallet procedure.

Inputs are verified historical wallet records, not reconstructed combat scores.
The receipt key is the original native transaction so retries cannot pay twice.
"""
from __future__ import annotations

from datetime import datetime
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import re

LOG_TABLE = re.compile(r'tbls_LogGPInOut(?:[0-9]{8})?\Z')
RECEIPTS = 'SdgoBattleRewardGbRecovery'


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=False)


def digest(value):
    return hashlib.sha256(canonical(value).encode()).hexdigest()


def integer(value, maximum=2147483647):
    if type(value) is not int or not 0 <= value <= maximum:
        raise ValueError('Invalid native wallet integer')
    return value


def transaction(value):
    expected={'account','gb','after','local_time','table'}
    if not isinstance(value,dict) or set(value)!=expected:
        raise ValueError('A complete native GB source record is required')
    result={key:value[key] for key in expected}
    if integer(result['account'])==0:raise ValueError('Account is required')
    integer(result['gb'],65535);integer(result['after'])
    if result['after']<result['gb']:raise ValueError('Source balance is inconsistent')
    if not LOG_TABLE.fullmatch(result['table']):raise ValueError('Invalid native log table')
    stamp=datetime.fromisoformat(result['local_time'])
    if stamp.tzinfo is not None:raise ValueError('Native source time must be database-local')
    result['local_time']=stamp.isoformat(timespec='milliseconds')
    return result


def average(samples):
    if not samples:raise ValueError('Verified normal settlement samples are required')
    keys=set();total=Fraction(0)
    for sample in samples:
        source=transaction(sample['source']);key=digest(source)
        if key in keys:raise ValueError('Duplicate average sample')
        keys.add(key)
        event=sample['event_multiplier']
        if type(event) is not int or event not in (1,2):raise ValueError('Invalid event multiplier')
        total+=Fraction(source['gb'],event)
    return total/len(samples)


def build_entries(samples,sources):
    benchmark=average(samples);keys=set();entries=[]
    for candidate in sources:
        source=transaction(candidate['source']);key=digest(source)
        if key in keys:raise ValueError('Duplicate compensation source')
        keys.add(key)
        event=candidate['event_multiplier']
        if type(event) is not int or event not in (1,2):raise ValueError('Invalid event multiplier')
        target=benchmark*event;target=target.numerator//target.denominator
        integer(target,65535)
        entries.append({'source':source,'source_key':key,'event_multiplier':event,
            'target_gb':target,'delta_gb':max(0,target-source['gb'])})
    return entries


def verify_plan(plan):
    if plan.get('schema_version')!=1 or plan.get('method')!='healthy_average_gb':
        raise ValueError('Unsupported GB compensation plan')
    expected=build_entries(plan['samples'],plan['sources'])
    if expected!=plan['entries']:raise ValueError('Compensation amounts do not match the approved average')
    if not re.fullmatch(r'[0-9a-f-]{36}',plan['expected_scope']):raise ValueError('Wallet scope is required')
    if not plan.get('expected_data_root'):raise ValueError('Explicit database file scope is required')
    if len(expected)>10000:raise ValueError('Compensation plan is too large')
    return expected


DDL = """
IF OBJECT_ID(N'dbo.SdgoBattleRewardGbRecovery',N'U') IS NULL
BEGIN
 CREATE TABLE dbo.SdgoBattleRewardGbRecovery(
  SourceKey char(64) NOT NULL PRIMARY KEY,
  PlanHash char(64) NOT NULL,
  AccountNumber int NOT NULL,
  OriginalGb int NOT NULL,
  TargetGb int NOT NULL,
  DeltaGb int NOT NULL,
  BeforeGb int NOT NULL,
  AfterGb int NOT NULL,
  NativeRetVal int NOT NULL,
  AppliedUtc datetime2(3) NOT NULL,
  CONSTRAINT CK_SdgoBattleRewardGbRecovery_Delta CHECK(DeltaGb>=0 AND AfterGb=BeforeGb+DeltaGb),
  CONSTRAINT CK_SdgoBattleRewardGbRecovery_Native CHECK(NativeRetVal=0)
 );
 EXEC sys.sp_addextendedproperty @name=N'SDGO.BattleRewardRecovery',@value=N'gb-average-v1',
  @level0type=N'SCHEMA',@level0name=N'dbo',@level1type=N'TABLE',@level1name=N'SdgoBattleRewardGbRecovery';
END;
IF NOT EXISTS(SELECT 1 FROM sys.extended_properties WHERE class=1
 AND major_id=OBJECT_ID(N'dbo.SdgoBattleRewardGbRecovery')
 AND name=N'SDGO.BattleRewardRecovery' AND CONVERT(nvarchar(128),value)=N'gb-average-v1')
 THROW 51301,'GB recovery table is not owned by this protocol',1;
"""


def scalar(cursor,sql,*parameters):
    cursor.execute(sql,*parameters)
    while True:
        if cursor.description:
            rows=cursor.fetchall()
            if len(rows)!=1:raise ValueError('Expected exactly one native result')
            return rows[0]
        if not cursor.nextset():raise ValueError('Native result is missing')


def verify_source(cursor,source):
    source=transaction(source)
    table='dbo.['+source['table']+']'
    count=scalar(cursor,'SELECT COUNT(*) FROM '+table+' WITH(HOLDLOCK) WHERE '
        'f_idx=? AND f_GP=? AND f_retGP=? AND f_regdate=? AND f_from=2 AND f_etc=1',
        source['account'],source['gb'],source['after'],source['local_time'])[0]
    if count!=1:raise ValueError('Original native GB credit cannot be uniquely verified')


def resolve_database_files(paths, data_directory=None):
    """Resolve legacy LiteSQL Data paths against the instance's own data directory."""
    result=[]
    for raw in paths:
        path=Path(raw)
        if not path.is_absolute():
            # Original LiteSQL records .\..\Data\name rather than an absolute path.
            # Never resolve that against this maintenance tool's working directory.
            if path.parts[:2]!=('..','Data') or len(path.parts)!=3:
                raise ValueError('Unrecognized relative native database path')
            if not data_directory or not Path(data_directory).is_absolute():
                raise ValueError('Native instance data directory is required')
            path=Path(data_directory)/path.name
        result.append(path.resolve())
    return result


def apply(connection,plan):
    entries=verify_plan(plan)
    cursor=connection.cursor();owned=False
    try:
        database=scalar(cursor,'SELECT DB_NAME()')[0]
        if database!='GDStoreBilling':raise ValueError('Select the native StoreBilling database')
        paths=[row[0] for row in cursor.execute('SELECT physical_name FROM sys.database_files').fetchall()]
        data_directory=None
        if any(not Path(path).is_absolute() for path in paths):
            data_directory=scalar(cursor,"SELECT CONVERT(nvarchar(260),SERVERPROPERTY('InstanceDefaultDataPath'))")[0]
            master=Path(scalar(cursor,'SELECT physical_name FROM sys.master_files WHERE database_id=1 AND file_id=1')[0])
            if not data_directory or not master.is_absolute() or master.parent.resolve()!=Path(data_directory).resolve():
                raise ValueError('Native instance data directory could not be verified')
        files=resolve_database_files(paths,data_directory)
        data_root=Path(plan['expected_data_root']).resolve()
        if not files or any(not path.is_relative_to(data_root) for path in files):
            raise ValueError('Database files do not belong to the selected data scope')
        scope=str(scalar(cursor,'SELECT ScopeId FROM dbo.SdgoBattleRewardWalletMeta WHERE Singleton=1')[0]).lower()
        if scope!=plan['expected_scope']:raise ValueError('Native wallet scope changed')
        owned=scalar(cursor,'SELECT @@TRANCOUNT')[0]==0
        cursor.execute('SET XACT_ABORT ON;'+('BEGIN TRANSACTION;' if owned else 'SAVE TRANSACTION SdgoGbRecovery;'))
        lock=scalar(cursor,"DECLARE @r int;EXEC @r=sys.sp_getapplock @Resource=N'SDGO.BattleRewardGbRecovery',"
            "@LockMode='Exclusive',@LockOwner='Transaction',@LockTimeout=10000;SELECT @r;")[0]
        if lock<0:raise ValueError('Another compensation transaction owns the recovery ledger')
        for sample in plan['samples']:
            verify_source(cursor,sample['source'])
            proof=scalar(cursor,'SELECT COUNT(*) FROM dbo.SdgoBattleRewardMbReceipt WHERE MatchId=? AND AccountNumber=? AND PlanHash=?',
                sample['match_id'],sample['source']['account'],sample['plan_hash'])[0]
            if proof!=1:raise ValueError('Average sample was not an acknowledged healthy settlement')
        cursor.execute(DDL)
        results=[]
        for entry in entries:
            source=entry['source'];key=entry['source_key']
            row_hash=digest({'entry':entry,'benchmark':{'samples':plan['samples']},'method':plan['method']})
            previous=cursor.execute('SELECT PlanHash,DeltaGb,AfterGb FROM dbo.'+RECEIPTS+' WITH(UPDLOCK,HOLDLOCK) WHERE SourceKey=?',key).fetchone()
            if previous:
                if previous[0].strip()!=row_hash:raise ValueError('A compensation receipt already exists with different terms')
                results.append({'source_key':key,'account':source['account'],'delta_gb':previous[1],'after_gb':previous[2],'already_applied':True})
                continue
            verify_source(cursor,source)
            before=scalar(cursor,'SELECT f_GP FROM dbo.tbls_AccountPocket WITH(UPDLOCK,HOLDLOCK) WHERE f_idx=?',source['account'])[0]
            delta=entry['delta_gb']
            if before>2147483647-delta:raise ValueError('Native GB wallet would overflow')
            after=before;code=0
            if delta:
                code,after=scalar(cursor,'DECLARE @r int=-1,@b int=-1;EXEC dbo.sp_PocketGP_IO '
                    '@type=2,@s_idx=?,@s_GP=?,@s_from=2,@s_etc=31603,@retVal=@r OUTPUT,@retGP=@b OUTPUT;SELECT @r,@b;',
                    source['account'],delta)
                if code!=0 or after!=before+delta:raise ValueError('Native GB credit was not acknowledged')
            cursor.execute('INSERT dbo.'+RECEIPTS+'(SourceKey,PlanHash,AccountNumber,OriginalGb,TargetGb,DeltaGb,BeforeGb,AfterGb,NativeRetVal,AppliedUtc) '
                'VALUES(?,?,?,?,?,?,?,?,?,SYSUTCDATETIME())',key,row_hash,source['account'],source['gb'],entry['target_gb'],delta,before,after,code)
            results.append({'source_key':key,'account':source['account'],'delta_gb':delta,'after_gb':after,'already_applied':False})
        if owned:cursor.execute('COMMIT TRANSACTION;')
        return results
    except Exception:
        try:
            state=scalar(cursor,'SELECT XACT_STATE()')[0]
            if state:cursor.execute('ROLLBACK TRANSACTION;' if owned or state==-1 else 'ROLLBACK TRANSACTION SdgoGbRecovery;')
        except Exception:pass
        raise
    finally:cursor.close()
