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
