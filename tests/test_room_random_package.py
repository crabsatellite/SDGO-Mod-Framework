"""The real overlay builder must retain Random's declared Stable status."""
from pathlib import Path
import json,subprocess,sys,tempfile,unittest,zipfile

ROOT=Path(__file__).resolve().parents[1]

class RandomPackage(unittest.TestCase):
    def test_published_overlay_does_not_reintroduce_beta_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            work=Path(temporary).resolve();binaries=work/'binaries';binaries.mkdir()
            for name in ('core','pvp_ai','coop_ai','room_random','room_random_client'):
                (binaries/(name+'.dll')).write_bytes(b'fixture; never loaded')
            server=work/'server.zip';client=work/'client.zip'
            subprocess.run([sys.executable,str(ROOT/'tools/package_room_random.py'),'--binaries',str(binaries),
                '--out',str(server),'--client-out',str(client)],cwd=ROOT,capture_output=True,check=True,timeout=30)
            with zipfile.ZipFile(server) as z:
                manifest=json.loads(z.read('manifest.json'))
                self.assertEqual(manifest['status'],'stable')
                self.assertNotIn('beta',manifest['version'])
                self.assertNotIn('No public release',z.read('README.txt').decode())
                self.assertEqual(set(manifest['files']),set(z.namelist())-{'manifest.json'})
            with zipfile.ZipFile(client) as z:
                self.assertIn(b'channel=stable',z.read('client/sdgo/mods/room_random_client/mod.ini'))
                self.assertFalse(any(n.endswith('.exe') for n in z.namelist()))

if __name__=='__main__':unittest.main()
