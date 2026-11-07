"""Synthetic protocol/resource fixtures; real source packs remain local."""
import base64,copy,hashlib,io,json,struct,tempfile,unittest,zipfile
from pathlib import Path
from PIL import Image
from manager.engines.operators import assets,legacy_pack,model,native_resources
from manager.engines.operators.workspace import Workspace,_installed_upload

def gdt(rows):
    size=len(rows[0]);extra=8;n=(size+len(rows)+extra)%65+192
    key=bytes(range(n));stride=((size+n-1)//n)*n
    out=bytearray(b'\xf9\xf8'+struct.pack('<III',size,len(rows),extra)+key)
    for row in rows:
        padded=row+bytes([0x5A])*(stride-size)
        for start in range(0,stride,n):
            block=bytearray(n)
            for j in range(n):block[key[j]]=padded[start+j]^key[(768+3*j)%n]
            out+=block
    return bytes(out)

def png():
    out=io.BytesIO();Image.new('RGBA',(16,16),(240,30,50,255)).save(out,'PNG');return out.getvalue()

def pattern_png():
    image=Image.new('RGBA',(4,4))
    image.putdata([
        (x*50,y*50,x+y,255)
        for y in range(image.height) for x in range(image.width)
    ])
    out=io.BytesIO();image.save(out,'PNG');return out.getvalue()

def one_frame_sprite(uv):
    out=bytearray(struct.pack('<III',11000,1,20))
    row=bytearray(172);struct.pack_into('<II4f',row,0,4,4,*uv)
    assets.put_wchar(row,40,assets.SPR_TEXTURE_WCHARS,'txrs\\0001.txr')
    assets.put_wchar(row,104,32,'independent author')
    row[168:172]=b'TAIL';out+=row
    return bytes(out)

def two_frame_sprite():
    out=bytearray(struct.pack('<III',11000,2,20))
    for index,uv in enumerate(((0,0,1,1),(1.002,.1,.8,.8))):
        row=bytearray(172);struct.pack_into('<II4f',row,0,16,16,*uv)
        assets.put_wchar(row,40,assets.SPR_TEXTURE_WCHARS,'txrs\\0001.txr')
        assets.put_wchar(row,104,32,f'frame author {index}')
        row[168:172]=bytes((0xA0+index,0xB0+index,0xC0+index,0xD0+index))
        out+=row
    return bytes(out)

def layout_sprite(version):
    stride,path_offset=(164,32) if version==10000 else (172,40)
    out=bytearray(struct.pack('<III',version,1,20));row=bytearray(stride)
    struct.pack_into('<II4f',row,0,16,16,0,0,1,1)
    assets.put_wchar(row,path_offset,assets.SPR_TEXTURE_WCHARS,'txrs\\0001.txr')
    assets.put_wchar(row,path_offset+64,32,'independent second string')
    row[-4:]=b'TAIL';out+=row
    return bytes(out),path_offset,stride

def fixture_files():
    row=bytearray(710);assets.put_wchar(row,0,50,'battle.start')
    assets.put_wchar(row,100,200,'战斗开始');row[501]=1
    assets.put_wchar(row,502,50,'sdrs\\1001.zsd');assets.put_wchar(row,602,50,'sdrs\\1002.zsd')
    row[702:710]=b'layout!!'
    return {'gdrs/0004.gdt':gdt([bytes(row)]),'sprs/1000.spr':two_frame_sprite(),
            'txrs/0001.txr':png(),'sdrs/1001.zsd':b'ID3synthetic-one','sdrs/1002.zsd':b'ID3synthetic-two'}

def archive(files):
    out=io.BytesIO()
    with zipfile.ZipFile(out,'w') as z:
        for key,value in files.items():z.writestr(key,value)
    return out.getvalue()

class Base:
    index_hash='fixture'
    def __init__(self,root=None):
        self.root=Path(root or '.');self.files=fixture_files();self.entries={k:None for k in self.files}
        self.row=bytearray(1671);struct.pack_into('<I',self.row,0,60001)
        assets.put_wchar(self.row,5,50,'模板');assets.put_wchar(self.row,105,50,'gdrs\\0004.gdt')
        for offset,capacity in assets.SPRITE_FIELDS:assets.put_wchar(self.row,offset,capacity,'sprs\\1000.spr')
    def records(self,name,size):return {60001:bytes(self.row)}
    def read(self,name):return self.files[name.replace('\\','/').lower()]
    def portrait(self,identity):return png()

class ResourceProtocol(unittest.TestCase):
    def test_gdt_codec_preserves_padding_and_both_voice_variants(self):
        raw=fixture_files()['gdrs/0004.gdt'];table=legacy_pack.Gdt(raw,710)
        self.assertEqual(table.encode(),raw)
        self.assertEqual(assets.wchar(table.rows[0],502,50),'sdrs\\1001.zsd')
        self.assertEqual(assets.wchar(table.rows[0],602,50),'sdrs\\1002.zsd')

    def test_private_namespace_retargets_all_frames_and_both_voices(self):
        original=fixture_files();p=legacy_pack.Pack(archive(original),Base())
        self.assertEqual(len(p.voice_events),2)
        files,profile=p.compile(60047,60001)
        self.assertTrue(all(
            k.startswith('sdgo/mods/operator_catalog/assets/60047/')
            for k in files if not k.endswith('.txr')
        ))
        self.assertEqual(
            [k for k in files if k.endswith('.txr')],
            ['sdgo/optex/60047/4.txr'],
        )
        table=legacy_pack.Gdt(next(v for k,v in files.items() if k.endswith('.gdt')),710)
        first=assets.wchar(table.rows[0],502,50);second=assets.wchar(table.rows[0],602,50)
        self.assertNotEqual(first,second);self.assertTrue(first.startswith('sdgo\\mods\\operator_catalog'))
        self.assertTrue(second.startswith('sdgo\\mods\\operator_catalog'))
        self.assertEqual(table.rows[0][702:710],b'layout!!')
        spr=next(v for k,v in files.items() if k.endswith('.spr'));_,frames=legacy_pack.sprite(spr,allow_compiled=True)
        self.assertEqual(len(frames),2);self.assertGreater(frames[1]['uv'][0],1)
        self.assertTrue(all(f['texture'].startswith('sdgo/optex/') for f in frames))
        source=original['sprs/1000.spr']
        for index in range(2):
            start=12+index*172
            self.assertEqual(spr[start+104:start+172],source[start+104:start+172])
        self.assertEqual(original,fixture_files())

    def test_both_spr_versions_rewrite_only_the_32_wchar_texture_field(self):
        for version in (10000,11000):
            with self.subTest(version=version):
                source,path_offset,stride=layout_sprite(version)
                files=fixture_files();files['sprs/1000.spr']=source
                generated,_profile=legacy_pack.Pack(
                    archive(files),Base()
                ).compile(60047,60001)
                result=next(
                    raw for path,raw in generated.items() if path.endswith('.spr')
                )
                _raw,frames=legacy_pack.sprite(result,allow_compiled=True)
                self.assertEqual(frames[0]['texture'],'sdgo/optex/60047/4.txr')
                preserved=12+path_offset+assets.SPR_TEXTURE_WCHARS*2
                self.assertEqual(result[preserved:12+stride],source[preserved:12+stride])

    def test_portrait_supports_normal_flipped_and_wrapped_native_uv(self):
        cases=[
            ((.25,.25,.75,.75),[1,2],[1,2]),
            ((.75,.75,.25,.25),[2,1],[2,1]),
            ((.75,-.5,1.25,.5),[3,0],[2,3,0,1]),
            ((1.25,.5,.75,-.5),[0,3],[1,0,3,2]),
            ((-1,0,0,1),[0,1,2,3],[0,1,2,3]),
            ((2,1,1,0),[3,2,1,0],[3,2,1,0]),
        ]
        source=Image.open(io.BytesIO(pattern_png())).convert('RGBA')
        for uv,xs,ys in cases:
            with self.subTest(uv=uv):
                files=fixture_files()
                files['sprs/1000.spr']=one_frame_sprite(uv)
                files['txrs/0001.txr']=pattern_png()
                original_sprite=files['sprs/1000.spr']
                original_texture=files['txrs/0001.txr']
                pack=legacy_pack.Pack(archive(files),Base())
                with Image.open(io.BytesIO(pack.portrait())) as rendered:
                    rendered.load()
                    self.assertEqual(rendered.size,(len(xs),len(ys)))
                    expected=[
                        source.getpixel((x,y))
                        for y in ys for x in xs
                    ]
                    self.assertEqual(list(rendered.getdata()),expected)
                    self.assertLessEqual(rendered.width,source.width)
                    self.assertLessEqual(rendered.height,source.height)
                self.assertEqual(pack.files['sprs/1000.spr'],original_sprite)
                self.assertEqual(pack.files['txrs/0001.txr'],original_texture)

    def test_protocol_is_deterministic_and_client_only(self):
        files=fixture_files();profile={'dialogue':'gdrs/0004.gdt','sprites':['sprs/1000.spr']*7,'intro_voice':''}
        args=dict(pack_id='example.operator',name='示例',source_template=60001,locale='zh-HK')
        one=legacy_pack.package(files,profile,**args);two=legacy_pack.package(files,profile,**args)
        self.assertEqual(one,two)
        p=legacy_pack.Pack(one,Base());self.assertEqual(p.describe()['distribution'],{'scope':'client-content','bundle_with_manager':False})

    def test_source_language_never_falls_back_to_another_clients_audio(self):
        files=fixture_files();table=legacy_pack.Gdt(files['gdrs/0004.gdt'],710)
        assets.put_wchar(table.rows[0],602,50,'sdrs\\9999.zsd');files['gdrs/0004.gdt']=table.encode()
        profile={'dialogue':'gdrs/0004.gdt','sprites':['sprs/1000.spr']*7,'intro_voice':''}
        raw=legacy_pack.package(files,profile,pack_id='example.voice',name='粤语',locale='zh-HK')
        pack=legacy_pack.Pack(raw,Base())
        self.assertNotIn('sdrs/9999.zsd',pack.describe()['base_dependencies'])
        self.assertIn('sdrs/9999.zsd',pack.describe()['suppressed_audio_dependencies'])
        generated,_=pack.compile(60047,60001)
        new=legacy_pack.Gdt(next(v for k,v in generated.items() if k.endswith('.gdt')),710)
        self.assertTrue(assets.wchar(new.rows[0],502,50))
        self.assertEqual(assets.wchar(new.rows[0],602,50),'')

    def test_unsafe_paths_duplicates_and_tampered_hashes_are_rejected(self):
        for bad in ('../escape.txt','C:/outside.txt','gdrs/../../out.gdt'):
            with self.subTest(path=bad),self.assertRaises(ValueError):legacy_pack.Pack(archive({**fixture_files(),bad:b'x'}),Base())
        files=fixture_files();raw=archive({**files,'TXRS/0001.TXR':files['txrs/0001.txr']})
        with self.assertRaises(ValueError):legacy_pack.Pack(raw,Base())
        profile={'dialogue':'gdrs/0004.gdt','sprites':['sprs/1000.spr']*7,'intro_voice':''}
        valid=legacy_pack.package(files,profile,pack_id='example.hash',name='示例')
        with zipfile.ZipFile(io.BytesIO(valid)) as z:changed={k:z.read(k) for k in z.namelist()}
        changed['txrs/0001.txr']+=b'tampered'
        with self.assertRaises(ValueError):legacy_pack.Pack(archive(changed),Base())

    def test_scripts_are_never_exported_as_assets(self):
        p=legacy_pack.Pack(archive({**fixture_files(),'install.cmd':b'not executed'}),Base())
        self.assertEqual(p.ignored,['install.cmd'])
        generated,_=p.compile(60047,60001)
        self.assertFalse(any(k.endswith('.cmd') for k in generated))

    def test_workspace_preview_commit_export_and_share(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);w=Workspace.__new__(Workspace);w.root=root;w.assets=Base(root);w.path=root/'operators.json'
            w.server_ini=None;w.module_binary=root/'operator_catalog.dll';w.module_binary.write_bytes(b'fixture DLL; not executable')
            w.baseline={'operators':[{'id':60001,'name':'模板'}],'native_operator_ids':[60001]}
            catalog={'schema_version':1,'native_operator_ids':[60001],'client_index_sha256':'fixture','uploaded_assets':{},'native_server_data':{},'items':[],
                'operators':[{'id':60001,'category_id':1219001,'template_id':60001,'name':'模板','managed':False,'enabled':True,'delivery':'product',
                    'avatar':{'kind':'native','operator_id':60001},'drops':[]}]}
            model.save(w.path,catalog);before=w.read();revision=model.revision(before)
            preview=w.preview_pack(archive(fixture_files()),'新副官.zip',revision)
            self.assertEqual(w.read(),before)
            updated=w.import_pack(preview['pack_sha256'],revision,name='新副官')
            identity=updated['selected_id'];self.assertEqual(identity,60002)
            op=next(o for o in updated['operators'] if o['id']==identity)
            self.assertEqual(op['avatar']['kind'],'pack');self.assertEqual(op['delivery'],'product')
            public=w.package()
            with zipfile.ZipFile(io.BytesIO(public)) as z:
                prefix=f'client/sdgo/mods/operator_catalog/assets/{identity}/'
                self.assertFalse(any(k.endswith('.zsd') for k in z.namelist()))
                self.assertTrue(any(k.endswith('.opack') for k in z.namelist()))
                receipt=native_resources.validate_bundle(identity,
                    z.read(prefix+native_resources.CONFIG_NAME),
                    z.read(prefix+native_resources.INDEX_NAME),
                    z.read(prefix+native_resources.ARCHIVE_NAME),
                    include_payload=True)
                self.assertEqual(receipt['layout'],'compiled')
                self.assertTrue(any(k.endswith('.zsd') for k in receipt['resources']))
                manifest=json.loads(z.read('manifest.json'))
                self.assertTrue(all(hashlib.sha256(z.read(k)).hexdigest()==v for k,v in manifest['files'].items()))
                generated=json.loads(z.read('client/sdgo/mods/operator_catalog/manifest.json'))
                self.assertIn(f'assets/{identity}/resources.zpk',generated['files'])
                self.assertFalse(any(k.endswith('.zsd') for k in generated['files']))
            shared=w.share(identity);again=legacy_pack.Pack(shared,w.assets)
            self.assertEqual(again.label,'新副官');self.assertEqual(len(again.voice_events),2)
            with self.assertRaises(ValueError):w.import_pack(preview['pack_sha256'],revision)

    def test_workspace_archives_flat_upload_resources_without_loose_duplicates(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);w=Workspace.__new__(Workspace);w.root=root;w.assets=Base(root);w.path=root/'operators.json'
            w.server_ini=None;w.module_binary=root/'operator_catalog.dll';w.module_binary.write_bytes(b'fixture DLL; not executable')
            w.baseline={'operators':[{'id':60001,'name':'模板'}],'native_operator_ids':[60001]}
            raw=png();sha=hashlib.sha256(raw).hexdigest()
            catalog={'schema_version':1,'native_operator_ids':[60001],'client_index_sha256':'fixture',
                'uploaded_assets':{sha:{'width':16,'height':16}},'native_server_data':{},'items':[],
                'operators':[{'id':60001,'category_id':1219001,'template_id':60001,'name':'模板','managed':False,'enabled':True,
                    'delivery':'product','avatar':{'kind':'native','operator_id':60001},'drops':[]}]}
            catalog,identity=model.create_operator(catalog,60001)
            catalog=model.update_operator(catalog,identity,{'avatar':{'kind':'upload','sha256':sha}})
            (root/'uploads').mkdir();(root/'uploads'/(sha+'.png')).write_bytes(raw)
            model.save(w.path,catalog)
            with zipfile.ZipFile(io.BytesIO(w.package())) as z:
                prefix=f'client/sdgo/mods/operator_catalog/assets/{identity}/'
                receipt=native_resources.validate_bundle(identity,
                    z.read(prefix+native_resources.CONFIG_NAME),
                    z.read(prefix+native_resources.INDEX_NAME),
                    z.read(prefix+native_resources.ARCHIVE_NAME),
                    include_payload=True)
                expected={native_resources.upload_texture_key(identity),
                    *(f'sdgo/mods/operator_catalog/assets/{identity}_{index}.spr' for index in range(7))}
                self.assertEqual(set(receipt['resources']),expected)
                self.assertFalse(any(name.startswith('client/sdgo/mods/operator_catalog/assets/') and
                    name.endswith(('.txr','.spr')) for name in z.namelist()))

    def test_fresh_workspace_recovers_upload_from_installed_native_bundle(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);identity=60047;raw=png()
            resources={native_resources.upload_texture_key(identity):raw}
            resources.update({
                f'sdgo/mods/operator_catalog/assets/{identity}_{index}.spr':f'sprite {index}'.encode()
                for index in range(7)
            })
            bundle=native_resources.build_bundle(identity,resources)
            target=root/'sdgo/mods/operator_catalog/assets'/str(identity);target.mkdir(parents=True)
            for name,data in bundle['files'].items():(target/name).write_bytes(data)
            self.assertEqual(_installed_upload(root,identity),raw)
            legacy_resources={
                native_resources.legacy_upload_texture_key(identity):raw,
                **{
                    f'sdgo/mods/operator_catalog/assets/{identity}_{index}.spr':
                    f'legacy sprite {index}'.encode()
                    for index in range(7)
                },
            }
            legacy=native_resources.build_bundle(identity,legacy_resources)
            for name,data in legacy['files'].items():(target/name).write_bytes(data)
            self.assertEqual(_installed_upload(root,identity),raw)

if __name__=='__main__':unittest.main()
