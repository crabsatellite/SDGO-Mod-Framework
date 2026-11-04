from pathlib import Path
import json,sys,unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'));sys.path.insert(0,str(ROOT/'tests'))
from tools import game_test_unit_scenario as scope
import test_game_test as runner_fixture
game_test=runner_fixture.game_test

class UnitScenarioTests(unittest.TestCase):
    def test_blueprint_book_preview_is_sealed_and_cannot_purchase_or_mix_scopes(self):
        context={'pages_only':True,'play_unit_uid':17}
        value={'schema':1,'capture_frames':True,'blueprint':{'recipe_id':201,'book_uid':2**32+7}}
        result=scope.validate(value,context)
        self.assertEqual(scope.cases(result)[-2:],('pages.blueprint.book','pages.blueprint.recipe'))
        self.assertFalse(any('draw' in name or 'claim' in name or 'synthesis' in name for name in scope.cases(result)))
        settings=scope.settings(result)
        self.assertEqual(settings['BlueprintBookUid'],str(2**32+7))
        self.assertEqual(settings['UnitPresentations'],'0')
        for change in ({'book_uid':True},{'recipe_id':0},{'book_uid':2**63},{'synthesize':True}):
            with self.assertRaises(ValueError):scope.validate(value|{'blueprint':value['blueprint']|change},context)
        for change in ({'portraits':True},{'collection_preview':True},{'activity_draw':True}):
            with self.assertRaises(ValueError):scope.validate(value|change,context)
        with self.assertRaises(ValueError):scope.validate(value,context|{'page_group':'hangar'})

    def test_one_activity_draw_requires_typed_pairs_and_retains_legacy_inputs(self):
        context={'pages_only':True,'play_unit_uid':17}
        old={'schema':1,'capture_frames':True,'portraits':True,'collection_preview':True,
            'activity_target':{'machine_id':7,'price':13,'currency':0}}
        self.assertEqual(scope.validate(old,context),old)
        draw=old|{'activity_draw':True,'reward_pairs':[[1110001,1],[1520001,1],[2220001,2]]}
        result=scope.bind(draw,context)['unit_scenario']
        self.assertEqual(scope.cases(result)[-1],'pages.activity.draw')
        self.assertNotIn('pages.collection.delivery',scope.cases(result))
        self.assertEqual(scope.settings(result)['ActivityRewardAmount2'],'2')
        for change in ({'reward_pairs':[[1110001,1]]},{'reward_pairs':[[1110001,1],[1110001,1]]},
            {'reward_pairs':[[1300001,1],[2220001,1]]},{'reward_pairs':[[1110001,True],[2220001,1]]}):
            with self.assertRaises(ValueError):scope.bind(draw|change,context)
        with self.assertRaises(ValueError):scope.bind(old|{'reward_pairs':draw['reward_pairs']},context)

    def test_custom_activity_target_is_preview_only_and_typed(self):
        context={'pages_only':True,'play_unit_uid':17}
        target={'machine_id':7,'price':13,'currency':2}
        binding=scope.bind({'schema':1,'collection_preview':True,'activity_target':target},context)
        settings=scope.settings(binding['unit_scenario'])
        self.assertEqual(settings['CapsulePreviewMachine'],'7')
        self.assertEqual(settings['CapsulePreviewPrice'],'13')
        self.assertEqual(settings['CapsulePreviewCurrency'],'2')
        self.assertEqual(scope.cases(binding['unit_scenario'])[-1],'pages.collection.preview')
        self.assertNotIn('pages.collection.draw',scope.cases(binding['unit_scenario']))
        for invalid in (target|{'machine_id':True},target|{'price':0},target|{'currency':3}):
            with self.assertRaises(ValueError):scope.bind({'schema':1,'collection_preview':True,'activity_target':invalid},context)
        with self.assertRaises(ValueError):scope.bind({'schema':1,'activity_target':target},context)

    def test_unit_scope_is_bounded_and_has_original_ui_cases(self):
        plan={'pages_only':True,'play_unit_uid':17}
        binding=scope.bind({'schema':1,'capture_frames':True,'portraits':True},plan)
        cases=game_test.required_cases_for_plan({**plan,**binding})
        self.assertEqual(cases[:4],('framework.bootstrap','flow.login','flow.onboarding','setup.active_unit'))
        self.assertIn('pages.unit_index.pose_ready',cases)
        self.assertNotIn('pages.storage.claim',cases)
        self.assertNotIn('pvp.start',cases)
        self.assertEqual(len(cases),11)

    def test_rejects_scope_conflicts_and_untyped_ids(self):
        good={'pages_only':True,'play_unit_uid':17}
        for plan in ({}, {'pages_only':True}, {**good,'collection_draw':True}, {**good,'shop_revision':'x'}):
            with self.assertRaises(ValueError):scope.bind({'schema':1},plan)
        for change in ({'schema':True},{'schema':1,'capture_frames':1},
            {'schema':1,'portraits':True},{'schema':1,'unknown':True},
            {'schema':1,'acquisition':{'kind':2,'machine_id':7,'unit_id':9}},
            {'schema':1,'acquisition':{'kind':1,'machine_id':True,'unit_id':9}},
            {'schema':1,'collection_preview':True,'acquisition':{'kind':1,'machine_id':7,'unit_id':9}}):
            with self.assertRaises(ValueError):scope.bind(change,good)

class SealedUnitScenarioTests(unittest.TestCase):
    setUp=runner_fixture.GameTestRunnerTests.setUp
    tearDown=runner_fixture.GameTestRunnerTests.tearDown
    make_normal_client=runner_fixture.GameTestRunnerTests.make_normal_client
    make_stage=runner_fixture.GameTestRunnerTests.make_stage
    prepare=runner_fixture.GameTestRunnerTests.prepare
    def test_prepares_and_reads_exact_native_unit_settings(self):
        run=self.prepare('unit-pages',pages_only=True,play_unit_uid=17,
            unit_scenario={'schema':1,'capture_frames':True,
                'acquisition':{'kind':3,'machine_id':7,'unit_id':9}})
        meta=game_test.validate_metadata(run)
        public=game_test.read_public_plan(run/'client')
        self.assertEqual(meta['plan']['unit_scenario'],public['unit_scenario'])
        self.assertEqual(game_test.required_cases_for_plan(public)[-2:],('catalog.capsule','pages.unit_acquisition'))
        observer=game_test.read_ini(run/'client/sdgo/mods/game_test_client/mod.ini')
        self.assertEqual(observer['settings']['UnitPagesOnly'],'1')
        self.assertEqual(observer['settings']['UnitAcquisitionUnit'],'9')
        game_test.verify_hashes(meta)

    def test_activity_confirmation_binds_its_own_native_identity_and_price(self):
        target={'machine_id':7,'price':13,'currency':0}
        run=self.prepare('activity-preview',pages_only=True,play_unit_uid=17,
            unit_scenario={'schema':1,'collection_preview':True,'activity_target':target})
        public=game_test.read_public_plan(run/'client')
        self.assertEqual(public['unit_scenario']['activity_target'],target)
        path=run/'client/sdgo/mods/game_test_client/mod.ini'
        config=game_test.read_ini(path);config['settings']['CapsulePreviewPrice']='14';game_test.write_ini(path,config)
        with self.assertRaises(game_test.GameTestError):game_test.validate_metadata(run)

    def test_tampered_scenario_bytes_or_observer_settings_are_rejected(self):
        for field in ('input','settings'):
            run=self.prepare('unit-tamper-'+field,pages_only=True,play_unit_uid=17,unit_scenario={'schema':1})
            if field=='input':
                path=run/'client/sdgo/gametest'/scope.INPUT;path.write_bytes(path.read_bytes()+b' ')
            else:
                path=run/'client/sdgo/mods/game_test_client/mod.ini'
                config=game_test.read_ini(path);config['settings']['UnitPagesOnly']='0';game_test.write_ini(path,config)
            with self.assertRaises(game_test.GameTestError):game_test.validate_metadata(run)

    def test_metadata_cannot_add_or_remove_the_unit_scope(self):
        for present in (False,True):
            run=self.prepare('unit-metadata-'+str(present),pages_only=True,play_unit_uid=17,
                **({'unit_scenario':{'schema':1}} if present else {}))
            path=run/'run.json';metadata=json.loads(path.read_text())
            if present:
                metadata['plan'].pop('unit_scenario');metadata['plan'].pop('unit_scenario_sha256')
            else:metadata['plan'].update(scope.bind({'schema':1},metadata['plan']))
            path.write_text(json.dumps(metadata))
            with self.assertRaises(game_test.GameTestError):game_test.validate_metadata(run)

if __name__=='__main__':unittest.main()
