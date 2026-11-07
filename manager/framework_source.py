"""Verified framework release artifacts transported through configured Git mirrors."""
from pathlib import Path
import json,re,subprocess
from . import git_source,storage,updates

FEED='framework-feed.json'


def binding(value):
    if (not isinstance(value,dict)or value.get('schema_version')!=1 or value.get('kind')!='sdgo-framework-runtime'
        or not re.fullmatch('[0-9a-f]{32}',str(value.get('source_id','')))):
        raise ValueError('框架更新源身份无效')
    feeds=value.get('feeds');channel=value.get('channel','stable')
    if value.get('transport')=='github-releases':
        if channel not in('stable','beta'):raise ValueError('框架更新通道无效')
        return {'schema_version':1,'kind':'sdgo-framework-runtime','source_id':value['source_id'],
                'repository':updates.repository(value.get('repository')),'channel':channel,'transport':'github-releases','feeds':[]}
    if not isinstance(feeds,list)or not 1<=len(feeds)<=3 or len(set(feeds))!=len(feeds):raise ValueError('配置一至三个框架 Git 源')
    for url in feeds:git_source.parse(url)
    if channel not in('stable','beta'):raise ValueError('框架更新通道无效')
    return {'schema_version':1,'kind':'sdgo-framework-runtime','source_id':value['source_id'],
            'repository':updates.repository(value.get('repository')),'channel':channel,'feeds':feeds}


def load(controller,source,constraints=None):
    """Pin a feed commit and verify the same ZIP format as the Release updater."""
    source=binding(source);errors=[];lock=(constraints or{}).get('locked_framework',{})
    if source.get('transport')=='github-releases':
        suffix='/releases/tags/v'+lock['version']if lock.get('version')else'/releases/latest'if source['channel']=='stable'else'/releases?per_page=30'
        data=json.loads(updates.fetch('https://api.github.com/repos/'+source['repository']+suffix,2*1024*1024,controller.stop_event))
        if isinstance(data,list):
            choices=[]
            for item in data:
                try:choices.append(updates.release_asset(item,source['channel'],source['repository']))
                except(ValueError,KeyError,TypeError):continue
            if not choices:raise ValueError('官方源没有可用框架版本')
            asset=max(choices,key=lambda x:updates.version(x['version']))
        else:asset=updates.release_asset(data,source['channel'],source['repository'])
        if lock and(asset['sha256']!=lock.get('sha256')or asset['version']!=lock.get('version')):raise ValueError('锁定的官方框架 Release 版本或摘要不一致')
        folder=controller.cache/'framework'/(asset['version']+'-'+asset['sha256']);archive=folder.parent/(folder.name+'.zip');archive.parent.mkdir(parents=True,exist_ok=True)
        if not archive.is_file()or updates.digest(archive.read_bytes())!=asset['sha256']:storage.atomic(archive,updates.fetch(asset['url'],updates.MAX_ARCHIVE,controller.stop_event))
        manifest=updates.unpack(archive.read_bytes(),asset,folder,source['repository'])
        return {'source_id':source['source_id'],'commit':manifest['source_commit'],'version':manifest['version'],'channel':manifest['channel'],'sha256':asset['sha256'],'mirror':1,'bundle':str(folder),'manifest':manifest}
    for index,url in enumerate(source['feeds']):
        remote,ref=git_source.parse(url);repo=controller.cache/'git'/updates.digest(url.encode());repo.mkdir(parents=True,exist_ok=True)
        try:
            if not(repo/'HEAD').is_file():controller.git(repo,['init','--bare']);controller.git(repo,['remote','add','origin',remote])
            if controller.git(repo,['config','--get','remote.origin.url']).decode().strip()!=remote:raise ValueError('框架缓存身份不一致')
            controller.git(repo,['fetch','--filter=blob:none','--no-tags','--depth=1','origin',lock.get('commit')or'refs/heads/'+ref])
            commit=controller.git(repo,['rev-parse','FETCH_HEAD']).decode().strip()
            if not re.fullmatch('[0-9a-f]{40}',commit):raise ValueError('框架提交身份无效')
            feed=json.loads(controller.git(repo,['show',commit+':'+FEED]))
            if(feed.get('schema_version')!=1 or feed.get('kind')!='sdgo-framework-runtime'or feed.get('source_id')!=source['source_id']
               or feed.get('repository')!=source['repository']or feed.get('channel')!=source['channel']):raise ValueError('框架发布与所选身份或通道不一致')
            label=feed.get('version');updates.version(label)
            if lock and(label!=lock.get('version')or feed.get('asset',{}).get('sha256')!=lock.get('sha256')):raise ValueError('框架镜像与服主锁定的版本或摘要不一致')
            if ('-beta.'in label)!=(source['channel']=='beta'):raise ValueError('框架包版本与通道不一致')
            if constraints:
                core=updates.version(label)[:3]
                if core<updates.version(constraints['minimum_framework'])[:3]or constraints.get('maximum_framework')and core>updates.version(constraints['maximum_framework'])[:3]:
                    raise ValueError('这个框架镜像尚未提供服主内容需要的兼容版本')
            name='SDGO-ModFramework-'+label+'.zip';entry=feed.get('asset',{})
            if(entry.get('path')!='releases/'+name or not re.fullmatch('[0-9a-f]{64}',str(entry.get('sha256','')))
               or type(entry.get('bytes'))is not int or not 0<entry['bytes']<=updates.MAX_ARCHIVE):raise ValueError('框架发布资产命名或摘要无效')
            folder=controller.cache/'framework'/(label+'-'+entry['sha256']);archive=folder.parent/(folder.name+'.zip')
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
            if constraints:
                mods={m['id']:m for m in storage.read_json(folder/'client/sdgo/catalog.json')['mods']if m['target']=='client'}
                for identity,required in constraints.get('required_mods',{}).items():
                    if identity not in mods or updates.version(mods[identity]['version'])<updates.version(required):raise ValueError('这个框架镜像缺少服主内容需要的 Mod：'+identity)
            return {'source_id':source['source_id'],'commit':commit,'version':label,'channel':source['channel'],
                    'sha256':entry['sha256'],'mirror':controller.fetch_mirror.get(str(repo.resolve()),index+1),
                    'bundle':str(folder),'manifest':manifest}
        except InterruptedError:raise
        except (OSError,ValueError,subprocess.SubprocessError)as error:errors.append('框架源 '+str(index+1)+'：'+str(error))
    raise ValueError('；'.join(errors)+'。保留当前安装。')
