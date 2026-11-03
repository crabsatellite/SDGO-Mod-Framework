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


class BlueprintBaseline(Baseline):
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
                family['blueprints']=[declaration()];family['metadata']={'description':'Private author content'}
                family['status']='confirmed';family['published']=copy.deepcopy(family)
                state=engine.read();state['drafts'][family['id']]=family
                transaction=root/'transaction';transaction.mkdir()
                images={role:png() for role in ('unit_primary','shop','custom','capsule_fixed','index_owned','index_missing')}
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
                self.assertEqual(90001,struct.unpack_from('<I',records[(33,209000)],229)[0])
                self.assertEqual(209000,struct.unpack_from('<I',records[(34,1599000)],105)[0])
                self.assertEqual(209000,struct.unpack_from('<I',records[(85,0x375BE2)],10)[0])
                self.assertEqual('Private author content',output['registrations'][0]['metadata']['description'])
                self.assertEqual(209000,server['families'][0]['blueprints'][0]['id'])
                self.assertFalse(result.get('player_tables_changed',False))
            finally:engine.close()

    def test_type13_remains_rejected_by_native_container(self):
        with self.assertRaises(ValueError):compiler.native_catalog([(13,70000,bytes(1132))])


if __name__=='__main__':unittest.main()
