"""Authoring -> approved registration -> shop/capsule selection, without a game DB."""
import base64
import copy
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from contextlib import contextmanager
from unittest.mock import patch

from manager import storage
from manager.providers import attach
from manager.workspace import Workspace
from manager.engines.capsules.capsule_assets import Assets, imported_art
from manager.engines.capsules.capsule_catalog import custom_candidate_allowed
from owner.units.repository import database_target_key
from owner.units.workspace import Workspace as UnitWorkspace
from unit_import_support import Baseline, Repository, archive, complete, png, publisher
from manager_capsule_fixture import fixture_catalog

ROOT = Path(__file__).resolve().parents[1]


@contextmanager
def native_frame_fixture():
    """Only archive donor pixels are synthetic; the real renderer does the work."""
    from PIL import Image
    frame=Image.new('RGBA',(183,107),(12,34,56,255))
    header=Image.new('RGBA',(183,107));header.putpixel((12,12),(200,120,40,255))
    legal=Image.new('RGBA',(183,107));legal.putpixel((12,70),(220,220,220,255))
    with patch.object(Assets,'_native_frame',return_value=frame), \
         patch.object(Assets,'_native_header_overlay',return_value=header), \
         patch.object(Assets,'_native_legal_overlay',return_value=legal):
        yield frame,header,legal


class UnitManagerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.root = Path(self.temp.name)
        self.manager = Workspace(self.root/'manager', ROOT/'mods'); attach(self.manager)
        self.ini = self.root/'world/WorldServer.ini'; self.ini.parent.mkdir()
        self.ini.write_text('[DB Game]\nDBServerName=fixture-server\nDBName=fixture-game\n')
        self.repo = Repository(configured=True)
        apply = self.repo.apply
        self.repo.apply = lambda plan: {**apply(plan), 'server_target': database_target_key(self.ini)}
        self.engine = UnitWorkspace(self.root/'units', baseline=Baseline(), repository=self.repo, publisher=publisher)
        self.units = self.manager.providers['units']; self.units.engine = self.engine
        self.manager.settings['modules']['units'] = {'client': 'fixture-client', 'server_ini': str(self.ini)}
        self.catalog = self.root/'capsules.json'; storage.atomic(self.catalog, storage.json_bytes(fixture_catalog()))
        self.capsules = self.manager.providers['capsules']
        self.capsules.configure({'catalog': str(self.catalog), 'server_ini': str(self.ini)})

    def tearDown(self):
        self.manager.close(); self.temp.cleanup()

    def publish(self, online=True):
        self.repo.configured = online
        state = complete(self.engine, self.engine.inspect('fixture.zip', archive(), self.engine.public()['revision']))
        draft = state['draft']
        return self.engine.confirm(draft['id'], {'preview_revision': draft['content_revision'], 'shop': True, 'capsule': True}, state['revision'])

    def sync(self, revision=None):
        if revision is None: revision = self.capsules.request('GET', '/api/bootstrap', {}, {})['catalog_revision']
        return self.capsules.request('POST', '/api/units/import', {'expected_revision': revision}, {})

    def test_native_dependency_upload_uses_existing_provider_revision_and_stays_private_draft(self):
        from test_unit_native_content import fixture as native_fixture,attachment
        state=self.engine.inspect('unit.zip',archive(),self.engine.public()['revision']);identity=state['draft']['id']
        manifest,files=native_fixture();before=self.catalog.read_bytes()
        payload={'filename':'dependencies.zip','data':base64.b64encode(attachment(manifest,files)).decode()}
        path='/api/units/drafts/'+identity+'/native-content'
        updated=self.units.request('POST',path,payload,{'X-Catalog-Revision':state['revision']})
        self.assertEqual(1,len(updated['draft']['native_content_groups']))
        self.assertFalse(updated['draft']['can_confirm']);self.assertEqual(before,self.catalog.read_bytes())
        with self.assertRaises(ValueError):self.units.request('POST',path,payload,{'X-Catalog-Revision':state['revision']})
        exported=self.units.request('GET','/api/units/drafts/'+identity+'/project',{},{}).data
        import zipfile
        with zipfile.ZipFile(io.BytesIO(exported)) as archive_file:
            project=json.loads(archive_file.read('project.json'))
            self.assertEqual(1,len(project['native_content']));self.assertFalse(project['runtime_installable'])
            self.assertFalse(any(name.endswith('.dll') for name in archive_file.namelist()))

    def test_metadata_only_project_media_is_reused_without_granting_a_prize(self):
        state=self.units.request('POST','/api/units/projects',{'name':'Owner information',
            'metadata':{'description':'Private owner description','aliases':['Owner alias']}},
            {'X-Catalog-Revision':self.engine.public()['revision']})
        state=self.units.request('POST','/api/units/drafts/'+state['draft']['id']+'/asset',
            {'filename':'poster.png','data':base64.b64encode(png()).decode(),'role':'activity_poster'},
            {'X-Catalog-Revision':state['revision']})
        library=self.capsules.request('GET','/api/projects',{}, {})
        project=library['items'][0];media=project['media']['activity_poster']
        self.assertFalse(project['native_ready'])
        app=self.capsules.get_engine();before,_=app.store.read()
        from manager.engines.capsules.capsule_catalog import digest
        result=self.capsules.request('POST','/api/projects/media',{'project_id':project['project_id'],
            'source_revision':project['revision'],'role':'activity_poster','asset_sha':media['asset_sha'],
            'expected_revision':digest(before)}, {})
        after,_=app.store.read()
        self.assertEqual(before['units'],after['units'])
        self.assertEqual(before['fixed_machines'],after['fixed_machines'])
        self.assertEqual(before['custom_machines'],after['custom_machines'])
        self.assertFalse(result['native_units_registered'])
        self.assertEqual([],self.repo.applied)
        entry=after['project_assets'][0]
        self.assertEqual('Private owner description',entry['metadata']['description'])
        from PIL import Image
        with Image.open(io.BytesIO(base64.b64decode(entry['png_base64']))) as image:self.assertEqual((361,322),image.size)
        from manager.engines.capsules.capsule_catalog import export_bundle
        out=self.root/'portable';export_bundle(after,out)
        exported=json.loads((out/'client.json').read_text(encoding='utf-8'))
        self.assertEqual(entry,exported['project_assets'][0])

    def test_stale_project_media_reference_does_not_modify_capsule_catalog(self):
        state=self.units.request('POST','/api/units/projects',{'name':'Owner information'},
            {'X-Catalog-Revision':self.engine.public()['revision']})
        app=self.capsules.get_engine();before,raw=app.store.read()
        from manager.engines.capsules.capsule_catalog import digest
        with self.assertRaisesRegex(ValueError,'变化'):
            self.capsules.request('POST','/api/projects/media',{'project_id':state['draft']['id'],
                'source_revision':'stale','role':'activity_poster','asset_sha':'a'*64,'expected_revision':digest(before)}, {})
        self.assertEqual(raw,app.store.read()[1])

    def activity_fixture(self):
        from owner.units.model import content_revision
        from owner.collection_capsule.client import normalize_activity_cover
        state=self.publish();identity=state['draft']['id'];authored=self.engine.read()
        project=authored['drafts'][identity]
        project['published']['forms'][0]['server']['UnitGradeNumber']=4
        storage.atomic(self.engine.path,storage.json_bytes(authored))
        raw=normalize_activity_cover(png());sha=hashlib.sha256(raw).hexdigest()
        key=project['family_key']+':activity_poster'
        catalog=json.loads(self.catalog.read_bytes())
        catalog['project_assets']=[{'key':key,'project_key':project['family_key'],
            'project_revision':content_revision(project),'name':project['name'],'role':'activity_poster',
            'asset_sha256':sha,'source_asset_sha256':sha,'original_sha256':sha,
            'png_base64':base64.b64encode(raw).decode(),'metadata':{}}]
        catalog['activity_drafts']=[{'schema_version':1,'id':1990001,'project_key':project['family_key'],
            'unit_id':90001,'unit_category_id':1119000,'name':'Fixture activity','enabled':False,
            'status':'draft','changes_applied':False,'artwork_key':key,'artwork_sha256':sha,
            'currency_type':0,'price':10000,'metadata':{}}]
        storage.atomic(self.catalog,storage.json_bytes(catalog))
        return {'expected_revision':self.capsules.request('GET','/api/bootstrap',{}, {})['catalog_revision'],
            'id':1990001,'name':'Fixture activity','pattern_id':'gp_rare','currency_type':0,
            'price':10000,'template_machine_id':1101,
            'pool':[{'category_id':1119000,'amount':1,'entries':20},
                    {'category_id':2220001,'amount':1,'entries':4980},
                    {'category_id':2220002,'amount':1,'entries':5000}]}

    def test_activity_edit_saves_reviewed_portable_pool_without_installing(self):
        body=self.activity_fixture()
        with patch('manager.unit_activity.ActivityRepository') as repository:
            repository.return_value.preflight.return_value={'valid':True}
            result=self.capsules.request('POST','/api/projects/activity',body,{})
            self.assertFalse(result['changes_applied'])
            repository.return_value.apply.assert_not_called()
            reviewed=repository.return_value.preflight.call_args.args[0]
            self.assertEqual(20,reviewed['pool'][0]['entries'])
            self.assertNotIn('artwork_key',reviewed)
        current=self.capsules.request('GET','/api/projects/activity',{}, {})
        self.assertEqual(['gp_rare','token_rare','token_99'],[r['id'] for r in current['patterns']])
        self.assertEqual('gp_rare',current['machines'][0]['pattern_id'])
        saved=self.catalog.read_bytes()
        with self.assertRaises(Exception):self.capsules.request('POST','/api/projects/activity',body,{})
        self.assertEqual(saved,self.catalog.read_bytes())

    def test_activity_number_change_stages_paired_migration_and_preserves_rewards_and_art(self):
        body=self.activity_fixture()
        with patch('manager.unit_activity.ActivityRepository') as repository:
            repository.return_value.preflight.return_value={'valid':True}
            saved=self.capsules.request('POST','/api/projects/activity',body,{})
            repository.return_value.preflight_renumber.return_value={'valid':True,'writes_performed':False}
            app=self.capsules.get_engine();before,_=app.store.read()
            result=self.capsules.request('POST','/api/projects/activity-number',{
                'expected_revision':saved['catalog_revision'],'id':body['id'],'number':94},{})
            after,_=app.store.read();changed=after['activity_machines'][0]
            self.assertEqual(9401,changed['id'])
            for key in ('pool','currency_type','price','artwork_key','artwork_sha256','unit_id','project_key'):
                self.assertEqual(before['activity_machines'][0][key],changed[key],key)
            self.assertEqual(9401,after['activity_drafts'][0]['id'])
            self.assertEqual([{'from_id':1990001,'to_id':9401,'project_key':changed['project_key'],
                              'unit_id':90001}],after['activity_machine_renames'])
            self.assertFalse(result['changes_applied'])
            repository.return_value.renumber.assert_not_called()
            from manager.engines.capsules.capsule_catalog import export_bundle
            export_bundle(after,self.root/'renumber-export')
            exported=json.loads((self.root/'renumber-export/server.json').read_bytes())
            self.assertEqual(after['activity_machine_renames'],exported['activity_machine_renames'])

    def test_pinned_activity_number_is_rejected_without_catalog_change(self):
        body=self.activity_fixture()
        with patch('manager.unit_activity.ActivityRepository') as repository:
            repository.return_value.preflight.return_value={'valid':True}
            saved=self.capsules.request('POST','/api/projects/activity',body,{})
            app=self.capsules.get_engine();before,raw=app.store.read()
            with self.assertRaisesRegex(ValueError,'置顶'):
                self.capsules.request('POST','/api/projects/activity-number',{
                    'expected_revision':saved['catalog_revision'],'id':body['id'],'number':9900},{})
            self.assertEqual(raw,app.store.read()[1])
            repository.return_value.preflight_renumber.assert_not_called()

    def test_invalid_activity_pool_is_rejected_before_native_review_or_catalog_write(self):
        body=self.activity_fixture();saved=self.catalog.read_bytes()
        body['pool'][0]['entries']=19
        with patch('manager.unit_activity.ActivityRepository') as repository:
            with self.assertRaises(ValueError):self.capsules.request('POST','/api/projects/activity',body,{})
            repository.return_value.preflight.assert_not_called()
        self.assertEqual(saved,self.catalog.read_bytes())

    def test_activity_candidates_include_only_imports_published_to_this_host(self):
        body=self.activity_fixture()
        result=self.capsules.request('GET','/api/projects/activity-items?q=90001',{}, {})
        self.assertTrue(any(row['category_id']==1119000 for row in result['items']))
        authored=self.engine.read();draft=next(iter(authored['drafts'].values()))
        draft['published_server_result']['server_target']='another-host'
        storage.atomic(self.engine.path,storage.json_bytes(authored));saved=self.catalog.read_bytes()
        result=self.capsules.request('GET','/api/projects/activity-items?q=90001',{}, {})
        self.assertFalse(any(row['category_id']==1119000 for row in result['items']))
        with patch('manager.unit_activity.ActivityRepository') as repository:
            with self.assertRaisesRegex(ValueError,'当前配置'):
                self.capsules.request('POST','/api/projects/activity',body,{})
            repository.return_value.preflight.assert_not_called()
        self.assertEqual(saved,self.catalog.read_bytes())

    def test_owner_template_parameters_stay_in_authoring_data_and_do_not_change_machines(self):
        before=json.loads(self.catalog.read_bytes())
        self.assertNotIn('activity_preferences',before)
        revision=self.capsules.request('GET','/api/bootstrap',{}, {})['catalog_revision']
        preferences={'schema_version':1,'patterns':{'gp_rare':{'featured_entries':20,'price':10000}}}
        with patch('manager.unit_activity.ActivityRepository') as repository:
            result=self.capsules.request('POST','/api/projects/activity-preferences',
                {'expected_revision':revision,'preferences':preferences},{})
            repository.assert_not_called()
        after=json.loads(self.catalog.read_bytes())
        self.assertEqual(preferences,after.pop('activity_preferences'));self.assertEqual(before,after)
        self.assertFalse(result['changes_applied'])
        from manager.engines.capsules.capsule_catalog import export_bundle
        out=self.root/'portable-preferences';export_bundle(json.loads(self.catalog.read_bytes()),out)
        self.assertEqual(preferences,json.loads((out/'server.json').read_bytes())['activity_preferences'])

    def test_only_published_content_is_visible_and_edits_keep_old_approval(self):
        state = complete(self.engine, self.engine.inspect('fixture.zip', archive(), self.engine.public()['revision']))
        self.assertEqual(self.units.registrations(), [])
        draft = state['draft']
        state = self.engine.confirm(draft['id'], {'preview_revision': draft['content_revision'], 'shop': True, 'capsule': True}, state['revision'])
        before = self.units.registrations()
        self.engine.update(draft['id'], {'name': 'Unapproved edit'}, state['revision'])
        self.assertEqual(before, self.units.registrations())
        self.assertTrue(before[0]['server_applied'])

    def test_sync_preserves_machines_and_is_idempotent(self):
        self.publish(); before = json.loads(self.catalog.read_bytes())
        result = self.sync(); after = json.loads(self.catalog.read_bytes())
        for key in ('fixed_machines', 'custom_machines'): self.assertEqual(before[key], after[key])
        imported = next(u for u in after['units'] if u['id'] == 90001)
        self.assertEqual(imported['unit_import']['category_id'], 1119000)
        self.assertEqual(imported['capsule_sprites'], [])
        image = imported_art(imported); self.assertEqual(image.size, (32,24))
        assets = object.__new__(Assets); assets.units = {90001: imported}
        self.assertEqual(assets.card_provenance(90001), 'native-frame-and-imported-art')
        self.assertEqual(assets._source_art(90001,1).tobytes(),image.tobytes())
        with native_frame_fixture() as (frame,header,legal):
            card=assets.card(90001,1);self.assertEqual(card.size,(183,107))
            self.assertEqual(card.getpixel((0,0)),frame.getpixel((0,0)))
            self.assertEqual(card.getpixel((12,12)),header.getpixel((12,12)))
            self.assertEqual(card.getpixel((12,70)),legal.getpixel((12,70)))
        saved = self.catalog.read_bytes(); revision = result['catalog_revision']
        self.sync(revision); self.assertEqual(saved, self.catalog.read_bytes())
        found = self.capsules.request('GET', '/api/units?q=90001', {}, {})['units'][0]
        self.assertTrue(found['has_asset'])
        from test_collection_client import make_client
        self.capsules.engine.store.client_path=make_client(self.root/'preview-client')
        with native_frame_fixture():preview = self.capsules.request('GET', '/assets/unit/90001.png', {}, {})
        self.assertTrue(preview.data.startswith(b'\x89PNG'))

    def test_synced_unit_can_be_selected_without_rewriting_probability(self):
        self.publish(); state = self.sync()
        catalog = json.loads(self.catalog.read_bytes()); machine = copy.deepcopy(catalog['fixed_machines'][0])
        original_weight = machine['slots'][0]['weight']; machine['slots'][0]['unit_id'] = 90001
        self.capsules.request('POST', '/api/machines/update', {'kind': 'fixed_machines', 'id': 101,
            'expected_revision': state['catalog_revision'], 'machine': machine}, {})
        updated = json.loads(self.catalog.read_bytes())['fixed_machines'][0]
        self.assertEqual(updated['slots'][0]['unit_id'], 90001)
        self.assertEqual(updated['slots'][0]['weight'], original_weight)

    def test_offline_other_server_and_stale_revision_are_rejected(self):
        self.publish(online=False); before = self.catalog.read_bytes()
        with self.assertRaisesRegex(ValueError, 'WorldServer'): self.sync()
        self.assertEqual(before, self.catalog.read_bytes())
        self.publish()
        other = self.root/'other.ini'; other.write_text('[DB Game]\nDBServerName=other\nDBName=other\n')
        self.capsules.configure({'server_ini': str(other)})
        with self.assertRaises(ValueError): self.sync()
        self.capsules.configure({'server_ini': str(self.ini)})
        with self.assertRaises(Exception): self.sync('stale')
        self.assertEqual(before, self.catalog.read_bytes())

    def test_matching_database_in_different_native_ini_is_supported(self):
        self.publish(); store = self.ini.with_name('StoreServer.ini')
        store.write_bytes(self.ini.read_bytes()); self.capsules.configure({'server_ini': str(store)})
        self.assertEqual(self.sync()['unit_import_count'], 1)

    def test_foreign_identity_and_tampered_image_are_rejected(self):
        self.publish(); catalog = json.loads(self.catalog.read_bytes())
        catalog['units'].append({'id': 90001, 'name': 'Foreign', 'rank': 1, 'subgrade': 2, 'grade': 'C'})
        storage.atomic(self.catalog, storage.json_bytes(catalog)); before = self.catalog.read_bytes()
        with self.assertRaisesRegex(ValueError, '90001'): self.sync()
        self.assertEqual(before, self.catalog.read_bytes())
        with self.assertRaises(ValueError): imported_art({'unit_import': {
            'image_png': base64.b64encode(png()).decode(), 'image_sha256': '0'*64}})

    def test_manager_close_releases_existing_authored_workspace(self):
        self.publish(); self.manager.close()
        reopened = UnitWorkspace(self.root/'units', baseline=Baseline(), repository=self.repo, publisher=publisher)
        try: self.assertEqual(len(reopened.read()['drafts']), 1)
        finally: reopened.close()

    def test_capsule_install_defaults_do_not_require_hidden_developer_paths(self):
        from test_collection_client import make_client
        client=make_client(self.root/'client')
        lookup=self.root/'lookup.json';lookup.write_text('{}')
        self.capsules.configure({'install_client':str(client),'client':str(client),'lookup':str(lookup)})
        self.assertIsNotNone(self.capsules.get_engine().store.apply_callback)

    def test_shop_uses_confirmed_image_but_never_creates_products(self):
        self.publish(); shop = self.manager.providers['shop']
        shop.configure({'workspace': str(self.root/'shop'), 'world': str(self.ini.parent)})
        value = {'items': [{'Id': 1119000, 'Name': 'SQL internal name'}], 'catalog': {'products': []}}
        class Bridge:
            def call(self, request): return copy.deepcopy(value)
        with patch.object(self.manager.services, 'resource', return_value=Bridge()):
            result = shop.request('GET', '/api/shop/catalog', {}, {})
        self.assertEqual(result['catalog']['products'], [])
        self.assertIn('/api/providers/units/', result['unit_imports']['1119000']['shop_image_url'])

    def test_shop_serves_generated_published_icon_and_rejects_stale_binding(self):
        from PIL import Image
        from urllib.parse import urlsplit,parse_qs
        state=self.publish();draft=state['draft'];registered=self.units.registrations()[0]
        path=registered['shop_image_url'].removeprefix('/api/providers/units')
        image=self.units.request('GET',path,{},{}).data
        self.assertEqual(Image.open(io.BytesIO(image)).size,(122,122))
        self.assertEqual(hashlib.sha256(image).hexdigest(),registered['shop_image_sha256'])
        self.assertNotEqual(registered['shop_image_sha256'],registered['shop_source_sha256'])
        self.assertEqual(registered['shop_sprite'],'sprs/u90001o.spr')
        self.engine.update(draft['id'],{'name':'Unapproved draft edit'},state['revision'])
        self.assertEqual(self.units.request('GET',path,{},{}).data,image)
        self.assertEqual(self.units.registrations()[0]['shop_image_url'],registered['shop_image_url'])
        query=parse_qs(urlsplit(path).query)
        stale=path.replace(query['image'][0],'0'*64)
        with self.assertRaises(ValueError):self.units.request('GET',stale,{}, {})
        current=self.engine.public(draft['id'])
        self.engine.disable(draft['id'],current['revision'])
        with self.assertRaises(ValueError):self.units.request('GET',path,{}, {})

    def test_custom_imported_common_candidates_keep_high_grade_policy(self):
        self.publish(); self.sync(); catalog = json.loads(self.catalog.read_bytes())
        catalog['custom_candidate_policy'] = {'version': 1, 'native_candidate_unit_ids': []}
        unit = next(u for u in catalog['units'] if u['id'] == 90001)
        self.assertTrue(custom_candidate_allowed(catalog, unit))
        high = copy.deepcopy(unit)
        high.update(rank=4,subgrade=2)
        self.assertFalse(custom_candidate_allowed(catalog, high))
        high['subgrade']=4
        self.assertTrue(custom_candidate_allowed(catalog, high))

    def test_sr_custom_candidate_cannot_be_framed_as_an_abc_fixed_card(self):
        raw=png();unit={'id':25061,'rank':4,'subgrade':4,'grade':'SR','capsule_sprites':[],
            'unit_import':{'image_sha256':hashlib.sha256(raw).hexdigest(),'image_png':base64.b64encode(raw).decode()}}
        assets=object.__new__(Assets);assets.units={25061:unit}
        with self.assertRaisesRegex(ValueError,'verified native A/B/C capsule frame'):assets.card(25061)

    def test_real_capsule_compiler_carries_approved_images_into_overlay(self):
        from test_collection_client import make_client
        from manager.engines.capsules.capsule_client_compile import compile_catalog
        client = make_client(self.root/'compiler-client')
        lookup = self.root/'lookup.json'; lookup.write_text('{}')
        catalog = fixture_catalog(); catalog['custom_machines'] = []
        raw = png(); meta = {'image_png': base64.b64encode(raw).decode(),
                            'image_sha256': hashlib.sha256(raw).hexdigest()}
        for unit in catalog['units']:
            unit['capsule_sprites'] = []; unit['unit_import'] = meta
        with native_frame_fixture(),patch('manager.engines.capsules.capsule_client_compile.patch_capsule_price_defaults',
                   side_effect=lambda data: (data, {'fixture': True})):
            result = compile_catalog(catalog, client, lookup, self.root/'compiled')
        manifest = json.loads((self.root/'compiled/manifest.json').read_bytes())
        self.assertEqual(result['generated_cards'], 12)
        self.assertTrue(all(r['source'] == 'native_frame_and_imported_art' for r in manifest['generated_cards']))
        self.assertTrue((self.root/'compiled/overlay.zpk').is_file())

    def test_icon_approval_records_generated_outputs_without_faking_native_ui(self):
        state=complete(self.engine,self.engine.inspect('fixture.zip',archive(),self.engine.public()['revision']))
        draft=state['draft']
        with patch('owner.units.presentation.capsule_card',return_value=png()):
            response=self.units.request('POST','/api/units/drafts/'+draft['id']+'/confirm',
                {'preview_revision':draft['content_revision'],'shop':True,'capsule':True},
                {'X-Catalog-Revision':state['revision']})
        self.assertEqual(len(self.repo.applied),1)
        self.assertNotIn('native_ui',response['draft']['confirmation'])
        self.assertEqual(response['draft']['confirmation']['presentation_review']['method'],'shared_asset_generators')

    def test_preview_rebuild_does_not_reuse_an_imported_roots_old_card(self):
        import struct
        from types import SimpleNamespace
        from owner.units.native_preview import _capsule_catalog
        from manager.engines.capsules import capsule_client_compile
        row=bytearray(1019);ids=list(range(90001,90007));struct.pack_into('<I',row,5,101)
        struct.pack_into('<I',row,15,1200);struct.pack_into('<6I',row,27,*ids)
        for slot in range(6):
            for variant in range(2):
                path=('sprs/previous'+str(slot)+str(variant)+'.spr').encode('utf-16-le')
                start=59+(slot*2+variant)*60;row[start:start+60]=path.ljust(60,b'\0')
        units={uid:{'UnitModelName':'Fixture','UnitGradeNumber':1,'UnitGradeNumber2':1} for uid in ids}
        raw=png();sha=hashlib.sha256(raw).hexdigest()
        family={'root_unit_id':ids[0],'name':'New artwork','forms':[{'id':ids[0],'server':units[ids[0]]}],
                'presentation':{'capsule':{'asset_sha':sha}}}
        engine=SimpleNamespace(repository=SimpleNamespace(catalog={'units':units}),blob=lambda value:raw)
        with patch('manager.engines.capsules.capsule_catalog.ClientData') as reader, \
             patch('manager.engines.capsules.capsule_catalog.gdt_records',return_value=(1019,[bytes(row)])), \
             patch.object(capsule_client_compile,'compile_catalog') as compile_:
            reader.return_value.read_resource.return_value=b'fixture'
            _capsule_catalog(engine,family,self.root/'client',self.root/'output')
        catalog=compile_.call_args.args[0]
        root=next(unit for unit in catalog['units'] if unit['id']==ids[0])
        self.assertEqual(root['capsule_sprites'],[])
        self.assertEqual(root['unit_import']['image_sha256'],sha)
        other=next(unit for unit in catalog['units'] if unit['id']==ids[1])
        self.assertEqual(len(other['capsule_sprites']),2)

    def test_native_receipts_are_bound_to_revision_nonce_and_staged_resources(self):
        from owner.units.native_preview import status,require_seen,RENDERER_CONTRACT
        from owner.units.model import content_revision
        state=complete(self.engine,self.engine.inspect('fixture.zip',archive(),self.engine.public()['revision']))
        draft=state['draft'];identity=draft['id'];revision=content_revision(self.engine.read()['drafts'][identity])
        client=self.engine.root/'native-previews/fixture/client';runtime=client/'sdgo/mods/unit_catalog';runtime.mkdir(parents=True)
        (runtime/'unit_catalog.dll').write_bytes(b'fixture-code');(runtime/'catalog.bin').write_bytes(b'fixture-records')
        (client/'GPackData.zpk').write_bytes(b'fixture-base')
        (client/'GPackData.zdx').write_bytes(b'fixture-index')
        resource=client/'sprs/candidate.spr';resource.parent.mkdir();resource.write_bytes(b'fixture-sprite')
        digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
        entry={'client':str(client),'content_revision':revision,'nonce':'a'*64,'machine_id':101,
               'renderer_contract':RENDERER_CONTRACT,
               'module_sha256':digest(runtime/'unit_catalog.dll'),'catalog_sha256':digest(runtime/'catalog.bin'),
               'files':{'sprs/candidate.spr':digest(resource),'GPackData.zdx':digest(client/'GPackData.zdx')},'base_archive':[(client/'GPackData.zpk').stat().st_size,(client/'GPackData.zpk').stat().st_mtime_ns]}
        storage.atomic(self.engine.root/'native-previews/index.json',storage.json_bytes({identity:entry}))
        def receipt(role,nonce='a'*64):
            storage.atomic(runtime/f'native-preview-{role}.json',storage.json_bytes({'schema_version':1,'role':role,
                'nonce':nonce,'content_revision':revision,'unit_id':draft['root_unit_id'],'category_id':draft['category_id'],
                'native_logic':True,'detail':'controlled fixture; not rendering proof'}))
        receipt('shop');self.assertFalse(status(self.engine,identity)['ready'])
        receipt('capsule','wrong');self.assertFalse(status(self.engine,identity)['ready'])
        receipt('capsule');self.assertTrue(require_seen(self.engine,identity)['ready'])
        from owner.units.native_preview import cleanup
        self.assertEqual(cleanup(self.engine)['removed_copies'],0)
        self.assertTrue(client.exists())
        (client/'GPackData.zdx').write_bytes(b'changed-index');self.assertFalse(status(self.engine,identity)['ready'])
        (client/'GPackData.zdx').write_bytes(b'fixture-index')
        resource.write_bytes(b'changed');self.assertFalse(status(self.engine,identity)['ready'])
        resource.write_bytes(b'fixture-sprite')
        self.engine.update(identity,{'name':'Changed after native preview'},state['revision'])
        self.assertFalse(status(self.engine,identity)['prepared'])

    def test_cleanup_removes_only_owned_stopped_stale_previews_and_index(self):
        from owner.units.native_preview import cleanup
        state=complete(self.engine,self.engine.inspect('fixture.zip',archive(),self.engine.public()['revision']))
        identity=state['draft']['id'];base=self.engine.root/'native-previews'
        stopped=base/('1'*32);running=base/('2'*32);foreign=base/('3'*32)
        for directory in (stopped,running,foreign):
            (directory/'client').mkdir(parents=True);(directory/'client/GOnline.exe').write_bytes(b'fixture')
        for directory in (stopped,running):
            storage.atomic(directory/'manager-preview-owner.json',storage.json_bytes({'provider':'unit_import','token':directory.name}))
        storage.atomic(base/'index.json',storage.json_bytes({identity:{'client':str(stopped/'client'),'content_revision':'stale'},
            'deleted-family':{'client':str(running/'client'),'content_revision':'stale'}}))
        with patch('manager.packages._running',side_effect=lambda executable:executable==running/'client/GOnline.exe'):
            result=cleanup(self.engine)
        self.assertEqual(result['removed_copies'],1);self.assertFalse(stopped.exists())
        self.assertTrue(running.exists());self.assertTrue(foreign.exists())
        self.assertEqual(json.loads((base/'index.json').read_bytes()),{})


if __name__ == '__main__': unittest.main()
