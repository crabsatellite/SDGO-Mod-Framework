"""Build paired default-preset data without modifying an owner catalog or SQL."""
from __future__ import annotations
import copy
import hashlib
import io
import json
from pathlib import Path
import struct
import zipfile
from . import compiler,model,preset_recipe,skill_atlases,stages,acquisition_sources
from .assets import sprite_binary,_image_to_png
from .blueprint_resources import sprite_frames
from .indexes import catalog_records
from owner.native_archive import build_archive

def sha(raw):return hashlib.sha256(raw).hexdigest()
def json_bytes(value):return json.dumps(value,ensure_ascii=False,sort_keys=True,indent=2).encode('utf-8')

class _Context:
    index_scope='presets'
    def __init__(self,root,baseline,repository,binary):
        self.root=Path(root);self.baseline=baseline;self.repository=repository;self.module_binary=Path(binary);self.objects={}
    def store(self,raw):
        digest=sha(raw);self.objects[digest]=raw;return digest
    def blob(self,digest):return self.objects[digest]
    def plan(self,family):
        template=self.repository.catalog['items'][family['item_template_id']]
        item={**template,'CatalogueID':family['category_id'],'RefIndex':family['root_unit_id'],'Name':'Demo_'+str(family['root_unit_id'])}
        return {'family_key':family['family_key'],'revision':model.content_revision(family),'root_unit_id':family['root_unit_id'],
            'item_template_id':family['item_template_id'],'behavior_profile':family['behavior_profile'],
            'units':[r['server'] for r in family['forms']],'weapons':[r['server'] for r in family['weapons']],'item':item,
            'names':[{'UnitNumber':r['id'],'NameCN':family['name'] if r['id']==family['root_unit_id'] else r['name'],
                      'NameEN':r['server']['UnitModelName']} for r in family['forms']]}

def build(output,baseline,repository,module_binary,recipe=None):
    """An immutable candidate publication. Apply/install remain separate gates."""
    recipe=recipe or preset_recipe.load();output=Path(output);output.mkdir(parents=True,exist_ok=False)
    composed=preset_recipe.compose(recipe,baseline,repository.catalog)
    context=_Context(output,baseline,repository,module_binary);state={'drafts':{}}
    for family in composed['families']:
        entry=family['recipe'];identity=entry['key'];forms=[];weapons=[];files=[]
        from .codec import decode_unit,decode_weapon
        for kind,rows,target,decoder in [(2,family['units'],forms,decode_unit),(10,family['weapons'],weapons,decode_weapon)]:
            for row in rows:
                key='UnitNumber' if kind==2 else 'WeaponNo';raw=family['records'][kind,row[key]];record=model.new_record(decoder(raw),'unit' if kind==2 else 'weapon')
                record['server']=copy.deepcopy(row);context.store(raw);target.append(record)
                files.append({'path':f'gdrs/demo_{row[key]}.bin','sha':sha(raw),'bytes':len(raw)})
        draft={'id':identity,'family_key':entry['family_key'],'name':entry['name'],'status':'builtin',
            'root_unit_id':entry['root_unit_id'],'behavior_profile':entry['profile'],'category_id':entry['item_category_id'],
            'item_template_id':entry['item_template_id'],'forms':forms,'weapons':weapons,'files':files,'assets':[],
            'presentation':{},'blueprints':[],
            'item_text':{'description':entry['description'][:95],'usage':'通用演示机；获取途径由服主另行配置。'},
            'metadata':{'description':entry['description'],'gameplay_notes':entry['gameplay_notes'],'visibility':'private',
                        'tags':['通用演示预设'],'series':'原生换装试验计划'}}
        root=next(r for r in forms if r['id']==entry['root_unit_id'])
        for role,field in [('shop','portrait_shop'),('capsule','portrait_capsule')]:
            png=baseline.portrait(root['client'][field]);digest=context.store(png)
            if all(a['sha']!=digest for a in draft['assets']):
                draft['assets'].append({'sha':digest,'kind':'image','source':'baseline','filename':role+'.png','bytes':len(png)})
            draft['presentation'][role]={'asset_sha':digest,'source_mode':'native','caption':'由服主配置获取途径'}
        draft['published']=copy.deepcopy(draft);state['drafts'][identity]=draft
    # This composer changes record bytes/revision below and emits its own final
    # graph requests. Never retain an editor graph bound to the intermediate catalog.
    core=output/'base';core.mkdir();compiled=compiler.publish(context,state,next(iter(state['drafts'])),core,include_server_graphs=False)
    with zipfile.ZipFile(compiled['client_zip']) as archive:files={name:archive.read(name) for name in archive.namelist()}
    prefix='client/sdgo/mods/unit_catalog/';records=catalog_records(files[prefix+'catalog.bin'])
    all_records={(kind,identity):raw for identity,(kind,raw) in records.items()}
    resources={name[7:]:raw for name,raw in files.items() if name.startswith('client/') and not name.startswith(prefix)}
    # Each icon receives its own independent native sprite/texture, retaining
    # the exact original selected frame without a global atlas replacement.
    source_frames=sprite_frames(baseline.read('sprs/0594.spr'))['frames'];atlas_entries=[];sprites=[]
    for identity,source in composed['skill_records'].items():
        raw=bytearray(source);frame=source_frames[struct.unpack_from('<H',raw,426)[0]]
        png=_image_to_png(baseline.read(frame['texture']),require_png=False,crop=tuple(frame['uv']))
        from PIL import Image
        with Image.open(io.BytesIO(png)) as image:width,height=image.size
        texture=f'txrs/demoskill{identity}.txr';sprite=f'sprs/demoskill{identity}.spr'
        resources[texture]=png;resources[sprite]=sprite_binary(texture,width,height)
        struct.pack_into('<H',raw,426,0);all_records[13,identity]=bytes(raw)
        atlas_entries.append({'id':identity,'record_sha256':sha(raw),'sprite':sprite})
        sprites.append({'path':sprite,'sha256':sha(resources[sprite]),'textures':[{'path':texture,'sha256':sha(png)}]})
    catalog=compiler.native_catalog([(kind,identity,raw) for (kind,identity),raw in all_records.items()])
    files[prefix+'catalog.bin']=catalog
    zdx,zpk,_=build_archive(resources,lambda path:path.split('/',1)[0] in {'mdrs','anrs','txrs','fxrs','sprs','sdrs','scp'})
    files[prefix+'resources.zdx']=zdx;files[prefix+'resources.zpk']=zpk
    files[prefix+'resource-overlay.ini']=('[runtime]\nschema=1\nindex=resources.zdx\nindex_sha256='+sha(zdx)+'\narchive=resources.zpk\narchive_sha256='+sha(zpk)+'\n').encode()
    # A rule or description change is a new publication even when it leaves
    # every native record and image unchanged.
    revision=sha(catalog+zdx+zpk+json_bytes(recipe))
    skill_records={identity:raw for (kind,identity),raw in all_records.items() if kind==13}
    atlas={'schema':'sdgo.unit-skill-atlases/1','owner':recipe['owner'],'revision':revision,'sprites':sprites,'skills':atlas_entries}
    files[prefix+'skill-atlases.bin']=skill_atlases.compile_bundle([atlas],sha(catalog),sha(zdx),sha(zpk),skill_records,resources)
    files[prefix+'stage-rules.bin']=stages.encode(composed['stage_rules'],catalog,all_records)
    availability={'schema_version':1,'provider':'unit_catalog','revision':revision,'blueprints':{}}
    files[prefix+'blueprint-availability.json']=json_bytes(availability)
    files[prefix+'blueprint-availability.bin']=acquisition_sources.encode_availability(availability,struct.unpack_from('<I',catalog,24)[0])
    plans=json.loads(Path(compiled['server_plan']).read_bytes())['families']
    families=[{key:value for key,value in plan.items() if key!='revision'} for plan in plans]
    server_skills={'schema':'sdgo.unit-server-dependencies/1','owner':recipe['owner'],'revision':revision,
        'skills':[{'id':identity,'client_sha256':sha(skill_records[identity]),
                   'definition':{'kind':'explicit','origin':'authored','values':row}} for identity,row in composed['skill_rows'].items()]}
    publication=output/'publication';package=publication/'package';package.mkdir(parents=True)
    plan={'schema':'sdgo.unit-roster-server-plan/1','owner':recipe['owner'],'revision':revision,'families':families,
        'skills':server_skills,'funnels':{'schema':'sdgo.unit-server-funnels/1','owner':recipe['owner'],'revision':revision,'funnels':[]},'affiliations':[]}
    requests=[{'schema':'sdgo.unit-form-graphs/1','owner':family['family_key'],'families':[family['family_key']],
        'revision':revision,'roots':[family['root_unit_id']],
        'records':[{'id':row['UnitNumber'],'client_sha256':sha(all_records[2,row['UnitNumber']])} for row in family['units']]} for family in families]
    (publication/'server-plan.json').write_bytes(json_bytes(plan))
    (publication/'form-graph-requests.json').write_bytes(json_bytes({'schema':'sdgo.unit-roster-form-graphs/1','owner':recipe['owner'],'revision':revision,'requests':requests}))
    for name in ('registrations.json',prefix+'definitions.json'):
        value=json.loads(files[name]);value['revision']=revision;files[name]=json_bytes(value)
    (publication/'registrations.json').write_bytes(files['registrations.json'])
    for path,raw in resources.items():files['client/'+path]=raw
    manifest=json.loads(files['manifest.json']);manifest['revision']=revision;manifest['owned_paths']=sorted(resources)
    manifest['resource_overlay']['paths']=sorted(resources);manifest['files']={name:sha(raw) for name,raw in files.items() if name!='manifest.json'}
    files['manifest.json']=json_bytes(manifest)
    for name,raw in files.items():
        target=package/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(raw)
    (publication/'preset-recipe.json').write_bytes(json_bytes(recipe))
    from .batch_server import load_publication
    ready=load_publication(publication)
    return {'publication':str(publication),'revision':revision,'summary':ready.summary(),'client_data_directory':str(package/prefix),
            'server_applied':False,'client_installed':False,'acquisition_configured':False}
