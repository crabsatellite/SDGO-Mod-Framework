import json,shutil,tempfile,unittest
from pathlib import Path
from manager.workspace import Workspace
from manager.newcomer_protection import NewcomerProtection
from manager import storage

ROOT=Path(__file__).resolve().parents[1]

class NewcomerProtectionTests(unittest.TestCase):
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory();self.root=Path(self.temporary.name)
        self.host=self.root/'battle';self.host.mkdir();(self.host/'BattleServer.exe').write_bytes(b'fixture only')
        shutil.copytree(ROOT/'mods/newcomer_protection',self.host/'sdgo/mods/newcomer_protection')
        self.workspace=Workspace(self.root/'manager',ROOT/'mods');self.workspace.add_host(self.host)
        self.provider=NewcomerProtection(self.workspace);self.identity=self.workspace.settings['hosts'][0]['id']
    def tearDown(self):
        self.workspace.close();self.temporary.cleanup()
    def save(self,accounts,health=2,attack=2,revisions=None):
        details=self.workspace.details(self.identity,'newcomer_protection')
        return self.provider.request('POST','/api/newcomer-protection/policy',{'host':self.identity,'revisions':details['revisions']if revisions is None else revisions,'values':{'enabled':True,'health_multiplier':health,'attack_multiplier':attack,'protected_accounts':accounts}}, {})
    def test_private_policy_and_restart_contract(self):
        saved=self.save([17,7,17],1.5,2.5)
        self.assertEqual(saved['values']['protected_accounts'],'7,17')
        self.assertEqual(saved['activation'],'restart_required')
        self.assertEqual(saved['values']['health_multiplier'],1.5)
        self.assertEqual(saved['values']['attack_multiplier'],2.5)
        default=storage.ini_values((ROOT/'mods/newcomer_protection/mod.ini').read_bytes())
        self.assertEqual(default.get('settings','ProtectedAccounts'),'')
        self.assertEqual(default.get('settings','Enabled'),'0')
        self.assertFalse((ROOT/'mods/newcomer_protection/config.ini').exists())
    def test_bad_policy_preserves_previous_file(self):
        self.save([7]);path=self.host/'sdgo/mods/newcomer_protection/config.ini';before=path.read_bytes()
        for accounts,health,attack in [([True],2,2),([0],2,2),([2147483648],2,2),([7],0,2),([7],2,float('nan')),([7],11,2)]:
            with self.assertRaises(ValueError):self.save(accounts,health,attack)
            self.assertEqual(path.read_bytes(),before)
    def test_revision_conflict_does_not_replace_roster(self):
        revisions=self.workspace.details(self.identity,'newcomer_protection')['revisions']
        self.save([7])
        with self.assertRaises(storage.Conflict):self.save([17],revisions=revisions)
        self.assertEqual(self.workspace.details(self.identity,'newcomer_protection',internal=True)['values']['protected_accounts'],'7')
    def test_generic_user_settings_cannot_write_hidden_roster(self):
        details=self.workspace.details(self.identity,'newcomer_protection')
        self.assertNotIn('protected_accounts',details['values'])
        with self.assertRaises(ValueError):self.workspace.configure(self.identity,'newcomer_protection',{'protected_accounts':'7'},details['revisions'])
        self.save([7])
        self.assertEqual(self.workspace.mode,'standard')
    def test_native_parser_and_manager_have_same_roster_limit(self):
        with self.assertRaises(ValueError):self.save(list(range(1,258)))

if __name__=='__main__':unittest.main()
