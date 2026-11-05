import copy
import hashlib
import struct
from pathlib import Path
import tempfile
import unittest

from owner.units.repository import SKILL_COLUMNS
from owner.units.server_dependencies import canonical, prepare, row_hash, validate_manifest


def fixture(identity=9000800):
    row={key:0 for key in SKILL_COLUMNS}
    row.update(SkillID=70001,SkillName='Example skill',Scale=1.0,Activate=2)
    raw=bytearray(1132);struct.pack_into('<I',raw,0,identity)
    raw[155]=2;raw[1131]=2
    manifest={'schema':'sdgo.unit-server-dependencies/1','owner':'example','revision':'a'*64,
        'skills':[{'id':identity,'client_sha256':hashlib.sha256(raw).hexdigest(),
                   'definition':{'kind':'template','id':70001,'sha256':row_hash(row)}}]}
    return manifest,{identity:bytes(raw)},{70001:row}


def planned(manifest,records,templates,**kwargs):
    options={'templates':templates,'current':{},'ownership':{},'available_ids':set(templates)}
    options.update(kwargs)
    return prepare(manifest,records,**options)


class ServerDependencies(unittest.TestCase):
    def test_clones_complete_template_without_deriving_server_battle_mode_from_client_dispatch(self):
        manifest,records,templates=fixture()
        result=planned(manifest,records,templates)
        self.assertEqual('inserted',result[0]['action'])
        self.assertEqual({**templates[70001],'SkillID':9000800},result[0]['row'])
        self.assertEqual(0,result[0]['row']['BattleMode'])
        self.assertEqual(2,records[9000800][1131])
        self.assertEqual(70001,templates[70001]['SkillID'])

    def test_missing_or_changed_template_never_supplies_defaults(self):
        manifest,records,templates=fixture()
        with self.assertRaisesRegex(ValueError,'missing or changed'):planned(manifest,records,{})
        templates[70001]['ActiveResult']=1
        with self.assertRaisesRegex(ValueError,'missing or changed'):planned(manifest,records,templates)

    def test_explicit_definition_requires_all_fields_and_declared_origin(self):
        manifest,records,templates=fixture()
        definition={'kind':'explicit','origin':'authored','values':{**templates[70001],'SkillID':9000800}}
        manifest['skills'][0]['definition']=definition
        self.assertEqual(9000800,planned(manifest,records,{})[0]['id'])
        del definition['values']['Scale']
        with self.assertRaisesRegex(ValueError,'fields differ'):planned(manifest,records,{})

    def test_client_identity_projection_hash_and_unknown_dispatch_are_preserved_and_rejected(self):
        manifest,records,templates=fixture()
        for offset,value in ((0,1),(105,1),(1131,165)):
            bad=copy.deepcopy(manifest);raw=bytearray(records[9000800]);raw[offset]=value
            bad['skills'][0]['client_sha256']=hashlib.sha256(raw).hexdigest()
            with self.subTest(offset=offset),self.assertRaises(ValueError):planned(bad,{9000800:bytes(raw)},templates)
            self.assertEqual(value,raw[offset])
        bad=copy.deepcopy(manifest);bad['skills'][0]['client_sha256']='0'*64
        with self.assertRaisesRegex(ValueError,'SHA-256'):planned(bad,records,templates)

    def test_baseline_or_other_owner_cannot_be_adopted_even_when_bytes_equal(self):
        manifest,records,templates=fixture();desired=planned(manifest,records,templates)[0]
        for owner in (None,'other'):
            owned={} if owner is None else {9000800:{'owner':owner}}
            with self.subTest(owner=owner),self.assertRaisesRegex(ValueError,'baseline or belongs'):
                planned(manifest,records,templates,current={9000800:desired['row']},ownership=owned)

    def test_same_owned_revision_is_idempotent_and_outside_edits_refuse(self):
        manifest,records,templates=fixture();desired=planned(manifest,records,templates)[0]
        owned={9000800:{'owner':'example','installed_hash':desired['hash'],'revision':manifest['revision'],
            'client_hash':manifest['skills'][0]['client_sha256'],'definition_json':canonical(manifest['skills'][0]['definition'])}}
        current={9000800:dict(desired['row'])}
        self.assertEqual('reused',planned(manifest,records,templates,current=current,ownership=owned)[0]['action'])
        current[9000800]['Scale']=2.0
        with self.assertRaisesRegex(ValueError,'outside'):planned(manifest,records,templates,current=current,ownership=owned)
        with self.assertRaisesRegex(ValueError,'missing'):planned(manifest,records,templates,ownership=owned)

    def test_missing_link_and_client_server_field_mismatch_refuse(self):
        manifest,records,templates=fixture();raw=bytearray(records[9000800]);struct.pack_into('<I',raw,354,999)
        manifest['skills'][0]['client_sha256']=hashlib.sha256(raw).hexdigest()
        with self.assertRaisesRegex(ValueError,'linked'):planned(manifest,{9000800:bytes(raw)},templates)
        raw=bytearray(records[9000800]);struct.pack_into('<h',raw,272,15)
        manifest['skills'][0]['client_sha256']=hashlib.sha256(raw).hexdigest()
        with self.assertRaisesRegex(ValueError,'shared skill field'):planned(manifest,{9000800:bytes(raw)},templates)

    def test_owned_content_change_requires_a_new_revision(self):
        manifest,records,templates=fixture();desired=planned(manifest,records,templates)[0]
        owned={9000800:{'owner':'example','installed_hash':desired['hash'],'revision':manifest['revision'],
            'client_hash':manifest['skills'][0]['client_sha256'],'definition_json':canonical(manifest['skills'][0]['definition'])}}
        current={9000800:dict(desired['row'])}
        values={**desired['row'],'SkillName':'Authored label'}
        manifest['skills'][0]['definition']={'kind':'explicit','origin':'authored','values':values}
        with self.assertRaisesRegex(ValueError,'same server dependency revision'):
            planned(manifest,records,templates,current=current,ownership=owned)
        manifest['revision']='b'*64
        self.assertEqual('updated',planned(manifest,records,templates,current=current,ownership=owned)[0]['action'])

    def test_unknown_manifest_permission_and_duplicate_ids_are_refused(self):
        manifest,records,templates=fixture();manifest['native_registration_authorized']=True
        with self.assertRaises(ValueError):validate_manifest(manifest,records)
        del manifest['native_registration_authorized'];manifest['skills']*=2
        with self.assertRaisesRegex(ValueError,'duplicate'):validate_manifest(manifest,records)

    def test_data_library_rows_do_not_open_the_ordinary_unit_publisher(self):
        from test_unit_import_repository import make_snapshot,plan,write_json
        from owner.units.repository import Repository
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);make_snapshot(root)
            write_json(root/'dbo.sdgo_UnitImportDependencies.json',[{'RecordKind':'skill','RecordID':70001}])
            repository=Repository(snapshot=root)
            self.assertIn(70001,repository.catalog['skill_ids'])
            self.assertIn(70001,repository.catalog['pending_skill_ids'])
            with self.assertRaisesRegex(ValueError,'仅有制作数据'):repository.check_plan(plan())


if __name__=='__main__':unittest.main()
