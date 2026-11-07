"""Install one native mail adapter and an atomic server-side sort procedure.

No background process or SQL credential is required on players' computers.
The adapter reserves a nonexistent native recipient, derives the owner from
the server's authenticated mail call, and forwards all ordinary mail unchanged.
"""
from __future__ import annotations
import hashlib
from pathlib import Path
import re
from ... import storage
from ...database import connect

VERSION = '1.2.0'
RECIPIENT = 'SDGOHangar'
PREFIX = '!sdgo-sort-v1 '
GUARD = 'sdgo_mod_InventoryDeleteGuard'
AUDIT = 'sdgo_mod_HangarUnitDeleteAudit'
AUDIT_METADATA = {'AuditId','DeletedUtc','SessionId','ClientApplication','ClientHost'}
PARAMETERS = ['@v_idx','@v_pilot','@v_toIdx','@v_toPilot','@v_msg','@v_kind',
              '@v_productid','@v_giftboxIdx',*[f'@v_categoryNumber{i}' for i in range(1,5)],
              *[f'@v_validity{i}' for i in range(1,5)],'@v_outIdx','@retVal']
PARAMETER_TYPES = [('int',4,False),('nvarchar',40,False),('int',4,False),('nvarchar',40,False),
                   ('varchar',300,False),('char',1,False),('int',4,False),('bigint',8,False),
                   *[('int',4,False)]*8,('int',4,True),('int',4,True)]
HEADER = '''@v_idx int,@v_pilot nvarchar(20),@v_toIdx int,@v_toPilot nvarchar(20),
@v_msg varchar(300),@v_kind char(1),@v_productid int,@v_giftboxIdx bigint,
@v_categoryNumber1 int,@v_categoryNumber2 int,@v_categoryNumber3 int,@v_categoryNumber4 int,
@v_validity1 int,@v_validity2 int,@v_validity3 int,@v_validity4 int,
@v_outIdx int OUTPUT,@retVal int OUTPUT'''


def ident(name):
    if not isinstance(name,str) or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]{0,127}',name):
        raise ValueError('数据库标识符无效')
    return '['+name+']'


def digest(text):
    return hashlib.sha256(text.encode('utf-8')).hexdigest()


def unit_columns(cur):
    cur.execute("""SELECT name,TYPE_NAME(user_type_id),max_length,precision,scale,collation_name,
        is_nullable,is_identity,is_computed
        FROM sys.columns WHERE object_id=OBJECT_ID('dbo.tbl_UserUnits') ORDER BY column_id""")
    return [tuple(row) for row in cur.fetchall()]


def column_type(column):
    _,kind,length,precision,scale,collation,*_=column
    kind=kind.casefold()
    if kind in {'char','varchar','binary','varbinary'}:
        size='max' if length==-1 else str(length)
        result=f'{kind}({size})'
    elif kind in {'nchar','nvarchar'}:
        size='max' if length==-1 else str(length//2)
        result=f'{kind}({size})'
    elif kind in {'decimal','numeric'}:
        result=f'{kind}({precision},{scale})'
    elif kind in {'datetime2','datetimeoffset','time'}:
        result=f'{kind}({scale})'
    elif kind=='float':
        result=f'float({precision})'
    elif kind in {'bigint','int','smallint','tinyint','bit','date','datetime',
                  'smalldatetime','money','smallmoney','real','uniqueidentifier'}:
        result=kind
    else:
        raise ValueError('不支持的机库列类型：'+str(kind))
    if collation:
        if not isinstance(collation,str) or not re.fullmatch(r'[A-Za-z0-9_]{1,128}',collation):
            raise ValueError('不支持的机库排序规则名称')
        result+=' COLLATE '+collation
    return result


def column_shape(column):
    return tuple(column[:6])


def audit_shape(cur,columns):
    cur.execute("""SELECT name,TYPE_NAME(user_type_id),max_length,precision,scale,collation_name,
        is_nullable,is_identity,is_computed
        FROM sys.columns WHERE object_id=OBJECT_ID(?) ORDER BY column_id""",'dbo.'+AUDIT)
    rows=[tuple(row) for row in cur.fetchall()]
    if len(rows)!=len(columns)+5:return False
    metadata=rows[:5]
    if ([row[0] for row in metadata]!=['AuditId','DeletedUtc','SessionId','ClientApplication','ClientHost']
        or [row[1] for row in metadata]!=['bigint','datetime2','int','nvarchar','nvarchar']
        or tuple(bool(value) for value in metadata[0][6:])!=(False,True,False)
        or any(tuple(bool(value) for value in row[6:])!=(False,False,False)
               for row in metadata[1:])
        or metadata[1][4]!=3
        or metadata[3][2]!=256 or metadata[4][2]!=256):
        return False
    originals=rows[5:]
    return ([column_shape(row) for row in originals]==[column_shape(row) for row in columns]
            and all(tuple(bool(value) for value in row[6:])==(True,False,False)
                    for row in originals))


def guard_enabled(cur):
    cur.execute("""SELECT is_disabled,parent_id,OBJECT_ID('dbo.tbl_UserUnits')
        FROM sys.triggers WHERE object_id=OBJECT_ID(?)""",'dbo.'+GUARD)
    row=cur.fetchone()
    return bool(row and not row[0] and row[1]==row[2])


def create_audit(cur,columns):
    if AUDIT_METADATA&{row[0] for row in columns}:
        raise ValueError('机库列名与删除审计元数据冲突')
    fields=','.join(ident(row[0])+' '+column_type(row)+' NULL' for row in columns)
    cur.execute(f'''CREATE TABLE dbo.{AUDIT}(
        AuditId bigint IDENTITY(1,1) NOT NULL CONSTRAINT PK_{AUDIT} PRIMARY KEY,
        DeletedUtc datetime2(3) NOT NULL,
        SessionId int NOT NULL,
        ClientApplication nvarchar(128) NOT NULL,
        ClientHost nvarchar(128) NOT NULL,
        {fields})''')
    cur.execute(f'''CREATE INDEX IX_{AUDIT}_AccountTime ON dbo.{AUDIT}
        (AccountNumber,DeletedUtc) INCLUDE(UserUnitNumber,UnitNumber,UserUnitSlotNumber)''')


def rename_original(text, name, alter=False):
    pattern=r'\b(?:CREATE\s+(?:OR\s+ALTER\s+)?|ALTER\s+)(?:PROC|PROCEDURE)\s+(?:\[?dbo\]?\s*\.\s*)?\[?sp_SendMsgGame\]?'
    result,n=re.subn(pattern,('ALTER' if alter else 'CREATE')+' PROCEDURE dbo.'+name,text,count=1,flags=re.I)
    if n!=1: raise ValueError('原生邮件过程签名无法识别')
    return result


def wrapper(game):
    forwarded=','.join(p+(' OUTPUT' if p in ('@v_outIdx','@retVal') else '') for p in PARAMETERS)
    return f'''ALTER PROCEDURE dbo.sp_SendMsgGame {HEADER}
AS
BEGIN
 SET NOCOUNT ON;
 IF @v_toIdx=0 AND @v_toPilot=N'{RECIPIENT}' AND @v_kind='1'
 BEGIN
   SET @retVal=5; SET @v_outIdx=0;
   IF @v_productid<>0 OR @v_giftboxIdx<>0 OR @v_idx<=0 RETURN;
   IF NOT EXISTS(SELECT 1 FROM dbo.tbl_Account_Pilot WHERE f_idx=@v_idx AND f_pilot=@v_pilot) RETURN;
   -- Never route a reserved command into a real player's mailbox.
   IF EXISTS(SELECT 1 FROM dbo.tbl_Account_Pilot WHERE f_pilot=N'{RECIPIENT}') RETURN;
   IF LEFT(@v_msg,12)='!sdgo-gb-v1 '
   BEGIN
     EXEC {ident(game)}.dbo.sdgo_mod_HangarGBCommand @v_idx,@v_msg,@retVal OUTPUT;
     IF @retVal=0 SET @v_outIdx=@v_idx;
     RETURN;
   END;
   IF LEFT(@v_msg,{len(PREFIX)})<>'{PREFIX}' OR LEN(@v_msg)<>{len(PREFIX)+36} RETURN;
   DECLARE @Request uniqueidentifier;
   BEGIN TRY SET @Request=CONVERT(uniqueidentifier,SUBSTRING(@v_msg,{len(PREFIX)+1},36)); END TRY
   BEGIN CATCH RETURN; END CATCH;
   IF @Request IS NULL RETURN;
   DECLARE @Changed int,@Total int;
   BEGIN TRY
     EXEC {ident(game)}.dbo.sdgo_mod_SortHangar @v_idx,@Request,@Changed OUTPUT,@Total OUTPUT;
     SET @v_outIdx=@v_idx; SET @retVal=0;
   END TRY
   BEGIN CATCH
     IF XACT_STATE()<>0 ROLLBACK TRANSACTION;
     SET @retVal=5;
   END CATCH;
   RETURN;
 END;
 EXEC dbo.sdgo_mod_HangarMailOriginal {forwarded};
END;'''


class Server:
    def __init__(self, ini):
        self.ini=Path(ini)
        config=storage.ini_values(self.ini.read_bytes())
        game=config['DB Game'];common=config['DB Member'];billing=config['DB StoreBilling']
        if len({x['DBServerName'].strip().casefold() for x in (game,common,billing)})!=1:
            raise ValueError('机库排序要求 Game 与 Member 数据库位于同一 SQL 实例')
        self.game=game['DBName'];self.common=common['DBName'];self.billing=billing['DBName']
        ident(self.game);ident(self.common);ident(self.billing)

    def connection(self):
        return connect(self.ini,'DB Member')

    def status(self):
        cn=self.connection()
        try:
            cur=cn.cursor()
            cur.execute("SELECT OBJECT_DEFINITION(OBJECT_ID('dbo.sp_SendMsgGame'))")
            live=cur.fetchone()[0]
            cur.execute("SELECT OBJECT_ID('dbo.sdgo_mod_HangarMailBackup','U')")
            if not cur.fetchone()[0]:
                cur.execute('USE '+ident(self.game))
                columns=unit_columns(cur);audit_valid=audit_shape(cur,columns);audit_rows=0
                if audit_valid:
                    cur.execute('SELECT COUNT_BIG(*) FROM dbo.'+AUDIT)
                    audit_rows=int(cur.fetchone()[0])
                return {'installed':False,'version':VERSION,'activation':'server_install_required',
                        'delete_guard_installed':False,'delete_audit_rows':audit_rows,
                        'inventory_changed_by_check':False}
            cur.execute('SELECT InstalledHash,GameInstalledHash FROM dbo.sdgo_mod_HangarMailBackup WHERE Id=1')
            row=cur.fetchone()
            base_installed=bool(row and live and digest(live)==row[0])
            cur.execute("SELECT COL_LENGTH('dbo.sdgo_mod_HangarMailBackup','GBInstalledHash')")
            gb_hash=guard_hash=None
            if cur.fetchone()[0]:
                cur.execute('SELECT GBInstalledHash FROM dbo.sdgo_mod_HangarMailBackup WHERE Id=1')
                gb_row=cur.fetchone();gb_hash=gb_row[0] if gb_row else None
            cur.execute("SELECT COL_LENGTH('dbo.sdgo_mod_HangarMailBackup','GuardInstalledHash')")
            if cur.fetchone()[0]:
                cur.execute('SELECT GuardInstalledHash FROM dbo.sdgo_mod_HangarMailBackup WHERE Id=1')
                guard_row=cur.fetchone();guard_hash=guard_row[0] if guard_row else None
            cur.execute('USE '+ident(self.game))
            columns=unit_columns(cur)
            cur.execute("SELECT COLUMN_NAME,DATA_TYPE FROM "+ident(self.billing)+".INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA='dbo' AND TABLE_NAME='tbls_AccountPocket'")
            wallet_columns=dict(cur.fetchall())
            if wallet_columns.get('f_idx')!='int' or wallet_columns.get('f_GP')!='int':raise ValueError('原生 GB 钱包表结构不匹配，未安装')
            cur.execute("SELECT OBJECT_DEFINITION(OBJECT_ID('dbo.sdgo_mod_SortHangar'))")
            sql=cur.fetchone()[0]
            sort_installed=base_installed and bool(sql and row[1] and digest(sql)==row[1])
            cur.execute("SELECT OBJECT_DEFINITION(OBJECT_ID('dbo.sdgo_mod_HangarGBCommand'))")
            gb_sql=cur.fetchone()[0];gb_installed=bool(sort_installed and gb_hash and gb_sql and digest(gb_sql)==gb_hash)
            cur.execute("SELECT OBJECT_DEFINITION(OBJECT_ID('dbo."+GUARD+"'))")
            guard_sql=cur.fetchone()[0]
            audit_valid=audit_shape(cur,columns)
            guard_installed=bool(sort_installed and guard_hash and guard_sql and
                                 digest(guard_sql)==guard_hash and audit_valid and
                                 guard_enabled(cur))
            installed=sort_installed and guard_installed
            audit_rows=0
            if audit_valid:
                cur.execute('SELECT COUNT_BIG(*) FROM dbo.'+AUDIT)
                audit_rows=int(cur.fetchone()[0])
            gb_counts={}
            if gb_installed:
                cur.execute('SELECT State,COUNT(*) FROM dbo.sdgo_mod_HangarGBJob GROUP BY State')
                gb_counts={r[0]:int(r[1]) for r in cur.fetchall()}
            completed=0;last=None
            if installed:
                cur.execute('SELECT COUNT(*),MAX(CompletedUtc) FROM dbo.sdgo_mod_HangarSortReceipt')
                count=cur.fetchone();completed=int(count[0]);last=str(count[1]) if count[1] else None
            return {'installed':installed,'version':VERSION,'activation':'relogin_required',
                    'mechanism':'native authenticated request -> transactional server sort',
                    'completed_requests':completed,'last_completed_utc':last,
                    'gb_batch_installed':gb_installed,'gb_batches':gb_counts,
                    'delete_guard_installed':guard_installed,
                    'delete_audit_rows':audit_rows,
                    'unrelated_mail_preserved':True,'inventory_changed_by_check':False}
        finally:cn.close()

    def install(self):
        cn=self.connection()
        try:
            cur=cn.cursor()
            cur.execute('SELECT DB_NAME()')
            if cur.fetchone()[0]!=self.common:raise ValueError('数据库连接目标不一致')
            cur.execute("SELECT COLUMN_NAME,DATA_TYPE FROM "+ident(self.billing)+".INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA='dbo' AND TABLE_NAME='tbls_AccountPocket'")
            wallet_columns=dict(cur.fetchall())
            if wallet_columns.get('f_idx')!='int' or wallet_columns.get('f_GP')!='int':raise ValueError('原生 GB 钱包表结构不匹配，未安装')
            cur.execute("SELECT OBJECT_DEFINITION(OBJECT_ID('dbo.sp_SendMsgGame'))")
            live=cur.fetchone()[0]
            if not live:raise ValueError('未找到原生邮件存储过程')
            cur.execute("SELECT name,TYPE_NAME(user_type_id),max_length,is_output FROM sys.parameters WHERE object_id=OBJECT_ID('dbo.sp_SendMsgGame') ORDER BY parameter_id")
            parameters=cur.fetchall()
            if ([r[0].casefold() for r in parameters]!=[p.casefold() for p in PARAMETERS]
                or [(r[1],r[2],bool(r[3])) for r in parameters]!=PARAMETER_TYPES):
                raise ValueError('原生邮件参数不匹配，拒绝安装')
            cur.execute('SELECT COUNT(*) FROM dbo.tbl_Account_Pilot WHERE f_pilot=?',RECIPIENT)
            if cur.fetchone()[0]:raise ValueError('保留的机库通道名称已被玩家使用')
            cur.execute("IF OBJECT_ID('dbo.sdgo_mod_HangarMailBackup','U') IS NULL CREATE TABLE dbo.sdgo_mod_HangarMailBackup(Id int NOT NULL PRIMARY KEY CHECK(Id=1),OriginalSql nvarchar(max) NOT NULL,InstalledHash varchar(64) NULL,GameInstalledHash varchar(64) NULL)")
            cur.execute("IF COL_LENGTH('dbo.sdgo_mod_HangarMailBackup','GBInstalledHash') IS NULL ALTER TABLE dbo.sdgo_mod_HangarMailBackup ADD GBInstalledHash varchar(64) NULL")
            cur.execute("IF COL_LENGTH('dbo.sdgo_mod_HangarMailBackup','GuardInstalledHash') IS NULL ALTER TABLE dbo.sdgo_mod_HangarMailBackup ADD GuardInstalledHash varchar(64) NULL")
            cur.execute('SELECT OriginalSql,InstalledHash,GameInstalledHash,GBInstalledHash,GuardInstalledHash FROM dbo.sdgo_mod_HangarMailBackup WITH(UPDLOCK,HOLDLOCK) WHERE Id=1')
            old=cur.fetchone()
            if old:
                if digest(live)!=old[1]:raise ValueError('原生邮件过程有其他修改，请先核对，未覆盖')
                original=old[0]
            else:
                original=live
                cur.execute('INSERT dbo.sdgo_mod_HangarMailBackup(Id,OriginalSql) VALUES(1,?)',original)
                cur.execute(rename_original(original,'sdgo_mod_HangarMailOriginal'))
            cur.execute('USE '+ident(self.game))
            cur.execute("SELECT OBJECT_DEFINITION(OBJECT_ID('dbo.sdgo_mod_SortHangar'))")
            previous_sort=cur.fetchone()[0]
            if previous_sort and (not old or not old[2] or digest(previous_sort)!=old[2]):
                raise ValueError('服务端排序过程有其他修改，未覆盖')
            columns=unit_columns(cur)
            required={'UserUnitNumber','AccountNumber','UserUnitSlotNumber','UnitNumber','DateGet','LockState','BattleShipCount','UnitLevelNumber','TimeLimit','RunOut','Forbidden'}
            if not required.issubset({r[0] for r in columns}):raise ValueError('机库表结构不匹配')
            for column in columns:column_type(column)
            immutable=','.join(ident(r[0]) for r in columns if r[0]!='UserUnitSlotNumber')
            cur.execute("IF OBJECT_ID('dbo.sdgo_mod_HangarSortReceipt','U') IS NULL CREATE TABLE dbo.sdgo_mod_HangarSortReceipt(AccountNumber int NOT NULL,RequestId uniqueidentifier NOT NULL,ChangedSlots int NOT NULL,UnitCount int NOT NULL,CompletedUtc datetime2 NOT NULL DEFAULT SYSUTCDATETIME(),PRIMARY KEY(AccountNumber,RequestId))")
            sql=Path(__file__).with_name('sort.sql').read_text(encoding='utf-8')
            cur.execute("IF OBJECT_ID('dbo.sdgo_mod_SortHangar','P') IS NULL EXEC(N'CREATE PROCEDURE dbo.sdgo_mod_SortHangar AS RETURN;')")
            cur.execute(sql.replace('__IMMUTABLE_COLUMNS__',immutable))
            cur.execute("SELECT OBJECT_DEFINITION(OBJECT_ID('dbo.sdgo_mod_SortHangar'))")
            game_hash=digest(cur.fetchone()[0])
            cur.execute("SELECT OBJECT_DEFINITION(OBJECT_ID('dbo.sdgo_mod_HangarGBCommand'))")
            existing_gb=cur.fetchone()[0]
            if existing_gb and (not old or not old[3] or digest(existing_gb)!=old[3]):raise ValueError('批量回收过程有其他修改，未覆盖')
            cur.execute("SELECT OBJECT_DEFINITION(OBJECT_ID('dbo."+GUARD+"'))")
            existing_guard=cur.fetchone()[0]
            if existing_guard and (not old or not old[4] or digest(existing_guard)!=old[4]):
                raise ValueError('机体删除防护触发器有其他修改，未覆盖')
            cur.execute("IF OBJECT_ID('dbo.sp_DeleteUserUnit','P') IS NULL RAISERROR('Native dismantle procedure missing',16,1)")
            cur.execute("IF OBJECT_ID('dbo.sdgo_mod_HangarGBJob','U') IS NULL CREATE TABLE dbo.sdgo_mod_HangarGBJob(AccountNumber int NOT NULL,RequestId uniqueidentifier NOT NULL,HeaderSet bit NOT NULL DEFAULT 0,CommitRequested bit NOT NULL DEFAULT 0,KeepCopies int NULL,EntryCount int NULL,ExpectedGB bigint NULL,SortAfter bit NULL,State varchar(12) NOT NULL DEFAULT 'pending',RemovedUnits int NULL,AwardedGB bigint NULL,LastError nvarchar(200) NULL,CreatedUtc datetime2 NOT NULL DEFAULT SYSUTCDATETIME(),CompletedUtc datetime2 NULL,PRIMARY KEY(AccountNumber,RequestId))")
            cur.execute("IF OBJECT_ID('dbo.sdgo_mod_HangarGBSelection','U') IS NULL CREATE TABLE dbo.sdgo_mod_HangarGBSelection(AccountNumber int NOT NULL,RequestId uniqueidentifier NOT NULL,Position int NOT NULL,UserUnitNumber bigint NOT NULL,UnitNumber int NOT NULL,SlotNumber int NOT NULL,PRIMARY KEY(AccountNumber,RequestId,Position),UNIQUE(AccountNumber,RequestId,UserUnitNumber))")
            cur.execute("IF OBJECT_ID('dbo.sdgo_mod_HangarGBCommand','P') IS NULL EXEC(N'CREATE PROCEDURE dbo.sdgo_mod_HangarGBCommand AS RETURN;')")
            gb_template=Path(__file__).with_name('gb.sql').read_text(encoding='utf-8')
            cur.execute(gb_template.replace('__ALL_COLUMNS__',','.join(ident(r[0]) for r in columns)).replace('__BILLING_DB__',ident(self.billing)))
            cur.execute("SELECT OBJECT_DEFINITION(OBJECT_ID('dbo.sdgo_mod_HangarGBCommand'))")
            gb_hash=digest(cur.fetchone()[0])
            cur.execute("SELECT OBJECT_ID('dbo."+AUDIT+"','U')")
            if not cur.fetchone()[0]:create_audit(cur,columns)
            if not audit_shape(cur,columns):raise ValueError('机体删除审计表结构不匹配，未覆盖')
            cur.execute("IF OBJECT_ID('dbo."+GUARD+"','TR') IS NULL EXEC(N'CREATE TRIGGER dbo."+GUARD+" ON dbo.tbl_UserUnits AFTER DELETE AS RETURN;')")
            guard_template=Path(__file__).with_name('guard.sql').read_text(encoding='utf-8')
            all_columns=','.join(ident(r[0]) for r in columns)
            deleted_columns=','.join('D.'+ident(r[0]) for r in columns)
            cur.execute(guard_template.replace('__ALL_COLUMNS__',all_columns).replace('__DELETED_COLUMNS__',deleted_columns))
            cur.execute('ENABLE TRIGGER dbo.'+GUARD+' ON dbo.tbl_UserUnits')
            cur.execute("SELECT OBJECT_DEFINITION(OBJECT_ID('dbo."+GUARD+"'))")
            guard_hash=digest(cur.fetchone()[0])
            if not guard_enabled(cur):raise ValueError('机体删除防护触发器未启用')
            cur.execute('USE '+ident(self.common))
            cur.execute(wrapper(self.game))
            cur.execute("SELECT OBJECT_DEFINITION(OBJECT_ID('dbo.sp_SendMsgGame'))")
            installed=digest(cur.fetchone()[0])
            cur.execute('UPDATE dbo.sdgo_mod_HangarMailBackup SET InstalledHash=?,GameInstalledHash=?,GBInstalledHash=?,GuardInstalledHash=? WHERE Id=1',installed,game_hash,gb_hash,guard_hash)
            cn.commit()
        except Exception:
            cn.rollback();raise
        finally:cn.close()
        return self.status()

    def remove(self):
        cn=self.connection()
        try:
            cur=cn.cursor()
            cur.execute("SELECT COL_LENGTH('dbo.sdgo_mod_HangarMailBackup','GuardInstalledHash')")
            if cur.fetchone()[0]:
                cur.execute('SELECT OriginalSql,InstalledHash,GameInstalledHash,GBInstalledHash,GuardInstalledHash FROM dbo.sdgo_mod_HangarMailBackup WITH(UPDLOCK,HOLDLOCK) WHERE Id=1')
            else:
                cur.execute('SELECT OriginalSql,InstalledHash,GameInstalledHash,GBInstalledHash,CAST(NULL AS varchar(64)) FROM dbo.sdgo_mod_HangarMailBackup WITH(UPDLOCK,HOLDLOCK) WHERE Id=1')
            row=cur.fetchone()
            cur.execute("SELECT OBJECT_DEFINITION(OBJECT_ID('dbo.sp_SendMsgGame'))")
            if not row or digest(cur.fetchone()[0])!=row[1]:raise ValueError('邮件过程已变化，拒绝覆盖')
            cur.execute('USE '+ident(self.game))
            cur.execute("SELECT OBJECT_DEFINITION(OBJECT_ID('dbo.sdgo_mod_SortHangar'))")
            sql=cur.fetchone()[0]
            if not sql or digest(sql)!=row[2]:raise ValueError('排序过程已变化，拒绝移除')
            cur.execute("SELECT OBJECT_DEFINITION(OBJECT_ID('dbo.sdgo_mod_HangarGBCommand'))")
            gb=cur.fetchone()[0]
            if gb and (not row[3] or digest(gb)!=row[3]):raise ValueError('批量回收过程已变化，拒绝移除')
            cur.execute("SELECT OBJECT_DEFINITION(OBJECT_ID('dbo."+GUARD+"'))")
            guard=cur.fetchone()[0]
            if guard and (not row[4] or digest(guard)!=row[4]):raise ValueError('机体删除防护触发器已变化，拒绝移除')
            if row[4] and not guard:raise ValueError('机体删除防护触发器已丢失，拒绝移除')
            if guard:cur.execute('DROP TRIGGER dbo.'+GUARD)
            if gb:cur.execute('DROP PROCEDURE dbo.sdgo_mod_HangarGBCommand')
            cur.execute('DROP PROCEDURE dbo.sdgo_mod_SortHangar')
            cur.execute('USE '+ident(self.common))
            cur.execute(rename_original(row[0],'sp_SendMsgGame',alter=True))
            cur.execute('DROP PROCEDURE dbo.sdgo_mod_HangarMailOriginal')
            cur.execute('DELETE dbo.sdgo_mod_HangarMailBackup WHERE Id=1')
            cn.commit()
        except Exception:cn.rollback();raise
        finally:cn.close()
        return {'installed':False,'inventory_changed':False,'receipts_preserved':True,
                'delete_audit_preserved':True}
