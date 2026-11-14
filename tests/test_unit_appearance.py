"""Appearance upgrade preservation, switch matrix and transactional rollback."""
import base64,io,itertools,json,tempfile,unittest,zipfile
from pathlib import Path
from unittest.mock import patch
from manager import consolidation,packages,storage,updates

ROOT=Path(__file__).resolve().parents[1]

class AppearanceUpgradeTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)/'client';self.root.mkdir()
        self.host={'id':'fixture','name':'Fixture','role':'client','path':str(self.root)}
        class Workspace:
            def host(inner,identity):return self.host
            def public(inner):return {}
        self.workspace=Workspace()
        self.patcher=patch('manager.packages._running',return_value=False);self.patcher.start();self.addCleanup(self.patcher.stop)
    def old(self,identity,enabled,global_off=False):
        folder=self.root/'sdgo/mods'/identity;folder.mkdir(parents=True,exist_ok=True)
        data=f'; preserve user bytes\n[mod]\nid={identity}\ndistribution=public\ncategory=gameplay\nfeature={identity}\nenabled=1\n[settings]\nEnabled={int(enabled)}\nUnknown=keep\n'.encode()
        (folder/'mod.ini').write_bytes(data);(folder/(identity+'.dll')).write_bytes(b'old program');(folder/'manager.json').write_text('{}')
        if global_off:
            (self.root/'sdgo/sdgo.ini').write_bytes(f'[mods]\n{identity}=0\n'.encode())
        return folder,data
    def incoming(self,role='client'):
        identity='unit_appearance_'+role;source=ROOT/'mods'/identity
        pending={self.root/'sdgo/mods'/identity/name:(source/name).read_bytes() for name in ('mod.ini','manager.json')}
        pending[self.root/'sdgo/mods'/identity/(identity+'.dll')]=b'new public program'
        return pending
    def package(self):
        data=io.BytesIO()
        with zipfile.ZipFile(data,'w')as z:
            for path,raw in self.incoming().items():z.writestr(path.relative_to(self.root).as_posix(),raw)
        return base64.b64encode(data.getvalue()).decode()
    def test_all_legacy_switch_combinations_and_global_disable(self):
        for rgb,decal in itertools.product((False,True),repeat=2):
            with self.subTest(rgb=rgb,decal=decal):
                self.old('rgb_paint_client',rgb);self.old('surface_decal_client',decal)
                pending=self.incoming();retired=consolidation.prepare(self.root,pending)
                cfg=storage.ini_values(pending[self.root/'sdgo/mods/unit_appearance_client/mod.ini'])
                self.assertEqual(cfg.getboolean('settings','RgbEnabled'),rgb)
                self.assertEqual(cfg.getboolean('settings','DecalsEnabled'),decal)
                self.assertEqual(cfg.getboolean('settings','Enabled'),rgb or decal)
                self.assertEqual(len(retired),6)
        self.old('rgb_paint_client',True,global_off=True)
        pending=self.incoming();consolidation.prepare(self.root,pending)
        self.assertFalse(storage.ini_values(pending[self.root/'sdgo/mods/unit_appearance_client/mod.ini']).getboolean('settings','RgbEnabled'))
    def test_import_archives_old_programs_and_reimport_keeps_independent_choices(self):
        old,raw=self.old('rgb_paint_client',True);self.old('surface_decal_client',False)
        result=packages.import_client(self.workspace,'fixture',self.package())
        self.assertFalse((old/'mod.ini').exists())
        self.assertEqual((Path(result['scope_recovery'])/'sdgo/mods/rgb_paint_client/mod.ini').read_bytes(),raw)
        ini=self.root/'sdgo/mods/unit_appearance_client/mod.ini'
        ini.write_bytes(storage.patch_ini(ini.read_bytes(),[('settings','RgbEnabled','0'),('settings','DecalsEnabled','1'),('settings','Unknown','keep')]))
        packages.import_client(self.workspace,'fixture',self.package())
        cfg=storage.ini_values(ini.read_bytes());self.assertFalse(cfg.getboolean('settings','RgbEnabled'));self.assertTrue(cfg.getboolean('settings','DecalsEnabled'));self.assertEqual(cfg.get('settings','Unknown'),'keep')
    def test_failed_retirement_restores_every_program_and_configuration(self):
        self.old('rgb_paint_client',True);self.old('surface_decal_client',True)
        before={p.relative_to(self.root):p.read_bytes()for p in self.root.rglob('*')if p.is_file()}
        real=Path.unlink;failed=False
        def remove(path,*args,**kwargs):
            nonlocal failed
            if path.name=='surface_decal_client.dll'and not failed:
                failed=True;raise PermissionError('fixture locked program')
            return real(path,*args,**kwargs)
        with patch.object(Path,'unlink',remove),self.assertRaises(PermissionError):packages.import_client(self.workspace,'fixture',self.package())
        self.assertTrue(failed)
        after={p.relative_to(self.root):p.read_bytes()for p in (self.root/'sdgo/mods').rglob('*')if p.is_file()}
        self.assertEqual(before,after)
    def test_world_uses_same_preservation_without_touching_database_files(self):
        self.old('rgb_paint_server',False);self.old('surface_decal_server',True)
        db=self.root/'untouched.mdf';db.write_bytes(b'existing appearance data')
        pending=self.incoming('server');retired=consolidation.prepare(self.root,pending)
        cfg=storage.ini_values(pending[self.root/'sdgo/mods/unit_appearance_server/mod.ini'])
        self.assertFalse(cfg.getboolean('settings','RgbEnabled'));self.assertTrue(cfg.getboolean('settings','DecalsEnabled'))
        self.assertNotIn(db,retired);self.assertEqual(db.read_bytes(),b'existing appearance data')
    def test_rejects_legacy_identity_mismatch_before_writing(self):
        old,raw=self.old('rgb_paint_client',True)
        (old/'mod.ini').write_bytes(raw.replace(b'id=rgb_paint_client',b'id=another_mod'))
        with self.assertRaises(ValueError):packages.import_client(self.workspace,'fixture',self.package())
        self.assertFalse((self.root/'sdgo/mods/unit_appearance_client/mod.ini').exists())
    def test_framework_world_update_archives_legacy_components_and_keeps_switches(self):
        self.host['role']='world';self.old('rgb_paint_server',True);self.old('surface_decal_server',False)
        bundle=Path(self.temp.name)/'bundle';source=bundle/'server';identity='unit_appearance_server'
        folder=source/'sdgo/mods'/identity;folder.mkdir(parents=True)
        payload=['manager.json','rgb_schema.sql','decal_schema.sql']
        for name in ['mod.ini',*payload]:(folder/name).write_bytes((ROOT/'mods'/identity/name).read_bytes())
        (folder/(identity+'.dll')).write_bytes(b'new world program')
        (source/'sdgo/symbols').mkdir();(source/'sdgo/symbols/WorldServer-548A86E3-0010C000.ini').write_bytes(b'[host]\n')
        mod={'id':identity,'target':'server','hosts':['WorldServer-548A86E3-0010C000'],'version':'0.1.0','files':payload,'config_files':[],'feature':'unit-appearance','channel':'beta'}
        (source/'sdgo/catalog.json').write_bytes(storage.json_bytes({'schema_version':1,'mods':[mod],'features':{'unit-appearance':{}},'categories':{}}))
        carrier=self.root/'sdgo/mods/friend_unit_gift_server';carrier.mkdir()
        for name in ('mod.ini','manager.json'):(carrier/name).write_bytes((ROOT/'mods/friend_unit_gift_server'/name).read_bytes())
        manifest={'version':'99.0.0-beta.1','channel':'beta','source_commit':'a'*40}
        with patch('manager.updates.host_key',return_value='WorldServer-548A86E3-0010C000'):
            result=updates.install_host(self.host,bundle,manifest,Path(self.temp.name)/'cache',lambda _:False,framework=False)
        self.assertEqual(result['status'],'installed')
        cfg=storage.ini_values((self.root/'sdgo/mods'/identity/'mod.ini').read_bytes())
        self.assertTrue(cfg.getboolean('settings','RgbEnabled'));self.assertFalse(cfg.getboolean('settings','DecalsEnabled'))
        self.assertFalse((self.root/'sdgo/mods/rgb_paint_server/mod.ini').exists())
        self.assertTrue(list((Path(self.temp.name)/'cache/backups').rglob('rgb_paint_server.dll')))

    def test_owner_profile_precedence_is_not_replaced_by_personal_import_preservation(self):
        # Profile composition applies explicit shared/personal contracts after
        # first-install legacy migration, not unconditional local preservation.
        pending=self.incoming();ini=self.root/'sdgo/mods/unit_appearance_client/mod.ini'
        ini.parent.mkdir(parents=True)
        ini.write_bytes(storage.patch_ini(pending[ini],[('settings','Enabled','1'),('settings','RgbEnabled','0')]))
        consolidation.prepare(self.root,pending,preserve_existing=False)
        self.assertTrue(storage.ini_values(pending[ini]).getboolean('settings','RgbEnabled'))
        consolidation.prepare(self.root,pending)
        self.assertFalse(storage.ini_values(pending[ini]).getboolean('settings','RgbEnabled'))

    def test_contract_requires_conflicts_and_registered_independent_switches(self):
        contract=json.loads((ROOT/'mods/unit_appearance_client/manager.json').read_text('utf-8'))
        for key,value in [('consolidates',[{'id':'../outside','setting':'rgb_enabled'}]),('dependencies',{}),('consolidates',[{'id':'rgb_paint_client','setting':'enabled'}])]:
            with self.subTest(key=key),self.assertRaises(ValueError):consolidation.validate({**contract,key:value})

if __name__=='__main__':unittest.main()
