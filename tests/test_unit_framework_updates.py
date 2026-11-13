import base64
import copy
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from manager import storage, updates as u
from manager import unit_receipts as ownership
from test_manager_updates import bundle, fake_exe, KEY


class UnitFrameworkUpdates(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.host=self.root/'client';fake_exe(self.host/'GOnline.exe')
        self.record={'id':'fixture','name':'Fixture client','role':'client','path':str(self.host)}
        self.cache=self.root/'cache'
        (self.host/'sdgo').mkdir(exist_ok=True)
        symbols=self.host/'sdgo/symbols';symbols.mkdir()
        (symbols/(KEY+'.ini')).write_bytes(b'[custom.alias]\nrva=99\n')
        (self.host/'d3d9.dll').write_bytes(b'old loader');(self.host/'sdgo/core.dll').write_bytes(b'old core')
        self.mod=self.host/ownership.BASE;self.mod.mkdir(parents=True)
        old_ini=b'[mod]\nid=unit_catalog\nversion=0.3.0\ndistribution=public\nenabled=1\n[settings]\nEnabled=1\nOwnerSetting=keep\n'
        self.old_programs={'unit_catalog.dll':b'old unit program','mod.ini':old_ini,
                           'manager.json':b'{"id":"unit_catalog","distribution":"public"}'}
        contents={'catalog.bin':b'fixture native records','definitions.json':b'{"fixture":true}',
                  'resource-overlay.ini':b'fixture archive pointer','resources.zdx':b'fixture resource index',
                  'resources.zpk':b'fixture resource archive'}
        for name,data in self.old_programs.items():(self.mod/name).write_bytes(data)
        for name,data in contents.items():(self.mod/name).write_bytes(data)
        model=self.host/'mdrs/fixture.mod';model.parent.mkdir();model.write_bytes(b'owner model fixture')
        files=[]
        for path in [*self.mod.iterdir(),model]:
            files.append({'path':path.relative_to(self.host).as_posix().replace('/','\\'),
                          'installed_sha256':u.digest(path.read_bytes()),'owned':True})
        self.receipt={'module':'unit_catalog','schema_version':1,'revision':'fixture-owner-content',
                      'host':KEY,'host_sha256':u.digest((self.host/'GOnline.exe').read_bytes()),
                      'symbol_section_added':False,'files':files}
        self.save_receipt()
        self.bundle,self.manifest=self.framework('1.3.0-beta.1')

    def save_receipt(self):
        storage.atomic(self.host/ownership.RECEIPT,storage.json_bytes(self.receipt))

    def framework(self,version,legacy=False,fragment=False,presets=False):
        raw,_,_=bundle('beta',version)
        with zipfile.ZipFile(io.BytesIO(raw)) as archive:catalog=json.loads(archive.read('client/sdgo/catalog.json'))
        mod={'id':'unit_catalog','feature':'unit-import','target':'client','category':'gameplay',
             'hosts':[KEY],'version':'0.4.0','channel':'beta','files':['manager.json'],
             'config_files':[],'runtime_files':[]}
        if not legacy:mod['distribution']='public'
        if fragment:mod['files'].append('symbols.fragment.ini')
        if presets:mod['files'].append('default-presets.json')
        catalog['mods'].append(mod);catalog['features']['unit-import']={'category':'gameplay','mods':['unit_catalog']}
        scope=b'' if legacy else b'distribution=public\n'
        ini=b'[mod]\nid=unit_catalog\nversion=0.4.0\n'+scope+b'enabled=0\n[settings]\nEnabled=1\nNewOption=7\n'
        manager={'id':'unit_catalog'}
        if not legacy:manager['distribution']='public'
        payload={'client/sdgo/catalog.json':storage.json_bytes(catalog),
            'client/'+ownership.BASE+'unit_catalog.dll':b'verified new unit program',
            'client/'+ownership.BASE+'mod.ini':ini,
            'client/'+ownership.BASE+'manager.json':storage.json_bytes(manager)}
        if fragment:payload['client/'+ownership.BASE+'symbols.fragment.ini']=b'[unit.signature]\nrva=42\n'
        if presets:payload['client/'+ownership.BASE+'default-presets.json']=b'{"fixture_version":2}'
        raw,asset,_=bundle('beta',version,extra=payload)
        path=self.root/('framework-'+version);manifest=u.unpack(raw,asset,path)
        return path,manifest

    def set_already_updated(self,bundle=None,scope_migration=False):
        source=(bundle or self.bundle)/'client'/ownership.BASE
        for name in self.old_programs:
            data=(source/name).read_bytes()
            if name=='mod.ini':
                data=storage.patch_ini(data,[('mod','enabled','1'),('settings','OwnerSetting','keep')])
                if scope_migration:data=storage.patch_ini(data,[('mod','distribution','public')])
            elif name=='manager.json' and scope_migration:
                data=storage.json_bytes({**json.loads(data),'distribution':'public'})
            (self.mod/name).write_bytes(data)

    def all_bytes(self):return {p.relative_to(self.host):p.read_bytes() for p in self.host.rglob('*') if p.is_file()}

    def check_receipt(self):
        result=json.loads((self.host/ownership.RECEIPT).read_bytes())
        self.assertEqual(self.receipt['revision'],result['revision'])
        self.assertEqual([r['owned'] for r in self.receipt['files']],[r['owned'] for r in result['files']])
        for row in result['files']:
            self.assertEqual(row['installed_sha256'],u.digest((self.host/row['path'].replace('\\','/')).read_bytes()))
        return result

    def test_framework_update_and_content_receipt_commit_together(self):
        content_paths=[self.host/row['path'].replace('\\','/') for row in self.receipt['files']
                       if row['path'].replace('\\','/').casefold() not in ownership.PROGRAMS]
        before={p:p.read_bytes() for p in content_paths+[self.host/'GOnline.exe']}
        result=u.install_host(self.record,self.bundle,self.manifest,self.cache,lambda _:False)
        self.assertEqual('installed',result['status']);receipt=self.check_receipt()
        self.assertEqual('framework-update',receipt['framework_updates'][-1]['kind'])
        self.assertEqual('1',u.read_ini((self.mod/'mod.ini').read_bytes())['mod']['enabled'])
        self.assertEqual('keep',u.read_ini((self.mod/'mod.ini').read_bytes())['settings']['OwnerSetting'])
        for path,data in before.items():self.assertEqual(data,path.read_bytes())
        again=u.install_host(self.record,self.bundle,self.manifest,self.cache,lambda _:False)
        self.assertEqual(0,again['changed_files'])

    def test_optional_sealed_content_and_archive_parts_stay_unchanged_through_an_update(self):
        extra={'native-programs.bin':b'sealed programs','skill-atlases.bin':b'sealed atlases',
               'blueprint-availability.json':b'{"fixture_availability":1}',
               'blueprint-availability.bin':b'fixture catalog-bound blueprint availability',
               'stage-rules.bin':b'fixture catalog-bound stage rules',
               'resources.1.zpk':b'second archive part','resources.2.zpk':b'third archive part'}
        for name,data in extra.items():
            (self.mod/name).write_bytes(data)
            self.receipt['files'].append({'path':ownership.BASE+name,'installed_sha256':u.digest(data),'owned':True})
        self.save_receipt()
        result=u.install_host(self.record,self.bundle,self.manifest,self.cache,lambda _:False)
        self.assertEqual('installed',result['status'],result);self.check_receipt()
        for name,data in extra.items():self.assertEqual(data,(self.mod/name).read_bytes())
        self.assertNotIn(ownership.BASE+'resources.32.zpk',ownership.OPTIONAL_CONTENT)
        (self.mod/'resources.1.zpk').write_bytes(b'changed outside the receipt')
        source,manifest=self.framework('1.3.1-beta.1')
        with self.assertRaisesRegex(ValueError,'安装收据之外'):
            u.install_host(self.record,source,manifest,self.cache,lambda _:False)

    def test_framework_updates_a_recorded_symbol_fragment_without_claiming_foreign_ownership(self):
        fragment=self.mod/'symbols.fragment.ini';fragment.write_bytes(b'[unit.signature]\nrva=21\n')
        self.receipt['files'].append({'path':ownership.BASE+'symbols.fragment.ini',
            'installed_sha256':u.digest(fragment.read_bytes()),'owned':False});self.save_receipt()
        source,manifest=self.framework('1.3.2-beta.1',fragment=True)
        self.assertEqual('installed',u.install_host(self.record,source,manifest,self.cache,lambda _:False)['status'])
        receipt=self.check_receipt();self.assertFalse(receipt['files'][-1]['owned'])
        self.assertEqual((source/'client'/ownership.BASE/'symbols.fragment.ini').read_bytes(),fragment.read_bytes())
        fragment.write_bytes(fragment.read_bytes()+b'external change');before=self.all_bytes()
        with self.assertRaises(ValueError):u.install_host(self.record,source,manifest,self.cache,lambda _:False)
        self.assertEqual(before,self.all_bytes())

    def test_framework_updates_its_preset_descriptor_and_rejects_unrecorded_edits(self):
        descriptor=self.mod/'default-presets.json';descriptor.write_bytes(b'{"fixture_version":1}')
        self.receipt['files'].append({'path':ownership.BASE+'default-presets.json',
            'installed_sha256':u.digest(descriptor.read_bytes()),'owned':False});self.save_receipt()
        source,manifest=self.framework('1.3.3-beta.1',presets=True)
        self.assertEqual('installed',u.install_host(self.record,source,manifest,self.cache,lambda _:False)['status'])
        receipt=self.check_receipt();self.assertFalse(receipt['files'][-1]['owned'])
        self.assertEqual(b'{"fixture_version":2}',descriptor.read_bytes())
        descriptor.write_bytes(b'{"fixture_version":99}');before=self.all_bytes()
        with self.assertRaises(ValueError):u.install_host(self.record,source,manifest,self.cache,lambda _:False)
        self.assertEqual(before,self.all_bytes())

    def test_legacy_already_updated_programs_are_reconciled_without_touching_runtime(self):
        self.set_already_updated();before=self.all_bytes()
        result=u.repair_unit_receipt(self.record,self.bundle,self.manifest,self.cache,lambda _:False)
        self.assertEqual('reconciled',result['status']);self.assertFalse(result['program_files_changed'])
        for path,data in before.items():
            if path.as_posix()!=ownership.RECEIPT:self.assertEqual(data,(self.host/path).read_bytes())
        receipt=self.check_receipt();self.assertEqual('verified-framework-reconciliation',receipt['framework_updates'][-1]['kind'])
        self.assertEqual('current',u.repair_unit_receipt(self.record,self.bundle,self.manifest,self.cache,lambda _:False)['status'])

    def test_legacy_explicit_public_scope_migration_is_metadata_only(self):
        source,manifest=self.framework('1.1.15-beta.421',legacy=True)
        self.set_already_updated(source,scope_migration=True)
        self.assertEqual('reconciled',u.repair_unit_receipt(self.record,source,manifest,self.cache,lambda _:False)['status'])
        self.check_receipt()

    def test_unknown_program_or_changed_content_is_rejected_before_any_write(self):
        for path in [self.mod/'unit_catalog.dll',self.host/'mdrs/fixture.mod',self.mod/'mod.ini']:
            with self.subTest(path=path):
                previous=path.read_bytes()
                altered=storage.patch_ini(previous,[('mod','id','foreign')]) if path.name=='mod.ini' else previous+b'unknown edit'
                path.write_bytes(altered);snapshot=self.all_bytes()
                with self.assertRaises(ValueError):u.install_host(self.record,self.bundle,self.manifest,self.cache,lambda _:False)
                self.assertEqual(snapshot,self.all_bytes());path.write_bytes(previous)

    def test_mutable_settings_are_accepted_only_with_a_pinned_manifest_snapshot(self):
        self.receipt['framework_mod_ini_base64']=base64.b64encode((self.mod/'mod.ini').read_bytes()).decode()
        self.save_receipt();(self.mod/'mod.ini').write_bytes(storage.patch_ini((self.mod/'mod.ini').read_bytes(),[('mod','enabled','0'),('settings','Enabled','0')]))
        u.install_host(self.record,self.bundle,self.manifest,self.cache,lambda _:False);self.check_receipt()
        ini=u.read_ini((self.mod/'mod.ini').read_bytes());self.assertEqual('0',ini['mod']['enabled']);self.assertEqual('0',ini['settings']['Enabled'])

    def test_failed_receipt_write_restores_all_framework_files(self):
        before=self.all_bytes();real=storage.atomic;failed=False
        def fail_once(path,raw):
            nonlocal failed
            if path==self.host/ownership.RECEIPT and not failed:failed=True;raise PermissionError('receipt locked')
            return real(path,raw)
        with patch.object(storage,'atomic',side_effect=fail_once),self.assertRaises(PermissionError):
            u.install_host(self.record,self.bundle,self.manifest,self.cache,lambda _:False)
        self.assertEqual(before,self.all_bytes())

    def test_tampered_artifact_and_changed_receipt_snapshot_are_rejected(self):
        source=self.bundle/'client'/ownership.BASE/'unit_catalog.dll';source.write_bytes(b'cache tamper')
        before=self.all_bytes()
        with self.assertRaises(ValueError):u.repair_unit_receipt(self.record,self.bundle,self.manifest,self.cache,lambda _:False)
        self.assertEqual(before,self.all_bytes())

    def test_content_change_after_review_invalidates_the_whole_plan(self):
        self.set_already_updated()
        plan=ownership.prepare(self.host,self.bundle/'client',self.manifest,{},u.child,KEY,repair=True)
        (self.mod/'catalog.bin').write_bytes(b'concurrent content edit')
        with self.assertRaises(ValueError):ownership.verify_snapshot(plan)

    def test_failed_readback_restores_framework_and_receipt(self):
        before=self.all_bytes()
        with patch.object(ownership,'verify_installed',side_effect=ValueError('readback failed')):
            with self.assertRaisesRegex(ValueError,'readback'):
                u.install_host(self.record,self.bundle,self.manifest,self.cache,lambda _:False)
        self.assertEqual(before,self.all_bytes())

    def test_in_host_receipt_symlink_cannot_redirect_the_update(self):
        path=self.host/ownership.RECEIPT;foreign=self.host/'foreign.json'
        previous=path.read_bytes();foreign.write_bytes(previous);path.unlink()
        try:path.symlink_to(foreign)
        except OSError:self.skipTest('Symlinks are not available')
        with self.assertRaisesRegex(ValueError,'目录链接'):
            u.repair_unit_receipt(self.record,self.bundle,self.manifest,self.cache,lambda _:False)
        self.assertEqual(previous,foreign.read_bytes())

    def test_running_client_defers_without_reconciling_its_receipt(self):
        self.set_already_updated();before=self.all_bytes()
        result=u.repair_unit_receipt(self.record,self.bundle,self.manifest,self.cache,lambda _:True)
        self.assertEqual('waiting',result['status']);self.assertEqual(before,self.all_bytes())


if __name__=='__main__':unittest.main()
