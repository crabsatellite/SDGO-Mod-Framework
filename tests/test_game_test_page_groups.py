import json
import unittest

from tests import test_game_test as runner_tests

gt=runner_tests.game_test
HANGAR=('pages.hangar.read','pages.hangar.preview','pages.hangar.cancel')
HANGAR_CONFIRM=('pages.hangar.read','pages.hangar.confirm_gb')
HANGAR_SORT_CONFIRM=('pages.hangar.read','pages.hangar.sort','pages.hangar.confirm_gb')

class PageGroupTests(unittest.TestCase):
    def setUp(self):
        self.fixture=runner_tests.GameTestRunnerTests(methodName='runTest')
        self.fixture.setUp()
        self.addCleanup(self.cleanup_fixture)
        manifest=self.fixture.stage/'client/sdgo/mods/game_test_client/mod.ini'
        data=gt.read_ini(manifest);data['mod']['version']='0.1.27';gt.write_ini(manifest,data)

    def cleanup_fixture(self):
        try:self.fixture.tearDown()
        finally:self.fixture.doCleanups()

    def test_hangar_group_contains_exact_read_only_native_cases(self):
        prepared=self.fixture.prepare('hangar-group',pages_only=True,page_group='hangar')
        plan=json.loads((prepared/'run.json').read_text())
        self.assertEqual(gt.required_cases_for_plan(plan['plan']),gt.FLOW_CASES[:3]+HANGAR)
        root=self.fixture.root/'hangar-group'
        checked=gt.validate_metadata(root)
        self.assertEqual(checked['plan']['page_group'],'hangar')
        public=gt.read_public_plan(root/'client')
        self.assertEqual(public['page_group'],'hangar')
        self.assertNotIn('pages.storage.claim',gt.required_cases_for_plan(public))

    def test_storage_gift_group_claims_once_then_checks_gift_origins(self):
        manifest=self.fixture.stage/'client/sdgo/mods/game_test_client/mod.ini'
        data=gt.read_ini(manifest);data['mod']['version']='0.1.29';gt.write_ini(manifest,data)
        with self.assertRaisesRegex(gt.GameTestError,'0.1.30'):
            self.fixture.prepare('storage-gift-old',pages_only=True,page_group='storage_gift')
        data['mod']['version']='0.1.30';gt.write_ini(manifest,data)
        prepared=self.fixture.prepare('storage-gift',pages_only=True,page_group='storage_gift')
        plan=json.loads((prepared/'run.json').read_text())
        self.assertEqual(
            gt.required_cases_for_plan(plan['plan']),
            gt.FLOW_CASES[:3]+('pages.storage.read','pages.storage.claim','pages.hangar.gift_origin','pages.hangar.gift_plain'),
        )
        public=gt.read_public_plan(self.fixture.root/'storage-gift'/'client')
        self.assertEqual(public['page_group'],'storage_gift')
        self.assertNotIn('pages.hangar.preview',gt.required_cases_for_plan(public))
        with self.assertRaises(gt.GameTestError):
            self.fixture.prepare('storage-gift-gb',pages_only=True,page_group='storage_gift',hangar_confirm_gb=True)

    def test_existing_full_page_group_is_unchanged(self):
        prepared=self.fixture.prepare('all-pages',pages_only=True)
        plan=json.loads((prepared/'run.json').read_text())
        self.assertNotIn('page_group',plan['plan'])
        self.assertIn('pages.storage.claim',gt.required_cases_for_plan(plan['plan']))
        self.assertIn('catalog.capsule',gt.required_cases_for_plan(plan['plan']))

    def test_explicit_gb_confirmation_is_sealed_and_replaces_read_only_preview(self):
        prepared=self.fixture.prepare(
            'hangar-confirm-gb',
            pages_only=True,
            page_group='hangar',
            hangar_confirm_gb=True,
        )
        plan=json.loads((prepared/'run.json').read_text())
        self.assertEqual(
            gt.required_cases_for_plan(plan['plan']),
            gt.FLOW_CASES[:3]+HANGAR_CONFIRM,
        )
        root=self.fixture.root/'hangar-confirm-gb'
        checked=gt.validate_metadata(root)
        self.assertTrue(checked['plan']['hangar_confirm_gb'])
        public=gt.read_public_plan(root/'client')
        self.assertTrue(public['hangar_confirm_gb'])
        cases=gt.required_cases_for_plan(public)
        self.assertNotIn('pages.hangar.preview',cases)
        self.assertNotIn('pages.hangar.cancel',cases)

    def test_sort_is_sealed_before_the_gb_confirmation(self):
        prepared=self.fixture.prepare(
            'hangar-sort-gb',
            pages_only=True,
            page_group='hangar',
            hangar_sort=True,
            hangar_confirm_gb=True,
        )
        plan=json.loads((prepared/'run.json').read_text())
        self.assertEqual(
            gt.required_cases_for_plan(plan['plan']),
            gt.FLOW_CASES[:3]+HANGAR_SORT_CONFIRM,
        )
        public=gt.read_public_plan(self.fixture.root/'hangar-sort-gb'/'client')
        self.assertTrue(public['hangar_sort'] and public['hangar_confirm_gb'])

    def test_live_refresh_checks_require_the_live_refresh_client(self):
        manifest=self.fixture.stage/'client/sdgo/mods/game_test_client/mod.ini'
        data=gt.read_ini(manifest);data['mod']['version']='0.1.26';gt.write_ini(manifest,data)
        for name,options in [('old-sort',dict(hangar_sort=True)),('old-live-gb',dict(hangar_confirm_gb=True))]:
            with self.subTest(name=name),self.assertRaisesRegex(gt.GameTestError,'0.1.27'):
                self.fixture.prepare(name,pages_only=True,page_group='hangar',**options)

    def test_group_rejects_unknown_or_mutating_scope(self):
        for options in [dict(page_group='hangar'),dict(pages_only=True,page_group='storage'),
                        dict(pages_only=True,page_group=''),dict(pages_only=True,page_group='hangar',collection_draw=True)]:
            with self.subTest(options=options),self.assertRaises(gt.GameTestError):
                gt.required_flow_cases('1',**options)
        with self.assertRaises(gt.GameTestError):
            gt.required_flow_cases('1',pages_only=True,page_group='hangar',shop_catalog=True)
        for options in [
            dict(hangar_confirm_gb=True),
            dict(pages_only=True,hangar_confirm_gb=True),
            dict(pages_only=True,page_group='hangar',hangar_confirm_gb=True,oc_panel=True,play_unit_uid=1),
        ]:
            with self.subTest(options=options),self.assertRaises(gt.GameTestError):
                gt.required_flow_cases('1',**options)

    def test_metadata_cannot_change_group_after_sealing(self):
        self.fixture.prepare('tampered-group',pages_only=True,page_group='hangar')
        root=self.fixture.root/'tampered-group'
        path=root/'run.json';data=json.loads(path.read_text())
        del data['plan']['page_group'];path.write_text(json.dumps(data))
        with self.assertRaisesRegex(gt.GameTestError,'sealed page group'):
            gt.validate_metadata(root)

    def test_metadata_cannot_remove_gb_confirmation_after_sealing(self):
        self.fixture.prepare(
            'tampered-confirm-gb',
            pages_only=True,
            page_group='hangar',
            hangar_confirm_gb=True,
        )
        root=self.fixture.root/'tampered-confirm-gb'
        path=root/'client/sdgo/gametest/run.ini'
        data=gt.read_ini(path)
        del data['run']['hangar_confirm_gb']
        gt.write_ini(path,data)
        with self.assertRaises(gt.GameTestError):
            gt.validate_metadata(root)

    def test_native_case_list_cannot_expand_or_shrink(self):
        self.fixture.prepare('case-list',pages_only=True,page_group='hangar')
        root=self.fixture.root/'case-list'
        path=root/'client/sdgo/gametest/run.ini';data=gt.read_ini(path)
        data['run']['required_cases']+=',pages.storage.claim';gt.write_ini(path,data)
        with self.assertRaises(gt.GameTestError):gt.read_public_plan(root/'client')

    def test_old_sdk_rejects_group_before_launch(self):
        manifest=self.fixture.stage/'client/sdgo/mods/game_test_client/mod.ini'
        data=gt.read_ini(manifest);data['mod']['version']='0.1.7';gt.write_ini(manifest,data)
        with self.assertRaisesRegex(gt.GameTestError,'0.1.8'):
            self.fixture.prepare('old-observer',pages_only=True,page_group='hangar')

    def test_old_sdk_rejects_gb_confirmation_before_launch(self):
        manifest=self.fixture.stage/'client/sdgo/mods/game_test_client/mod.ini'
        data=gt.read_ini(manifest);data['mod']['version']='0.1.19';gt.write_ini(manifest,data)
        with self.assertRaisesRegex(gt.GameTestError,'0.1.20'):
            self.fixture.prepare(
                'old-gb-observer',
                pages_only=True,
                page_group='hangar',
                hangar_confirm_gb=True,
            )

if __name__=='__main__':unittest.main()
