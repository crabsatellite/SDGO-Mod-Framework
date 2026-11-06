"""Verified framework release artifacts transported through configured Git mirrors."""
from pathlib import Path
import json,re
from . import git_source,storage,updates

FEED='framework-feed.json'


def binding(value):
    if (not isinstance(value,dict)or value.get('schema_version')!=1 or value.get('kind')!='sdgo-framework-runtime'
        or not re.fullmatch('[0-9a-f]{32}',str(value.get('source_id','')))):
        raise ValueError('框架更新源身份无效')
    feeds=value.get('feeds');channel=value.get('channel','stable')
    if not isinstance(feeds,list)or not 1<=len(feeds)<=3 or len(set(feeds))!=len(feeds):raise ValueError('配置一至三个框架 Git 源')
    for url in feeds:git_source.parse(url)
    if channel not in('stable','beta'):raise ValueError('框架更新通道无效')
    return {'schema_version':1,'kind':'sdgo-framework-runtime','source_id':value['source_id'],
            'repository':updates.repository(value.get('repository')),'channel':channel,'feeds':feeds}


def load(controller,source):
    """Pin a feed commit and verify the same ZIP format as the Release updater."""
    source=binding(source);errors=[]
    for index,url in enumerate(source['feeds']):
        remote,ref=git_source.parse(url);repo=controller.cache/'git'/updates.digest(url.encode());repo.mkdir(parents=True,exist_ok=True)
        try:
            if not(repo/'HEAD').is_file():controller.git(repo,['init','--bare']);controller.git(repo,['remote','add','origin',remote])
            if controller.git(repo,['config','--get','remote.origin.url']).decode().strip()!=remote:raise ValueError('框架缓存身份不一致')
            controller.git(repo,['fetch','--filter=blob:none','--no-tags','--depth=1','origin','refs/heads/'+ref])
            commit=controller.git(repo,['rev-parse','FETCH_HEAD']).decode().strip()
            if not re.fullmatch('[0-9a-f]{40}',commit):raise ValueError('框架提交身份无效')
            feed=json.loads(controller.git(repo,['show',commit+':'+FEED]))
            if(feed.get('schema_version')!=1 or feed.get('kind')!='sdgo-framework-runtime'or feed.get('source_id')!=source['source_id']
               or feed.get('repository')!=source['repository']or feed.get('channel')!=source['channel']):raise ValueError('框架发布与所选身份或通道不一致')
            label=feed.get('version');updates.version(label)
            if ('-beta.'in label)!=(source['channel']=='beta'):raise ValueError('框架包版本与通道不一致')
            name='SDGO-ModFramework-'+label+'.zip';entry=feed.get('asset',{})
            if(entry.get('path')!='releases/'+name or not re.fullmatch('[0-9a-f]{64}',str(entry.get('sha256','')))
               or type(entry.get('bytes'))is not int or not 0<entry['bytes']<=updates.MAX_ARCHIVE):raise ValueError('框架发布资产命名或摘要无效')
            folder=controller.cache/'framework'/(label+'-'+entry['sha256']);archive=folder.with_suffix('.zip')
            if not archive.is_file()or archive.stat().st_size!=entry['bytes']or updates.digest(archive.read_bytes())!=entry['sha256']:
                key=str(repo.resolve());controller.fetch_mirrors[key]=[(i+1,git_source.parse(v)[0])for i,v in enumerate(source['feeds'])if i>index]
                oid=controller.git(repo,['rev-parse',commit+':'+entry['path']]).decode().strip()
                if not re.fullmatch('[0-9a-f]{40}',oid):raise ValueError('框架资产对象无效')
                controller.git(repo,['fetch','--no-tags','--no-write-fetch-head','--recurse-submodules=no','--filter=blob:none','--stdin','origin'],input=(oid+'\n').encode())
                archive.parent.mkdir(parents=True,exist_ok=True);temporary=archive.with_suffix('.download')
                try:
                    controller.git(repo,['cat-file','blob',oid],output=temporary)
                    if temporary.stat().st_size!=entry['bytes']or updates.digest(temporary.read_bytes())!=entry['sha256']:raise ValueError('框架资产校验失败')
                    temporary.replace(archive)
                finally:temporary.unlink(missing_ok=True)
            manifest=updates.unpack(archive.read_bytes(),{**entry,'version':label,'channel':source['channel']},folder,source['repository'])
            return {'source_id':source['source_id'],'commit':commit,'version':label,'channel':source['channel'],
                    'sha256':entry['sha256'],'mirror':controller.fetch_mirror.get(str(repo.resolve()),index+1),
                    'bundle':str(folder),'manifest':manifest}
        except InterruptedError:raise
        except (OSError,ValueError)as error:errors.append('框架源 '+str(index+1)+'：'+str(error))
    raise ValueError('；'.join(errors)+'。保留当前安装。')
