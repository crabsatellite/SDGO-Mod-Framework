"""Explicit, one-use reviews for data-changing Manager provider operations."""
from __future__ import annotations
from contextlib import contextmanager
import hashlib
from pathlib import Path
import re
import secrets
import threading
import time
from . import operations, storage


class ReviewRequired(ValueError):
    def __init__(self,plan):super().__init__('请先核对本次变更的影响与维护条件');self.plan=plan


def operation(provider,method,path):
    if method!='POST':return None
    if provider=='units'and re.fullmatch(r'/api/units/drafts/[0-9a-f]{32}/confirm',path):
        return {'title':'发布机体定义','effects':['写入固定机体、武器及物品定义','保留现有玩家资产；服务重启确认后开放新内容'],'kind':'fixed_catalog'}
    if provider=='units'and path=='/api/units/batch/apply':
        # The batch job enters the same server_change gate in its own thread.
        return {'title':'应用批量机体阵容','effects':['写入共享技能、浮游炮和势力行，再按依赖顺序写入每个机体族的固定定义',
                '为每个 BattleServer 编译形态图并启用机体导入服务端绑定','不发放机体，不改商城、扭蛋或玩家资产；服务重启并核对就绪后生效'],
                'kind':'fixed_catalog','background':True}
    if provider=='capsules'and path=='/api/apply':
        return {'title':'发布扭蛋目录','effects':['同步客户端资源及服务端固定目录','保留玩家资产和现有记录；需要维护窗口'],'kind':'fixed_catalog'}
    if provider=='operators'and path=='/api/operators/apply':
        return {'title':'发布副官目录','effects':['更新副官定义、业务过程与掉落规则','已启用的默认副官规则可能迁移玩家已有副官的默认名称及使用期限；原值由模块备份'],'kind':'player_migration'}
    if provider=='collection_capsule'and path=='/api/upgrade-catalog':
        return {'title':'升级收藏扭蛋数据规则','effects':['更换模块自有业务过程并退役旧历史表','旧历史先保留到本地备份；已获得机体不会由此清除'],'kind':'schema_migration'}
    if provider=='collection_capsule'and path=='/api/install-server':
        return {'title':'安装收藏扭蛋服务端','effects':['安装模块与固定机台定义','保留现有玩家资产；需要维护窗口'],'kind':'fixed_catalog'}
    return None


class ChangeReviews:
    def __init__(self,workspace):self.workspace=workspace;self.lock=threading.RLock();self.reviews={}
    def prepare(self,provider,method,path,body,headers):
        declaration=operation(provider.id,method,path)
        ini=provider.config.get('server_ini')
        if not declaration or not ini:return None
        operations.require_local_database(ini)
        group,nodes,consumers=operations.topology(ini)
        ports=operations.channel_ports(ini,consumers)
        revisions={key:headers.get(key,'')for key in ('X-Catalog-Revision','X-Shop-Revision','X-Reward-Revision')}
        digest=hashlib.sha256(storage.json_bytes({'provider':provider.id,'path':path,'body':body,
            'config':provider.config,'revisions':revisions,'target':operations.target_key(ini),'battle_ports':ports,'nodes':[str(n)for n in consumers]})).hexdigest()
        return {**declaration,'digest':digest,'nodes':[n.parent.name for n in consumers],
                'maintenance':'先停止此服务组；提交后重启并核对就绪。Manager 不会替你结束游戏进程。',
                'changes_player_assets':declaration['kind']=='player_migration'}
    @contextmanager
    def execute(self,provider,method,path,body,headers):
        if method=='POST'and((provider.id=='shop'and path.endswith('/publish'))or(provider.id=='capsules'and path=='/api/apply')):
            ini=provider.config.get('server_ini')or(str(Path(provider.config['world'])/'WorldServer.ini')if provider.config.get('world')else None)
            if ini and operations.pending(ini):raise ValueError('固定目录尚待原生服务重启与核对，暂不能发布新的获取途径')
        plan=self.prepare(provider,method,path,body,headers)
        if plan is None:
            yield;return
        with self.lock:
            self.reviews={k:v for k,v in self.reviews.items()if v['expires']>time.monotonic()}
            key=headers.get('X-Change-Review','');prior=self.reviews.get(key)
            if prior is None or prior['digest']!=plan['digest']:
                if len(self.reviews)>=128:self.reviews.pop(next(iter(self.reviews)))
                key=secrets.token_urlsafe(24);self.reviews[key]={'digest':plan['digest'],'expires':time.monotonic()+300}
                raise ReviewRequired({**plan,'review':key})
            del self.reviews[key]
        if plan.get('background'):
            # A background operation holds the gate for its whole run, not for this request.
            yield;return
        with operations.server_change(provider.config['server_ini'],provider.id+':'+path,(plan['kind'],)):
            yield
    def state(self):
        groups={};rows=[]
        for provider in self.workspace.providers.values():
            ini=provider.config.get('server_ini')
            if not ini:continue
            try:
                group,_,_=operations.topology(ini)
                if group in groups:continue
                groups[group]=True
                for path in sorted((group/'sdgo/local-state/changes').glob('*.json'),key=lambda p:p.stat().st_mtime,reverse=True)[:30]:
                    value=storage.read_json(path)
                    state=operations.activation(ini,value['id'])
                    prefix=value['action'].split(':')[0].split('.')[0]
                    title={'units':'机体目录','unit_catalog':'机体目录','operators':'副官目录','operator_catalog':'副官目录','capsules':'扭蛋目录','capsule_catalog':'扭蛋目录','collection_capsule':'收藏扭蛋规则'}.get(prefix,'目录变更')
                    rows.append({'id':value['id'],'title':title,'status':state['status'],
                                 'message':state.get('message',''),'nodes':state.get('nodes',[]),'effects':value['effects']})
            except (OSError,ValueError,KeyError):continue
        return {'changes':rows,'upgrade_changes_player_data':False}
