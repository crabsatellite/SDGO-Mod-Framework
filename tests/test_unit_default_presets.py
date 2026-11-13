import copy
import hashlib
import json
import shutil
import subprocess
from pathlib import Path
import tempfile
import threading
from contextlib import nullcontext
import struct
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from manager.unit_presets import DefaultPresets, SERVICE, register
from owner.units import compiler, preset_install, preset_recipe


class RecipeValidation(unittest.TestCase):
    def setUp(self):
        self.recipe = copy.deepcopy(preset_recipe.load())

    def test_bundled_recipes_have_no_acquisition_or_original_asset_bytes(self):
        self.assertEqual(2, len(self.recipe['presets']))
        self.assertEqual({'creates_shop_products': False, 'creates_gacha_slots': False,
                          'grants_inventory_units': False}, self.recipe['acquisition'])
        self.assertEqual({2, 10, 13}, {row['kind'] for row in self.recipe['templates']})

    def test_duplicate_new_identity_and_missing_template_are_refused(self):
        changed = copy.deepcopy(self.recipe)
        changed['presets'][1]['forms'][0]['id'] = changed['presets'][0]['root_unit_id']
        with self.assertRaisesRegex(ValueError, '重复'):
            preset_recipe.validate(changed)
        self.recipe['templates'].pop(0)
        with self.assertRaisesRegex(ValueError, '模板摘要'):
            preset_recipe.validate(self.recipe)

    def test_dispatch_and_extra_slot_edits_cannot_bypass_composition(self):
        for key, value in [('BattleMode', 1), ('SkillID', 74002), ('unknown', 1)]:
            changed = copy.deepcopy(self.recipe)
            changed['skills'][0]['fields'][key] = value
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, '修改字段'):
                preset_recipe.validate(changed)
        self.recipe['skills'][0]['id'] = 79950
        with self.assertRaisesRegex(ValueError, '普通技能槽'):
            preset_recipe.validate(self.recipe)

    def test_stage_edge_and_native_scalar_types_are_checked_before_reading_assets(self):
        changed = copy.deepcopy(self.recipe)
        changed['stage_rules'][0]['target_unit_id'] = changed['presets'][1]['root_unit_id']
        with self.assertRaisesRegex(ValueError, '阶段规则'):
            preset_recipe.validate(changed)
        self.recipe['presets'][0]['forms'][0]['fields']['HP'] = True
        baseline = Mock()
        with self.assertRaises(ValueError):
            preset_recipe.compose(self.recipe, baseline, {})
        baseline.read.assert_not_called()

    @unittest.skipUnless(shutil.which('node'),'Node.js is required for the ES-module syntax check')
    def test_unit_panel_is_a_valid_browser_module(self):
        path=Path(__file__).resolve().parents[1]/'manager/static/panels/units.js'
        result=subprocess.run([shutil.which('node'),'--input-type=module','--check'],
            input=path.read_text(encoding='utf-8'),text=True,encoding='utf-8',capture_output=True,timeout=45)
        self.assertEqual(0,result.returncode,result.stderr[-1500:])


class DefaultService(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.workspace = SimpleNamespace(settings={}, services=Mock(), save_workspace=Mock())
        self.provider = SimpleNamespace(workspace=self.workspace, config={}, lock=threading.RLock())

    def tearDown(self):
        self.tmp.cleanup()

    def test_default_enablement_and_explicit_opt_out_survive_registration(self):
        controller = register(self.provider)
        self.assertTrue(self.workspace.settings['services'][SERVICE]['enabled'])
        self.workspace.services.register.assert_called_once_with(
            SERVICE, '默认演示机体预设', controller, controller.start)
        self.workspace.settings['services'][SERVICE]['enabled'] = False
        self.workspace.save_workspace.reset_mock()
        register(self.provider)
        self.assertFalse(self.workspace.settings['services'][SERVICE]['enabled'])
        self.workspace.save_workspace.assert_not_called()

    def test_missing_configuration_or_old_programmes_never_touch_sql(self):
        controller = DefaultPresets(self.provider)
        with patch('owner.units.repository.Repository', side_effect=AssertionError('SQL must stay untouched')):
            controller._ensure()
            self.assertEqual('waiting_configuration', controller.state()['status'])
            self.provider.config = {'client': str(self.root), 'server_ini': str(self.root/'world.ini')}
            controller._ensure()
            self.assertEqual('waiting_programmes', controller.state()['status'])
            module = self.root/'sdgo/mods/unit_catalog'
            module.mkdir(parents=True)
            (module/'mod.ini').write_text('[mod]\nversion=0.5.9\n')
            (module/'unit_catalog.dll').write_bytes(b'old programme')
            controller._ensure()
            self.assertEqual('waiting_programmes', controller.state()['status'])

    def test_verified_unchanged_install_does_not_repeat_server_registration(self):
        from owner.units.preset_publication import json_bytes
        module = self.root/'sdgo/mods/unit_catalog'
        module.mkdir(parents=True)
        binary = b'test programme'
        (module/'mod.ini').write_text('[mod]\nversion=0.6.0\n')
        (module/'unit_catalog.dll').write_bytes(binary)
        (self.root/'GOnline.exe').write_bytes(b'test compatible host')
        ini = self.root/'world.ini'
        self.provider.config = {'client': str(self.root), 'server_ini': str(ini)}
        recipe = copy.deepcopy(preset_recipe.load())
        recipe['host_sha256'] = hashlib.sha256((self.root/'GOnline.exe').read_bytes()).hexdigest()
        fingerprint = hashlib.sha256(json_bytes(recipe)+binary+str(ini.resolve()).casefold().encode()).hexdigest()
        controller = DefaultPresets(self.provider)
        controller.verified = fingerprint
        with patch('owner.units.preset_recipe.load', return_value=recipe), \
             patch('owner.units.preset_install.verify', return_value={'fingerprint':fingerprint,'revision':'r1'}), \
             patch('owner.units.batch_server.running_services', side_effect=AssertionError('no repeated server access')):
            controller._ensure()
        self.assertEqual('installed', controller.state()['status'])
        self.assertEqual('r1', controller.state()['revision'])

    def test_default_preset_preflight_reuses_the_selected_clients_parameter_record(self):
        from owner.units import skill_parameters
        module=self.root/'sdgo/mods/unit_catalog';module.mkdir(parents=True)
        (module/'mod.ini').write_text('[mod]\nversion=0.6.2\n')
        (module/'unit_catalog.dll').write_bytes(b'fixture programme')
        (self.root/'GOnline.exe').write_bytes(b'fixture compatible host')
        self.provider.config={'client':str(self.root),'server_ini':str(self.root/'world.ini')}
        self.provider._units_root=lambda:self.root/'workspace'
        recipe=copy.deepcopy(preset_recipe.load());recipe['host_sha256']=hashlib.sha256((self.root/'GOnline.exe').read_bytes()).hexdigest()
        raw=bytearray(1132);struct.pack_into('<I',raw,0,74020);struct.pack_into('<I',raw,284,8000789)
        parameter=bytearray(89);struct.pack_into('<I',parameter,0,8000789)
        selected=SimpleNamespace(record=Mock(return_value=bytes(parameter)))
        publication=SimpleNamespace(revision='r-parameters',records={13:{74020:bytes(raw)}})
        def build(prepared,*args):(prepared/'publication').mkdir(parents=True)
        def preflight(folder,ini,*,baseline=None):
            result=skill_parameters.validate_publication(publication,baseline)
            return {'ready':result['complete'],'problems':[]}
        with patch('owner.units.preset_recipe.load',return_value=recipe), \
             patch('owner.units.preset_install.verify',return_value=None), \
             patch('owner.units.assets.Baseline',return_value=selected), \
             patch('owner.units.repository.Repository',return_value=object()), \
             patch('owner.units.batch_server.running_services',return_value=[]), \
             patch('owner.units.preset_publication.build',side_effect=build), \
             patch('owner.units.batch_server.load_publication',return_value=publication), \
             patch('owner.units.batch_server.preflight',side_effect=preflight)as checked, \
             patch('owner.units.batch_server.apply')as applied, \
             patch('owner.units.preset_install.install'), \
             patch('owner.units.batch_registry.register'):
            controller=DefaultPresets(self.provider);controller._ensure()
        self.assertEqual('installed',controller.state()['status'])
        self.assertIs(checked.call_args.kwargs['baseline'],selected)
        self.assertIs(applied.call_args.kwargs['baseline'],selected)
        selected.record.assert_called_once_with('skill_parameter',8000789)


class ScopedPresetInstall(unittest.TestCase):
    def setUp(self):
        from owner.native_archive import build_archive
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.client = self.root/'client'
        self.module = self.client/'sdgo/mods/unit_catalog'
        self.module.mkdir(parents=True)
        (self.client/'GOnline.exe').write_bytes(b'fixture host')
        (self.module/'unit_catalog.dll').write_bytes(b'fixture unit programme')
        (self.module/'mod.ini').write_bytes(b'[mod]\nenabled=0\n[settings]\nPreserveMe=1\n')
        (self.module/'notes.txt').write_bytes(b'owner authored notes')
        self.primary = self.catalog(29000)
        (self.module/'catalog.bin').write_bytes(self.primary)
        self.publication = SimpleNamespace(root=self.root/'publication', owner='public-demo',
                                           revision='r1', families=[{'root_unit_id':29001}])
        source = self.publication.root/'package/client/sdgo/mods/unit_catalog'
        source.mkdir(parents=True)
        self.source = source
        for name in preset_install.ALLOWED:
            (source/name).write_bytes(b'generated fixture '+name.encode())
        (source/'unit_catalog.dll').write_bytes((self.module/'unit_catalog.dll').read_bytes())
        (source/'catalog.bin').write_bytes(self.catalog(29001))
        for root, path in ((self.module, 'sprs/owner.spr'), (source, 'sprs/demo.spr')):
            zdx, zpk, _ = build_archive({path:b'fixture sprite'}, lambda name:True)
            (root/'resources.zdx').write_bytes(zdx)
            (root/'resources.zpk').write_bytes(zpk)
        self.loaded = patch('owner.units.preset_install.load_publication', return_value=self.publication)
        self.unmapped = patch('manager.operations.unmapped_image', side_effect=lambda path:nullcontext())
        self.loaded.start()
        self.unmapped.start()

    def tearDown(self):
        self.loaded.stop()
        self.unmapped.stop()
        self.tmp.cleanup()

    @staticmethod
    def catalog(identity):
        raw = bytearray(4976)
        struct.pack_into('<I',raw,0,identity)
        return compiler.native_catalog([(2,identity,bytes(raw))])

    def install(self, fingerprint='recipe-1'):
        return preset_install.install(self.client,self.publication.root,fingerprint)

    def test_update_preserves_primary_content_and_cleans_generated_rollback(self):
        original_archive = (self.module/'resources.zpk').read_bytes()
        self.assertTrue(self.install()['changed'])
        self.assertFalse(self.install()['changed'])
        self.publication.revision = 'r2'
        (self.source/'definitions.json').write_bytes(b'revised generated definitions')
        self.assertTrue(self.install('recipe-2')['changed'])
        self.assertEqual(self.primary,(self.module/'catalog.bin').read_bytes())
        self.assertEqual(original_archive,(self.module/'resources.zpk').read_bytes())
        self.assertEqual(b'owner authored notes',(self.module/'notes.txt').read_bytes())
        self.assertIn(b'PreserveMe=1',(self.module/'mod.ini').read_bytes())
        self.assertEqual([],list(self.module.glob('.presets-*')))
        self.assertFalse(preset_install.verify(self.client)['grants_inventory_units'])

    def test_modified_author_data_and_colliding_records_refuse_without_writes(self):
        self.install()
        target = self.module/'presets/definitions.json'
        target.write_bytes(b'owner manual edit')
        before = (self.module/'mod.ini').read_bytes()
        with self.assertRaisesRegex(ValueError,'外部修改'):
            self.install('recipe-2')
        self.assertEqual(b'owner manual edit',target.read_bytes())
        self.assertEqual(before,(self.module/'mod.ini').read_bytes())
        (self.source/'catalog.bin').write_bytes(self.primary)
        with self.assertRaisesRegex(ValueError,'编号冲突'):
            self.install()

    def test_post_install_verification_failure_restores_previous_preset_and_settings(self):
        self.install()
        prior = preset_install.verify(self.client)
        original = (self.module/'presets/definitions.json').read_bytes()
        ini = (self.module/'mod.ini').read_bytes()
        self.publication.revision = 'r2'
        (self.source/'definitions.json').write_bytes(b'new generated version')
        with patch('owner.units.preset_install.verify',side_effect=[prior,ValueError('failed final verify')]):
            with self.assertRaisesRegex(ValueError,'failed final verify'):
                self.install('recipe-2')
        self.assertEqual(original,(self.module/'presets/definitions.json').read_bytes())
        self.assertEqual(ini,(self.module/'mod.ini').read_bytes())
        self.assertEqual(prior,preset_install.verify(self.client))
        self.assertEqual(1,len(list(self.module.glob('.presets-failed-*'))))


if __name__ == '__main__':
    unittest.main()
