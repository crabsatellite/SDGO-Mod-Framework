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


class BlueprintBaseline(Baseline):
    def exists(self,path):return path=='sprs/uniqueplan.spr' or super().exists(path)
    def record(self,kind,identity):
        if kind=='blueprint' and identity==200001:return donor()
        if kind=='item' and identity==1520001:
            raw=bytearray(614);struct.pack_into('<I',raw,0,identity);wchar(raw,5,'Original Plan')
            struct.pack_into('<I',raw,105,200001);return bytes(raw)
        return super().record(kind,identity)


class BlueprintCompiler(unittest.TestCase):
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
                    manifest=json.loads(z.read('manifest.json'))
                    import hashlib
                    self.assertEqual(hashlib.sha256(z.read(fragment)).hexdigest(),manifest['files'][fragment])
                    raw=z.read('client/sdgo/mods/unit_catalog/catalog.bin')
                    output=json.loads(z.read('client/sdgo/mods/unit_catalog/definitions.json'))
                    server=json.loads(Path(result['server_plan']).read_text(encoding='utf-8'))
                records={};cursor=32
                for i in range(struct.unpack_from('<I',raw,16)[0]):
                    kind,identity,size,*_=struct.unpack_from('<6I',raw,cursor);cursor+=24
                    records[(kind,identity)]=raw[cursor:cursor+size];cursor+=size
                self.assertEqual(1197,len(records[(33,209000)]))
                self.assertEqual('sprs/u90001r.spr',records[(33,209000)][169:229].decode('utf-16-le').split('\0')[0].replace('\\','/'))
                self.assertEqual(donor()[109:169],records[(33,209000)][109:169])
                self.assertEqual('sprs/u90001b.spr',records[(33,209000)][289:349].decode('utf-16-le').split('\0')[0].replace('\\','/'))
                self.assertIn('client/txrs/u90001b.txr',manifest['files'])
                self.assertIn('client/txrs/u90001r.txr',manifest['files'])
                self.assertEqual(90001,struct.unpack_from('<I',records[(33,209000)],229)[0])
                self.assertEqual(209000,struct.unpack_from('<I',records[(34,1599000)],105)[0])
                self.assertEqual(209000,struct.unpack_from('<I',records[(85,0x375BE2)],10)[0])
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
