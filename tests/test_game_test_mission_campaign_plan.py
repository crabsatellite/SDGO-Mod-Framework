from copy import deepcopy
import json
from pathlib import Path
import tempfile
import unittest
import uuid

from tools import game_test as G
from tools import game_test_mission_campaign as C


class MissionCampaignPlanTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.client = self.root/'client'
        self.client.mkdir()
        self.profile = G.ini_parser()
        self.profile['account'] = {'username':'Campaign','password':'offline','password_format':'plain'}
        self.profile['registration'] = {'state':'existing_account','retry_count':'1','expect_first_fail':'0'}

    def plan(self):
        plan = G.make_plan('127.236.0.1',5001,'1','1','1',1,False,3,None,
                           mission_mode=5,mission_game_mode_id=88074)
        plan.update(network_policy='direct',quiet=True)
        plan['mission_campaign'] = C.validate({'schema':1,'target_npc':89501},plan)
        return plan

    def write(self, plan):
        run_id = str(uuid.uuid4())
        G.write_job(self.client,run_id,plan,self.profile,self.root/'account.ini')
        return run_id

    def metadata(self, plan):
        run_id = self.write(plan)
        game = self.client/G.GAME_EXE
        game.write_bytes(b'not executed')
        return {'schema':G.SCHEMA_VERSION,'run_id':run_id,'suite':G.metadata_suite_for_plan(plan),
                'status':'prepared','created_utc':'offline','source':{'offline':True},
                'template':str(self.root/'template'),'stage':str(self.root/'stage'),'plan':deepcopy(plan),
                'client':{'root':str(self.client.resolve()),'entry':str(game.resolve()),'game':str(game.resolve()),
                    'host_key':G.HOST_KEY,'events':str((self.client/'sdgo/gametest/events.jsonl').resolve()),
                    'stdout':str(self.root/'stdout.log'),'stderr':str(self.root/'stderr.log'),
                    'immutable_hashes':{G.GAME_EXE:G.sha256(game),
                        'sdgo/gametest/run.ini':G.sha256(self.client/'sdgo/gametest/run.ini')},'initial_inputs':{}},
                'execution':None}

    def test_exact_mission_inputs_roundtrip_without_post_seal_changes(self):
        plan = self.plan();self.write(plan)
        self.assertEqual(G.read_public_plan(self.client),plan)
        run = G.read_ini(self.client/'sdgo/gametest/run.ini')['run']
        self.assertEqual(run['mission_campaign'],'1')
        self.assertEqual(run['mission_campaign_target_npc'],'89501')
        self.assertEqual(run['mission_campaign_contact_ms'],'20000')
        self.assertEqual(C.settings(plan['mission_campaign'])['BattlePlayerCombat'],'1')
        self.assertEqual(C.settings(plan['mission_campaign'])['BattlePlayerMissionAdvanceMs'],'20000')
        self.assertEqual(C.settings(plan['mission_campaign'])['BattlePlayerWeaponSlot'],'1')

    def test_partial_unknown_and_noncanonical_run_fields_are_refused(self):
        plan=self.plan();self.write(plan);path=self.client/'sdgo/gametest/run.ini'
        original=path.read_bytes()
        changes=[('mission_campaign_contact_ms','020000'),('mission_campaign_skip','true'),
                 ('mission_campaign','2'),('mission_campaign_teleport','1'),
                 ('mission_campaign_capture',None)]
        for key,value in changes:
            path.write_bytes(original);ini=G.read_ini(path)
            if value is None:del ini['run'][key]
            else:ini['run'][key]=value
            G.write_ini(path,ini)
            with self.subTest(key=key),self.assertRaises(G.GameTestError):G.read_public_plan(self.client)

    def test_imported_campaign_seals_existing_native_hard_wingmen_command(self):
        plan=G.make_plan('127.236.0.1',5001,'1','1','1',1,False,3,None,
                         mission_mode=5,mission_game_mode_id=88074,mission_ai_difficulty='Hard')
        plan.update(network_policy='direct',quiet=True)
        plan['mission_campaign']=C.validate({'target_npc':89501},plan)
        self.write(plan)
        self.assertEqual(G.read_public_plan(self.client),plan)
        self.assertEqual(plan['room_title'],plan['title_nonce']+' !ai hard')
        ini=G.read_ini(self.client/'sdgo/gametest/run.ini')
        ini['run']['mission_ai_difficulty']='Easy'
        G.write_ini(self.client/'sdgo/gametest/run.ini',ini)
        with self.assertRaises(G.GameTestError):G.read_public_plan(self.client)

    def test_metadata_cannot_change_campaign_or_conflate_bool_with_number(self):
        plan=self.plan();meta=self.metadata(plan);path=self.root/'run.json'
        path.write_text(json.dumps(meta),encoding='utf-8')
        self.assertEqual(G.validate_metadata(self.root)['plan'],plan)
        for key,value in [('contact_ms',30000),('capture',1),('schema',True),('target_npc',89501.0)]:
            bad=deepcopy(meta);bad['plan']['mission_campaign'][key]=value
            path.write_text(json.dumps(bad),encoding='utf-8')
            with self.subTest(key=key),self.assertRaises(G.GameTestError):G.validate_metadata(self.root)

    def test_invalid_scope_is_refused_before_any_client_files_are_written(self):
        for patch in [{'target_host':'203.0.113.1'},{'mission_game_mode_id':None},
                      {'mission_behavior_profile':'oc'},{'mission_respawns':'infinite'},
                      {'pages_only':True},{'fps_cap':60}]:
            plan=self.plan();plan.update(patch)
            with self.subTest(patch=patch),self.assertRaises(G.GameTestError):self.write(plan)
        self.assertEqual(list(self.client.iterdir()),[])

    def test_zero_ai_can_capture_native_hit_without_faking_teammates(self):
        plan=self.plan();plan['mission_ai_count']=0
        with self.assertRaisesRegex(ValueError,'three wingmen'):C.validate(plan['mission_campaign'],plan)
        result=C.validate({**plan['mission_campaign'],'tab_capture':False},plan)
        self.assertFalse(result['tab_capture'])

    def test_campaign_input_cannot_supply_synthetic_world_results(self):
        plan=self.plan()
        for key in ('hp','position','phase','mission_result','injected_packet','attack_damage'):
            with self.subTest(key=key),self.assertRaises(ValueError):
                C.validate({'target_npc':89501,key:1},plan)
        path=self.root/'campaign.json'
        path.write_text('{"target_npc":1,"target_npc":2}',encoding='utf-8')
        with self.assertRaisesRegex(ValueError,'duplicate'):C.load(path)

    def test_old_sdk_is_refused_before_copying_or_creating_a_run(self):
        stage=self.root/'stage'
        ini=stage/'client/sdgo/mods/game_test_client/mod.ini'
        ini.parent.mkdir(parents=True)
        ini.write_bytes(b'[mod]\nversion=0.1.39\n')
        target=self.root/'run'
        with self.assertRaisesRegex(G.GameTestError,'0.1.40'):
            G.prepare(self.client,stage,target,target_host='127.236.0.1',mission_mode=5,
                      mission_game_mode_id=88074,mission_campaign={'target_npc':89501})
        self.assertFalse(target.exists())
        self.assertEqual(list(self.client.iterdir()),[])


if __name__=='__main__':unittest.main()
