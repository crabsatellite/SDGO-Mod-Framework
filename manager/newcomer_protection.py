"""Owner-only player selection; policy stays in the selected Battle host."""
from urllib.parse import parse_qs,urlsplit
from .providers import Provider


class NewcomerProtection(Provider):
    id,title='newcomer_protection','新手保护'
    fields={'server_ini':'本服 WorldServer.ini（读取玩家名称，不修改数据库）'}

    def connection(self):
        from .database import connect
        if not self.config.get('server_ini'):raise ValueError('请先选择本服 WorldServer.ini，以便按玩家名字选择')
        return connect(self.config['server_ini'])

    def hosts(self):
        result=[]
        for host in self.workspace.settings['hosts']:
            if host['role']=='battle' and (self.workspace._mod_directory(host,self.id)/'mod.ini').is_file():
                result.append({'id':host['id'],'name':host['name'],'details':self.workspace.details(host['id'],self.id,internal=True)})
        return result

    def request(self,method,path,body,headers):
        parsed=urlsplit(path);query=parse_qs(parsed.query);path=parsed.path
        if method=='GET' and path=='/api/newcomer-protection':
            rows=self.hosts();wanted=set()
            for row in rows:
                wanted.update(int(n.strip()) for n in row['details']['values']['protected_accounts'].split(',') if n.strip())
            labels={}
            if wanted and self.config.get('server_ini'):
                connection=self.connection()
                try:
                    # The roster is bounded by the native parser as well.
                    for account,name in connection.cursor().execute('SELECT AccountNumber,PilotName FROM dbo.tbl_Users'):
                        if int(account) in wanted:labels[str(account)]=str(name)
                finally:connection.close()
            return {'hosts':rows,'players':labels,'privacy':'本服名单仅保存于服务端，不随模组程序或玩家内容包发布'}
        if method=='GET' and path=='/api/newcomer-protection/players':
            connection=self.connection()
            try:
                rows=connection.cursor().execute('SELECT TOP 50 AccountNumber,PilotName FROM dbo.tbl_Users WHERE PilotName LIKE ? ORDER BY PilotName',
                    '%'+query.get('q',[''])[0][:64]+'%').fetchall()
                return {'rows':[{'value':int(r[0]),'title':str(r[1])} for r in rows]}
            finally:connection.close()
        if method=='POST' and path=='/api/newcomer-protection/policy':
            if body.get('host') not in {row['id'] for row in self.hosts()}:raise ValueError('请选择已安装新手保护的 Battle 服务端')
            values=body.get('values',{})
            if set(values)!={'enabled','health_multiplier','attack_multiplier','protected_accounts'}:raise ValueError('请提交完整的新手保护设置')
            accounts=values.get('protected_accounts')
            if not isinstance(accounts,list) or len(accounts)>256 or any(type(n)is not int or not 1<=n<=2147483647 for n in accounts):
                raise ValueError('新手保护名单最多 256 个有效玩家')
            values={**values,'protected_accounts':','.join(str(n)for n in sorted(set(accounts)))}
            return self.workspace.configure(body['host'],self.id,values,body.get('revisions',{}),internal=True)
        raise ValueError('新手保护操作未登记')
