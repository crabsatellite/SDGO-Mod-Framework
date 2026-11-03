"""Exercise actual Windows shortcut serialization in isolated local folders."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from manager.entrypoints import create,DESKTOP_NAME,CLIENT_NAME
from manager.workspace import Workspace

ROOT=Path(__file__).resolve().parents[1]

@unittest.skipUnless(os.name=='nt','Windows shortcuts')
class EntrypointTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name).resolve()
        self.desktop=self.root/'desktop';self.desktop.mkdir()
        self.client=self.root/'游戏 客户端';self.client.mkdir();(self.client/'GOnline.exe').write_bytes(b'fixture')
        self.exe=self.root/'SDGO Mod Manager.exe';self.exe.write_bytes(b'fixture')
        self.workspace=Workspace(self.root/'服主 工作区',ROOT/'mods');self.workspace.add_host(self.client)

    def read(self,path):
        command="$s=(New-Object -ComObject WScript.Shell).CreateShortcut($env:SDGO_TEST_LINK);[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false);@{target=$s.TargetPath;arguments=$s.Arguments;description=$s.Description}|ConvertTo-Json -Compress"
        return json.loads(subprocess.check_output(['powershell.exe','-NoProfile','-NonInteractive','-Command',command],
            env=dict(os.environ,SDGO_TEST_LINK=str(path)),encoding='utf-8',timeout=20,creationflags=subprocess.CREATE_NO_WINDOW))

    def test_shortcuts_bind_existing_workspace_and_only_client_roots(self):
        server=self.root/'server';server.mkdir();(server/'BattleServer.exe').write_bytes(b'fixture')
        self.workspace.add_host(server)
        result=create(self.workspace,self.exe,self.desktop)
        self.assertEqual([r['status'] for r in result],['created','created'])
        for path in (self.desktop/DESKTOP_NAME,self.client/CLIENT_NAME):
            link=self.read(path)
            self.assertEqual(Path(link['target']),self.exe)
            self.assertEqual(link['arguments'],'--workspace "'+str(self.workspace.root)+'"')
            self.assertIn('服主',link['description'])
        self.assertFalse((server/CLIENT_NAME).exists())

    def test_existing_usable_shortcuts_retain_their_original_workspace(self):
        create(self.workspace,self.exe,self.desktop)
        paths=(self.desktop/DESKTOP_NAME,self.client/CLIENT_NAME)
        before={p:p.read_bytes() for p in paths}
        other=Workspace(self.root/'another-workspace',ROOT/'mods');other.add_host(self.client)
        result=create(other,self.exe,self.desktop)
        self.assertTrue(all(r['status']=='kept' for r in result))
        self.assertTrue(all(p.read_bytes()==raw for p,raw in before.items()))
        self.assertIn(str(self.workspace.root),self.read(paths[0])['arguments'])

if __name__=='__main__':unittest.main()
