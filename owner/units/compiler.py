"""Compile approved source records and presentation into a paired publication."""
from __future__ import annotations

import configparser
import copy
import hashlib
import io
import json
from pathlib import Path
import struct
import zipfile

from . import model

ROOT=Path(__file__).resolve().parents[2]
HOST='GOnline-54BE1568-00C97000'
HOST_SHA256='e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92'
UNIT_PATCHES={'UnitGradeNumber':(308,'B'),'UnitGradeNumber2':(309,'B'),'MasteredWeaponType':(310,'B'),
    'BattleCost':(311,'H'),'Star':(313,'B'),'UnitNumber_Transform':(316,'I'),'HP':(321,'I'),
    'Shield':(325,'I'),'ShieldType':(329,'B'),'ShieldRate':(330,'I'),
    'WeaponNumber_1':(585,'I'),'WeaponNumber_2':(589,'I'),'WeaponNumber_3':(593,'I'),'WeaponNumber_FatalAttack':(597,'I')}


def fnv(raw):
    value=2166136261
    for byte in raw:value=((value^byte)*16777619)&0xffffffff
    return value


def native_catalog(records):
    seen=set();parts=[]
    for kind,identity,raw in sorted(records,key=lambda r:r[1]):
        if kind not in {2,10,33,34,85} or len(raw)!={2:4976,10:1871,33:1197,34:614,85:614}[kind]:raise ValueError('不支持的原生记录类型或长度')
        if type(identity) is not int or identity<=0 or identity in seen or struct.unpack_from('<I',raw)[0]!=identity or raw[4]!=0:
            raise ValueError('原生记录身份、发布标记或全局编号冲突')
        if kind==33:
            from .blueprint_codec import decode
            decode(raw)
        seen.add(identity);parts.append(struct.pack('<6I',kind,identity,len(raw),fnv(raw),0,0)+raw)
    if not seen:raise ValueError('没有可以发布的机体记录')
    payload=b''.join(parts)
    if len(parts)>4096 or len(payload)>64*1024*1024:raise ValueError('发布目录超过原生适配器的容量限制')
    return struct.pack('<8s6I',b'SDGOUN1\0',1,32,len(parts),len(payload),fnv(payload),0)+payload


def wchar(raw,offset,capacity,value):
    data=value.encode('utf-16-le')
    if len(data)>=capacity*2:raise ValueError('展示名称或资源路径超过原生字段容量')
    raw[offset:offset+capacity*2]=data.ljust(capacity*2,b'\0')


def _png_dimensions(raw):
    if raw[:8]!=b'\x89PNG\r\n\x1a\n' or len(raw)<24:raise ValueError('展示图片不是 PNG')
    return struct.unpack_from('>II',raw,16)


def _write_zip(path,files):
    with zipfile.ZipFile(path,'x',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for name,raw in sorted(files.items()):z.writestr(name,raw)
    with zipfile.ZipFile(path) as z:
        if z.testzip() is not None:raise ValueError('发布包校验失败')
        if set(z.namelist())!=set(files):raise ValueError('发布清单不完整')
        for name,raw in files.items():
            if z.read(name)!=raw:raise ValueError('发布包内容不匹配')


def _adapter_files(workspace):
    if not workspace.module_binary or not workspace.module_binary.is_file():raise ValueError('缺少 unit_catalog 客户端组件')
    binary=workspace.module_binary.read_bytes()
    if len(binary)<256 or binary[:2]!=b'MZ':raise ValueError('客户端组件不是受支持的原生 DLL')
    pe=struct.unpack_from('<I',binary,0x3c)[0]
    if pe+24>len(binary) or binary[pe:pe+4]!=b'PE\0\0' or struct.unpack_from('<H',binary,pe+4)[0]!=0x14c:
        raise ValueError('客户端组件必须为 Win32 DLL')
    module=ROOT/'mods/unit_catalog';manifest=(module/'mod.ini').read_bytes()
    return binary,manifest,(module/'symbols.fragment.ini').read_bytes(),(ROOT/'tools/install_unit_catalog.ps1').read_bytes()


def publish(workspace,state,selected,transaction):
    from .assets import sprite_binary
    from .codec import patch_weapon,patch_unit,UNIT_FIELDS
    from owner.native_archive import build_archive
    binary,mod_manifest,symbols,installer=_adapter_files(workspace)
    records=[];assets={};registrations=[];plans=[];published=[]
    index_rows=workspace.baseline.unit_index()
    next_index=0x375BE1+len(index_rows)
    def asset(name,raw):
        name=name.replace('\\','/').lower()
        if name in assets and assets[name]!=raw:raise ValueError('不同机体使用了内容不同的相同资源路径：'+name)
        if workspace.baseline.exists(name) and workspace.baseline.read(name)!=raw:
            owned=getattr(workspace.baseline,'owns_unchanged_resource',lambda _:False)(name)
            if not owned:raise ValueError('包内资源会覆盖原版或外部修改的文件：'+name+'；请使用独立资源名称')
        assets[name]=raw
    for draft in state['drafts'].values():
        # Published snapshots exclude host release paths and SQL result metadata.
        # They also preserve the last active record while an update is a draft.
        family=draft.get('published')
        if not family:continue
        family=copy.deepcopy(family);published.append(family)
        root=family['root_unit_id'];plan=workspace.plan(family);plans.append(plan)
        for entry in family['files']:
            path=entry['path'].replace('\\','/').lower()
            if path.startswith(('mdrs/','anrs/','txrs/','fxrs/','sprs/','sdrs/','scp/')):
                asset(path,workspace.blob(entry['sha']))
        from .presentation import publication_images,contract
        generated=publication_images(workspace,family)
        display={}
        for role,letter in (('unit_primary','p'),('shop','s'),('custom','i'),('capsule_fixed','c'),
                            ('index_owned','o'),('index_missing','g')):
            png=generated[role]
            if png is None:continue
            width,height=_png_dimensions(png)
            texture=f'txrs/u{root}{letter}.txr';sprite=f'sprs/u{root}{letter}.spr'
            asset(texture,png);asset(sprite,sprite_binary(texture.replace('/','\\'),width,height));display[role]=sprite
        display['capsule']=display.get('capsule_fixed',display['custom'])
        for row in family['forms']:
            source=workspace.blob(row['source_sha'])
            shared={key:value for key,value in row['server'].items() if key in UNIT_FIELDS and UNIT_FIELDS[key]['type'] not in ('utf16le',)}
            raw=bytearray(patch_unit(source,shared))
            if len(raw)!=4976:raise ValueError('机体原始记录已改变')
            wchar(raw,612,30,row['model'].replace('/','\\'))
            for offset in (4654,4754,4854):
                original=raw[offset:offset+100].decode('utf-16-le').split('\0')[0]
                wchar(raw,offset,50,original.replace('/','\\'))
            # Only this adapter's verified shared fields can be written back.
            # Native movement and skill encodings are retained; no byte-offset guesses.
            if row['id']==root:
                for offset in (5,105,205):wchar(raw,offset,50,family['name'])
                wchar(raw,4654,50,display['unit_primary'].replace('/','\\'))
                wchar(raw,4754,50,display['shop'].replace('/','\\'))
                # Custom capsule candidates use this distinct native portrait;
                # the original UI derives the rank border from unit grade.
                wchar(raw,4854,50,display['custom'].replace('/','\\'))
            records.append((2,row['id'],bytes(raw)))
        for row in family['weapons']:
            raw=workspace.blob(row['source_sha'])
            if len(raw)!=1871:raise ValueError('武器原始记录已改变')
            # Keep the package's localized client name; server varchar internal
            # names are separate. Combat properties use the same verified codec.
            raw=patch_weapon(raw,{key:value for key,value in row['server'].items() if key!='WeaponName'})
            records.append((10,row['id'],raw))
        template=workspace.baseline.record('item',family['item_template_id'])
        if not template or len(template)!=614 or template[4]!=0:raise ValueError('客户端缺少已发布的原生物品类别模板')
        item=bytearray(template);struct.pack_into('<I',item,0,family['category_id']);wchar(item,5,50,family['name'])
        struct.pack_into('<I',item,105,root);records.append((34,family['category_id'],bytes(item)))
        if workspace.baseline.any_id(next_index):
            raise ValueError('机体图鉴连续编号被其他原生记录占用，拒绝覆盖或跳号')
        index_id=next_index;next_index+=1
        index=bytearray(614)
        struct.pack_into('<I',index,0,index_id)
        index[5]=int(state['drafts'][family['id']]['status']!='disabled')
        struct.pack_into('<I',index,6,root)
        wchar(index,14,50,display['index_owned'].replace('/','\\'))
        wchar(index,114,50,display['index_missing'].replace('/','\\'))
        index_position=len(records)
        records.append((85,index_id,bytes(index)))
        if family.get('blueprints'):
            from . import blueprints,blueprint_codec
            for declaration in family['blueprints']:
                template=workspace.baseline.record('blueprint',declaration['recipe_template_id'])
                if template is None:raise ValueError('客户端缺少所选设计图模板')
                compiling=copy.deepcopy(declaration);compiling['enabled']=True
                raw=blueprint_codec.patch(template,compiling,root_unit_id=root)
                records.append((33,declaration['id'],raw))
                template_item=workspace.baseline.record('item',declaration['item_template_id'])
                if not template_item or len(template_item)!=614 or template_item[4]!=0:
                    raise ValueError('客户端缺少启用的原生图纸物品模板')
                if struct.unpack_from('<I',template_item)[0]//100000!=15:raise ValueError('图纸模板不是原生15类物品')
                item=bytearray(template_item)
                struct.pack_into('<I',item,0,declaration['item_category_id'])
                wchar(item,5,50,declaration['name'])
                struct.pack_into('<I',item,105,declaration['id'])
                records.append((34,declaration['item_category_id'],bytes(item)))
            primary=blueprints.primary_id(family['blueprints'])
            if primary:struct.pack_into('<I',index,10,primary)
            # Replace only this root's pending index bytes, not a native table.
            records[index_position]=(85,index_id,bytes(index))
        registrations.append({'key':'unit:'+str(root),'provider':'unit_catalog','kind':'unit','unit_id':root,
            'category_id':family['category_id'],'family_key':family['family_key'],'revision':model.content_revision(family),
            'unit_index_id':index_id,
            'index_owned_sprite':display['index_owned'],'index_missing_sprite':display['index_missing'],
            'display_name':family['name'],'forms':[r['id'] for r in family['forms']],
            'shop_sprite':display['index_owned'],'secret_store_sprite':display['shop'],'capsule_sprite':display['capsule'],
            'unit_primary_sprite':display['unit_primary'],'custom_candidate_sprite':display['custom'],
            'capsule_fixed_sprite':display.get('capsule_fixed'),
            'presentation':contract(workspace,family),
            'metadata':copy.deepcopy(family.get('metadata',{})),
            'primary_blueprint_id':blueprints.primary_id(family['blueprints']) if family.get('blueprints') else 0,
            'blueprints':[{'id':r['id'],'item_category_id':r['item_category_id'],'enabled':r['enabled']} for r in family.get('blueprints',[])],
            'shop_image_sha256':hashlib.sha256(generated['index_owned']).hexdigest(),
            'shop_source_sha256':family['presentation']['shop']['asset_sha'],'capsule_image_sha256':family['presentation']['capsule']['asset_sha'],
            'enabled':state['drafts'][family['id']]['status']!='disabled','storefront_changes':False,'gacha_changes':False})
    catalog=native_catalog(records)
    content_revision=model.revision({'families':published,'record_hash':hashlib.sha256(catalog).hexdigest()})
    prefix='client/sdgo/mods/unit_catalog/'
    overlay_extensions={'mdrs':'.mod','anrs':'.ani','txrs':'.txr','fxrs':'.zfx',
                        'sprs':'.spr','sdrs':'.zsd','scp':'.scp'}
    def allowed_overlay(name):
        return name.split('/',1)[0] in overlay_extensions and name.endswith(overlay_extensions[name.split('/',1)[0]])
    zdx,zpk,_=build_archive(assets,allowed_overlay)
    overlay_config=('[runtime]\nschema=1\nindex=resources.zdx\nindex_sha256='+hashlib.sha256(zdx).hexdigest()+
                    '\narchive=resources.zpk\narchive_sha256='+hashlib.sha256(zpk).hexdigest()+'\n').encode('ascii')
    # An owner-generated publication enables only its own module, not global switches.
    text=mod_manifest.decode('utf-8-sig')
    import re
    text=re.sub(r'(?m)^enabled\s*=.*$','enabled=1',text)
    text=re.sub(r'(?m)^files\s*=.*$','files=symbols.fragment.ini,catalog.bin,definitions.json,resource-overlay.ini,resources.zdx,resources.zpk',text)
    files={prefix+'unit_catalog.dll':binary,prefix+'mod.ini':text.encode(),prefix+'catalog.bin':catalog,
        prefix+'manager.json':(ROOT/'mods/unit_catalog/manager.json').read_bytes(),
        prefix+'symbols.fragment.ini':symbols,
        prefix+'resource-overlay.ini':overlay_config,prefix+'resources.zdx':zdx,prefix+'resources.zpk':zpk,
        prefix+'definitions.json':json.dumps({'protocol':'sdgo.unit-family','schema_version':1,'revision':content_revision,
            'families':published,'registrations':registrations},ensure_ascii=False,indent=2).encode(),
        'symbols.fragment.ini':symbols,'Install-UnitCatalog.ps1':installer,
        'registrations.json':json.dumps({'schema_version':1,'provider':'unit_catalog','revision':content_revision,'items':registrations},ensure_ascii=False,indent=2).encode(),
        'README.zh-CN.txt':('关闭游戏，运行 Install-UnitCatalog.ps1 -ClientRoot <客户端目录>。之后使用原启动器。\n'
            '此包登记机体和展示资源，不自动给玩家发机体、不自动添加商城商品或扭蛋条目。\n'
            '商城/扭蛋模块使用 registrations.json 中的同版原生图片路径。\n'
            '本包的静态图片预览不是实际游戏内截图。\n').encode()}
    for path,raw in assets.items():files['client/'+path]=raw
    manifest={'module':'unit_catalog','schema_version':1,'revision':content_revision,'host':HOST,'host_sha256':HOST_SHA256,
        'files':{name:hashlib.sha256(raw).hexdigest() for name,raw in files.items()},
        'owned_paths':sorted(assets),
        'resource_overlay':{'schema':1,'config':prefix+'resource-overlay.ini','index':prefix+'resources.zdx',
                            'archive':prefix+'resources.zpk','paths':sorted(assets)},
        'restart_required':True,'storefront_changes':False,'gacha_changes':False}
    files['manifest.json']=json.dumps(manifest,ensure_ascii=False,indent=2).encode()
    client_zip=transaction/'SDGO-UnitImport-Client.zip';_write_zip(client_zip,files)
    server_plan=transaction/'server-plan.json'
    server_plan.write_bytes(json.dumps({'schema_version':1,'revision':content_revision,'families':plans,'credentials_included':False},ensure_ascii=False,indent=2).encode())
    return {'revision':content_revision,'client_zip':str(client_zip),'client_zip_sha256':hashlib.sha256(client_zip.read_bytes()).hexdigest(),
        'server_plan':str(server_plan),'families':len(published),'records':len(records),'assets':len(assets),'restart_required':True}
