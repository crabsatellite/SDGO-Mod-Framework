import copy,hashlib,json,tempfile,time,unittest,uuid
from pathlib import Path
from datetime import datetime,timezone
from unittest.mock import patch
from owner.rewards import gathering,model,progression
from owner.rewards.runtime_control import compile_ini
from owner.rewards.workspace import Workspace
from owner.rewards.worker import WalletWorker,ini_bytes,read_job
from test_reward_worker import Ledger

def epoch(value):return int(datetime.fromisoformat(value).replace(tzinfo=timezone.utc).timestamp())
def rule(**changes):return {'id':'meetup','title':'Test meetup','enabled':True,'minimum_humans':3,'period':'week','modes':['pvp','pve'],'gb':12345,'mb':678,**changes}
def config():return {**gathering.defaults(),'enabled':True,'rules':[rule()]}
def job(**changes):return {'schema_version':3,'match_id':'realm.one','completed_unix':epoch('2026-11-01T16:00:00'),
    'active_seconds':180,'contributed':True,'mode':'pvp','participants':[{'active_seconds':180,'contributed':True} for _ in range(3)],**changes}

class GatheringContracts(unittest.TestCase):
    def test_public_defaults_do_not_embed_a_server_economy_or_enable_grants(self):
        self.assertEqual(gathering.defaults()['rules'],[]);self.assertFalse(gathering.defaults()['enabled'])
        self.assertEqual(gathering.awards(job(),gathering.defaults()),[])
        self.assertFalse(progression.defaults()['enabled'])
        self.assertEqual(progression.defaults()['daily_first_mb']+progression.defaults()['daily_mb'],500)
        self.assertNotIn('human_multipliers',model.defaults())

    def test_quorum_counts_only_qualifying_native_humans_and_self_must_qualify(self):
        c=config();self.assertEqual(len(gathering.awards(job(),c)),1)
        for person in ({'active_seconds':179,'contributed':True},{'active_seconds':180,'contributed':False}):
            j=job();j['participants'][2]=person;self.assertEqual(gathering.awards(j,c),[])
        self.assertEqual(gathering.awards(job(active_seconds=179),c),[])
        self.assertEqual(gathering.awards(job(contributed=False),c),[])
        self.assertEqual(gathering.awards(job(schema_version=2),c),[])
        c['rules'][0]['modes']=['pve'];self.assertEqual(gathering.awards(job(mode='pvp'),c),[])
        c['require_contribution']=False
        self.assertEqual(len(gathering.awards(job(mode='pve',contributed=False),c)),1)

    def test_match_day_week_use_captured_time_and_configured_zone(self):
        c=config();c['utc_offset_minutes']=480;r=c['rules'][0]
        self.assertEqual(gathering.awards(job(),c)[0]['period_key'],'2026-11-02')
        self.assertEqual(gathering.awards(job(completed_unix=epoch('2026-11-01T15:59:59')),c)[0]['period_key'],'2026-10-26')
        r['period']='day';self.assertEqual(gathering.awards(job(),c)[0]['period_key'],'2026-11-02')
        r['period']='match';self.assertEqual(gathering.awards(job(),c)[0]['period_key'],'realm.one')

    def test_rules_validate_before_replacing_policy_and_preserve_custom_old_values(self):
        with tempfile.TemporaryDirectory() as root:
            w=Workspace(root);old=w.public();policy=copy.deepcopy(old['policy'])
            policy['progression']={**progression.defaults(),'enabled':True,'daily_first_mb':4321}
            w.save(policy,old['revision']);old=w.public()
            bad=config();bad['rules']+=[rule()]
            before=w.path.read_bytes()
            with self.assertRaises(ValueError):w.save_social_rules({'human_multipliers':[],'gathering':bad},old['revision'])
            self.assertEqual(w.path.read_bytes(),before)
            new=w.save_social_rules({'human_multipliers':['1','1.1']+['1.5']*10,'gathering':config()},old['revision'])
            self.assertEqual(new['policy']['progression']['daily_first_mb'],4321)
            self.assertEqual(new['human_rule'],'configured_table');self.assertIn(b'[humans]',w.runtime.policy_path.read_bytes())
            with self.assertRaises(ValueError):w.save_social_rules({'human_multipliers':[],'gathering':config()},old['revision'])

    def test_v3_native_job_validation_and_worker_commit_ack_replay(self):
        class Repository(Ledger):
            def __init__(self):super().__init__();self.gb=0;self.grants={}
            def current_gp(self,account):return self.gb
            def apply_gathering(self,j,a):
                key=(j['account_number'],a['id'],a['period'],a['period_key'])
                if key not in self.grants:self.gb+=a['gb'];self.balance+=a['mb'];self.grants[key]={'gb':a['gb'],'mb':a['mb']}
                return self.grants[key]
        with tempfile.TemporaryDirectory() as root:
            w=Workspace(root);p=w.read();p['gathering']=config();s=w.save(p,w.public()['revision'])
            repo=Repository();worker=WalletWorker(w.runtime.root,repo,clock=lambda:epoch('2026-11-02T01:00:00'))
            path=worker.outbox/'v3.ini'
            v={'SchemaVersion':3,'MatchId':'native.three','AccountNumber':1001,'Mb':0,'PlanHash':'a'*64,'PolicyRevision':s['revision'],
               'ExpectedScope':repo.scope,'CompletedUnix':epoch('2026-11-01T16:00:00'),'ActiveSeconds':180,'Contributed':1,
               'Mode':'pvp','Participants':'180:1,180:1,180:1'}
            path.write_bytes(ini_bytes('reward',v));self.assertEqual(len(read_job(path)['participants']),3)
            with patch('owner.rewards.worker._atomic_write',side_effect=OSError('lost acknowledgement')):
                with self.assertRaises(OSError):worker.process(path)
            self.assertEqual((repo.gb,repo.balance),(12345,1678))
            self.assertTrue(worker.poll());self.assertEqual((repo.gb,repo.balance),(12345,1678))
            self.assertIn(b'GatheringGb=12345', (worker.receipts/path.name).read_bytes())
            for extra in ({'Participants':'180:1,'},{'Participants':','.join(['180:1']*13)},{'Participants':'180:2'},{'Mode':'invalid'},{'Participants':'179:1,180:0'}):
                path.write_bytes(ini_bytes('reward',{**v,**extra}))
                with self.assertRaises(ValueError):read_job(path)

if __name__=='__main__':unittest.main()
