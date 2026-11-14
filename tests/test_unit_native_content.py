import copy
import hashlib
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

from owner.units.native_content import validate_project_bindings
from owner.units.repository import SKILL_COLUMNS
from owner.units.server_dependencies import validate_manifest as validate_server
from owner.units.skill_atlases import compile_bundle as compile_atlases
from owner.units.assets import sprite_binary
from owner.units.workspace import Workspace
from owner.units.project import export_project
from unit_import_support import Baseline, Repository, archive, png, publisher, complete


def sha(raw):return hashlib.sha256(raw).hexdigest()


def fixture(dispatch=0,opaque=False):
    files={}
    skill=bytearray(1132);struct.pack_into('<I',skill,0,9800123)
    name='Example skill'.encode('utf-16-le');skill[5:5+len(name)]=name;skill[1131]=dispatch
    funnel=bytearray(732);struct.pack_into('<I',funnel,0,9800124);funnel[5]=3
    struct.pack_into('<III',funnel,719,1,1,2);funnel[731]=1
    records=[]
    for kind,identity,profile,data in [(13,9800123,'native-type13-1132-v1',bytes(skill)),(11,9800124,'native-type11-732-v1',bytes(funnel))]:
        member=f'dependencies/{identity}.bin';files[member]=data
        records.append({'kind':kind,'id':identity,'profile':profile,'member':member,'sha256':sha(data)})
    resources=[]
    for virtual,data in [('txrs/example_skill.txr',png()),('sprs/example_skill.spr',sprite_binary('txrs/example_skill.txr',32,24))]:
        member='dependencies/media/'+Path(virtual).name;files[member]=data
        resources.append({'path':virtual,'member':member,'sha256':sha(data)})
    row={key:0 for key in SKILL_COLUMNS};row.update(SkillID=9800123,SkillName='Example skill',Scale=1.0)
    server={'schema':'sdgo.unit-server-dependencies/1','owner':'example','revision':'a'*64,
        'skills':[{'id':9800123,'client_sha256':sha(skill),'definition':{'kind':'explicit','origin':'authored','values':row}}]}
    atlas={'schema':'sdgo.unit-skill-atlases/1','owner':'example','revision':'b'*64,
        'sprites':[{'path':'sprs/example_skill.spr','sha256':resources[1]['sha256'],'textures':[{'path':'txrs/example_skill.txr','sha256':resources[0]['sha256']}]}],
        'skills':[{'id':9800123,'record_sha256':sha(skill),'sprite':'sprs/example_skill.spr'}]}
    sfunnel={'schema':'sdgo.unit-server-funnels/1','owner':'example','revision':'c'*64,
        'funnels':[{'id':9800124,'client_sha256':sha(funnel),'definition':{'kind':'explicit','origin':'authored',
            'values':{'FunnelID':9800124,'FunnelProp':3,'AniTime':1,'AttackDelay':1,'AttackCount':2,'Stealable':1}}}]}
    manifest={'schema':'sdgo.unit-native-content/1','owner':'example','revision':'d'*64,'records':records,'resources':resources,
        'evidence':[{'member':'dependencies/source-record.bin','sha256':sha(b'original opaque source record')}]}
    files['dependencies/source-record.bin']=b'original opaque source record'
    for key,data in [('server_skills',server),('server_funnels',sfunnel),('skill_atlases',atlas)]:
        member='dependencies/'+key+'.json';raw=json.dumps(data).encode();files[member]=raw
        manifest[key]={'manifest_path':member,'manifest_sha256':sha(raw)}
    if opaque:
        raw=struct.pack('<I',9800125)+bytes([173])*85;member='dependencies/unknown-native.bin';files[member]=raw
        manifest['records'].append({'kind':79,'id':9800125,'profile':'pending-type79-89-v1','member':member,'sha256':sha(raw)})
    return manifest,files


def bind(manifest,files):
    files=dict(files);raw=json.dumps(manifest).encode();member='dependencies/native.json';files[member]=raw
    return [{'manifest_path':member,'manifest_sha256':sha(raw)}],files


def project_archive(manifest,files):
    bindings,files=bind(manifest,files)
    with zipfile.ZipFile(io.BytesIO(archive())) as z:files={**{name:z.read(name) for name in z.namelist()},**files}
    data={'protocol':'sdgo.unit-project','schema_version':1,'name':'Example dependency project','native_content':bindings,
        'files':{name:sha(raw) for name,raw in files.items()},'definitions':{},'assets':[]}
    files['project.json']=json.dumps(data).encode();out=io.BytesIO()
    with zipfile.ZipFile(out,'w',zipfile.ZIP_DEFLATED) as z:
        for name,raw in files.items():z.writestr(name,raw)
    return out.getvalue()


def attachment(manifest,files):
    out=io.BytesIO()
    with zipfile.ZipFile(out,'w',zipfile.ZIP_DEFLATED) as z:
        z.writestr('manifest.json',json.dumps(manifest))
        for member,raw in files.items():z.writestr(member,raw)
    return out.getvalue()


class NativeContent(unittest.TestCase):
    def test_skill_parameter_record_is_hash_bound_and_counted_without_actor_authority(self):
        manifest,files=fixture();raw=bytearray(89);struct.pack_into('<I',raw,0,8000789)
        struct.pack_into('<6f',raw,65,1,1,1,1,1,1);raw=bytes(raw)
        member='dependencies/example-skill-parameters.bin';files[member]=raw
        manifest['records'].append({'kind':79,'id':8000789,'profile':'native-type79-89-v1','member':member,'sha256':sha(raw)})
        bindings,packed=bind(manifest,files);result=validate_project_bindings(bindings,files=packed)[0]
        self.assertEqual(1,result['summary']['skill_parameters']);self.assertEqual(0,result['summary']['unsupported_records'])
        self.assertFalse(result['native_registration_authorized']);self.assertFalse(result['runtime_authority_issued'])
        self.assertEqual([],result['requirements'])

    def test_complete_kind12_is_carried_as_a_typed_dependency_without_actor_authority(self):
        manifest,files=fixture();raw=struct.pack('<I',59051)+bytes(1555)
        member='dependencies/example-special-feature.bin';files[member]=raw
        manifest['records'].append({'kind':12,'id':59051,'profile':'native-type12-1559-v1','member':member,'sha256':sha(raw)})
        bindings,packed=bind(manifest,files);result=validate_project_bindings(bindings,files=packed)[0]
        self.assertEqual(1,result['summary']['special_features']);self.assertEqual(0,result['summary']['unsupported_records'])
        self.assertEqual('record-transport-only',result['records'][-1]['consumer_scope'])
        self.assertFalse(result['native_registration_authorized']);self.assertFalse(result['runtime_authority_issued'])
        for changed in (raw[:-1],raw[:4]+b'\1'+raw[5:]):
            with self.subTest(bytes=len(changed)):
                data=dict(files);data[member]=changed;bad=copy.deepcopy(manifest);bad['records'][-1]['sha256']=sha(changed)
                request,payload=bind(bad,data)
                with self.assertRaisesRegex(ValueError,'complete record/profile'):
                    validate_project_bindings(request,files=payload)

    def test_complete_typed_records_and_resources_are_read_without_issuing_authority(self):
        manifest,files=fixture();bindings,files=bind(manifest,files)
        result=validate_project_bindings(bindings,files=files)[0]
        self.assertEqual((1,1,2),(result['summary']['skills'],result['summary']['funnels'],result['summary']['resources']))
        self.assertEqual([],result['requirements'])
        self.assertIn('dependencies/source-record.bin',result['members'])
        self.assertFalse(result['native_registration_authorized']);self.assertFalse(result['runtime_authority_issued'])

    def test_unknown_records_and_dispatch_are_retained_and_reported_not_dropped(self):
        manifest,files=fixture(dispatch=165,opaque=True);bindings,files=bind(manifest,files)
        result=validate_project_bindings(bindings,files=files)[0]
        self.assertEqual(165,result['records'][0]['dispatch'])
        self.assertEqual(1,result['summary']['unsupported_records'])
        self.assertEqual({'native_dispatch','native_profile'},{row['code'] for row in result['requirements']})
        self.assertEqual(173,files['dependencies/unknown-native.bin'][4])
        # Authoring retention does not open either the server writer or compiler.
        server=json.loads(files[manifest['server_skills']['manifest_path']])
        with self.assertRaisesRegex(ValueError,'dispatch'):validate_server(server,{9800123:files['dependencies/9800123.bin']})
        atlas=json.loads(files[manifest['skill_atlases']['manifest_path']])
        resources={row['path']:files[row['member']] for row in manifest['resources']}
        with self.assertRaisesRegex(ValueError,'dispatch'):compile_atlases([atlas],'1'*64,'2'*64,'3'*64,{9800123:files['dependencies/9800123.bin']},resources)

    def test_missing_components_become_explicit_requirements_but_missing_members_refuse(self):
        manifest,files=fixture()
        for key in ('server_skills','server_funnels','skill_atlases'):manifest.pop(key)
        bindings,files=bind(manifest,files)
        result=validate_project_bindings(bindings,files=files)[0]
        self.assertEqual({'native_server_skills','native_server_funnels','native_skill_atlases'},{row['code'] for row in result['requirements']})
        files.pop('dependencies/9800123.bin')
        with self.assertRaisesRegex(ValueError,'missing or changed'):validate_project_bindings(bindings,files=files)

    def test_changed_hash_owner_permission_flag_and_resource_executable_refuse(self):
        for change in ('hash','owner','permission','executable'):
            manifest,files=fixture()
            if change=='hash':files['dependencies/9800123.bin']+=b'bad'
            elif change=='owner':manifest['owner']='another'
            elif change=='permission':manifest['native_registration_authorized']=True
            else:manifest['resources'][0]['path']='txrs/payload.exe'
            bindings,files=bind(manifest,files)
            with self.subTest(change=change),self.assertRaises(ValueError):validate_project_bindings(bindings,files=files)

    def test_project_round_trip_preserves_records_evidence_and_missing_state(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);engines=[]
            try:
                for name in ('one','two'):engines.append(Workspace(root/name,baseline=Baseline(),repository=Repository(),publisher=publisher))
                manifest,files=fixture(dispatch=165,opaque=True)
                first=engines[0].inspect('project.zip',project_archive(manifest,files),engines[0].public()['revision'])
                draft=first['draft'];self.assertEqual(1,len(draft['forms']));self.assertEqual(2,len(draft['weapons']))
                self.assertEqual(1,len(draft['native_content_groups']));self.assertFalse(draft['can_confirm'])
                self.assertIn('native_dispatch',{row['code'] for row in draft['requirements']})
                self.assertEqual([],draft['removable_missing_skills'])
                exported=export_project(engines[0],draft)
                with zipfile.ZipFile(io.BytesIO(exported)) as z:
                    self.assertEqual(files['dependencies/9800123.bin'],z.read('dependencies/9800123.bin'))
                    self.assertEqual(files['dependencies/unknown-native.bin'],z.read('dependencies/unknown-native.bin'))
                    self.assertEqual(files['dependencies/source-record.bin'],z.read('dependencies/source-record.bin'))
                    self.assertFalse(json.loads(z.read('project.json'))['runtime_installable'])
                second=engines[1].inspect('again.zip',exported,engines[1].public()['revision'])
                self.assertEqual(draft['native_content'],second['draft']['native_content'])
                self.assertEqual(draft['native_content_groups'],second['draft']['native_content_groups'])
            finally:
                for engine in engines:engine.close()

    def test_hash_failure_does_not_create_a_partial_workspace_draft(self):
        with tempfile.TemporaryDirectory() as directory:
            engine=Workspace(Path(directory),baseline=Baseline(),repository=Repository(),publisher=publisher)
            try:
                manifest,files=fixture();manifest['records'][0]['sha256']='0'*64
                before=engine.read()
                with self.assertRaises(ValueError):engine.inspect('bad.zip',project_archive(manifest,files),engine.public()['revision'])
                self.assertEqual(before,engine.read())
            finally:engine.close()

    def test_attachment_and_explicit_replacement_preserve_primary_records_and_publication(self):
        with tempfile.TemporaryDirectory() as directory:
            engine=Workspace(Path(directory)/'one',baseline=Baseline(),repository=Repository(),publisher=publisher)
            other=Workspace(Path(directory)/'two',baseline=Baseline(),repository=Repository(),publisher=publisher)
            try:
                state=complete(engine,engine.inspect('unit.zip',archive(),engine.public()['revision']))
                state=engine.confirm(state['draft']['id'],{'preview_revision':state['draft']['content_revision'],'shop':True,'capsule':True},state['revision'])
                before=copy.deepcopy(state['draft']);identity=before['id']
                manifest,files=fixture();state=engine.upload_native_content(identity,'data.zip',attachment(manifest,files),state['revision'])
                first=state['draft'];self.assertEqual('draft',first['status']);self.assertEqual(before['published'],first['published'])
                self.assertEqual(before['forms'],first['forms']);self.assertEqual(before['weapons'],first['weapons'])
                self.assertNotIn('confirmation',first);self.assertFalse(first['can_confirm'])
                self.assertFalse(any(row['code']=='resource' and 'example_skill' in row['message'] for row in first['requirements']))
                old=first['native_content'][0]['manifest_path']
                manifest,files=fixture(dispatch=2,opaque=True)
                state=engine.upload_native_content(identity,'updated.zip',attachment(manifest,files),state['revision'],old)
                current=state['draft'];self.assertEqual(1,len(current['native_content']))
                self.assertNotEqual(old,current['native_content'][0]['manifest_path'])
                self.assertEqual(1,current['native_content_groups'][0]['summary']['unsupported_records'])
                self.assertEqual(before['forms'],current['forms']);self.assertEqual(before['published'],current['published'])
                imported=other.inspect('project.zip',export_project(engine,current),other.public()['revision'])
                self.assertEqual(current['native_content_groups'],imported['draft']['native_content_groups'])
                # Portable definitions carry complete values; a receiver does
                # not need the sender's local template selection/provenance.
                self.assertEqual([row['server'] for row in before['forms']],[row['server'] for row in imported['draft']['forms']])
                self.assertEqual([row['source_sha'] for row in before['forms']],[row['source_sha'] for row in imported['draft']['forms']])
            finally:engine.close();other.close()

    def test_bad_or_stale_attachment_keeps_authored_workspace_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            engine=Workspace(Path(directory),baseline=Baseline(),repository=Repository(),publisher=publisher)
            try:
                state=engine.inspect('unit.zip',archive(),engine.public()['revision']);identity=state['draft']['id'];before=engine.read()
                manifest,files=fixture();valid=attachment(manifest,files)
                with self.assertRaises(ValueError):engine.upload_native_content(identity,'data.zip',valid,'stale')
                with self.assertRaises(ValueError):engine.upload_native_content(identity,'data.zip',valid,state['revision'],'missing.json')
                manifest['records'][0]['sha256']='0'*64
                with self.assertRaises(ValueError):engine.upload_native_content(identity,'bad.zip',attachment(manifest,files),state['revision'])
                self.assertEqual(before,engine.read())
            finally:engine.close()


    def test_original_weapon_evidence_does_not_become_a_second_program_binding(self):
        from test_unit_native_programs import fixture as program_fixture
        from owner.units.project import resolve_native_parts
        content,files=fixture();program,program_files,weapons=program_fixture(identity=90010)
        source='dependencies/original-weapon.bin';files[source]=weapons[90010]
        content['evidence'].append({'member':source,'sha256':sha(files[source])})
        bindings,files=bind(content,files)
        files.update(program_files);files['target-weapon.bin']=weapons[90010]
        raw=json.dumps(program).encode();files['program.json']=raw
        parts=resolve_native_parts(files=files,definitions={},native_content=bindings,
            native_programs=[{'manifest_path':'program.json','manifest_sha256':sha(raw)}])
        self.assertEqual(1,len(parts['programs']))
        self.assertIn(source,parts['content'][0]['members'])


if __name__=='__main__':unittest.main()
