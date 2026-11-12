import copy
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from owner.units import compiler
from owner.units.workspace import Workspace
from unit_import_support import Baseline,Repository,archive,complete,png,publisher,wchar
from test_unit_blueprints import declaration
from test_unit_blueprint_codec import donor
from test_unit_blueprint_resources import closure_fixture,sha
from manager import configuration,storage


class BlueprintBaseline(Baseline):
    def exists(self,path):return path=='sprs/uniqueplan.spr' or super().exists(path)
    def record(self,kind,identity):
        if kind=='blueprint' and identity==200001:return donor()
        if kind=='item' and identity==1520001:
            raw=bytearray(614);struct.pack_into('<I',raw,0,identity);wchar(raw,5,'Original Plan')
            struct.pack_into('<I',raw,105,200001);return bytes(raw)
        return super().record(kind,identity)


class BlueprintCompiler(unittest.TestCase):
    def test_runtime_manifest_has_final_framework_contract_for_source_and_rendered_templates(self):
        source=(compiler.ROOT/'mods/unit_catalog/mod.ini').read_bytes()
        contract=json.loads((compiler.ROOT/'mods/unit_catalog/manager.json').read_bytes())
        already_rendered=configuration.render(source,contract)
        for raw in (source,already_rendered):
            with self.subTest(rendered=raw is already_rendered):
                output=compiler._runtime_manifest(raw)
                manifest=storage.ini_values(output)
                self.assertEqual('1',manifest['mod']['enabled'])
                self.assertEqual('1',manifest['settings']['Enabled'])
                self.assertEqual('1',manifest['settings']['FormChains'])
                payload=set(manifest['mod']['files'].split(','))
                self.assertTrue({'blueprint-availability.json','blueprint-availability.bin','resources.zpk'}<=payload)
                expected=configuration.files(manifest,contract)
                self.assertEqual(expected,manifest['framework.configuration']['files'].split(','))
                self.assertEqual(['blueprint-availability.bin','blueprint-availability.json','catalog.bin','definitions.json','mod.ini'],expected)
                self.assertNotIn('resources.zpk',expected)
                self.assertEqual('Enabled',manifest['framework.dependencies']['enabled_key'])
                self.assertEqual(output,configuration.render(output,contract))
        self.assertEqual(compiler._runtime_manifest(source),compiler._runtime_manifest(already_rendered))

    def test_pair_contains_recipe_item_primary_link_and_owner_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);engine=Workspace(root/'workspace',baseline=BlueprintBaseline(),repository=Repository(),publisher=publisher)
            try:
                state=complete(engine,engine.inspect('unit.zip',archive(),engine.public()['revision']))
                family=copy.deepcopy(engine.read()['drafts'][state['draft']['id']])
                recipe=declaration();recipe.pop('recipe_sprite',None)
                family['blueprints']=[recipe];family['metadata']={'description':'Private author content'}
                family['status']='confirmed';family['published']=copy.deepcopy(family)
                state=engine.read();state['drafts'][family['id']]=family
                transaction=root/'transaction';transaction.mkdir()
                images={role:png() for role in ('unit_primary','shop','custom','capsule_fixed','index_owned','index_missing','blueprint_preview','blueprint_card')}
                with patch.object(compiler,'_adapter_files',return_value=(b'MZ'+bytes(510),b'[mod]\nenabled=0\nfiles=\n',b'symbols',b'installer')), \
                     patch('owner.units.presentation.publication_images',return_value=images), \
                     patch('owner.units.presentation.contract',return_value={'schema':3}):
                    result=compiler.publish(engine,state,family['id'],transaction)
                with zipfile.ZipFile(result['client_zip']) as z:
                    fragment='client/sdgo/mods/unit_catalog/symbols.fragment.ini'
                    self.assertEqual(z.read('symbols.fragment.ini'),z.read(fragment))
                    self.assertIn(b'files=symbols.fragment.ini,',z.read('client/sdgo/mods/unit_catalog/mod.ini'))
                    runtime_manifest=storage.ini_values(z.read('client/sdgo/mods/unit_catalog/mod.ini'))
                    self.assertEqual('1',runtime_manifest['framework.dependencies']['schema'])
                    self.assertEqual(['catalog.bin','definitions.json','mod.ini'],runtime_manifest['framework.configuration']['files'].split(','))
                    manifest=json.loads(z.read('manifest.json'))
                    import hashlib
                    self.assertEqual(hashlib.sha256(z.read(fragment)).hexdigest(),manifest['files'][fragment])
                    raw=z.read('client/sdgo/mods/unit_catalog/catalog.bin')
                    output=json.loads(z.read('client/sdgo/mods/unit_catalog/definitions.json'))
                    availability=json.loads(z.read('client/sdgo/mods/unit_catalog/blueprint-availability.json'))
                    availability_binary=z.read('client/sdgo/mods/unit_catalog/blueprint-availability.bin')
                    server=json.loads(Path(result['server_plan']).read_text(encoding='utf-8'))
                records={};cursor=32
                for i in range(struct.unpack_from('<I',raw,16)[0]):
                    kind,identity,size,*_=struct.unpack_from('<6I',raw,cursor);cursor+=24
                    records[(kind,identity)]=raw[cursor:cursor+size];cursor+=size
                self.assertEqual(1197,len(records[(33,209000)]))
                # The compiler derives this complete fixed-width carrier from
                # the bytes it actually publishes; it never edits source blobs.
                from owner.units.model_integrity import model_digest
                with zipfile.ZipFile(result['client_zip']) as z:
                    published_model=z.read('client/mdrs/test.mod')
                self.assertEqual(model_digest(published_model),records[(2,90001)][672:736])
                source_form=family['forms'][0]
                self.assertEqual(bytes(64),engine.blob(source_form['source_sha'])[672:736])
                self.assertEqual('sprs/u90001r.spr',records[(33,209000)][169:229].decode('utf-16-le').split('\0')[0].replace('\\','/'))
                self.assertEqual(donor()[109:169],records[(33,209000)][109:169])
                self.assertEqual('sprs/u90001b.spr',records[(33,209000)][289:349].decode('utf-16-le').split('\0')[0].replace('\\','/'))
                self.assertIn('client/txrs/u90001b.txr',manifest['files'])
                self.assertIn('client/txrs/u90001r.txr',manifest['files'])
                self.assertEqual(90001,struct.unpack_from('<I',records[(33,209000)],229)[0])
                self.assertEqual(209000,struct.unpack_from('<I',records[(34,1599000)],105)[0])
                self.assertEqual(209000,struct.unpack_from('<I',records[(85,0x375BE2)],10)[0])
                self.assertEqual({'state':1,'item_category_id':1599000,'unit_id':90001},
                                 availability['blueprints']['209000'])
                availability_header=struct.unpack_from('<8s6I',availability_binary)
                self.assertEqual((b'SDGOBP1\0',1,32,1,12),availability_header[:5])
                self.assertEqual(struct.unpack_from('<I',raw,24)[0],availability_header[6])
                self.assertEqual('Private author content',output['registrations'][0]['metadata']['description'])
                self.assertEqual(209000,server['families'][0]['blueprints'][0]['id'])
                self.assertFalse(result.get('player_tables_changed',False))
                # A generated-picture correction keeps its SPR path and all
                # record bytes, but must invalidate the published resource revision.
                import io
                from PIL import Image
                stream=io.BytesIO();Image.new('RGBA',(2,2),(20,90,140,255)).save(stream,'PNG')
                corrected=dict(images,blueprint_preview=stream.getvalue())
                second=root/'corrected';second.mkdir()
                with patch.object(compiler,'_adapter_files',return_value=(b'MZ'+bytes(510),b'[mod]\nenabled=0\nfiles=\n',b'symbols',b'installer')), \
                     patch('owner.units.presentation.publication_images',return_value=corrected), \
                     patch('owner.units.presentation.contract',return_value={'schema':3}):
                    updated=compiler.publish(engine,state,family['id'],second)
                with zipfile.ZipFile(updated['client_zip']) as z:
                    self.assertEqual(raw,z.read('client/sdgo/mods/unit_catalog/catalog.bin'))
                self.assertNotEqual(result['revision'],updated['revision'])
            finally:engine.close()

    def test_changed_and_disabled_primary_rewrites_index_and_authority(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);engine=Workspace(root/'workspace',baseline=BlueprintBaseline(),repository=Repository(),publisher=publisher)
            try:
                inspected=complete(engine,engine.inspect('unit.zip',archive(),engine.public()['revision']))
                family=copy.deepcopy(engine.read()['drafts'][inspected['draft']['id']])
                first=declaration();first.pop('recipe_sprite',None);first['encyclopedia_primary']=True
                second=declaration();second.pop('recipe_sprite',None);second.update(
                    id=209001,name='第二设计图',internal_name='Second Plan',item_category_id=1599001,
                    encyclopedia_primary=False)
                family['blueprints']=[first,second];family['status']='confirmed';family['published']=copy.deepcopy(family)
                state=engine.read();state['drafts'][family['id']]=family
                images={role:png() for role in ('unit_primary','shop','custom','capsule_fixed','index_owned','index_missing','blueprint_preview','blueprint_card')}
                def compile_into(name,current):
                    target=root/name;target.mkdir()
                    with patch.object(compiler,'_adapter_files',return_value=(b'MZ'+bytes(510),b'[mod]\nenabled=0\nfiles=\n',b'symbols',b'installer')), \
                         patch('owner.units.presentation.publication_images',return_value=images), \
                         patch('owner.units.presentation.contract',return_value={'schema':3}):
                        result=compiler.publish(engine,current,family['id'],target)
                    with zipfile.ZipFile(result['client_zip']) as archive_file:
                        raw=archive_file.read('client/sdgo/mods/unit_catalog/catalog.bin')
                        authority=json.loads(archive_file.read('client/sdgo/mods/unit_catalog/blueprint-availability.json'))
                    rows={};cursor=32
                    for _ in range(struct.unpack_from('<I',raw,16)[0]):
                        kind,identity,size,*_=struct.unpack_from('<6I',raw,cursor);cursor+=24
                        rows[(kind,identity)]=raw[cursor:cursor+size];cursor+=size
                    return struct.unpack_from('<I',rows[(85,0x375BE2)],10)[0],authority
                primary,authority=compile_into('first',state)
                self.assertEqual(209000,primary)
                self.assertEqual((1,1),(authority['blueprints']['209000']['state'],authority['blueprints']['209001']['state']))

                parent_disabled=copy.deepcopy(state)
                parent_disabled['drafts'][family['id']]['status']='disabled'
                primary,authority=compile_into('parent-disabled',parent_disabled)
                self.assertEqual(209000,primary)
                self.assertEqual((2,2),(authority['blueprints']['209000']['state'],authority['blueprints']['209001']['state']))

                changed=copy.deepcopy(state)
                recipes=changed['drafts'][family['id']]['published']['blueprints']
                recipes[0].update(enabled=False,encyclopedia_primary=False)
                recipes[1]['encyclopedia_primary']=True
                primary,authority=compile_into('changed',changed)
                self.assertEqual(209001,primary)
                self.assertEqual((2,1),(authority['blueprints']['209000']['state'],authority['blueprints']['209001']['state']))

                disabled=copy.deepcopy(changed)
                disabled['drafts'][family['id']]['published']['blueprints'][1].update(
                    enabled=False,encyclopedia_primary=False)
                primary,authority=compile_into('disabled',disabled)
                self.assertEqual(0,primary)
                self.assertEqual((2,2),(authority['blueprints']['209000']['state'],authority['blueprints']['209001']['state']))
            finally:engine.close()

    def test_type13_remains_rejected_by_native_container(self):
        with self.assertRaises(ValueError):compiler.native_catalog([(13,70000,bytes(1132))])

    def test_bound_raw_templates_preserve_item_name_and_emit_diff_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);engine=Workspace(root/'workspace',baseline=BlueprintBaseline(),repository=Repository(),publisher=publisher)
            try:
                state=complete(engine,engine.inspect('unit.zip',archive(),engine.public()['revision']))
                family=copy.deepcopy(engine.read()['drafts'][state['draft']['id']])
                manifest,files=closure_fixture()
                # A transport member may resemble a native path. Only the
                # prepared graph may install it; source-only art stays evidence.
                for resource in manifest['resources']:
                    old=resource['member'];resource['member']=resource['path']
                    files[resource['member']]=files.pop(old)
                manifest_path='blueprints/209000/client-templates.json'
                encoded=json.dumps(manifest,sort_keys=True).encode();files[manifest_path]=encoded
                binding={'manifest_path':manifest_path,'manifest_sha256':sha(encoded)}
                for path,data in files.items():
                    digest=engine._store(data)
                    family['files'].append({'path':path,'sha':digest,'bytes':len(data)})
                recipe=declaration()
                recipe['recipe_sprite']='sprs/owned-b.spr'
                recipe['client_templates']=binding
                family['blueprints']=[recipe]
                family['status']='confirmed';family['published']=copy.deepcopy(family)
                current=engine.read();current['drafts'][family['id']]=family
                transaction=root/'transaction';transaction.mkdir()
                images={role:png() for role in ('unit_primary','shop','custom','capsule_fixed','index_owned','index_missing')}
                with patch.object(compiler,'_adapter_files',return_value=(b'MZ'+bytes(510),b'[mod]\nenabled=0\nfiles=\n',b'symbols',b'installer')), \
                     patch('owner.units.presentation.publication_images',return_value=images), \
                     patch('owner.units.presentation.contract',return_value={'schema':3}):
                    result=compiler.publish(engine,current,family['id'],transaction)
                with zipfile.ZipFile(result['client_zip']) as z:
                    catalog=z.read('client/sdgo/mods/unit_catalog/catalog.bin')
                    receipts=json.loads(z.read('blueprint-template-receipts.json'))
                    registrations=json.loads(z.read('registrations.json'))
                    self.assertIn('client/sprs/owned-a.spr',z.namelist())
                    self.assertIn('client/sprs/owned-b.spr',z.namelist())
                    self.assertIn('client/sprs/i1.spr',z.namelist())
                    self.assertIn('client/txrs/a.txr',z.namelist())
                    self.assertNotIn('client/sprs/source-a.spr',z.namelist())
                    self.assertNotIn('client/sprs/source-b.spr',z.namelist())
                    self.assertNotIn('client/sprs/i0.spr',z.namelist())
                records={};cursor=32
                for _ in range(struct.unpack_from('<I',catalog,16)[0]):
                    kind,identity,size,*_=struct.unpack_from('<6I',catalog,cursor);cursor+=24
                    records[(kind,identity)]=catalog[cursor:cursor+size];cursor+=size
                item=records[(34,1599000)]
                self.assertEqual('Separate Item Name',item[5:105].decode('utf-16-le').split('\0')[0])
                self.assertEqual('示例设计图',records[(33,209000)][5:105].decode('utf-16-le').split('\0')[0])
                self.assertEqual(1,len(receipts['items']))
                self.assertTrue(receipts['resource_closure_verified'])
                self.assertTrue(receipts['native_registration_authorized'])
                self.assertFalse(receipts['items'][0]['native_runtime_accepted'])
                self.assertTrue(receipts['items'][0]['records'][0]['compiled_changes'])
                registered=registrations['items'][0]['blueprints'][0]
                self.assertTrue(registered['client_template_bound'])
                self.assertIn('client_template_compiled_sha256',registered)
            finally:engine.close()


if __name__=='__main__':unittest.main()
