"""Staged unit-family imports, required fields, preview approval and publication."""
from __future__ import annotations

import copy
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import threading
import uuid

from . import model


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')


def save_json(path, value):
    path=Path(path);path.parent.mkdir(parents=True,exist_ok=True)
    temporary=path.with_name('.'+path.name+'.'+uuid.uuid4().hex+'.tmp')
    try:
        with temporary.open('wb') as stream:
            stream.write(model.canonical(value)+b'\n');stream.flush();os.fsync(stream.fileno())
        os.replace(temporary,path)
    finally:
        temporary.unlink(missing_ok=True)


class Workspace:
    def __init__(self,root,client=None,snapshot=None,server_ini=None,module_binary=None,
                 baseline=None,repository=None,publisher=None,archive_executable=None):
        if baseline is None:
            from .assets import Baseline
            baseline=Baseline(Path(client) if client else None,Path(snapshot) if snapshot else None)
        if repository is None:
            from .repository import Repository
            repository=Repository(Path(snapshot) if snapshot else None,Path(server_ini) if server_ini else None)
        self.root=Path(root).resolve();self.root.mkdir(parents=True,exist_ok=True)
        self.path=self.root/'unit-families.json';self.lock=threading.RLock()
        self.baseline=baseline
        self.repository=repository
        self.module_binary=Path(module_binary).resolve() if module_binary else None
        self.publisher=publisher;self.archive_executable=archive_executable
        self._lock_stream=(self.root/'.workspace.lock').open('a+b')
        try:
            self._lock_stream.seek(0);self._lock_stream.write(b'0');self._lock_stream.flush();self._lock_stream.seek(0)
            if os.name=='nt':
                import msvcrt
                msvcrt.locking(self._lock_stream.fileno(),msvcrt.LK_NBLCK,1)
            else:
                import fcntl
                fcntl.flock(self._lock_stream.fileno(),fcntl.LOCK_EX|fcntl.LOCK_NB)
        except OSError:
            self._lock_stream.close();raise ValueError('该机体工作区已被另一窗口打开') from None
        if not self.path.exists():save_json(self.path,{'schema_version':1,'drafts':{},'retired_categories':[]})
        self._recover()

    def close(self):
        if not self._lock_stream.closed:self._lock_stream.close()

    def _recover(self):
        for pending in (self.root/'transactions').glob('*/pending.json'):
            row=json.loads(pending.read_text(encoding='utf-8'))
            if row.get('stage')=='applied':
                current=self.read()
                if model.revision(current)==row['before_revision']:
                    save_json(self.path,row['state']);row['stage']='published';save_json(pending,row)
                elif model.revision(current)==model.revision(row['state']):
                    row['stage']='published';save_json(pending,row)
                else:raise ValueError('存在服务端已应用、工作区待恢复的发布；请先处理恢复收据')

    def read(self):
        value=json.loads(self.path.read_text(encoding='utf-8'))
        if value.get('schema_version')!=1 or not isinstance(value.get('drafts'),dict):raise ValueError('不支持的机体工作区格式')
        return value

    def check(self,expected):
        state=self.read()
        if expected!=model.revision(state):raise model.RevisionConflict('配置已改变，请刷新后重试')
        return state

    def blob(self,sha):
        if not isinstance(sha,str) or not re.fullmatch('[0-9a-f]{64}',sha):raise ValueError('资源指纹无效')
        path=self.root/'objects'/sha
        raw=path.read_bytes()
        if hashlib.sha256(raw).hexdigest()!=sha:raise ValueError('资源内容已改变，请重新上传')
        return raw

    def _store(self,raw):
        sha=hashlib.sha256(raw).hexdigest();path=self.root/'objects'/sha;path.parent.mkdir(exist_ok=True)
        if path.exists():
            if path.read_bytes()!=raw:raise ValueError('工作区资源指纹冲突')
        else:
            with path.open('xb') as stream:stream.write(raw)
        return sha

    def _commit(self,state,selected):
        before=self.read();history=self.root/'history'/(model.revision(before)+'.json')
        if not history.exists():save_json(history,before)
        save_json(self.path,state)
        return self.public(selected)

    def _get(self,state,identity):
        if identity not in state['drafts']:raise ValueError('机体草稿不存在')
        return state['drafts'][identity]

    def _touch(self,draft):
        draft['updated']=now();draft['status']='draft'
        for key in ('confirmation','release','server_result'):draft.pop(key,None)
        draft['content_revision']=model.content_revision(draft)

    def inspect(self,filename,raw,expected):
        from .archive import inspect_upload
        with self.lock:
            state=self.check(expected)
            parsed=inspect_upload(filename,raw,self.baseline,archive_executable=self.archive_executable)
            project=parsed.get('project')
            sha=hashlib.sha256(raw).hexdigest()
            existing=next((d['id'] for d in state['drafts'].values() if d.get('source_sha')==sha),None)
            if existing:return self.public(existing)
            if project and project.get('project_key') and any(d['family_key']==project['project_key'] for d in state['drafts'].values()):
                raise model.RevisionConflict('该制作项目已存在，请先核对并明确更新，不能覆盖现有资料')
            identity=uuid.uuid4().hex
            forms=[model.new_record(r,'unit') for r in parsed['forms']]
            weapons=[model.new_record(r,'weapon') for r in parsed['weapons']]
            single=len(forms)==1 and not forms[0]['transform_target']
            draft={'id':identity,'family_key':'unit-import.'+identity,'name':project['name'] if project else forms[0]['name'],
                'source_filename':Path(filename).name,'source_sha':sha,'root_unit_id':forms[0]['id'] if single else None,
                'behavior_profile':'native-single-form-v1' if single else None,'item_template_id':None,'category_id':None,
                'forms':forms,'weapons':weapons,'files':[],'assets':[],
                'presentation':{role:{'asset_sha':None,'caption':'商城设定售价' if role=='shop' else '由扭蛋模块设定概率'} for role in ('shop','capsule')},
                'parse_warnings':parsed.get('warnings',[]),'created':now(),'updated':now(),'status':'draft'}
            if project:
                if project.get('project_key'):draft['family_key']=project['project_key']
                if project.get('native_programs'):draft['native_programs']=copy.deepcopy(project['native_programs'])
                draft['metadata']=copy.deepcopy(project['metadata']);draft['media']=copy.deepcopy(project['media'])
                draft['project_kind']='unit-family' if forms else 'metadata-only'
            elif parsed.get('blueprints'):
                from .blueprints import from_record
                draft['blueprints']=[]
                for source_recipe in parsed['blueprints']:
                    row=source_recipe['blueprint']
                    draft['blueprints'].append(from_record(row,domain='archive-sha256:'+sha))
            if single:
                forms[0]['server']['SallyReady']=1;forms[0]['sources']['SallyReady']='single-form'
            for record in parsed['forms']+parsed['weapons']+parsed.get('blueprints',[]):self._store(record['raw'])
            for name,data in parsed['files'].items():
                blob=self._store(data);draft['files'].append({'path':name,'sha':blob,'bytes':len(data)})
            if project:
                from .assets import normalize_png
                for asset in project['assets']:
                    data=parsed['files'][asset['path']];normalize_png(data)
                    self._store(data)
                    draft['assets'].append({k:v for k,v in asset.items() if k!='path'})
            self._preview_candidates(draft,parsed)
            if project:
                patch=model.definitions(project['definitions']) if project.get('definitions') else {}
                if project.get('presentation'):patch['presentation']=project['presentation']
                if patch:draft=model.update(draft,patch,self.repository.catalog)
            draft['content_revision']=model.content_revision(draft)
            state['drafts'][identity]=draft
            response=self._commit(state,identity)
            if (project and draft.get('item_template_id')) or draft.get('blueprints'):
                return self.update(identity,{},response['revision'])
            return response

    def create_project(self,value,expected):
        from .project import normalize_metadata
        if not isinstance(value,dict) or set(value)-{'name','metadata'}:raise ValueError('新建资料项目字段无效')
        with self.lock:
            state=self.check(expected);identity=uuid.uuid4().hex
            draft={'id':identity,'family_key':'unit-import.'+identity,'name':model.text(value.get('name',''),'项目名称'),
                'project_kind':'metadata-only','metadata':normalize_metadata(value.get('metadata',{})),'media':{},
                'source_filename':None,'source_sha':None,'root_unit_id':None,'behavior_profile':None,
                'item_template_id':None,'category_id':None,'forms':[],'weapons':[],'files':[],'assets':[],
                'presentation':{role:{'asset_sha':None,'caption':''} for role in ('shop','capsule')},
                'parse_warnings':[],'created':now(),'updated':now(),'status':'draft'}
            draft['content_revision']=model.content_revision(draft);state['drafts'][identity]=draft
            return self._commit(state,identity)

    def _preview_candidates(self,draft,parsed):
        from .assets import normalize_png,render_portrait
        overlay={name.replace('\\','/').lower():raw for name,raw in parsed['files'].items()}
        def read(name):
            normalized=name.replace('\\','/').lower()
            return overlay[normalized] if normalized in overlay else self.baseline.read(normalized)
        def candidate(path,label,source):
            try:png=normalize_png(render_portrait(path,read))
            except (ValueError,KeyError,OSError):return
            sha=self._store(png)
            if not any(a['sha']==sha for a in draft['assets']):
                draft['assets'].append({'sha':sha,'filename':label,'kind':'image','source':source})
        for record in parsed['forms']:
            for offset,role in ((4654,'单位图A'),(4754,'单位图B'),(4854,'单位图C')):
                path=record['raw'][offset:offset+100].decode('utf-16-le').split('\0')[0]
                if not path:continue
                candidate(path,f"{record['id']} {role} ({path})",'package-sprite' if path.replace('\\','/').lower() in overlay else 'baseline-sprite')
        for name,raw in parsed['files'].items():
            if name.lower().endswith(('.spr','.txr')):
                candidate(name,name,'package-sprite' if name.lower().endswith('.spr') else 'package-texture')
            elif name.lower().endswith('.png'):
                try:png=normalize_png(raw)
                except ValueError:continue
                sha=self._store(png)
                if not any(a['sha']==sha for a in draft['assets']):draft['assets'].append({'sha':sha,'filename':name,'kind':'image','source':'package-texture'})

    def update(self,identity,patch,expected):
        with self.lock:
            state=self.check(expected);before=self._get(state,identity)
            value=model.update(before,patch,self.repository.catalog)
            if value.get('root_unit_id') and value.get('category_id') is None:
                occupied=set(self.repository.catalog.get('items',{}))|set(state.get('retired_categories',[]))
                occupied|={d['category_id'] for d in state['drafts'].values() if d.get('category_id')}
                category=next((n for n in range(1119000,1120000) if n not in occupied and not self._any_id(n)),None)
                if category is None:raise ValueError('本模块的机体物品编号空间已用满')
                value['category_id']=category
                state['retired_categories']=sorted(set(state.get('retired_categories',[]))|{category})
            if value.get('blueprints'):
                from .blueprints import RECIPE_FIRST,RECIPE_LAST,CATEGORY_FIRST,CATEGORY_LAST
                used_recipes=set(state.get('retired_blueprints',[]))
                retired_categories=set(state.get('retired_categories',[]))
                used_categories=set(self.repository.catalog.get('items',{}))|retired_categories
                for other in state['drafts'].values():
                    for recipe in other.get('blueprints',[]):
                        if recipe.get('id'):used_recipes.add(recipe['id'])
                        if recipe.get('item_category_id'):used_categories.add(recipe['item_category_id'])
                for recipe in value['blueprints']:
                    if recipe['id'] is None:
                        recipe['id']=next((i for i in range(RECIPE_FIRST,RECIPE_LAST+1) if i not in used_recipes and not self._any_id(i)),None)
                        if recipe['id'] is None:raise ValueError('设计图登记编号空间已用满')
                    used_recipes.add(recipe['id'])
                    if recipe['item_category_id'] is None:
                        recipe['item_category_id']=next((i for i in range(CATEGORY_FIRST,CATEGORY_LAST+1) if i not in used_categories and not self._any_id(i)),None)
                        if recipe['item_category_id'] is None:raise ValueError('图纸物品编号空间已用满')
                    used_categories.add(recipe['item_category_id'])
                    retired_categories.add(recipe['item_category_id'])
                state['retired_blueprints']=sorted(used_recipes)
                state['retired_categories']=sorted(retired_categories)
            self._touch(value);state['drafts'][identity]=value
            return self._commit(state,identity)

    def upload_definitions(self,identity,filename,raw,expected):
        if len(raw)>2*1024*1024 or not filename.lower().endswith('.json'):raise ValueError('请补传不超过 2 MiB 的 JSON 定义')
        try:value=json.loads(raw.decode('utf-8-sig'))
        except (UnicodeError,json.JSONDecodeError):raise ValueError('服务端定义不是有效的 UTF-8 JSON') from None
        return self.update(identity,model.definitions(value),expected)

    def removable_missing_skills(self,draft):
        known=self.repository.catalog.get('skill_ids')
        if known is None or self.repository.public_info().get('source')=='unavailable':return []
        result={}
        # Only these two skills have a verified shared client/server encoding.
        for row in draft['forms']:
            for field in ('SkillNumber_1','SkillNumber_2'):
                skill=row['server'].get(field)
                if skill and skill not in known:
                    result.setdefault(skill,[]).append({'form_id':row['id'],'field':field})
        return [{'skill_id':skill,'references':refs} for skill,refs in sorted(result.items())]

    def drop_missing_skills(self,identity,skill_ids,expected):
        if (not isinstance(skill_ids,list) or not skill_ids or len(skill_ids)>1024 or
            any(type(n) is not int or n<=0 for n in skill_ids) or len(set(skill_ids))!=len(skill_ids)):
            raise ValueError('请明确选择要移除的缺失技能编号')
        with self.lock:
            state=self.check(expected);draft=copy.deepcopy(self._get(state,identity))
            if draft['status']=='disabled':raise ValueError('已停用的草稿不能移除技能')
            available={r['skill_id']:r for r in self.removable_missing_skills(draft)}
            if set(skill_ids)-available.keys():
                raise ValueError('只能移除当前缺失且已验证可同步编码的技能1/2；不能删除已支持技能或猜测其他字段')
            for skill in skill_ids:
                for ref in available[skill]['references']:
                    row=next(r for r in draft['forms'] if r['id']==ref['form_id'])
                    row['server'][ref['field']]=0
                    row['sources'][ref['field']]='owner:removed-missing-skill'
                    draft.setdefault('skill_omissions',[]).append({**ref,'skill_id':skill,'replacement':0,'at':now()})
            self._touch(draft);state['drafts'][identity]=draft
            return self._commit(state,identity)

    def upload_asset(self,identity,filename,raw,role,expected):
        from .assets import normalize_png
        from .codec import decode_record
        with self.lock:
            state=self.check(expected);draft=copy.deepcopy(self._get(state,identity))
            if role=='activity_poster':
                from .project import normalize_media
                png=normalize_png(raw);source_sha=self._store(raw);sha=self._store(png)
                for digest,kind in ((source_sha,'source-image'),(sha,'image')):
                    existing=next((a for a in draft['assets'] if a['sha']==digest),None)
                    if existing is None:draft['assets'].append({'sha':digest,'filename':Path(filename).name,'kind':kind,'source':'owner-upload'})
                    elif kind=='image':existing['kind']='image'
                media=copy.deepcopy(draft.get('media',{}));media['activity_poster']={
                    'asset_sha':sha,'source_sha':source_sha,'filename':Path(filename).name,'caption':'',
                    'source_kind':'owner-provided'}
                draft['media']=normalize_media(media,draft['assets'])
            elif role in ('shop','capsule'):
                png=normalize_png(raw);sha=self._store(png)
                if not any(a['sha']==sha for a in draft['assets']):draft['assets'].append({'sha':sha,'filename':Path(filename).name,'kind':'image','source':'upload'})
                draft['presentation'][role]['asset_sha']=sha
                draft['presentation'][role]['source_mode']='selected'
            elif role=='dependency':
                suffix=Path(filename).suffix.lower()
                if suffix=='.bin':
                    record=decode_record(raw);kind=record['kind']
                    if kind=='blueprint':
                        from .blueprints import from_record
                        prior=draft.get('blueprints',[])
                        if any(r.get('source',{}).get('record_sha256')==record['blueprint']['source_sha256'] for r in prior):
                            raise ValueError('该设计图原始记录已存在')
                        draft.setdefault('blueprints',[]).append(from_record(record['blueprint'],domain='uploaded-record'))
                        sha=self._store(raw)
                        draft['files'].append({'path':'records/blueprint-'+str(record['id'])+'.bin','sha':sha,'bytes':len(raw)})
                        self._touch(draft);state['drafts'][identity]=draft
                        result=self._commit(state,identity)
                        return self.update(identity,{},result['revision'])
                    key='forms' if kind in ('unit','units') else 'weapons'
                    if any(r['id']==record['id'] for r in draft['forms']+draft['weapons']):raise ValueError('补传记录编号已存在，不能覆盖原始记录')
                    draft[key].append(model.new_record(record,'unit' if key=='forms' else 'weapon'))
                    self._store(raw)
                else:
                    directories={'.mod':'mdrs','.ani':'anrs','.txr':'txrs','.zfx':'fxrs','.spr':'sprs','.zsd':'sdrs','.scp':'scp'}
                    if suffix not in directories or not 0<len(raw)<=32*1024*1024:raise ValueError('不支持该补传资源格式或大小')
                    leaf=Path(filename.replace('\\','/')).name
                    if not re.fullmatch(r'[A-Za-z0-9_-]+\.[A-Za-z0-9]+',leaf):raise ValueError('资源文件名须使用字母、数字、下划线或连字符')
                    virtual=directories[suffix]+'/'+leaf.lower()
                    if any(f['path'].lower()==virtual for f in draft['files']):raise ValueError('该资源路径已存在，请检查要补传的文件')
                    sha=self._store(raw);draft['files'].append({'path':virtual,'sha':sha,'bytes':len(raw)})
            else:raise ValueError('不支持的资源用途')
            self._touch(draft);state['drafts'][identity]=draft
            return self._commit(state,identity)

    def _any_id(self,identity):
        fn=getattr(self.baseline,'any_id',None)
        return bool(fn(identity)) if fn else any(self.baseline.record(kind,identity) is not None for kind in ('unit','weapon','item'))

    def plan(self,draft):
        root=draft['root_unit_id'];template=self.repository.catalog.get('items',{}).get(draft.get('item_template_id'))
        item=None
        if template and root and draft.get('category_id'):
            root_record=next(r for r in draft['forms'] if r['id']==root)
            item=copy.deepcopy(template);item.update(CatalogueID=draft['category_id'],RefIndex=root,Name=root_record['server'].get('UnitModelName',''))
        result={'family_key':draft['family_key'],'revision':model.content_revision(draft),'root_unit_id':root,
                'item_template_id':draft.get('item_template_id'),
                'units':[copy.deepcopy(r['server']) for r in draft['forms']],
                'weapons':[copy.deepcopy(r['server']) for r in draft['weapons']], 'item':item,
                'names':[{'UnitNumber':r['id'],'NameCN':draft['name'] if r['id']==root else r['name'],
                          'NameEN':r['server'].get('UnitModelName',r['name'])} for r in draft['forms']]}
        if draft.get('blueprints'):result['blueprints']=copy.deepcopy(draft['blueprints'])
        return result

    def requirements(self,draft,state=None):
        from .codec import scan_resource_references,patch_unit,patch_weapon,UNIT_FIELDS
        issues=[]
        def need(code,message,field=None,level='required',accept=None):
            issues.append({'code':code,'message':message,'field':field,'level':level,**({'accept':accept} if accept else {})})
        root=draft.get('root_unit_id');forms={r['id']:r for r in draft['forms']};newweapons={r['id'] for r in draft['weapons']}
        if draft.get('native_programs'):
            from .native_programs import validate_project_bindings
            try:
                validate_project_bindings(draft['native_programs'],
                    files={entry['path']:self.blob(entry['sha']) for entry in draft['files']})
                need('native_program_actor_adapter',
                     '请保存制作项目；带有新增初始化脚本的机体尚不能发布',
                     'native_programs','error')
            except (ValueError,KeyError) as error:
                need('native_program_binding',str(error),'native_programs','error')
        file_entries={entry['path']:entry for entry in draft.get('files',[])}
        member_files={}
        try:
            member_files={path:self.blob(entry['sha']) for path,entry in file_entries.items()}
        except (ValueError,OSError) as error:
            need('project_blob',f'制作项目资源已丢失或被修改：{error}',None,'error')
        template_resource_entries={}
        template_evidence_members=set()
        if draft.get('blueprints'):
            try:
                from .blueprint_templates import validate_project_bindings
                validate_project_bindings(
                    {'blueprints':draft['blueprints']},files=member_files)
            except (ValueError,OSError) as error:
                need('blueprint_template_project_binding',str(error),
                    'blueprints.client_templates','error')
        from . import blueprints
        for recipe in draft.get('blueprints',[]):
            template_field=f"blueprints.{recipe.get('id') or recipe.get('name','draft')}.client_templates"
            validating=recipe
            if recipe.get('enabled',True) is False:
                validating=copy.deepcopy(recipe);validating['enabled']=True
            issues.extend(blueprints.requirements(validating,root_unit_id=root,
                unit_ids=set(self.repository.catalog.get('units',{}))|set(forms)))
            if recipe.get('client_templates'):
                try:
                    from .blueprint_templates import validate_binding,require_native_admission
                    resolved=validate_binding(recipe['client_templates'],files=member_files)
                    try:require_native_admission(resolved['manifest'])
                    except ValueError as error:
                        need('blueprint_template_native_adapter',str(error),template_field,'error')
                    template_evidence_members.update(resolved['members'])
                    for resource in resolved['manifest']['runtime_resources']:
                        member=resource['member']
                        if member not in file_entries:
                            raise ValueError('blueprint client-template resource is missing: '+member)
                        template_resource_entries[resource['path']]=file_entries[member]
                    if resolved['manifest']['admission']['resource_closure_verified']:
                        need('blueprint_template_native_ui',
                            '准备版本的 SPR→TXR 引用与纹理解码已验证；目标客户端的真实显示与交互仍需验收',template_field,'warning')
                    else:
                        need('blueprint_template_semantic_closure',
                            f"设计图 {recipe.get('id') or recipe.get('name','')} 的原始/准备记录、哈希和直接资源引用已验证；"
                            'SPR→TXR 等递归语义闭包仍须由资源解析和实机验收确认',
                            template_field,'warning')
                except (ValueError,OSError) as error:
                    need('blueprint_template_binding',str(error),
                        template_field,'error')
        if draft.get('blueprints') and not blueprints.NATIVE_REGISTRATION_SUPPORTED:
            need('blueprint_native_adapter','设计图客户端、事务和实机适配尚未开放；可以保存草稿，不能发布','blueprints','error')
        if draft.get('blueprints'):
            try:blueprints.primary_id(draft['blueprints'])
            except ValueError as error:need('blueprint_primary',str(error),'blueprints','required')
        if root not in forms:need('root','请选择进入机库、商店和扭蛋的根形态','root_unit_id')
        profile=draft.get('behavior_profile')
        if profile not in model.PROFILES:need('behavior','请选择原生变形类型','behavior_profile')
        elif profile=='native-single-form-v1' and (len(forms)!=1 or any(r['transform_target'] for r in forms.values())):
            need('behavior_mismatch','这份记录有变形目标，不能作为无变形机体发布','behavior_profile','error')
        elif profile=='native-linked-forms-v1':
            if len(forms)!=2:need('unsupported_forms','当前适配器要求双形态；不支持任意多段变形','behavior_profile','error')
            if root in forms:
                reached=set();cursor=root
                while cursor in forms and cursor not in reached:reached.add(cursor);cursor=forms[cursor]['transform_target']
                if reached!=set(forms):need('unreachable_form','有形态无法从根形态到达，请核对原始变形关系',None,'error')
        for key,kind,columns in (('forms','unit',model.UNIT_COLUMNS),('weapons','weapon',model.WEAPON_COLUMNS)):
            for row in draft[key]:
                try:
                    raw=self.blob(row['source_sha'])
                    values={k:v for k,v in row['server'].items() if v is not None}
                    if kind=='unit':patch_unit(raw,{k:v for k,v in values.items() if k in UNIT_FIELDS and UNIT_FIELDS[k]['type']!='utf16le'})
                    else:patch_weapon(raw,{k:v for k,v in values.items() if k!='WeaponName'})
                except (ValueError,OSError) as e:need('record_encoding',f"{row['id']} 记录不能安全编码：{e}",None,'error')
                for name in columns:
                    value=row['server'].get(name)
                    if value is None:need('field',f"{row['id']} 缺少 {model.LABELS.get(name,name)}",f'{key}.{row["id"]}.{name}')
                    else:
                        try:model.field_value(name,value)
                        except ValueError as e:need('field_invalid',str(e),f'{key}.{row["id"]}.{name}','error')
                if kind=='unit':
                    if row['server'].get('HP',1)<=0:need('hp','机体耐久必须大于零',f'forms.{row["id"]}.HP','error')
                    if root and row['server'].get('SallyReady')!=int(row['id']==root):need('sortie','根形态出击标记与隐藏形态不一致',None,'error')
                    for name in model.WEAPON_REFS:
                        weapon=row['server'].get(name)
                        if weapon and weapon not in newweapons and (weapon not in self.repository.catalog.get('weapons',{}) or self.baseline.record('weapon',weapon) is None):
                            need('weapon',f'{row["id"]} 的武器 {weapon} 缺少客户端或服务端定义',f'weapons.{weapon}',accept='.bin,.json')
                    for name in model.SKILL_REFS:
                        skill=row['server'].get(name)
                        if skill and skill not in self.repository.catalog.get('skill_ids',set()):
                            need('skill',f'{row["id"]} 引用的技能 {skill} 未在宿主登记；可补齐定义，或明确选择移除缺失技能后重新预览；本版不生成新技能行为',
                                 f'forms.{row["id"]}.{name}','error')
        for omission in draft.get('skill_omissions',[]):
            form=forms.get(omission['form_id'])
            if form and form['server'].get(omission['field'])==0:
                need('skill_omitted',f"已主动移除形态 {omission['form_id']} 的技能 {omission['skill_id']}；该技能效果不会生效，客户端与服务端同时清零",
                     f"forms.{omission['form_id']}.{omission['field']}",'warning')
        required={r['recipe_sprite'] for r in draft.get('blueprints',[]) if r.get('recipe_sprite')}
        for row in draft['forms']+draft['weapons']:required.update(row.get('resource_refs',[]))
        owned={f['path'].replace('\\','/').lower():f for f in draft['files']
               if f['path'] not in template_evidence_members}
        for path,entry in template_resource_entries.items():
            if path in owned and owned[path]['sha']!=entry['sha']:
                need('blueprint_template_resource_conflict',
                    '客户端展示模板资源与项目同名资源内容不同：'+path,
                    'blueprints.client_templates','error')
        owned.update(template_resource_entries)
        required.update(template_resource_entries)
        for name,entry in owned.items():
            if Path(name).suffix.lower()!='.bin':
                required.update(scan_resource_references(self.blob(entry['sha']),Path(name).suffix))
        for name in sorted(required):
            name=name.replace('\\','/').lower()
            if name not in owned and not self.baseline.exists(name):need('resource','缺少资源：'+name,name,accept=Path(name).suffix)
        for row in draft['forms']:
            if not row.get('model') or (row['model'].lower() not in owned and not self.baseline.exists(row['model'])):
                need('model','缺少核心模型：'+str(row.get('model')),None,'error')
            target=row.get('transform_target',0)
            if target and target not in forms and self.baseline.record('unit',target) is None:need('transform','缺少变形目标记录：'+str(target),None,'error')
        if draft.get('item_template_id') is None:need('item_template','请选择原生机体物品模板','item_template_id')
        else:
            item_record=self.baseline.record('item',draft['item_template_id'])
            if item_record is None:need('item_template_client','客户端缺少所选物品模板',None,'error')
            elif item_record[4]!=0:need('item_template_client','所选物品模板在当前客户端未启用，请改选列表中的已发布原生模板','item_template_id','error')
            item=self.repository.catalog.get('items',{}).get(draft['item_template_id'],{})
            if (item.get('TimeLimit'),item.get('Option1'),item.get('Option2'))!=(0,1,2):
                need('item_template_permanent','请选择永久机体物品模板；机体的租赁售价由商城选项另行配置','item_template_id')
        if root and draft.get('item_template_id') and draft.get('category_id') is None:
            need('item_identity','本地物品类别待分配，请保存草稿','category_id')
        for role,label in (('shop','商城'),('capsule','扭蛋')):
            sha=draft['presentation'][role].get('asset_sha')
            if sha is None:need('preview',f'请选择或上传{label}展示图片','presentation.'+role,accept='.png')
            elif not any(a['sha']==sha and a['kind']=='image' for a in draft['assets']):need('preview_invalid',label+'图片不属于当前草稿',None,'error')
            else:
                try:self.blob(sha)
                except (ValueError,OSError):need('preview_missing',label+'图片已丢失或被修改',None,'error')
        state=state or self.read()
        total=len(draft['forms'])+len(draft['weapons'])+2+2*len(draft.get('blueprints',[]))
        total+=sum(len(other['published']['forms'])+len(other['published']['weapons'])+2+
            2*len(other['published'].get('blueprints',[]))
            for other in state['drafts'].values()
            if other['id']!=draft['id'] and other.get('published'))
        if total>4096:need('catalog_capacity','已登记内容超过客户端目录的 4096 条记录上限',None,'error')
        ownids={r['id'] for r in draft['forms']+draft['weapons']}
        claimed={r['id']:other['id'] for other in state['drafts'].values() if other['id']!=draft['id'] and other['status'] in ('confirmed','disabled') for r in other['forms']+other['weapons']}
        for identity in ownids:
            if identity in claimed:need('id_owned',f'编号 {identity} 已由另一个已登记机体占用',None,'error')
            if self._any_id(identity):need('id_native',f'编号 {identity} 已被原生数据占用；此版本不盲目改号',None,'error')
        if not any(i['code'] in ('field','field_invalid','root','item_template','item_identity') for i in issues):
            try:self.repository.check_plan(self.plan(draft))
            except (ValueError,KeyError) as e:need('server_plan',str(e),None,'required',accept='.json')
        if self.publisher is None and (not self.module_binary or not self.module_binary.is_file()):
            need('runtime','缺少配套的 unit_catalog 客户端组件，不能发布',None,'error')
        for warning in draft.get('parse_warnings',[]):need('scan_note',warning,None,'warning')
        need('manual_acceptance','静态预览不等于实机验收；变形条件、同步和各武器行为仍需游戏内测试',None,'warning')
        return issues

    def public(self,selected=None):
        with self.lock:
            from .blueprints import NATIVE_REGISTRATION_SUPPORTED,RAW_TEMPLATE_NATIVE_REGISTRATION_SUPPORTED
            from .project import search_text
            state=self.read();result={'revision':model.revision(state),
                'capabilities':{'projects':{'protocol':'sdgo.unit-project','schema_version':1,'metadata_first':True,'bundled_media':True},
                    'native_programs':{'schema':'sdgo.unit-native-programs/1','authoring':True,'native_publication':False},
                    'blueprints':{'schema_version':1,'authoring':True,
                    'native_registration':NATIVE_REGISTRATION_SUPPORTED,'target_record_kind':33,
                    'target_record_size':1197,'material_minimum':3,'material_maximum':5,'raw_material_maximum':7,
                    'client_templates':{'schema':'sdgo.unit-blueprint-client-templates/1',
                        'schemas':['sdgo.unit-blueprint-client-templates/1','sdgo.unit-blueprint-client-templates/2'],
                        'prepared_sprite_texture_closure':True,
                        'recipe_kind':33,'recipe_bytes':1197,'item_kind':34,'item_bytes':614,
                        'source_prepared_sha_bound':True,'direct_resource_refs_verified':True,
                        'semantic_resource_closure_required':True,
                        'native_registration_schemas':['sdgo.unit-blueprint-client-templates/2'] if RAW_TEMPLATE_NATIVE_REGISTRATION_SUPPORTED else [],
                        'native_registration_authorized':RAW_TEMPLATE_NATIVE_REGISTRATION_SUPPORTED}}},'drafts':[{
                'id':d['id'],'name':d['name'],'status':d['status'],'updated':d['updated'],
                'search_text':search_text(d)}
                for d in state['drafts'].values()],
                'baseline':self.baseline.public_info(),'server':self.repository.public_info(),
                'confirmation_destination':'server' if self.repository.public_info().get('server_configured') else 'workspace',
                'templates':{'units':[{'id':n,'name':str(r.get('UnitModelName',n))} for n,r in self.repository.catalog.get('units',{}).items()],
                    'weapons':[{'id':n,'name':str(r.get('WeaponName',n))} for n,r in self.repository.catalog.get('weapons',{}).items()],
                    'blueprints':getattr(self.baseline,'blueprint_templates',lambda:[])(),
                    'blueprint_items':[{'id':n,'name':str(r.get('Name',n)).split('\0')[0],'recipe_id':r.get('RefIndex')}
                        for n,r in self.repository.catalog.get('items',{}).items() if n//100000==15 and r.get('TimeLimit')==0],
                    'items':[{'id':n,'name':str(r.get('Name',n)).split('\0')[0],'unit_id':r.get('RefIndex')} for n,r in self.repository.catalog.get('items',{}).items()
                        if n//100000==11 and (r.get('TimeLimit'),r.get('Option1'),r.get('Option2'))==(0,1,2)
                        and (template:=self.baseline.record('item',n)) is not None and template[4]==0]}}
            if selected is None and state['drafts']:selected=next(reversed(state['drafts']))
            if selected:
                draft=copy.deepcopy(self._get(state,selected));draft['content_revision']=model.content_revision(draft)
                draft['requirements']=self.requirements(draft,state);draft['can_confirm']=not any(i['level']!='warning' for i in draft['requirements'])
                draft['removable_missing_skills']=self.removable_missing_skills(draft)
                for key,kind in (('forms','unit'),('weapons','weapon')):
                    for row in draft[key]:
                        row['server_fields']=[{**field,'value':row['server'].get(field['name']),'source':row['sources'].get(field['name'],'missing')} for field in model.schema(kind)]
                draft['previews']={role:'/api/units/assets/'+value['asset_sha'] if value.get('asset_sha') else None for role,value in draft['presentation'].items()}
                external={r['server'].get(name) for r in draft['forms'] for name in model.WEAPON_REFS}-{r['id'] for r in draft['weapons']}-{0,None}
                draft['summary']={'files':len(draft['files']),'forms':len(draft['forms']),'weapons':len(draft['weapons']),
                    'external_weapons':sorted(external),'resources':sum(not f['path'].lower().endswith('.bin') for f in draft['files']),
                    'native_program_groups':len(draft.get('native_programs',[]))}
                result['draft']=draft
            return result

    def disable(self,identity,expected):
        with self.lock:
            state=self.check(expected);draft=self._get(state,identity)
            draft['status']='disabled';draft['updated']=now()
            return self._commit(state,identity)

    def confirm(self,identity,value,expected):
        from .compiler import publish
        with self.lock:
            state=self.check(expected);draft=self._get(state,identity);revision=model.content_revision(draft)
            if value.get('preview_revision')!=revision or value.get('shop') is not True or value.get('capsule') is not True:
                raise ValueError('请重新查看当前版本的商城和扭蛋预览并逐项确认')
            problems=[r for r in self.requirements(draft,state) if r['level']!='warning']
            if problems:raise ValueError('仍有必填或关键缺项：'+problems[0]['message'])
            candidate=copy.deepcopy(state);entry=candidate['drafts'][identity]
            entry['status']='confirmed';entry['updated']=now();entry['confirmation']={'content_revision':revision,'shop':True,'capsule':True,'at':now()}
            if value.get('_native_ui'):entry['confirmation']['native_ui']=copy.deepcopy(value['_native_ui'])
            if value.get('_presentation_review'):entry['confirmation']['presentation_review']=copy.deepcopy(value['_presentation_review'])
            entry['published']={k:copy.deepcopy(v) for k,v in entry.items() if k not in ('published','release','server_result','published_server_result')}
            transaction=self.root/'transactions'/uuid.uuid4().hex;transaction.mkdir(parents=True)
            publication=(self.publisher or publish)(self,candidate,identity,transaction)
            entry['release']=publication
            pending={'stage':'prepared','before_revision':model.revision(state),'state':candidate,'family_id':identity}
            save_json(transaction/'pending.json',pending)
            try:
                if self.repository.public_info().get('server_configured'):
                    entry['server_result']=self.repository.apply(self.plan(entry))
                else:entry['server_result']={'applied':False,'mode':'offline','message':'已登记到本模块目录并生成发布物；尚未写入游戏服务端'}
                entry['published_server_result']=copy.deepcopy(entry['server_result'])
                pending['stage']='applied';pending['state']=candidate;save_json(transaction/'pending.json',pending)
                response=self._commit(candidate,identity)
                pending['stage']='published';save_json(transaction/'pending.json',pending)
                return response
            except Exception:
                # A committed SQL apply is preserved in pending.json for startup recovery.
                # A rejected SQL transaction never turns a draft into confirmed content.
                raise

    def export(self,identity):
        with self.lock:
            draft=self._get(self.read(),identity)
            if draft['status']!='confirmed' or draft.get('confirmation',{}).get('content_revision')!=model.content_revision(draft):
                raise ValueError('只有已确认的当前版本可以导出')
            path=Path(draft['release']['client_zip']).resolve()
            if not path.is_relative_to(self.root/'transactions'):raise ValueError('发布物路径无效')
            raw=path.read_bytes()
            if hashlib.sha256(raw).hexdigest()!=draft['release']['client_zip_sha256']:raise ValueError('已发布文件被修改')
            return raw
