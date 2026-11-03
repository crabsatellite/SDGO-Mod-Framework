"""Exercise actual Windows shortcut serialization in isolated local folders."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import struct
from manager.entrypoints import create,CLIENT_NAME,ShellLink
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
        # Independently inspect MS-SHLLINK Unicode StringData. WScript shortcut
        # automation corrupts non-ANSI paths on English Windows installations.
        raw=path.read_bytes();self.assertEqual(struct.unpack_from('<I',raw)[0],76)
        flags=struct.unpack_from('<I',raw,20)[0];offset=76
        if flags&1:offset+=2+struct.unpack_from('<H',raw,offset)[0]
        if flags&2:offset+=struct.unpack_from('<I',raw,offset)[0]
        result={}
        self.assertTrue(flags&128)
        for flag,name in [(4,'description'),(8,'relative'),(16,'directory'),(32,'arguments'),(64,'icon')]:
            if flags&flag:
                size=2*struct.unpack_from('<H',raw,offset)[0];offset+=2
                result[name]=raw[offset:offset+size].decode('utf-16le');offset+=size
        with ShellLink() as link:link.load(path);result['target']=link.target()
        self.assertEqual(Path(result['icon']),self.exe)
        return result

    def test_shortcuts_bind_existing_workspace_and_only_client_roots(self):
        server=self.root/'server';server.mkdir();(server/'BattleServer.exe').write_bytes(b'fixture')
        self.workspace.add_host(server)
        result=create(self.workspace,self.exe)
        self.assertEqual([r['status'] for r in result],['created'])
        self.assertEqual(list(self.desktop.iterdir()),[])
        for path in (self.client/CLIENT_NAME,):
            link=self.read(path)
            self.assertEqual(Path(link['target']),self.exe)
            self.assertEqual(link['arguments'],'--workspace "'+str(self.workspace.root)+'"')
            self.assertIn('服主',link['description'])
        self.assertFalse((server/CLIENT_NAME).exists())

    def test_existing_usable_shortcuts_retain_their_original_workspace(self):
        create(self.workspace,self.exe)
        paths=(self.client/CLIENT_NAME,)
        before={p:p.read_bytes() for p in paths}
        other=Workspace(self.root/'another-workspace',ROOT/'mods');other.add_host(self.client)
        result=create(other,self.exe)
        self.assertTrue(all(r['status']=='kept' for r in result))
        self.assertTrue(all(p.read_bytes()==raw for p,raw in before.items()))
        self.assertIn(str(self.workspace.root),self.read(paths[0])['arguments'])

if __name__=='__main__':unittest.main()
