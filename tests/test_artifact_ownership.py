from pathlib import Path
import tempfile,unittest,zipfile
from tools.check_artifact_ownership import source,archive


class Ownership(unittest.TestCase):
    def test_every_tracked_sql_has_explicit_generic_ownership(self):
        self.assertGreater(source(),0)

    def test_private_profile_cannot_enter_a_framework_archive(self):
        with tempfile.TemporaryDirectory()as temporary:
            path=Path(temporary)/'framework.zip'
            for name in('server-profile.json','client/sdgo/shared-settings.json','client/content/unit.bin'):
                with zipfile.ZipFile(path,'w')as z:z.writestr(name,b'private')
                with self.subTest(name=name),self.assertRaises(ValueError):archive(path)
            with zipfile.ZipFile(path,'w')as z:z.writestr('server/sdgo/mods/example/schema.sql',b'generic schema')
            self.assertTrue(archive(path))
