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
                    '-InputPath',str(payload)],capture_output=True,text=True,timeout=45)
            accepted=run(index)
            self.assertEqual(accepted.returncode,0,accepted.stdout+accepted.stderr)
            old=b''.join(struct.pack('<H',ord(c)^0xAA) for c in 'body.mod')
            new=b''.join(struct.pack('<H',ord(c)^0xAA) for c in 'base.mod')
            rejected=run(index.replace(old,new))
            self.assertNotEqual(rejected.returncode,0)
            self.assertIn('paths differ from owned_paths',rejected.stderr)

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
