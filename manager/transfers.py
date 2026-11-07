"""Resumable, cancellable downloads pinned to an expected size and digest."""
from pathlib import Path
import hashlib,re,urllib.request
from . import updates


def verify(path,entry):
    path=Path(path)
    if not path.is_file()or path.stat().st_size!=entry['bytes']:return False
    digest=hashlib.sha256()
    with path.open('rb')as stream:
        for block in iter(lambda:stream.read(1024*1024),b''):digest.update(block)
    return digest.hexdigest()==entry['sha256']


def download(url,target,entry,stop=None,progress=None):
    target=Path(target);target.parent.mkdir(parents=True,exist_ok=True)
    report=progress or(lambda n:None)
    if verify(target,entry):report(entry['bytes']);return target
    if entry['bytes']==0:
        if entry['sha256']!=hashlib.sha256(b'').hexdigest():raise ValueError('空文件摘要无效')
        target.write_bytes(b'');report(0);return target
    partial=target.with_name(target.name+'.part')
    offset=partial.stat().st_size if partial.is_file()else 0
    if offset>entry['bytes']:partial.unlink();offset=0
    if offset==entry['bytes']:
        if verify(partial,entry):partial.replace(target);report(offset);return target
        partial.unlink();offset=0
    headers={'User-Agent':'SDGO-Mod-Manager','Accept-Encoding':'identity'}
    if offset:headers['Range']='bytes='+str(offset)+'-'
    request=urllib.request.Request(url,headers=headers)
    # This is a socket inactivity timeout, never a total transfer duration or
    # minimum speed. read1 reports available bytes even on a very slow stream.
    with urllib.request.urlopen(request,timeout=20,context=updates.tls_context())as response:
        status=response.status
        if status==206:
            match=re.fullmatch(r'bytes (\d+)-(\d+)/(\d+)',response.headers.get('Content-Range',''))
            if not match or tuple(map(int,match.groups()))!=(offset,entry['bytes']-1,entry['bytes']):raise ValueError('下载续传范围与锁定文件不一致')
        elif status==200:offset=0
        else:raise ValueError('内容服务器未提供完整文件或合法续传范围')
        report(offset)
        with partial.open('ab'if offset else'wb')as stream:
            while True:
                if stop is not None and stop.is_set():raise InterruptedError('下载已取消；已接收内容保留以供继续')
                block=response.read1(256*1024)
                if not block:break
                offset+=len(block)
                if offset>entry['bytes']:stream.close();partial.unlink(missing_ok=True);raise ValueError('下载超过锁定文件大小')
                stream.write(block);report(offset)
    if offset!=entry['bytes']:raise OSError('下载连接提前关闭；保留已接收内容等待续传')
    if not verify(partial,entry):
        partial.unlink();raise ValueError('下载摘要与服主锁定文件不一致；拒绝安装')
    partial.replace(target);return target
