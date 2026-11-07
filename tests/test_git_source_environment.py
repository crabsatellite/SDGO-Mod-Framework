import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from manager import git_source
from manager.owned_process import run


class GitEnvironment(unittest.TestCase):
    def test_existing_command_config_and_ssh_identity_remain_local(self):
        original={'GIT_CONFIG_COUNT':'1','GIT_CONFIG_KEY_0':'credential.helper',
                  'GIT_CONFIG_VALUE_0':'manager','GIT_SSH_COMMAND':'ssh -i configured-key'}
        with patch.dict(os.environ,original):
            env=git_source.environment()
            self.assertEqual(env['GIT_CONFIG_KEY_0'],'credential.helper')
            self.assertEqual(env['GIT_CONFIG_VALUE_0'],'manager')
            self.assertEqual(env['GIT_SSH_COMMAND'],original['GIT_SSH_COMMAND'])
            self.assertEqual(os.environ['GIT_CONFIG_COUNT'],'1')
            self.assertEqual(env['GIT_CONFIG_COUNT'],'2'if os.name=='nt'else'1')

    @unittest.skipUnless(os.name=='nt','Git for Windows path regression')
    def test_partial_fetch_writes_pack_files_in_a_deep_workspace(self):
        with tempfile.TemporaryDirectory(prefix='manager-long-git-')as temporary:
            root=Path(temporary);source=root/'source';source.mkdir()
            env=git_source.environment()
            def git(directory,*args):
                return subprocess.check_output(['git','-C',str(directory),*args],env=env,stderr=subprocess.PIPE)
            git(source,'init','-b','main')
            git(source,'config','uploadpack.allowFilter','true')
            (source/'entry.txt').write_text('synthetic Git transport record\n',encoding='utf-8')
            git(source,'add','entry.txt')
            git(source,'-c','user.name=Fixture','-c','user.email=fixture@example.invalid',
                '-c','commit.gpgsign=false','-c','core.hooksPath='+str(root/'no-hooks'),
                'commit','-m','Transport fixture')
            expected=git(source,'rev-parse','HEAD').strip()
            target=root/('workspace-'+'a'*70)/('cache-'+'b'*70)/'repository'
            target.mkdir(parents=True)
            self.assertGreater(len(str(target/'objects/pack'/('pack-'+'0'*40+'.promisor'))),260)
            git(target,'init','--bare')
            git(target,'remote','add','origin',source.as_uri())
            fetched=run(['git','fetch','--filter=blob:none','--depth=1','--progress','origin','main'],
                        cwd=target,env=env,timeout=60,progress=lambda block:None)
            self.assertEqual(fetched.returncode,0,fetched.stderr.decode('utf-8','replace'))
            self.assertEqual(git(target,'rev-parse','FETCH_HEAD').strip(),expected)


if __name__=='__main__':unittest.main()
