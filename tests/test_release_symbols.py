from pathlib import Path
import configparser
import shutil
import tempfile
import unittest

from tools.build_release import validate_symbol_requirements
from tools.mod_catalog import read_catalog

ROOT=Path(__file__).resolve().parents[1]
HOST='GOnline-54BE1568-00C97000'
HEADER='[host]\nimage=GOnline.exe\ntimestamp=0x54BE1568\nsize_of_image=0x00C97000\n'


class ReleaseSymbolTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.stage=Path(self.temp.name)
        self.shared=self.stage/'client/sdgo/symbols'/(HOST+'.ini')
        self.shared.parent.mkdir(parents=True)
        self.shared.write_text(HEADER+'[shared.call]\nkind=code\n',encoding='utf-8')

    def mod(self,identity,required,fragment=None,declared=True):
        folder=self.stage/'client/sdgo/mods'/identity
        folder.mkdir(parents=True)
        (folder/'mod.ini').write_text('[mod]\nrequires='+required+'\n',encoding='utf-8')
        if fragment is not None:(folder/'symbols.fragment.ini').write_text(fragment,encoding='utf-8')
        return {'id':identity,'target':'client','hosts':[HOST],
                'files':['symbols.fragment.ini']if fragment is not None and declared else [],
                'runtime_files':[],'dll':identity+'.dll'}

    def test_other_mod_fragment_cannot_satisfy_a_dependency(self):
        consumer=self.mod('consumer','private.call')
        provider=self.mod('provider','private.call',HEADER+'[private.call]\nkind=code\n')
        with self.assertRaisesRegex(ValueError,'consumer.*private.call'):
            validate_symbol_requirements(self.stage,{'mods':[provider,consumer]})

    def test_global_and_own_fragment_are_combined(self):
        mod=self.mod('consumer','shared.call,private.call',HEADER+'[private.call]\nkind=code\n')
        validate_symbol_requirements(self.stage,{'mods':[mod]})

    def test_unshipped_fragment_does_not_satisfy_a_dependency(self):
        mod=self.mod('consumer','private.call',HEADER+'[private.call]\nkind=code\n',declared=False)
        with self.assertRaisesRegex(ValueError,'consumer.*private.call'):
            validate_symbol_requirements(self.stage,{'mods':[mod]})

    def test_different_host_fragment_is_rejected(self):
        mod=self.mod('consumer','private.call',HEADER.replace('0x54BE1568','0x12345678')+'[private.call]\nkind=code\n')
        with self.assertRaisesRegex(ValueError,'host differs'):
            validate_symbol_requirements(self.stage,{'mods':[mod]})

    def test_actual_mission_package_resolves_without_other_ai_mods(self):
        mod=next(m for m in read_catalog(ROOT)['mods']if m['id']=='mission_ai')
        shutil.copyfile(ROOT/'symbols'/(HOST+'.ini'),self.shared)
        folder=self.stage/'client/sdgo/mods/mission_ai';folder.mkdir(parents=True)
        for name in ('mod.ini','symbols.fragment.ini'):
            shutil.copyfile(ROOT/'mods/mission_ai'/name,folder/name)
        validate_symbol_requirements(self.stage,{'mods':[mod]})
        broken={**mod,'files':[n for n in mod['files']if n!='symbols.fragment.ini']}
        with self.assertRaisesRegex(ValueError,'mission_ai.*dialogue.battle_chat'):
            validate_symbol_requirements(self.stage,{'mods':[broken]})


if __name__=='__main__':unittest.main()
