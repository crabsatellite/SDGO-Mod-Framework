"""Publish a verified release ZIP on a Git runtime branch for SSH downloads."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess,sys,tempfile,zipfile
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
from manager import storage,updates
BOT='github-actions[bot]';BOT_EMAIL='41898282+github-actions[bot]@users.noreply.github.com'
PUBLISHER={'GIT_AUTHOR_NAME':BOT,'GIT_AUTHOR_EMAIL':BOT_EMAIL,'GIT_COMMITTER_NAME':BOT,'GIT_COMMITTER_EMAIL':BOT_EMAIL}


def publish(asset,repository=ROOT,remote='origin',push=False):
    repository=Path(repository).resolve();asset=Path(asset).resolve();raw=asset.read_bytes()
    with zipfile.ZipFile(asset)as z:manifest=json.loads(z.read('release.json'))
    version=manifest['version'];channel=manifest['channel'];name='SDGO-ModFramework-'+version+'.zip'
    if asset.name!=name:raise ValueError('Asset name does not match the Manager convention')
    digest=hashlib.sha256(raw).hexdigest();source_id=hashlib.sha256(('sdgo-framework-runtime:'+manifest['repository']).encode()).hexdigest()[:32]
    with tempfile.TemporaryDirectory()as temporary:updates.unpack(raw,{'version':version,'channel':channel,'bytes':len(raw),'sha256':digest},Path(temporary),manifest['repository'])
    branch='refs/heads/runtime/'+channel
    index=repository/'build/framework-publish'/((channel)+'.index');index.parent.mkdir(parents=True,exist_ok=True)
    env={**os.environ,'GIT_INDEX_FILE':str(index)}
    def run(*args,input=None,extra=None):return subprocess.run(['git','-C',str(repository),*args],env={**env,**(extra or{})},input=input,capture_output=True)
    def git(*args,input=None,extra=None):
        p=run(*args,input=input,extra=extra)
        if p.returncode:raise RuntimeError(p.stderr.decode(errors='replace'))
        return p.stdout
    previous=git('ls-remote',remote,branch).decode().split();parent=None
    if previous:
        parent=previous[0];git('fetch','--no-tags',remote,branch)
        prior=json.loads(git('show',parent+':framework-feed.json'))
        if updates.version(prior['version'])>updates.version(version):raise ValueError('A newer runtime is already published')
        if prior['version']==version:
            if prior['asset']['sha256']!=digest:raise ValueError('Published runtime versions are immutable')
            return prior,parent
    git('read-tree','--empty')
    feed={'schema_version':1,'kind':'sdgo-framework-runtime','source_id':source_id,'repository':manifest['repository'],
          'version':version,'channel':channel,'source_commit':manifest['source_commit'],
          'asset':{'path':'releases/'+name,'bytes':len(raw),'sha256':digest}}
    for path,data in {'framework-feed.json':storage.json_bytes(feed),'releases/'+name:raw,'releases/'+name[:-4]+'.sha256':(digest+'  '+name+'\n').encode()}.items():
        oid=git('hash-object','-w','--stdin',input=data).decode().strip();git('update-index','--add','--cacheinfo','100644',oid,path)
    tree=git('write-tree').decode().strip();args=['commit-tree',tree]
    if parent:args+=['-p',parent]
    # A maintainer keeps their configured identity; a runner without one commits as the Actions bot, for this call only.
    known=all(run('var',name).returncode==0 for name in('GIT_AUTHOR_IDENT','GIT_COMMITTER_IDENT'))
    commit=git(*args,input=('Publish Manager '+version+' verified runtime\n').encode(),extra=None if known else PUBLISHER).decode().strip()
    if push:
        git('push',remote,commit+':'+branch)
        if git('ls-remote',remote,branch).decode().split()[0]!=commit:raise ValueError('Published runtime readback differs')
    return feed,commit


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--asset',type=Path,required=True);p.add_argument('--repository',type=Path,default=ROOT)
    p.add_argument('--remote',default='origin');p.add_argument('--push',action='store_true');a=p.parse_args()
    feed,commit=publish(a.asset,a.repository,a.remote,a.push);print(json.dumps({'version':feed['version'],'channel':feed['channel'],'source_id':feed['source_id'],'commit':commit,'pushed':a.push}),flush=True)
