from pathlib import Path
import json,sys,unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'));sys.path.insert(0,str(ROOT/'tests'))
from tools import game_test_unit_scenario as scope
import test_game_test as runner_fixture
game_test=runner_fixture.game_test

class UnitScenarioTests(unittest.TestCase):
    def test_detail_only_uses_real_list_detail_and_close_without_form_changes(self):
        value=scope.bind({'schema':1,'presentation_target':10002,'detail_only':True},
                         {'pages_only':True,'play_unit_uid':17})['unit_scenario']
        self.assertEqual(scope.cases(value),('framework.bootstrap','flow.login','flow.onboarding',
            'setup.active_unit','pages.unit_index.list','pages.unit_index.detail','pages.unit_index.close'))
        self.assertEqual(scope.settings(value)['UnitPresentationDetailOnly'],'1')
        for extra in ({'portraits':True,'capture_frames':True},{'collection_preview':True},{'detail_only':False}):
            with self.assertRaises(ValueError):scope.bind({'schema':1,'detail_only':True,**extra},
                {'pages_only':True,'play_unit_uid':17})
    def test_early_navigation_is_sealed_first_page_action_with_basic_recovery_suite(self):
        context={'pages_only':True,'play_unit_uid':17}
        for target in ('catalog','capsule','room'):
            with self.subTest(target=target):
                value={'schema':1,'early_navigation':target}
                bound=scope.bind(value,context)['unit_scenario']
                self.assertEqual(scope.settings(bound)['EarlyNavigationTarget'],target)
                cases=scope.cases(bound)
                self.assertEqual(cases[:5],(
                    'framework.bootstrap','flow.login','flow.onboarding',
                    'pages.loading_navigation','setup.active_unit'))
                self.assertEqual(cases[-1],'pages.unit_index.close')

    def test_early_navigation_rejects_unsealed_targets_and_mixed_page_scopes(self):
        context={'pages_only':True,'play_unit_uid':17}
        for target in ('',0,True,'hangar','Catalog'):
            with self.assertRaises(ValueError):
                scope.bind({'schema':1,'early_navigation':target},context)
        base={'schema':1,'early_navigation':'catalog'}
        incompatible=(
            {'portraits':True,'capture_frames':True},
            {'collection_preview':True},
            {'acquisition':{'kind':1,'machine_id':7,'unit_id':9}},
            {'blueprint':{'recipe_id':301,'book_uid':45}},
            {'native_record_lifecycle':{'ids':[198131]}},
            {'native_id_probe':{'ids':[35000],'witness_unit_id':10003,'require_absent':True}},
        )
        for extra in incompatible:
            with self.assertRaises(ValueError):
                scope.bind(base|extra,context)
        with self.assertRaises(ValueError):
            scope.bind(base,context|{'page_group':'hangar'})

    def test_pre_inventory_navigation_is_sealed_inside_onboarding(self):
        context={'pages_only':True,'play_unit_uid':17}
        for target in ('catalog','capsule','room'):
            with self.subTest(target=target):
                value={'schema':1,'early_navigation_before_inventory':target}
                bound=scope.bind(value,context)['unit_scenario']
                settings=scope.settings(bound)
                self.assertEqual(settings['EarlyNavigationTarget'],target)
                self.assertEqual(settings['EarlyNavigationBeforeInventory'],'1')
                cases=scope.cases(bound)
                self.assertEqual(cases[:4],(
                    'framework.bootstrap','flow.login','flow.onboarding',
                    'setup.active_unit'))
                self.assertNotIn('pages.loading_navigation',cases)
        with self.assertRaises(ValueError):
            scope.bind({
                'schema':1,
                'early_navigation':'capsule',
                'early_navigation_before_inventory':'capsule',
            },context)

    def test_explicit_encyclopedia_target_is_sealed_separately_from_owned_unit(self):
        context={'pages_only':True,'play_unit_uid':839}
        value={'schema':1,'presentation_target':22150}
        bound=scope.bind(value,context)['unit_scenario']
        self.assertEqual(scope.settings(bound)['UnitPresentationTarget'],'22150')
        self.assertIn('pages.unit_index.detail',scope.cases(bound))
        for target in [0,-1,True,2**31,'22150']:
            with self.assertRaises(ValueError):scope.bind(value|{'presentation_target':target},context)

    def test_native_record_lifecycle_is_sealed_bounded_and_does_not_add_gameplay_actions(self):
        context={'pages_only':True,'play_unit_uid':17}
        value={'schema':1,'native_record_lifecycle':{'ids':[9_100_001,198131]}}
        bound=scope.bind(value,context)['unit_scenario']
        self.assertEqual([198131,9_100_001],bound['native_record_lifecycle']['ids'])
        self.assertEqual(('framework.bootstrap','flow.login','flow.onboarding','setup.active_unit'),scope.cases(bound))
        settings=scope.settings(bound)
        self.assertEqual('2',settings['NativeRecordLifecycleCount'])
        self.assertEqual('198131,9100001',settings['NativeRecordLifecycleIds'])
        self.assertEqual('0',settings['UnitPresentations'])
        for ids in ([],[1,1],[True],[0],[10_000_000],list(range(1,18))):
            with self.assertRaises(ValueError):
                scope.bind({'schema':1,'native_record_lifecycle':{'ids':ids}},context)
        for change in ({'blueprint':{'recipe_id':301,'book_uid':45}}, {'portraits':True,'capture_frames':True},
                       {'native_id_probe':{'ids':[11],'witness_unit_id':10003,'require_absent':True}},
                       {'collection_preview':True}):
            with self.assertRaises(ValueError):scope.bind(value|change,context)

    def test_native_id_probe_is_bounded_explicit_and_separate_from_game_actions(self):
        context={'pages_only':True,'play_unit_uid':17}
        value={'schema':1,'native_id_probe':{'ids':[9000001,35001,35000],
            'witness_unit_id':10003,'require_absent':True}}
        scope_value=scope.bind(value,context)['unit_scenario']
        self.assertEqual(scope_value['native_id_probe']['ids'],[35000,35001,9000001])
        self.assertEqual(scope.cases(scope_value),('framework.bootstrap','flow.login','flow.onboarding','setup.active_unit','catalog.native_id_occupancy'))
        settings=scope.settings(scope_value)
        self.assertEqual(settings['NativeIdProbeChunk0'],'35000,35001,9000001')
        self.assertEqual(settings['NativeIdProbeChunks'],'1')
        self.assertEqual(settings['UnitPresentations'],'0')
        for change in ({'ids':[]},{'ids':[1,1]},{'ids':[True]},{'ids':[10_000_000]},
                       {'ids':list(range(1,3074))},{'witness_unit_id':0},{'require_absent':1}):
            with self.assertRaises(ValueError):scope.bind(value|{'native_id_probe':value['native_id_probe']|change},context)
        for change in ({'portraits':True,'capture_frames':True},{'collection_preview':True},
                       {'blueprint':{'recipe_id':5,'book_uid':6}}):
            with self.assertRaises(ValueError):scope.bind(value|change,context)

    def test_blueprint_synthesis_requires_explicit_consumption_scope_and_output(self):
        context={'pages_only':True,'play_unit_uid':17}
        blueprint={'recipe_id':301,'book_uid':45,'materials':[{'unit_id':201+i,'uid':71+i} for i in range(3)]}
        value={'schema':1,'blueprint':blueprint|{'synthesize':True,'output_unit_id':299}}
        bound=scope.bind(value,context)['unit_scenario']
        self.assertEqual(scope.cases(bound)[-1],'pages.blueprint.synthesis')
        self.assertEqual(scope.settings(bound)['BlueprintSynthesize'],'1')
        for patch in ({'synthesize':False},{'synthesize':1},{'output_unit_id':True},{'materials':[]}):
            with self.assertRaises(ValueError):scope.bind(value|{'blueprint':value['blueprint']|patch},context)

    def test_blueprint_materials_bind_exact_native_instances_without_synthesis(self):
        context={'pages_only':True,'play_unit_uid':17}
        materials=[{'unit_id':201+i,'uid':2**32+i+1} for i in range(3)]
        value={'schema':1,'blueprint':{'recipe_id':301,'book_uid':45,'materials':materials}}
        result=scope.bind(value,context)['unit_scenario']
        self.assertEqual(scope.cases(result)[-1],'pages.blueprint.materials')
        settings=scope.settings(result)
        self.assertEqual(settings['BlueprintMaterialCount'],'3')
        self.assertEqual(settings['BlueprintMaterialUid2'],str(2**32+3))
        self.assertFalse(any('synthesis' in name for name in scope.cases(result)))
        invalids=[[],materials[:2],materials+[materials[0]],materials*2,
                  [materials[0]|{'uid':True},*materials[1:]],
                  [materials[0]|{'record':bytes(153).hex()},*materials[1:]]]
        for invalid in invalids:
            with self.assertRaises(ValueError):scope.bind(value|{'blueprint':value['blueprint']|{'materials':invalid}},context)

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

    def test_acquisition_ui_preview_is_an_explicit_read_only_page_scope(self):
        plan={'pages_only':True,'play_unit_uid':17}
        acquisition={'kind':3,'machine_id':7,'unit_id':9,'ui_preview_only':True}
        value=scope.bind({'schema':1,'capture_frames':True,'acquisition':acquisition},plan)['unit_scenario']
        self.assertEqual(scope.cases(value),(
            'framework.bootstrap','flow.login','flow.onboarding',
            'setup.active_unit','pages.unit_acquisition'))
        settings=scope.settings(value)
        self.assertEqual(settings['UnitAcquisitionUiPreviewOnly'],'1')
        self.assertEqual(settings['UnitPresentations'],'0')
        self.assertNotIn('catalog.capsule',scope.cases(value))
        self.assertFalse(any('unit_index' in case or 'draw' in case for case in scope.cases(value)))
        normal=scope.bind({'schema':1,'acquisition':{
            'kind':3,'machine_id':7,'unit_id':9}},plan)['unit_scenario']
        self.assertEqual(scope.cases(normal)[-2:],('catalog.capsule','pages.unit_acquisition'))
        self.assertEqual(scope.settings(normal)['UnitAcquisitionUiPreviewOnly'],'0')
        self.assertEqual(scope.settings(normal)['UnitPresentations'],'1')

    def test_acquisition_ui_preview_rejects_false_or_mixed_scopes(self):
        plan={'pages_only':True,'play_unit_uid':17}
        acquisition={'kind':3,'machine_id':7,'unit_id':9,'ui_preview_only':True}
        with self.assertRaises(ValueError):
            scope.bind({'schema':1,'acquisition':acquisition|{'ui_preview_only':False}},plan)
        for extra in (
            {'portraits':True,'capture_frames':True},
            {'presentation_target':9},
            {'collection_preview':True},
            {'blueprint':{'recipe_id':301,'book_uid':45}},
            {'native_id_probe':{'ids':[35000],'witness_unit_id':10003,'require_absent':True}},
        ):
            with self.assertRaises(ValueError):
                scope.bind({'schema':1,'acquisition':acquisition}|extra,plan)
        with self.assertRaises(ValueError):
            scope.bind({'schema':1,'acquisition':acquisition},plan|{'page_group':'hangar'})

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
    def test_historical_non_probe_capsules_keep_their_original_sealed_defaults(self):
        from unittest.mock import patch
        previous={key:value for key,value in scope.SETTINGS.items()
            if key not in ('EarlyNavigationTarget','EarlyNavigationBeforeInventory',
                           'UnitAcquisitionUiPreviewOnly') and
            not key.startswith(('NativeIdProbe','NativeRecordLifecycle'))}
        with patch.dict(scope.SETTINGS,previous,clear=True):
            run=self.prepare('historical-unit-scope',pages_only=True,play_unit_uid=17,unit_scenario={'schema':1})
        metadata=game_test.validate_metadata(run)
        self.assertNotIn('native_id_probe',metadata['plan']['unit_scenario'])
        game_test.verify_hashes(metadata)

    def test_large_candidate_set_is_sealed_in_bounded_chunks_and_tampering_is_rejected(self):
        ids=list(range(9000001,9000001+2587))
        scenario={'schema':1,'native_id_probe':{'ids':ids,'witness_unit_id':10003,'require_absent':True}}
        run=self.prepare('large-native-id-probe',pages_only=True,play_unit_uid=17,unit_scenario=scenario)
        meta=game_test.validate_metadata(run)
        self.assertEqual(meta['plan']['unit_scenario']['native_id_probe']['ids'],ids)
        self.assertEqual(game_test.required_cases_for_plan(meta['plan'])[-1],'catalog.native_id_occupancy')
        ini=run/'client/sdgo/mods/game_test_client/mod.ini';config=game_test.read_ini(ini)
        self.assertEqual(config['settings']['NativeIdProbeCount'],'2587')
        self.assertEqual(config['settings']['NativeIdProbeChunks'],'11')
        config['settings']['NativeIdProbeChunk10']='9000001';game_test.write_ini(ini,config)
        with self.assertRaises(game_test.GameTestError):game_test.validate_metadata(run)

    def test_synthesis_seals_material_instances_and_cannot_be_enabled_after_prepare(self):
        scenario={'schema':1,'blueprint':{'recipe_id':301,'book_uid':45,
            'materials':[{'unit_id':201+i,'uid':71+i} for i in range(3)],
            'synthesize':True,'output_unit_id':299}}
        run=self.prepare('blueprint-synthesis',pages_only=True,play_unit_uid=17,unit_scenario=scenario)
        meta=game_test.validate_metadata(run)
        self.assertEqual(game_test.required_cases_for_plan(meta['plan'])[-1],'pages.blueprint.synthesis')
        ini=run/'client/sdgo/mods/game_test_client/mod.ini'
        config=game_test.read_ini(ini)
        self.assertEqual(config['settings']['BlueprintMaterialUid2'],'73')
        config['settings']['BlueprintMaterialUid2']='74';game_test.write_ini(ini,config)
        with self.assertRaises(game_test.GameTestError):game_test.validate_metadata(run)

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

    def test_prepares_and_seals_read_only_acquisition_ui_preview(self):
        scenario={'schema':1,'capture_frames':True,'acquisition':{
            'kind':3,'machine_id':7,'unit_id':9,'ui_preview_only':True}}
        run=self.prepare('unit-acquisition-ui-preview',pages_only=True,
            play_unit_uid=17,unit_scenario=scenario)
        meta=game_test.validate_metadata(run)
        self.assertEqual(game_test.required_cases_for_plan(meta['plan']),(
            'framework.bootstrap','flow.login','flow.onboarding',
            'setup.active_unit','pages.unit_acquisition'))
        path=run/'client/sdgo/mods/game_test_client/mod.ini'
        config=game_test.read_ini(path)
        self.assertEqual(config['settings']['UnitAcquisitionUiPreviewOnly'],'1')
        self.assertEqual(config['settings']['UnitPresentations'],'0')
        config['settings']['UnitAcquisitionUiPreviewOnly']='0'
        game_test.write_ini(path,config)
        with self.assertRaises(game_test.GameTestError):
            game_test.validate_metadata(run)

    def test_prepares_and_seals_each_early_navigation_target(self):
        for target in ('catalog','capsule','room'):
            with self.subTest(target=target):
                run=self.prepare('early-navigation-'+target,pages_only=True,play_unit_uid=17,
                    unit_scenario={'schema':1,'early_navigation':target})
                meta=game_test.validate_metadata(run)
                self.assertEqual(
                    game_test.required_cases_for_plan(meta['plan'])[:5],
                    ('framework.bootstrap','flow.login','flow.onboarding',
                     'pages.loading_navigation','setup.active_unit'))
                path=run/'client/sdgo/mods/game_test_client/mod.ini'
                config=game_test.read_ini(path)
                self.assertEqual(config['settings']['EarlyNavigationTarget'],target)
                config['settings']['EarlyNavigationTarget']='off'
                game_test.write_ini(path,config)
                with self.assertRaises(game_test.GameTestError):
                    game_test.validate_metadata(run)

    def test_prepares_and_seals_pre_inventory_navigation_timing(self):
        run=self.prepare(
            'pre-inventory-navigation-capsule',
            pages_only=True,
            play_unit_uid=17,
            unit_scenario={
                'schema':1,
                'early_navigation_before_inventory':'capsule',
            })
        meta=game_test.validate_metadata(run)
        self.assertEqual(
            game_test.required_cases_for_plan(meta['plan'])[:4],
            ('framework.bootstrap','flow.login','flow.onboarding',
             'setup.active_unit'))
        self.assertNotIn(
            'pages.loading_navigation',
            game_test.required_cases_for_plan(meta['plan']))
        path=run/'client/sdgo/mods/game_test_client/mod.ini'
        config=game_test.read_ini(path)
        self.assertEqual(config['settings']['EarlyNavigationTarget'],'capsule')
        self.assertEqual(
            config['settings']['EarlyNavigationBeforeInventory'],'1')
        config['settings']['EarlyNavigationBeforeInventory']='0'
        game_test.write_ini(path,config)
        with self.assertRaises(game_test.GameTestError):
            game_test.validate_metadata(run)

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
