"""Independent date-gift Mod: one Manager panel and one supervised worker."""
from __future__ import annotations
import datetime as dt
import threading
from urllib.parse import urlsplit,parse_qs
from .providers import Provider,Binary
from . import storage


class CalendarGiftService:
    def __init__(self):
        self.thread=None;self.stop_event=threading.Event();self.healthy=False;self.error='';self.delivered=0

    def state(self):
        running=bool(self.thread and self.thread.is_alive())
        return {'running':running,'healthy':running and self.healthy,'error':self.error,
                'ownership':'manager_process','delivery':'native_storage','delivery_count':self.delivered}

    def start(self,ini,engine):
        if self.thread and self.thread.is_alive():return self.state()
        if not ini:raise ValueError('请先选择本服 WorldServer.ini')
        if not engine.policy()['enabled']:raise ValueError('请先在日期赠礼面板开启并保存规则')
        lease=(engine.root/'.manager-calendar-gifts.lock').open('a+b')
        try:
            import msvcrt
            lease.seek(0,2)
            if not lease.tell():lease.write(b'\0');lease.flush()
            lease.seek(0);msvcrt.locking(lease.fileno(),msvcrt.LK_NBLCK,1)
        except Exception:
            lease.close();raise ValueError('另一个 Manager 已经拥有此日期赠礼工作区') from None
        self.stop_event.clear();self.healthy=False;self.error='';ready=threading.Event()
        def run():
            connection=None;phase='connect'
            try:
                from .database import connect
                from owner.calendar_gifts.repository import Repository
                from owner.calendar_gifts import model
                connection=connect(ini,autocommit=True);repository=Repository(connection);phase='install';repository.install()
                phase='legacy';repository.import_legacy(engine.legacy())
                self.healthy=True;ready.set()
                while not self.stop_event.is_set():
                    phase='rules';policy=engine.policy();people=engine.people();pending=False
                    if policy['enabled']:
                        start,end=engine.window(policy)
                        occurrences=engine.occurrences(policy,people,start,end);budget=200
                        for occurrence in occurrences:
                            phase='recipients'
                            accounts=repository.accounts(occurrence,people)
                            for account in accounts:
                                if self.stop_event.is_set():break
                                if budget<=0:pending=True;break
                                phase='delivery';outcome=repository.deliver(occurrence,account)
                                self.delivered+=outcome['delivery_count'];budget-=1
                            if self.stop_event.is_set():break
                    self.healthy=True
                    self.stop_event.wait(1 if pending else 60)
            except Exception as error:
                self.error='日期赠礼未就绪（'+{'connect':'数据库连接','install':'安装赠礼记录','legacy':'迁移旧生日回执','rules':'读取规则','recipients':'读取接收名单','delivery':'写入原生保管库'}.get(phase,phase)+'）；请检查本服诊断记录。下次重试会先核对原回执。'
                # A bounded local diagnostic, never an HTTP/export payload.
                # ODBC login errors may include the selected INI's user ID.
                try:
                    from pathlib import Path
                    values=storage.ini_values(Path(ini).read_bytes());message=str(error)
                    for section in values.sections():
                        for key,value in values[section].items():
                            if key.casefold() in ('id','pwd','password') and value:message=message.replace(value,'[redacted]')
                    storage.atomic(engine.root/'calendar-errors.private.json',storage.json_bytes(
                        {'phase':phase,'type':type(error).__name__,'message':message[:4096]}))
                except Exception:pass
            finally:
                self.healthy=False;ready.set()
                try:
                    if connection:connection.close()
                finally:lease.close()
        self.thread=threading.Thread(target=run,name='sdgo-calendar-gifts',daemon=True)
        self.thread.start();ready.wait(20);return self.state()

    def stop(self):
        self.stop_event.set()
        if self.thread:self.thread.join(timeout=20)
        return self.state()


class CalendarGifts(Provider):
    id,title='calendar_gifts','节日与生日赠礼'
    fields={'workspace':'本服日期赠礼配置目录','server_ini':'本服 WorldServer.ini',
            'legacy_birthdays':'旧生日登记表 JSON 备份（可选）',
            'legacy_state':'旧生日发放记录 processor_state.json（可选）'}

    def __init__(self,workspace):
        super().__init__(workspace);self.controller=CalendarGiftService()
        workspace.services.register('calendar.gifts',self.title,self.controller,
            lambda:self.controller.start(self.config.get('server_ini'),self.get_engine()))

    def configure(self,values):
        if self.workspace.services.describe('calendar.gifts')['running'] or self.workspace.services.desired('calendar.gifts'):
            raise ValueError('更换本服目录或服务器前，请先停止日期赠礼后台任务')
        from pathlib import Path
        candidate={**self.config,**values}
        if candidate.get('workspace'):
            private_root=Path(candidate['workspace']).resolve()
            published=[Path(host['path']).resolve()/'sdgo/mods' for host in self.workspace.settings['hosts']]
            if candidate.get('server_ini'):published.append(Path(candidate['server_ini']).resolve().parent/'sdgo/mods')
            if any(private_root.is_relative_to(folder) for folder in published):
                raise ValueError('本服生日工作区不能放在会发布的 Mod 文件夹内，请选择独立的私人数据目录')
        return super().configure(values)

    def get_engine(self):
        if self.engine is None:
            if not self.config.get('workspace'):raise ValueError('请先选择本服日期赠礼配置目录')
            from owner.calendar_gifts.workspace import Workspace
            self.engine=Workspace(self.config['workspace'])
        return self.engine

    def connection(self,section='DB Game'):
        from .database import connect
        if not self.config.get('server_ini'):raise ValueError('请选择本服 WorldServer.ini')
        return connect(self.config['server_ini'],section,autocommit=True)

    def request(self,method,path,body,headers):
        engine=self.get_engine()
        parsed=urlsplit(path);path=parsed.path;query=parse_qs(parsed.query)
        if method=='GET' and path=='/api/calendar-gifts/labels':
            connection=self.connection()
            try:
                c=connection.cursor()
                return {'players':{str(r[0]):str(r[1]) for r in c.execute('SELECT AccountNumber,PilotName FROM dbo.tbl_Users')},
                        'units':{str(r[0]):str(r[1]) for r in c.execute('SELECT UnitNumber,UnitModelName FROM dbo.tbl_Units')},
                        'items':{str(r[0]):str(r[1]) for r in c.execute('SELECT CatalogueID,Name FROM dbo.tbl_ItemCategory')}}
            finally:connection.close()
        if method=='GET' and path in ('/api/calendar-gifts/players','/api/calendar-gifts/catalog'):
            search=query.get('q',[''])[0][:64];connection=self.connection()
            try:
                if path.endswith('/players'):
                    rows=connection.cursor().execute('SELECT TOP 50 AccountNumber,PilotName FROM dbo.tbl_Users WHERE PilotName LIKE ? ORDER BY PilotName',
                                                     '%'+search+'%').fetchall()
                    return {'rows':[{'value':int(r[0]),'title':str(r[1])} for r in rows]}
                rows=connection.cursor().execute('SELECT TOP 100 CatalogueID,Name,RefIndex FROM dbo.tbl_ItemCategory WHERE Name LIKE ? ORDER BY Name',
                                                 '%'+search+'%').fetchall()
                return {'rows':[{'value':int(r[0]),'title':str(r[1]),'unit_id':int(r[2]) if int(r[0])//100000==11 else None} for r in rows]}
            finally:connection.close()
        if method=='GET' and path=='/api/calendar-gifts':return {**engine.public(),'service':self.controller.state()}
        if method=='GET' and path=='/api/calendar-gifts/export':
            return Binary(engine.export(),'application/json','calendar-gift-rules.json')
        if method=='POST' and path in ('/api/calendar-gifts/policy','/api/calendar-gifts/private'):
            return engine.save(path.rsplit('/',1)[1],body['value'],body['revision'])
        if method=='POST' and path=='/api/calendar-gifts/import-legacy':
            if self.controller.state()['running'] or self.workspace.services.desired('calendar.gifts'):
                raise ValueError('请先停止日期赠礼后台任务，再迁移旧生日发放记录')
            if not self.config.get('legacy_state'):raise ValueError('请先选择旧脚本的 processor_state.json')
            return {'imported':engine.import_legacy(self.config['legacy_state'])}
        if method=='POST' and path=='/api/calendar-gifts/import-birthdays':
            current=engine.public()
            if body.get('revision')!=current['private_revision']:raise storage.Conflict('私人名单已改变，请刷新')
            if self.config.get('legacy_birthdays'):
                from pathlib import Path
                source=Path(self.config['legacy_birthdays'])
                if source.stat().st_size>2*1024*1024:raise ValueError('旧生日登记表备份过大')
                backup=storage.read_json(source)
                if not isinstance(backup,list):raise ValueError('旧生日登记表应为 JSON 记录列表')
                rows=[(r.get('AccountNumber'),r.get('Birthday')) for r in backup if isinstance(r,dict)]
            else:
                connection=self.connection('DB Member')
                try:
                    cursor=connection.cursor()
                    if not cursor.execute("SELECT OBJECT_ID(N'dbo.tbl_UserEmail',N'U')").fetchval():
                        raise ValueError('本服没有旧生日登记表；可选择其私人 JSON 备份导入，或按玩家名字登记生日')
                    rows=cursor.execute("SELECT AccountNumber,Birthday FROM dbo.tbl_UserEmail WHERE Birthday IS NOT NULL").fetchall()
                finally:connection.close()
            value=current['private'];seen={r['account_id'] for r in value['birthdays']};added=0
            from owner.calendar_gifts.model import month_day
            for account,raw in rows:
                raw=str(raw).strip()
                if isinstance(account,bool) or not isinstance(account,int) or account<1 or account>2147483647:continue
                if len(raw)!=4 or not raw.isdigit() or account in seen:continue
                date=raw[:2]+'-'+raw[2:]
                try:month_day(date)
                except ValueError:continue
                value['birthdays'].append({'account_id':int(account),'month_day':date,'enabled':True});seen.add(int(account));added+=1
            return {**engine.save('private',value,current['private_revision']),'imported':added}
        if method=='POST' and path=='/api/calendar-gifts/preview':
            from owner.calendar_gifts import model
            from owner.calendar_gifts.repository import Repository
            policy=engine.policy();people=engine.people();date=dt.date.fromisoformat(body.get('date') or engine.today().isoformat())
            account=body.get('account_id');model.integer(account,1,2147483647,'预览玩家编号')
            occurrences=engine.occurrences(policy,people,date,date);connection=self.connection()
            try:
                repository=Repository(connection);result=[]
                for occurrence in occurrences:
                    if occurrence['account_id'] not in (None,account):continue
                    if occurrence['group'] not in (None,'all') and account not in people['groups'].get(occurrence['group'],[]):continue
                    if occurrence['key'].startswith('birthday:') and any(row['account_id']==account and occurrence['key']=='birthday:'+row['date'][:4] for row in engine.legacy()):
                        result.append({'event':occurrence['key'],'title':occurrence['title'],'status':'legacy_recorded','delivery_count':0});continue
                    result.append({'event':occurrence['key'],'title':occurrence['title'],
                                   **repository.deliver(occurrence,account,preview=True)})
                return {'date':date.isoformat(),'account_id':account,'events':result}
            finally:connection.close()
        if method=='GET' and path=='/api/calendar-gifts/history':
            from owner.calendar_gifts.repository import Repository
            connection=self.connection()
            try:
                exists=connection.cursor().execute("SELECT OBJECT_ID(N'sdgo_calendar_gifts.Receipt',N'U')").fetchval()
                return {'records':Repository(connection).history() if exists else []}
            finally:connection.close()
        raise ValueError('日期赠礼操作未登记')
