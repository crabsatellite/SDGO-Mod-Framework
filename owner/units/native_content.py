"""Portable native dependency authoring, distinct from runtime admission.

Every declared member is hash-bound. Source evidence is retained as evidence;
it never becomes an implicit native record or resource. Unknown dispatch values
remain in the original bytes and in the authoring requirements.
"""
from __future__ import annotations

import hashlib
import json
import re
import struct
from pathlib import PurePosixPath

from .codec import scan_resource_references, _utf16
from .profiles.skill_target_1132 import recover_source_record

SCHEMA='sdgo.unit-native-content/1'
PROFILES={13:('native-type13-1132-v1',1132),11:('native-type11-732-v1',732)}
PARTS={'server_skills','server_funnels','skill_atlases','skill_identities','source_evidence'}


def _exact(value,allowed,required,label):
    if not isinstance(value,dict) or set(value)-set(allowed) or set(required)-set(value):
        raise ValueError(label+' fields differ')


def _digest(value):
    if not isinstance(value,str) or not re.fullmatch('[0-9a-f]{64}',value):raise ValueError('native content SHA-256 is invalid')
    return value


def _member(value):
    if (not isinstance(value,str) or not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_./-]{0,511}',value) or
            any(part in ('','.','..') for part in value.split('/'))):raise ValueError('native content archive member is invalid')
    return value


def normalize_bindings(value):
    if not isinstance(value,list) or len(value)>128:raise ValueError('native content binding count budget')
    result=[];seen=set()
    for row in value:
        _exact(row,{'manifest_path','manifest_sha256'},{'manifest_path','manifest_sha256'},'native content binding')
        name=_member(row['manifest_path'])
        if not name.endswith('.json') or name in seen:raise ValueError('native content manifest is invalid or repeated')
        seen.add(name);result.append({'manifest_path':name,'manifest_sha256':_digest(row['manifest_sha256'])})
    return result


def prepare_attachment(files,token):
    """Namespace one complete dependency ZIP without overwriting family files.

    Original manifest bytes and the original transport-name map are retained as
    evidence. Native virtual paths and record bytes are never rewritten here.
    """
    if not isinstance(token,str) or not re.fullmatch('[0-9a-f]{32}',token):
        raise ValueError('native content attachment token is invalid')
    roots=[name for name in ('manifest.json','native-content.json') if name in files]
    if len(roots)!=1:raise ValueError('配套资料 ZIP 必须包含一个 manifest.json 或 native-content.json')
    source=roots[0];original={'manifest_path':source,'manifest_sha256':hashlib.sha256(files[source]).hexdigest()}
    validate_project_bindings([original],files=files)
    manifest=_json(files,original)
    prefix='native-content/'+token+'/'
    mapping={name:prefix+'objects/'+hashlib.sha256(name.encode('utf-8')).hexdigest()+PurePosixPath(name).suffix.lower() for name in files}
    if len(set(mapping.values()))!=len(mapping):raise ValueError('native content transport namespace collision')
    output={mapping[name]:raw for name,raw in files.items()}
    for section in ('records','resources','evidence'):
        for row in manifest.get(section,[]):row['member']=mapping[row['member']]
    for part in PARTS:
        if part in manifest:manifest[part]['manifest_path']=mapping[manifest[part]['manifest_path']]
    claimed={row['member'] for section in ('records','resources','evidence') for row in manifest.get(section,[])}
    claimed.update(manifest[part]['manifest_path'] for part in PARTS if part in manifest)
    evidence=manifest.setdefault('evidence',[])
    for member,raw in output.items():
        if member not in claimed:evidence.append({'member':member,'sha256':hashlib.sha256(raw).hexdigest()})
    map_member=prefix+'member-map.json';map_raw=json.dumps({'source_members':mapping},sort_keys=True).encode('utf-8')
    output[map_member]=map_raw;evidence.append({'member':map_member,'sha256':hashlib.sha256(map_raw).hexdigest()})
    prepared=prefix+'manifest.json';raw=json.dumps(manifest,ensure_ascii=False,sort_keys=True).encode('utf-8');output[prepared]=raw
    binding={'manifest_path':prepared,'manifest_sha256':hashlib.sha256(raw).hexdigest()}
    validate_project_bindings([binding],files=output)
    return binding,output


def _bytes(files,member,digest):
    member=_member(member);_digest(digest)
    raw=files.get(member)
    if not isinstance(raw,bytes) or hashlib.sha256(raw).hexdigest()!=digest:
        raise ValueError('native content member is missing or changed: '+member)
    return raw


def _json(files,binding):
    _exact(binding,{'manifest_path','manifest_sha256'},{'manifest_path','manifest_sha256'},'native content manifest reference')
    raw=_bytes(files,binding['manifest_path'],binding['manifest_sha256'])
    if len(raw)>1024*1024:raise ValueError('native content manifest byte budget')
    def pairs(values):
        result={}
        for key,value in values:
            if key in result:raise ValueError('duplicate native content JSON field')
            result[key]=value
        return result
    try:
        result=json.loads(raw.decode('utf-8'),object_pairs_hook=pairs)
        if not isinstance(result,dict):raise ValueError('native content manifest must be an object')
        return result
    except (UnicodeError,json.JSONDecodeError):raise ValueError('native content manifest JSON is invalid') from None


def validate_project_bindings(value,*,files):
    result=[];identities=set();members_used=set();resource_owners={}
    for binding in normalize_bindings(value):
        manifest=_json(files,binding)
        _exact(manifest,{'schema','owner','revision','records','resources','evidence',*PARTS},
               {'schema','owner','revision','records','resources'},'native content manifest')
        owner=manifest['owner'];_digest(manifest['revision'])
        if manifest['schema']!=SCHEMA or not isinstance(owner,str) or not re.fullmatch('[a-z0-9][a-z0-9._-]{0,63}',owner):
            raise ValueError('native content schema/owner is invalid')
        records=manifest['records'];resources=manifest['resources'];evidence=manifest.get('evidence',[])
        if (not isinstance(records,list) or len(records)>4096 or not isinstance(resources,list) or len(resources)>512 or
                not isinstance(evidence,list) or len(evidence)>512):raise ValueError('native content declaration count budget')
        members={binding['manifest_path']};raw_records={13:{},11:{}};summaries=[];issues=[];direct_refs=set()
        def issue(code,message,level='required'):
            issues.append({'code':code,'message':message,'level':level,'field':'native_content'})
        for row in records:
            _exact(row,{'kind','id','profile','member','sha256'},{'kind','id','profile','member','sha256'},'native content record')
            kind,identity=row['kind'],row['id']
            if (type(kind)is not int or not 1<=kind<=255 or type(identity)is not int or not 1<=identity<=9999999 or
                    identity in identities):raise ValueError('native content kind/identity is invalid or repeated')
            raw=_bytes(files,row['member'],row['sha256'])
            if not isinstance(row['profile'],str) or not re.fullmatch('[a-z0-9][a-z0-9._-]{0,95}',row['profile']):
                raise ValueError('native content profile identity is invalid')
            known=kind in PROFILES and row['profile']==PROFILES[kind][0]
            if len(raw)<4 or len(raw)>1024*1024 or struct.unpack_from('<I',raw)[0]!=identity:
                raise ValueError('native content complete record/profile differs')
            if known:
                profile,extent=PROFILES[kind]
                if row['profile']!=profile or len(raw)!=extent or raw[4]!=0:
                    raise ValueError('native content complete record/profile differs')
            if row['member'] in members:raise ValueError('native content record member is repeated')
            identities.add(identity);members.add(row['member'])
            if known:raw_records[kind][identity]=raw
            entry={'kind':kind,'id':identity,'profile':row['profile'],'member':row['member'],'sha256':row['sha256'],
                   'bytes':len(raw),'layout_supported':known}
            if known and kind==13:
                recover_source_record(raw)
                entry.update(name=_utf16(raw,5,100,'skill name',required=False),dispatch=raw[1131],
                    icon_frame=struct.unpack_from('<H',raw,426)[0],linked_skill=struct.unpack_from('<I',raw,354)[0])
                if raw[1131]>3:issue('native_dispatch',f"技能 {identity} 的原生调度值 {raw[1131]} 尚未支持；原值已保留",'error')
            elif known and kind==11:entry.update(name='浮游定义 '+str(identity),funnel_prop=raw[5])
            else:
                entry.update(name='待适配原生定义 '+str(identity))
                issue('native_profile',f'原生类型 {kind}／{row["profile"]} 尚未支持；完整 {len(raw)} 字节资料已保留','error')
            direct_refs.update(scan_resource_references(raw));summaries.append(entry)
        native_resources={};resource_rows=[]
        for row in resources:
            _exact(row,{'path','member','sha256'},{'path','member','sha256'},'native content resource')
            path=row['path']
            if (not isinstance(path,str) or len(path)>240 or not re.fullmatch(r'(?:mdrs|anrs|txrs|fxrs|sprs|sdrs)/[a-z0-9][a-z0-9_./-]*',path) or
                    any(part in ('','.','..') for part in path.split('/')) or path in native_resources):
                raise ValueError('native content resource path is invalid or repeated')
            root=path.split('/')[0];suffix=path.rsplit('.',1)[-1]
            if suffix not in {'mdrs':{'mod'},'anrs':{'ani'},'txrs':{'txr'},'fxrs':{'zfx'},'sprs':{'spr'},'sdrs':{'wav','ogg','zsd'}}[root]:
                raise ValueError('native content resource kind is not supported')
            raw=_bytes(files,row['member'],row['sha256'])
            if row['member'] in members:raise ValueError('native content resource/evidence roles overlap')
            previous=resource_owners.get(path)
            if previous and previous!=(owner,row['sha256']):raise ValueError('native content resource crosses owner/version domains')
            resource_owners[path]=(owner,row['sha256']);native_resources[path]=raw;resource_rows.append(dict(row));members.add(row['member'])
        for row in evidence:
            _exact(row,{'member','sha256'},{'member','sha256'},'native content source evidence')
            _bytes(files,row['member'],row['sha256'])
            if row['member'] in members:raise ValueError('native content evidence is also declared as runtime data')
            members.add(row['member'])
        parts={}
        for part in sorted(PARTS):
            if part not in manifest:continue
            reference=manifest[part];data=_json(files,reference)
            if reference['manifest_path'] in members:raise ValueError('native content manifest reference is repeated')
            members.add(reference['manifest_path']);parts[part]=data
            if data.get('owner')!=owner:raise ValueError('native content component crosses owners')
            if part in ('server_skills','server_funnels'):
                from .server_dependencies import validate_manifest
                kind=13 if part=='server_skills' else 11;name='skill' if kind==13 else 'funnel'
                validate_manifest(data,raw_records[kind],kind=name,authoring=True)
                section='skills' if kind==13 else 'funnels'
                supplied={entry['id'] for entry in data[section]}
                if not supplied<=set(raw_records[kind]):raise ValueError('native content server record inventory differs')
                for identity in sorted(set(raw_records[kind])-supplied):
                    issue('native_server_fields',f'请补充{"技能" if kind==13 else "浮游"} {identity} 的服务端定义')
                from .server_dependencies import _spec
                columns=_spec(name)[3]
                for entry in data[section]:
                    if entry['definition']['kind']=='draft':
                        missing=[field for field in columns if field not in entry['definition']['values']]
                        issue('native_server_fields',f'服务端{"技能" if kind==13 else "浮游"} {entry["id"]} 尚为草稿'+
                              ('；缺少 '+', '.join(missing) if missing else '；请核对并保存完整定义'))
            elif part=='skill_atlases':
                from .skill_atlases import validate_manifest
                validate_manifest(data,raw_records[13],native_resources,authoring=True)
                if {entry['id'] for entry in data['skills']}!=set(raw_records[13]):raise ValueError('native content atlas record inventory differs')
            elif part=='skill_identities':
                from .skill_identity_runtime import SCHEMA as ID_SCHEMA,SUPPORTED_SOURCES
                _exact(data,{'schema','owner','revision','bindings'},{'schema','owner','revision','bindings'},'native content identity request')
                _digest(data['revision'])
                if data['schema']!=ID_SCHEMA or not isinstance(data['bindings'],list) or not 1<=len(data['bindings'])<=4096:
                    raise ValueError('native content identity request is invalid')
                seen=set()
                for entry in data['bindings']:
                    _exact(entry,{'target','source','client_sha256'},{'target','source','client_sha256'},'native content identity binding')
                    target=entry['target'];source=entry['source']
                    if (type(target)is not int or target not in raw_records[13] or target in seen or type(source)is not int or
                            source not in SUPPORTED_SOURCES or _digest(entry['client_sha256'])!=hashlib.sha256(raw_records[13][target]).hexdigest()):
                        raise ValueError('native content identity request differs from its actual skill record/profile')
                    seen.add(target)
            elif part=='source_evidence':
                from .dependencies import validate_manifest
                source_ir=validate_manifest(data)
                for entry in source_ir['records']:
                    record=raw_records[13].get(entry['skill_id'])
                    if record is None or hashlib.sha256(record).hexdigest()!=entry['target_sha256']:
                        raise ValueError('native content dependency evidence differs from its actual record')
        if raw_records[13] and 'server_skills' not in parts:issue('native_server_skills','请补充配套的服务端技能定义', 'required')
        if raw_records[11] and 'server_funnels' not in parts:issue('native_server_funnels','请补充配套的服务端浮游定义','required')
        if raw_records[13] and 'skill_atlases' not in parts:issue('native_skill_atlases','请补充技能图集及完整纹理','required')
        if 'server_skills' in parts:
            from .skill_identity_runtime import SUPPORTED_SOURCES
            mapped={entry['target'] for entry in parts.get('skill_identities',{}).get('bindings',[])}
            needed={entry['id'] for entry in parts['server_skills']['skills'] if entry['definition']['kind']=='template' and entry['definition']['id'] in SUPPORTED_SOURCES}
            for identity in sorted(needed-mapped):issue('native_skill_identities',f'技能 {identity} 需要配套的原生行为绑定','required')
        if 'skill_identities' in parts and 'server_skills' in parts:
            server=parts['server_skills'];request=parts['skill_identities']
            if server['revision']!=request['revision']:raise ValueError('native content identity/server revisions differ')
            definitions={entry['id']:entry['definition'] for entry in server['skills']}
            for entry in request['bindings']:
                definition=definitions.get(entry['target'],{})
                if definition.get('kind')!='template' or definition.get('id')!=entry['source']:
                    raise ValueError('native content identity request is not the declared server template')
        if members_used&members:raise ValueError('native content bindings reuse transport members')
        members_used.update(members)
        result.append({'binding':binding,'manifest':manifest,'members':members,'records':summaries,'resources':resource_rows,'components':parts,
            'direct_resource_refs':sorted(direct_refs),'requirements':issues,
            'summary':{'skills':len(raw_records[13]),'funnels':len(raw_records[11]),'resources':len(resource_rows),
                'unsupported_records':sum(not row['layout_supported'] for row in summaries),
                'server_skills':'server_skills' in parts,'server_funnels':'server_funnels' in parts,'skill_atlases':'skill_atlases' in parts,
                'identity_bindings':len(parts.get('skill_identities',{}).get('bindings',[]))},
            'native_registration_authorized':False,'runtime_authority_issued':False})
    if len(identities)>4096 or len(members_used)>512:raise ValueError('native content aggregate project budget')
    return result
