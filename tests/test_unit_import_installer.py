import hashlib
import json
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

from owner.native_archive import build_archive

ROOT=Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('pwsh'),'PowerShell is required for installer checks')
class InstallerResourceTests(unittest.TestCase):
    def test_symbol_comparison_accepts_native_ini_spacing_but_rejects_changed_definitions(self):
        script=r'''
param([string]$Installer,[string]$InputPath)
$ErrorActionPreference='Stop'
$ast=[Management.Automation.Language.Parser]::ParseFile($Installer,[ref]$null,[ref]$null)
foreach($f in $ast.FindAll({param($a) $a -is [Management.Automation.Language.FunctionDefinitionAst]},$true)){
 if($f.Name -in @('Get-IniSection','Get-NormalizedSection')){Invoke-Expression $f.Extent.Text}
}
$spec=Get-Content -LiteralPath $InputPath -Raw | ConvertFrom-Json
$left=Get-IniSection $spec.left 'unit_catalog.load_last_table'
$right=Get-IniSection $spec.right 'unit_catalog.load_last_table'
$equal=(Get-NormalizedSection $left) -ceq (Get-NormalizedSection $right)
if($equal -ne $spec.expected){throw 'Symbol equivalence differs'}
'''
        native=(ROOT/'symbols/GOnline-54BE1568-00C97000.ini').read_text(encoding='utf-8-sig')
        fragment=(ROOT/'mods/unit_catalog/symbols.fragment.ini').read_text(encoding='utf-8-sig')
        left='[unit_catalog.load_last_table]\nkind = code\nrva = 0x00017EF0\nexpect = 55 8B EC\n'
        right='[unit_catalog.load_last_table]\n; neutral comment\nexpect=55 8B EC\nrva=0x00017EF0\nkind=code\n'
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);runner=root/'symbols.ps1';runner.write_text(script,encoding='utf-8');spec=root/'input.json'
            def run(a,b,expected):
                spec.write_text(json.dumps({'left':a,'right':b,'expected':expected}),encoding='utf-8')
                return subprocess.run(['pwsh','-NoProfile','-File',str(runner),'-Installer',str(ROOT/'tools/install_unit_catalog.ps1'),
                    '-InputPath',str(spec)],capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=45)
            for a,b,equal in ((native,fragment,True),(left,right,True),(left,right.replace('0x00017EF0','0x00017EF1'),False),
                              (left,right.replace('55 8B EC','55 8B ED'),False)):
                result=run(a,b,equal);self.assertEqual(0,result.returncode,result.stdout+result.stderr)
            duplicate=run(left,right+'kind=code\n',False)
            self.assertNotEqual(0,duplicate.returncode);self.assertIn('Duplicate or empty symbol key',duplicate.stderr)

    def test_full_package_reader_accepts_availability_and_keeps_hash_and_path_guards(self):
        from owner.units import compiler,acquisition_sources
        script=r'''
param([string]$Installer,[string]$Package,[string]$Client)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$ast=[Management.Automation.Language.Parser]::ParseFile($Installer,[ref]$null,[ref]$null)
foreach($statement in $ast.EndBlock.Statements){
 if($statement -is [Management.Automation.Language.FunctionDefinitionAst]){Invoke-Expression $statement.Extent.Text}
 if($statement -is [Management.Automation.Language.AssignmentStatementAst] -and
    $statement.Left.VariablePath.UserPath -in @('RequiredModuleFiles','AllowedModuleFiles','AllowedResourceRoots','BlockedResourceExtensions')){
  Invoke-Expression $statement.Extent.Text
 }
}
$Module='unit_catalog'
$ExpectedHost='GOnline-54BE1568-00C97000'
$ExpectedHostSha256='e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92'
$script:ResolvedClientRoot=$Client
$result=Read-Package $Package
@($result.copy | ForEach-Object {$_.target_relative}) | ConvertTo-Json
'''
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);package=root/'package';module=package/'client/sdgo/mods/unit_catalog'
            module.mkdir(parents=True);client=root/'client';client.mkdir()
            runner=root/'read-package.ps1';runner.write_text(script,encoding='utf-8')
            record=bytearray(1132);struct.pack_into('<I',record,0,9901001)
            catalog=compiler.native_catalog([(13,9901001,bytes(record))])
            zdx,zpk,_=build_archive({'mdrs/fixture.mod':b'author fixture'},lambda _:True)
            sha=lambda raw:hashlib.sha256(raw).hexdigest()
            fragment=b'[host]\ntimestamp=0x54BE1568\nsize_of_image=0x00C97000\n[unit_catalog.load_last_table]\nrva=1\n'
            availability={'schema_version':1,'provider':'unit_catalog','revision':'fixture','blueprints':{}}
            files={'unit_catalog.dll':b'fixture program','mod.ini':compiler._runtime_manifest((ROOT/'mods/unit_catalog/mod.ini').read_bytes()),
                'catalog.bin':catalog,'definitions.json':b'{}','resources.zdx':zdx,'resources.zpk':zpk,
                'symbols.fragment.ini':fragment,
                'resource-overlay.ini':('[runtime]\nschema=1\nindex=resources.zdx\nindex_sha256='+sha(zdx)+
                    '\narchive=resources.zpk\narchive_sha256='+sha(zpk)+'\n').encode(),
                'blueprint-availability.json':json.dumps(availability).encode(),
                'blueprint-availability.bin':acquisition_sources.encode_availability(availability,struct.unpack_from('<I',catalog,24)[0])}
            for name,data in files.items():(module/name).write_bytes(data)
            (package/'symbols.fragment.ini').write_bytes(fragment)
            def manifest():
                values={'client/sdgo/mods/unit_catalog/'+name:sha(path.read_bytes()) for path in module.iterdir() if path.is_file() for name in [path.name]}
                values['symbols.fragment.ini']=sha(fragment)
                result={'module':'unit_catalog','schema_version':1,'revision':'fixture','host':compiler.HOST,'host_sha256':compiler.HOST_SHA256,
                    'owned_paths':[],'resource_overlay':{'schema':1,'paths':['mdrs/fixture.mod']},'files':values}
                (package/'manifest.json').write_text(json.dumps(result),encoding='utf-8')
            def run():
                return subprocess.run(['pwsh','-NoProfile','-File',str(runner),'-Installer',str(ROOT/'tools/install_unit_catalog.ps1'),
                    '-Package',str(package),'-Client',str(client)],capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=45)
            manifest();accepted=run()
            self.assertEqual(0,accepted.returncode,accepted.stdout+accepted.stderr)
            self.assertIn('blueprint-availability.bin',accepted.stdout)
            self.assertIn('blueprint-availability.json',accepted.stdout)
            (module/'blueprint-availability.bin').write_bytes(b'tampered')
            rejected=run();self.assertNotEqual(0,rejected.returncode);self.assertIn('Package hash mismatch',rejected.stderr)
            (module/'blueprint-availability.bin').write_bytes(files['blueprint-availability.bin'])
            unexpected=module/'blueprint-availability-extra.bin';unexpected.write_bytes(b'not a supported sidecar')
            manifest();rejected=run();self.assertNotEqual(0,rejected.returncode);self.assertIn('Unexpected file in unit_catalog runtime directory',rejected.stderr)
            unexpected.unlink()
            for name in ('blueprint-availability.json','blueprint-availability.bin'):(module/name).unlink()
            manifest();legacy=run();self.assertEqual(0,legacy.returncode,legacy.stdout+legacy.stderr)
            self.assertEqual([],list(client.iterdir()),'Package reading must not install files')

    def test_receipt_allows_only_pinned_user_setting_changes(self):
        import base64
        script=r'''
param([string]$Installer,[string]$InputPath)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$ast=[Management.Automation.Language.Parser]::ParseFile($Installer,[ref]$null,[ref]$null)
foreach($f in $ast.FindAll({param($a) $a -is [Management.Automation.Language.FunctionDefinitionAst]},$true)){
 if($f.Name -in @('Get-Sha256','Get-ProgramDefinition','Test-ReceiptFile')){Invoke-Expression $f.Extent.Text}
}
$spec=Get-Content -LiteralPath $InputPath -Raw | ConvertFrom-Json
$receipt=[pscustomobject]@{framework_mod_ini_base64=$spec.snapshot}
$file=[pscustomobject]@{installed_sha256=$spec.sha256;owned=$true}
$actual=Test-ReceiptFile $receipt $file $spec.path $spec.relative
if($actual -ne $spec.expected){throw 'Receipt setting classification differs'}
'''
        original=b'[mod]\nid=unit_catalog\nversion=0.3.0\nenabled=1\n[settings]\nEnabled=1\n'
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);runner=root/'receipt.ps1';runner.write_text(script,encoding='utf-8')
            target=root/'mod.ini';spec=root/'input.json'
            cases=[(original.replace(b'enabled=1',b'enabled=0').replace(b'Enabled=1',b'Enabled=0'),True),
                   (original.replace(b'version=0.3.0',b'version=9.0.0'),False)]
            for current,expected in cases:
                target.write_bytes(current)
                spec.write_text(json.dumps({'path':str(target),'relative':'sdgo\\mods\\unit_catalog\\mod.ini',
                    'snapshot':base64.b64encode(original).decode(),'sha256':hashlib.sha256(original).hexdigest(),
                    'expected':expected}))
                result=subprocess.run(['pwsh','-NoProfile','-File',str(runner),'-Installer',str(ROOT/'tools/install_unit_catalog.ps1'),
                    '-InputPath',str(spec)],capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=45)
                self.assertEqual(0,result.returncode,result.stdout+result.stderr)

    def test_installer_accepts_exact_overlay_and_rejects_an_undeclared_model(self):
        script=r'''
param([string]$Installer,[string]$InputPath)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$ast=[Management.Automation.Language.Parser]::ParseFile($Installer,[ref]$null,[ref]$null)
$names=@('Get-NormalizedRelativePath','Get-IniSection','Assert-ResourceOverlay','Test-NativeExport')
foreach($f in $ast.FindAll({param($a) $a -is [Management.Automation.Language.FunctionDefinitionAst]},$true)){
 if($f.Name -in $names){Invoke-Expression $f.Extent.Text}
}
$spec=Get-Content -LiteralPath $InputPath -Raw | ConvertFrom-Json
$copies=@{};foreach($f in $spec.files){$copies[$f.name]=$f}
$owned=@{};foreach($name in $spec.owned){$owned[$name]=$true}
Assert-ResourceOverlay $copies $owned
if(-not (Test-NativeExport $spec.dll 'SdgoGetResourceOverlayBrokerV1')){throw 'Exact C ABI export was not found'}
if(Test-NativeExport $spec.dll '_SdgoGetResourceOverlayBrokerV1'){throw 'A decorated name was accepted as the shared C ABI'}
'''
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);runner=root/'verify.ps1';runner.write_text(script,encoding='utf-8')
            index,archive,_=build_archive({'mdrs/body.mod':b'native model'},lambda _:True)
            self.write_export_fixture(root/'provider.dll')
            def run(current_index):
                config=('[runtime]\nschema=1\nindex=resources.zdx\nindex_sha256='+hashlib.sha256(current_index).hexdigest()+
                    '\narchive=resources.zpk\narchive_sha256='+hashlib.sha256(archive).hexdigest()+'\n').encode()
                files=[]
                for name,data in [('resource-overlay.ini',config),('resources.zdx',current_index),('resources.zpk',archive)]:
                    file=root/name;file.write_bytes(data)
                    files.append({'name':'sdgo\\mods\\unit_catalog\\'+name,'source':str(file),'sha256':hashlib.sha256(data).hexdigest()})
                payload=root/'input.json';payload.write_text(json.dumps({'files':files,'owned':['mdrs\\body.mod'],'dll':str(root/'provider.dll')}))
                return subprocess.run(['pwsh','-NoProfile','-File',str(runner),'-Installer',str(ROOT/'tools/install_unit_catalog.ps1'),
                    '-InputPath',str(payload)],capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=45)
            accepted=run(index)
            self.assertEqual(accepted.returncode,0,accepted.stdout+accepted.stderr)
            old=b''.join(struct.pack('<H',ord(c)^0xAA) for c in 'body.mod')
            new=b''.join(struct.pack('<H',ord(c)^0xAA) for c in 'base.mod')
            rejected=run(index.replace(old,new))
            self.assertNotEqual(rejected.returncode,0)
            self.assertIn('paths differ from the declared overlay paths',rejected.stderr)

    def test_installer_accepts_an_overlay_only_package_without_loose_copies(self):
        """resource_overlay.paths declares the archive entries; owned_paths may be empty."""
        script=r'''
param([string]$Installer,[string]$InputPath)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$ast=[Management.Automation.Language.Parser]::ParseFile($Installer,[ref]$null,[ref]$null)
$names=@('Get-NormalizedRelativePath','Get-IniSection','Assert-ResourceOverlay')
foreach($f in $ast.FindAll({param($a) $a -is [Management.Automation.Language.FunctionDefinitionAst]},$true)){
 if($f.Name -in $names){Invoke-Expression $f.Extent.Text}
}
$spec=Get-Content -LiteralPath $InputPath -Raw | ConvertFrom-Json
$copies=@{};foreach($f in $spec.files){$copies[$f.name]=$f}
$manifest=$spec.manifest
$ownedPaths=@{};foreach($raw in @($manifest.owned_paths)){$ownedPaths[(Get-NormalizedRelativePath ([string]$raw))]=$true}
$overlayPaths=$ownedPaths
$overlayInfo=$manifest.resource_overlay
if($null -ne $overlayInfo -and ($overlayInfo.PSObject.Properties.Name -contains 'paths')){
 $overlayPaths=@{}
 foreach($raw in @($overlayInfo.paths)){$overlayPaths[(Get-NormalizedRelativePath ([string]$raw))]=$true}
}
Assert-ResourceOverlay $copies $overlayPaths
'''
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);runner=root/'verify.ps1';runner.write_text(script,encoding='utf-8')
            index,archive,_=build_archive({'mdrs/body.mod':b'native model','anrs/walk.ani':b'native animation'},lambda _:True)
            config=('[runtime]\nschema=1\nindex=resources.zdx\nindex_sha256='+hashlib.sha256(index).hexdigest()+
                '\narchive=resources.zpk\narchive_sha256='+hashlib.sha256(archive).hexdigest()+'\n').encode()
            files=[]
            for name,data in [('resource-overlay.ini',config),('resources.zdx',index),('resources.zpk',archive)]:
                file=root/name;file.write_bytes(data)
                files.append({'name':'sdgo\\mods\\unit_catalog\\'+name,'source':str(file),'sha256':hashlib.sha256(data).hexdigest()})
            def run(paths):
                manifest={'owned_paths':[],'resource_overlay':{'schema':1,'paths':paths}}
                payload=root/'input.json';payload.write_text(json.dumps({'files':files,'manifest':manifest}))
                return subprocess.run(['pwsh','-NoProfile','-File',str(runner),'-Installer',str(ROOT/'tools/install_unit_catalog.ps1'),
                    '-InputPath',str(payload)],capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=45)
            accepted=run(['mdrs/body.mod','anrs/walk.ani'])
            self.assertEqual(accepted.returncode,0,accepted.stdout+accepted.stderr)
            incomplete=run(['mdrs/body.mod'])
            self.assertNotEqual(incomplete.returncode,0)
            self.assertIn('declared overlay paths',incomplete.stderr)

    def test_installer_accepts_a_split_archive_and_rejects_missing_or_extra_parts(self):
        from owner.native_archive import build_archive_file
        script=r'''
param([string]$Installer,[string]$InputPath)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$ast=[Management.Automation.Language.Parser]::ParseFile($Installer,[ref]$null,[ref]$null)
foreach($f in $ast.FindAll({param($a) $a -is [Management.Automation.Language.FunctionDefinitionAst]},$true)){
 if($f.Name -in @('Get-NormalizedRelativePath','Get-IniSection','Assert-ResourceOverlay')){Invoke-Expression $f.Extent.Text}
}
$spec=Get-Content -LiteralPath $InputPath -Raw | ConvertFrom-Json
$copies=@{};foreach($f in $spec.files){$copies[$f.name]=$f}
$overlayPaths=@{};foreach($raw in @($spec.manifest.resource_overlay.paths)){$overlayPaths[(Get-NormalizedRelativePath ([string]$raw))]=$true}
Assert-ResourceOverlay $copies $overlayPaths
'''
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);runner=root/'verify.ps1';runner.write_text(script,encoding='utf-8')
            module=root/'module';module.mkdir()
            resources={'mdrs/a.mod':b'first model','mdrs/b.mod':b'second model','txrs/c.txr':b'texture'}
            index,inventory=build_archive_file(resources,lambda _:True,module/'resources.zpk',part_chunks=2)
            self.assertEqual(['resources.zpk','resources.1.zpk'],[part['name'] for part in inventory['parts']])
            config=('[runtime]\nschema=1\nindex=resources.zdx\nindex_sha256='+hashlib.sha256(index).hexdigest()+
                '\narchive=resources.zpk\narchive_sha256='+inventory['archive_sha256']+'\nparts=2\npart_chunks=2\n').encode()
            (module/'resources.zdx').write_bytes(index);(module/'resource-overlay.ini').write_bytes(config)
            def run(names):
                files=[{'name':'sdgo\\mods\\unit_catalog\\'+name,'source':str(module/name),
                        'sha256':hashlib.sha256((module/name).read_bytes()).hexdigest()} for name in names]
                manifest={'owned_paths':[],'resource_overlay':{'schema':1,'paths':sorted(resources)}}
                payload=root/'input.json';payload.write_text(json.dumps({'files':files,'manifest':manifest}))
                return subprocess.run(['pwsh','-NoProfile','-File',str(runner),'-Installer',str(ROOT/'tools/install_unit_catalog.ps1'),
                    '-InputPath',str(payload)],capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=45)
            base=['resource-overlay.ini','resources.zdx','resources.zpk']
            accepted=run(base+['resources.1.zpk'])
            self.assertEqual(accepted.returncode,0,accepted.stdout+accepted.stderr)
            missing=run(base)
            self.assertIn('archive part is missing',missing.stderr)
            (module/'resources.2.zpk').write_bytes(b'\0'*520)
            extra=run(base+['resources.1.zpk','resources.2.zpk'])
            self.assertIn('undeclared archive part',extra.stderr)

    @staticmethod
    def write_export_fixture(path):
        data=bytearray(0x600);data[:2]=b'MZ';struct.pack_into('<I',data,0x3c,0x80)
        data[0x80:0x84]=b'PE\0\0';struct.pack_into('<HH',data,0x84,0x14c,1);struct.pack_into('<H',data,0x94,0xe0)
        struct.pack_into('<H',data,0x98,0x10b);struct.pack_into('<I',data,0x98+60,0x200)
        struct.pack_into('<II',data,0x98+96,0x1000,0x100)
        struct.pack_into('<IIII',data,0x178+8,0x400,0x1000,0x400,0x200)
        struct.pack_into('<IIIII',data,0x200+20,1,1,0x1040,0x1044,0x1048)
        struct.pack_into('<I',data,0x244,0x1050)
        name=b'SdgoGetResourceOverlayBrokerV1\0';data[0x250:0x250+len(name)]=name
        path.write_bytes(data)


if __name__=='__main__':unittest.main()
