"""Portable, content-addressed authoring metadata and media declarations."""
from __future__ import annotations

import copy
import hashlib
import io
import json
from pathlib import PurePosixPath
import re
import unicodedata
from urllib.parse import urlsplit
import zipfile


PROTOCOL='sdgo.unit-project'
SCHEMA_VERSION=1
METADATA_FIELDS={'description','short_description','gameplay_notes','aliases','search_examples','tags',
                 'series','model_number','english_name','sources','visibility','declared_unit_id'}
MEDIA_ROLES={'activity_poster'}


def plain(value,label,maximum,*,multiline=False):
    if not isinstance(value,str) or len(value)>maximum or '\0' in value:
        raise ValueError(label+' 内容无效或过长')
    if any(ord(c)<32 and not(multiline and c in '\n\r\t') for c in value):
        raise ValueError(label+' 含控制字符')
    return value.strip()


def normalize_metadata(value):
    if not isinstance(value,dict) or set(value)-METADATA_FIELDS:raise ValueError('机体资料包含未知字段')
    result={}
    for key,maximum in [('description',4000),('short_description',199),('gameplay_notes',2000),('series',128),('model_number',64),('english_name',128)]:
        result[key]=plain(value.get(key,''),key,maximum,multiline=key in ('description','gameplay_notes'))
    for key,count,length in [('aliases',32,64),('search_examples',16,128),('tags',32,48)]:
        rows=value.get(key,[])
        if not isinstance(rows,list) or len(rows)>count:raise ValueError(key+' 列表无效或过长')
        normalized=[];seen=set()
        for row in rows:
            item=plain(row,key,length)
            if not item:raise ValueError(key+' 不能包含空项')
            folded=unicodedata.normalize('NFKC',item).casefold()
            if folded in seen:raise ValueError(key+' 不能包含重复项')
            seen.add(folded);normalized.append(item)
        result[key]=normalized
    if value.get('visibility','private')!='private':raise ValueError('作者项目资料只能作为私有内容导入，不进入框架默认数据')
    result['visibility']='private'
    uid=value.get('declared_unit_id')
    if uid is not None and (type(uid) is not int or not 0<uid<=9_999_999):raise ValueError('资料机体编号无效')
    result['declared_unit_id']=uid
    sources=value.get('sources',[])
    if not isinstance(sources,list) or len(sources)>16:raise ValueError('资料来源列表无效')
    result['sources']=[]
    for source in sources:
        if not isinstance(source,dict) or set(source)-{'title','url','kind','note'}:raise ValueError('资料来源字段无效')
        row={'title':plain(source.get('title',''),'来源标题',200),'url':plain(source.get('url',''),'来源网址',2048),
             'kind':source.get('kind','author'),'note':plain(source.get('note',''),'来源说明',512)}
        parsed=urlsplit(row['url'])
        if row['url'] and (parsed.scheme not in ('http','https') or not parsed.netloc or parsed.username or parsed.password):
            raise ValueError('资料来源须使用无登录凭据的HTTP/HTTPS网址')
        if row['kind'] not in ('official','author','reference'):raise ValueError('资料来源类型无效')
        result['sources'].append(row)
    return result


def normalize_media(value,assets):
    if not isinstance(value,dict) or set(value)-MEDIA_ROLES:raise ValueError('项目素材角色无效')
    available={a['sha']:a for a in assets}
    result={}
    for role,entry in value.items():
        if not isinstance(entry,dict) or set(entry)-{'asset_sha','source_sha','filename','caption','source_kind'}:
            raise ValueError('项目素材字段无效')
        row=copy.deepcopy(entry)
        for key in ('asset_sha','source_sha'):
            sha=row.get(key)
            if not isinstance(sha,str) or not re.fullmatch('[0-9a-f]{64}',sha) or sha not in available:
                raise ValueError('项目素材必须引用当前项目内的完整资源')
        if available[row['asset_sha']]['kind']!='image':raise ValueError('活动海报必须引用图片')
        row['filename']=plain(row.get('filename',''),'素材名称',256)
        row['caption']=plain(row.get('caption',''),'素材说明',200)
        if row.get('source_kind') not in ('owner-provided','authored','source-recovered'):
            raise ValueError('请明确项目素材来源')
        result[role]=row
    return result


def shared_media(draft):
    """Authoring media only; references confer no native unit registration."""
    result=copy.deepcopy(draft.get('media',{}))
    assets={row['sha']:row for row in draft.get('assets',[])}
    for role,label in [('shop','shop_portrait'),('capsule','capsule_portrait')]:
        selected=draft.get('presentation',{}).get(role,{}).get('asset_sha')
        if not selected:continue
        asset=assets.get(selected)
        if not asset or asset.get('kind')!='image':continue
        result[label]={'asset_sha':selected,'source_sha':selected,'filename':asset.get('filename',label+'.png'),
            'caption':draft['presentation'][role].get('caption',''),
            'source_kind':'source-recovered' if str(asset.get('source','')).startswith(('package','baseline')) else 'owner-provided'}
    return result


def search_text(draft):
    metadata=normalize_metadata(draft.get('metadata',{}))
    values=[draft.get('name',''),str(draft.get('root_unit_id') or metadata.get('declared_unit_id') or ''),
        metadata['description'],metadata['short_description'],metadata['gameplay_notes'],metadata['series'],metadata['model_number'],
        metadata['english_name'],*metadata['aliases'],*metadata['tags']]
    return unicodedata.normalize('NFKC',' '.join(values)).casefold()


def matches(draft,query):
    haystack=search_text(draft)
    tokens=unicodedata.normalize('NFKC',query).casefold().split()
    return all(token in haystack for token in tokens)


def export_project(workspace,draft):
    """Source data only: no runtime DLL, host paths, credentials or receipts."""
    files={}
    for entry in draft.get('files',[]):files[entry['path']]=workspace.blob(entry['sha'])
    assets=[]
    for asset in draft.get('assets',[]):
        if asset.get('kind') not in ('image','source-image'):continue
        raw=workspace.blob(asset['sha'])
        if not raw.startswith(b'\x89PNG\r\n\x1a\n'):raise ValueError('制作项目图片不是PNG')
        path='assets/'+asset['sha']+'.png';files[path]=raw
        assets.append({**asset,'path':path})
    # A prior input project.json is replaced by this reviewed current declaration.
    files.pop('project.json',None)
    definitions={'schema_version':1,'root_unit_id':draft.get('root_unit_id'),
        'behavior_profile':draft.get('behavior_profile'),'item_template_id':draft.get('item_template_id'),
        'units':[r['server'] for r in draft.get('forms',[])],
        'weapons':[r['server'] for r in draft.get('weapons',[])],
        'blueprints':draft.get('blueprints',[])}
    from .model import item_text
    text=item_text(draft.get('item_text'))
    # Written only when authored, so projects without item text stay readable by earlier Managers.
    if any(text.values()):definitions['item_text']=text
    from .blueprint_templates import validate_project_bindings
    validate_project_bindings(definitions,files=files)
    manifest={'protocol':PROTOCOL,'schema_version':SCHEMA_VERSION,'name':draft['name'],
        'project_key':draft['family_key'],
        'metadata':normalize_metadata(draft.get('metadata',{})),
        'media':normalize_media(draft.get('media',{}),draft.get('assets',[])),
        'presentation':copy.deepcopy(draft.get('presentation',{})),
        'assets':assets,'definitions':definitions,
        'files':{name:hashlib.sha256(raw).hexdigest() for name,raw in sorted(files.items())},
        'runtime_installable':False,'contains_owner_content':True}
    if draft.get('native_programs'):
        from .native_programs import normalize_bindings,validate_project_bindings as validate_programs
        manifest['native_programs']=normalize_bindings(draft['native_programs'])
    if draft.get('native_content'):
        from .native_content import normalize_bindings,validate_project_bindings as validate_content
        manifest['native_content']=normalize_bindings(draft['native_content'])
        validate_content(manifest['native_content'],files=files)
    resolve_native_parts(files=files,definitions=definitions,native_programs=manifest.get('native_programs',[]),native_content=manifest.get('native_content',[]))
    files['project.json']=json.dumps(manifest,ensure_ascii=False,sort_keys=True).encode('utf-8')
    from .archive import MAX_FILES,MAX_EXPANDED
    if len(files)>MAX_FILES or sum(len(raw) for raw in files.values())>MAX_EXPANDED:
        raise ValueError('制作项目超过可重新导入的文件数量或展开容量上限')
    out=io.BytesIO()
    with zipfile.ZipFile(out,'w',zipfile.ZIP_DEFLATED) as archive:
        for name,raw in sorted(files.items()):archive.writestr(name,raw)
    if len(out.getvalue())>32*1024*1024:raise ValueError('制作项目超过32MiB导入上限')
    return out.getvalue()


def read_project(files):
    raw=files.get('project.json')
    if raw is None:return None
    try:value=json.loads(raw.decode('utf-8-sig'))
    except (UnicodeError,ValueError):raise ValueError('制作项目声明不是有效的UTF-8 JSON') from None
    if not isinstance(value,dict) or value.get('protocol')!=PROTOCOL or value.get('schema_version')!=SCHEMA_VERSION:
        raise ValueError('不支持的制作项目声明')
    expected=value.get('files')
    if not isinstance(expected,dict) or set(expected)!=set(files)-{'project.json'}:
        raise ValueError('制作项目资源清单不完整')
    for name,digest in expected.items():
        if not isinstance(digest,str) or hashlib.sha256(files[name]).hexdigest()!=digest:
            raise ValueError('制作项目资源被修改：'+name)
    name=plain(value.get('name',''),'项目名称',48)
    if not name:raise ValueError('请填写项目名称')
    project_key=value.get('project_key')
    if project_key is not None and (not isinstance(project_key,str) or not re.fullmatch('[A-Za-z0-9][A-Za-z0-9._-]{0,127}',project_key)):
        raise ValueError('制作项目稳定编号无效')
    assets=value.get('assets',[])
    if not isinstance(assets,list) or len(assets)>128:raise ValueError('制作项目素材列表无效')
    seen=set()
    for asset in assets:
        if not isinstance(asset,dict) or asset.get('sha') in seen:raise ValueError('制作项目素材身份无效或重复')
        seen.add(asset.get('sha'))
        path=asset.get('path')
        if not isinstance(path,str) or not path.startswith('assets/') or PurePosixPath(path).suffix.lower()!='.png' or path not in files:
            raise ValueError('制作项目缺少素材文件')
        if hashlib.sha256(files[path]).hexdigest()!=asset.get('sha'):raise ValueError('素材哈希与资源不一致')
        if asset.get('kind') not in ('image','source-image'):raise ValueError('素材类型无效')
    definitions=value.get('definitions',{})
    from .native_programs import normalize_bindings
    native_programs=normalize_bindings(value.get('native_programs',[]))
    from .native_content import normalize_bindings as normalize_content
    native_content=normalize_content(value.get('native_content',[]))
    parts=resolve_native_parts(files=files,definitions=definitions,native_programs=native_programs,native_content=native_content)
    return {'name':name,'project_key':project_key,'metadata':normalize_metadata(value.get('metadata',{})),
        'media':normalize_media(value.get('media',{}),assets),'presentation':value.get('presentation',{}),
        'assets':assets,'definitions':definitions,
        'client_template_bindings':parts['blueprints'],'native_programs':native_programs,
        'native_program_bindings':parts['programs'],'native_content':native_content,
        'native_content_bindings':parts['content']}


def resolve_native_parts(*,files,definitions,native_programs,native_content):
    from .blueprint_templates import validate_project_bindings as templates
    from .native_content import validate_project_bindings as content
    from .native_programs import validate_project_bindings as programs
    blueprints=templates(definitions,files=files)
    groups=content(native_content,files=files)
    excluded=set().union(*(row['members'] for row in blueprints+groups)) if blueprints or groups else set()
    return {'blueprints':blueprints,'content':groups,'programs':programs(native_programs,files=files,excluded_members=excluded)}
