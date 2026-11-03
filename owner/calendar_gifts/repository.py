"""Atomic receipts plus native Storage deliveries, without editing native code."""
from __future__ import annotations
import hashlib
import json
from pathlib import Path
import random
from . import model


class Repository:
    def __init__(self,connection):
        if not connection.autocommit:raise ValueError('日期赠礼需要独立的自动提交连接')
        self.connection=connection

    def install(self):
        c=self.connection.cursor()
        try:
            c.execute(Path(__file__).with_name('schema.sql').read_text(encoding='utf-8'))
            while c.nextset():pass
        finally:c.close()

    def accounts(self,occurrence,people):
        selected=occurrence['account_id']
        targets=None
        if selected is not None:targets=[selected]
        elif occurrence['group']!='all':
            if occurrence['group'] not in people['groups']:raise ValueError('活动的本服玩家组不存在')
            targets=sorted(set(people['groups'][occurrence['group']]))
        if targets==[]:return []
        c=self.connection.cursor()
        try:
            installed=c.execute("SELECT OBJECT_ID(N'sdgo_calendar_gifts.Receipt',N'U')").fetchval()
            query='SELECT u.AccountNumber FROM dbo.tbl_Users u WHERE 1=1 '
            missing='AND NOT EXISTS(SELECT 1 FROM sdgo_calendar_gifts.Receipt r WHERE r.AccountNumber=u.AccountNumber AND r.EventKey=?) ' if installed else ''
            if targets is None:
                params=(occurrence['key'],) if installed else ()
                return [int(r[0]) for r in c.execute(query+missing+'ORDER BY u.AccountNumber',*params)]
            result=[]
            for start in range(0,len(targets),1000):
                chunk=targets[start:start+1000];params=[*chunk,*([occurrence['key']] if installed else [])]
                result.extend(int(r[0]) for r in c.execute(query+'AND u.AccountNumber IN ('+','.join('?' for _ in chunk)+') '+missing+'ORDER BY u.AccountNumber',*params))
            return sorted(result)
        finally:c.close()

    def catalog(self,c):
        rows=c.execute('''SELECT u.UnitNumber,u.UnitGradeNumber,u.UnitGradeNumber2,MIN(c.CatalogueID)
            FROM dbo.tbl_Units u JOIN dbo.tbl_ItemCategory c ON c.RefIndex=u.UnitNumber
            AND c.CatalogueID/100000=11 AND c.TimeLimit=0 AND c.Option1=1 AND c.Option2=2
            WHERE u.UnitGradeNumber BETWEEN 1 AND 4 AND u.UnitGradeNumber2 BETWEEN 2 AND 5
            GROUP BY u.UnitNumber,u.UnitGradeNumber,u.UnitGradeNumber2 ORDER BY u.UnitNumber''').fetchall()
        return [{'unit_id':int(r[0]),'grade':(int(r[1]),int(r[2])),'category_id':int(r[3])} for r in rows
                if (int(r[1]),int(r[2])) in model.GRADE_NAMES]

    def owned(self,c,account):
        owned={int(r[0]) for r in c.execute('SELECT UnitNumber FROM dbo.tbl_UserUnits WITH(UPDLOCK,HOLDLOCK) WHERE AccountNumber=?',account)}
        owned.update(int(r[0]) for r in c.execute('''SELECT c.RefIndex FROM dbo.tbl_UserDropItemBox b WITH(UPDLOCK,HOLDLOCK)
            JOIN dbo.tbl_ItemCategory c ON c.CatalogueID=b.ItemCategory
            WHERE b.AccountNumber=? AND c.CatalogueID/100000=11 AND c.TimeLimit=0''',account))
        if c.execute("SELECT CASE WHEN OBJECT_ID(N'sdgo_friend_unit_gift.GiftOffer',N'U') IS NULL THEN 0 ELSE 1 END").fetchval():
            owned.update(int(r[0]) for r in c.execute('''SELECT e.UnitNumber FROM sdgo_friend_unit_gift.EscrowUnit e
                JOIN sdgo_friend_unit_gift.GiftOffer g ON g.UserUnitNumber=e.UserUnitNumber
                WHERE (g.SenderAccount=? AND g.State IN(0,2)) OR (g.RecipientAccount=? AND g.State=0)''',account,account))
        return owned

    def plan(self,c,occurrence,account):
        if not c.execute('SELECT AccountNumber FROM dbo.tbl_Users WITH(UPDLOCK,HOLDLOCK) WHERE AccountNumber=?',account).fetchone():
            return None
        rule=occurrence['rewards'];units=self.catalog(c);owned=self.owned(c,account)
        chosen=model.choose_missing(rule,units,owned,random.SystemRandom().choice)
        if rule['mode']=='fixed':
            available={u['unit_id'] for u in units}
            if not set(rule['unit_ids'])<=available:raise ValueError('固定机体缺少原生永久赠礼道具定义')
        gifts=[{'unit_id':u['unit_id'],'category_id':u['category_id'],'amount':1} for u in chosen]
        for item in rule['items']:
            row=c.execute('SELECT RefIndex FROM dbo.tbl_ItemCategory WHERE CatalogueID=?',item['category_id']).fetchone()
            if not row:raise ValueError('配置的道具在当前服务器不存在')
            # Unit gifts always use the unit selector, including fixed units;
            # item fields cannot bypass duplicate checks or permanent-unit validation.
            if item['category_id']//100000==11:raise ValueError('机体请放在机体规则中，不能作为普通道具重复发放')
            gifts.append({'unit_id':None,**item})
        return gifts

    def deliver(self,occurrence,account,*,preview=False):
        model.integer(account,1,2147483647,'玩家编号');model.rewards(occurrence['rewards'])
        if not isinstance(occurrence['key'],str) or not __import__('re').fullmatch(r'[a-z][a-z0-9_-]{0,47}:\d{4}',occurrence['key']):raise ValueError('日期赠礼标识无效')
        __import__('datetime').date.fromisoformat(occurrence['date'])
        c=self.connection.cursor()
        try:
            c.execute('SET NOCOUNT ON; SET XACT_ABORT ON; SET TRANSACTION ISOLATION LEVEL SERIALIZABLE; BEGIN TRANSACTION;')
            locked=c.execute('''DECLARE @r int; EXEC @r=sys.sp_getapplock @Resource=?,
                @LockMode='Exclusive',@LockOwner='Transaction',@LockTimeout=5000; SELECT @r;''',
                'sdgo.calendar_gifts.account.'+str(account)).fetchval()
            if locked is None or locked<0:raise ValueError('玩家日期赠礼正在处理，请稍后重试')
            installed=c.execute("SELECT OBJECT_ID(N'sdgo_calendar_gifts.Receipt',N'U')").fetchval()
            if not installed and not preview:raise ValueError('日期赠礼尚未安装')
            old=c.execute('SELECT Status FROM sdgo_calendar_gifts.Receipt WITH(UPDLOCK,HOLDLOCK) WHERE EventKey=? AND AccountNumber=?',occurrence['key'],account).fetchone() if installed else None
            if old:
                c.execute('ROLLBACK TRANSACTION;');return {'status':'already_recorded','delivery_count':0}
            gifts=self.plan(c,occurrence,account)
            if gifts is None:
                c.execute('ROLLBACK TRANSACTION;');return {'status':'account_unavailable','delivery_count':0}
            if preview:
                c.execute('ROLLBACK TRANSACTION;');return {'status':'preview','gifts':gifts,'delivery_count':len(gifts)}
            rule_hash=hashlib.sha256(json.dumps(occurrence['rewards'],sort_keys=True,separators=(',',':')).encode()).hexdigest()
            status='delivered' if gifts else 'exhausted'
            c.execute('INSERT sdgo_calendar_gifts.Receipt(EventKey,AccountNumber,RuleHash,GiftDate,Status,CommittedAt) VALUES(?,?,?,?,?,SYSUTCDATETIME())',
                      occurrence['key'],account,rule_hash,occurrence['date'],status)
            for ordinal,gift in enumerate(gifts,1):
                c.execute('''DECLARE @created TABLE(Serial bigint); INSERT dbo.tbl_UserDropItemBox(AccountNumber,ItemCategory,Amount,DropConditionID)
                    OUTPUT INSERTED.UserDropItemIdx INTO @created VALUES(?,?,?,0); SELECT Serial AS CalendarNativeSerial FROM @created;''',account,gift['category_id'],gift['amount'])
                serials=[]
                while True:
                    if c.description and len(c.description)==1 and c.description[0][0]=='CalendarNativeSerial':
                        serials.extend(int(r[0]) for r in c.fetchall())
                    if not c.nextset():break
                if len(serials)!=1:raise ValueError('原生保管库没有返回唯一赠礼编号')
                serial=serials[0]
                if not c.execute('SELECT UserDropItemIdx FROM dbo.tbl_UserDropItemBox WHERE UserDropItemIdx=? AND AccountNumber=? AND ItemCategory=? AND Amount=?',
                                 serial,account,gift['category_id'],gift['amount']).fetchone():raise ValueError('原生保管库读回不一致')
                c.execute('INSERT sdgo_calendar_gifts.Delivery(EventKey,AccountNumber,Ordinal,UnitNumber,ItemCategory,Amount,NativeSerial) VALUES(?,?,?,?,?,?,?)',
                          occurrence['key'],account,ordinal,gift['unit_id'],gift['category_id'],gift['amount'],serial)
            c.execute('COMMIT TRANSACTION;')
            return {'status':status,'delivery_count':len(gifts)}
        except BaseException:
            try:c.execute('IF @@TRANCOUNT>0 ROLLBACK TRANSACTION;')
            except Exception:pass
            raise
        finally:
            try:c.execute('SET TRANSACTION ISOLATION LEVEL READ COMMITTED;')
            finally:c.close()

    def history(self,limit=50):
        rows=self.connection.cursor().execute('SELECT TOP (?) EventKey,AccountNumber,GiftDate,Status,CommittedAt FROM sdgo_calendar_gifts.Receipt ORDER BY CommittedAt DESC',limit).fetchall()
        return [{'event':r[0],'account_id':int(r[1]),'date':str(r[2]),'status':r[3],'committed_at_utc':str(r[4])} for r in rows]

    def import_legacy(self,records):
        imported=0
        for record in records:
            account=model.integer(record['account_id'],1,2147483647,'旧回执玩家编号')
            date=__import__('datetime').date.fromisoformat(record['date']);key='birthday:'+str(date.year)
            c=self.connection.cursor()
            try:
                c.execute('SET NOCOUNT ON; SET XACT_ABORT ON; BEGIN TRANSACTION;')
                locked=c.execute("DECLARE @r int; EXEC @r=sys.sp_getapplock @Resource=?,@LockMode='Exclusive',@LockOwner='Transaction',@LockTimeout=5000; SELECT @r;",'sdgo.calendar_gifts.account.'+str(account)).fetchval()
                if locked is None or locked<0:raise ValueError('旧生日回执迁移正在处理')
                if c.execute('SELECT 1 FROM dbo.tbl_Users WHERE AccountNumber=?',account).fetchone() and not c.execute('SELECT 1 FROM sdgo_calendar_gifts.Receipt WITH(UPDLOCK,HOLDLOCK) WHERE EventKey=? AND AccountNumber=?',key,account).fetchone():
                    digest=hashlib.sha256(('legacy birthday '+record['date']).encode()).hexdigest()
                    c.execute("INSERT sdgo_calendar_gifts.Receipt(EventKey,AccountNumber,RuleHash,GiftDate,Status,CommittedAt) VALUES(?,?,?,?,N'legacy_recorded',SYSUTCDATETIME())",key,account,digest,record['date']);imported+=1
                c.execute('COMMIT TRANSACTION;')
            except BaseException:
                try:c.execute('IF @@TRANCOUNT>0 ROLLBACK TRANSACTION;')
                except Exception:pass
                raise
            finally:c.close()
        return imported
