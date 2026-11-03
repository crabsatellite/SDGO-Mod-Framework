from pathlib import Path
import json,shutil,sys,tempfile,unittest
from unittest import mock
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'));sys.path.insert(0,str(ROOT/'tests'))
from tools import game_test_capsule_package as packages
from manager.engines.capsules import capsule_deployment as deployment
import test_game_test as runner_fixture
game_test=runner_fixture.game_test

class CapsuleCandidateTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.source=self.root/'package';self.stage=self.root/'stage';self.client=self.root/'client'
        files={}
        for name in (*packages.PROGRAMS,'sdgo/mods/capsule_catalog/data/0099.gdt'):
            path=self.source/'player/client'/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(('fixture:'+name).encode())
            files[name]=packages.sha(path)
            if name in packages.PROGRAMS:
                target=self.stage/'client'/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,target)
        (self.source/'manifest.json').write_text(json.dumps({'catalog_revision':'a'*64}))
        (self.source/'player/package-manifest.json').write_text(json.dumps({'files':files}))
        self.guard=mock.patch.object(deployment,'_validate_package');self.guard.start();self.addCleanup(self.guard.stop)

    def fake_installer(self,command,**unused):
        self.command=command
        shutil.copytree(self.source/'player/client',self.client,dirs_exist_ok=True)
        return mock.Mock(returncode=0,stdout='',stderr='')

    def test_uses_trusted_installer_and_reads_back_installed_data(self):
        binding=packages.validate(self.source,self.stage)
        with mock.patch.object(packages.subprocess,'run',side_effect=self.fake_installer):
            receipt=packages.install(binding,self.client)
        self.assertEqual(receipt['manifest_sha256'],binding['manifest_sha256'])
        script=Path(self.command[self.command.index('-File')+1])
        self.assertEqual(script.resolve(),ROOT/'manager/engines/capsules/install_capsule_client.ps1')
        self.assertFalse(script.is_relative_to(self.source))

    def test_stage_mismatch_cannot_downgrade_candidate_programs(self):
        (self.stage/'client/sdgo/core.dll').write_bytes(b'newer candidate')
        with self.assertRaisesRegex(ValueError,'differs from the selected candidate stage'):
            packages.validate(self.source,self.stage)

    def test_changed_publication_is_rejected_before_installer_execution(self):
        binding=packages.validate(self.source,self.stage)
        (self.source/'manifest.json').write_text(json.dumps({'catalog_revision':'b'*64}))
        with mock.patch.object(packages.subprocess,'run') as execute:
            with self.assertRaisesRegex(ValueError,'changed before installation'):packages.install(binding,self.client)
            execute.assert_not_called()

    def test_success_exit_does_not_replace_resource_readback(self):
        binding=packages.validate(self.source,self.stage)
        def incomplete(command,**options):
            result=self.fake_installer(command,**options)
            (self.client/'sdgo/mods/capsule_catalog/data/0099.gdt').write_bytes(b'incomplete')
            return result
        with mock.patch.object(packages.subprocess,'run',side_effect=incomplete):
            with self.assertRaisesRegex(ValueError,'installed capsule candidate differs'):packages.install(binding,self.client)

class PreparedCapsuleTests(unittest.TestCase):
    setUp=runner_fixture.GameTestRunnerTests.setUp
    tearDown=runner_fixture.GameTestRunnerTests.tearDown
    make_normal_client=runner_fixture.GameTestRunnerTests.make_normal_client
    make_stage=runner_fixture.GameTestRunnerTests.make_stage
    prepare=runner_fixture.GameTestRunnerTests.prepare

    def test_capsule_installation_precedes_runtime_sealing(self):
        binding={'path':str(self.root/'publication'),'stage':str(self.stage),'revision':'a'*64,'manifest_sha256':'b'*64}
        def install(actual,client):
            self.assertEqual(actual,binding)
            marker=client/'sdgo/mods/capsule_catalog/approved.bin'
            marker.parent.mkdir(parents=True);marker.write_bytes(b'approved runtime')
            return {key:value for key,value in binding.items() if key!='stage'}
        with mock.patch.object(packages,'validate',return_value=binding),mock.patch.object(packages,'install',side_effect=install):
            run=self.prepare('capsule-before-seal',pages_only=True,capsule_package=self.root/'publication')
        metadata=game_test.validate_metadata(run)
        self.assertEqual(metadata['capsule_package']['revision'],'a'*64)
        self.assertIn('sdgo/mods/capsule_catalog/approved.bin',metadata['client']['immutable_hashes'])
        game_test.verify_hashes(metadata)

if __name__=='__main__':unittest.main()
