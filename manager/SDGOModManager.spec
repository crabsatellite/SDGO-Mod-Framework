# Build from the repository; all inputs are generic code/contracts, never owner state.
from pathlib import Path
import os
import subprocess
import sys
from importlib import metadata
root = Path(SPECPATH).parent
subprocess.run(
    [sys.executable, str(root/'tools/distribution_guard.py'),
     '--source-path', str(root)],
    check=True,
)
library = Path(os.environ['SDGO_MANAGER_SERVICES'])
data = [(str(root/'manager/static'),'manager/static'),
        (os.environ.get('SDGO_MANAGER_METADATA',str(root/'manager/repository.json')),'manager'),
        (str(root/'manager/addons'),'manager/addons'),
        (str(root/'manager/engines/operators/resources'),'manager/engines/operators/resources'),
        (str(root/'manager/engines/operators/Install-Operator.ps1'),'manager/engines/operators'),
        (str(root/'manager/engines/capsules/install_capsule_client.ps1'),'manager/engines/capsules'),
        (str(root/'manager/engines/shop/bridge.ps1'),'manager/engines/shop'),
        (str(root/'manager/engines/hangar/sort.sql'),'manager/engines/hangar'),
        (str(root/'manager/engines/hangar/gb.sql'),'manager/engines/hangar'),
        (str(root/'manager/engines/hangar/guard.sql'),'manager/engines/hangar'),
        (str(library),'manager/engines/shop'),
        (str(root/'owner/rewards/policy.example.json'),'owner/rewards'),
        (str(root/'owner/rewards/sql'),'owner/rewards/sql'),
        (str(root/'backend/install_ai_catalog_client.ps1'),'backend')]
data += [(str(root/'tools/install_unit_catalog.ps1'),'tools'),
         (str(root/'mods/capsule_catalog/symbol-proposal.ini'),'mods/capsule_catalog'),
         (str(root/'mods/unit_catalog/mod.ini'),'mods/unit_catalog'),
         (str(root/'mods/unit_catalog/symbols.fragment.ini'),'mods/unit_catalog'),
         (str(root/'mods/unit_catalog/preview.symbols.fragment.ini'),'mods/unit_catalog'),
         (str(Path(os.environ['SDGO_UNIT_CATALOG_BINARY'])),'unit-import'),
         (str(Path(os.environ['SDGO_CAPSULE_REGISTRY_BINARY'])),'unit-import')]
data += [(str(root/'owner/collection_capsule/sql'),'owner/collection_capsule/sql'),
         (str(root/'owner/calendar_gifts/festival_dates.json'),'owner/calendar_gifts'),
         (str(root/'owner/calendar_gifts/schema.sql'),'owner/calendar_gifts'),
         (str(root/'mods/collection_capsule/assets'),'mods/collection_capsule/assets'),
        (str(Path(os.environ['SDGO_COLLECTION_SUPPORT_BINARY'])),'collection-support'),
         (str(root/'support/capsule_registry'),'support/capsule_registry')]
data += [(str(root/'tools'/name),'tools') for name in ('install_server.ps1','install_common.ps1')]
data += [(str(path),str(path.parent.relative_to(root))) for path in sorted((root/'mods').glob('*/manager.json'))]
python_license=Path(sys.base_prefix)/'LICENSE.txt'
if python_license.is_file():data.append((str(python_license),'licenses/Python'))
for package in ('Pillow','pyodbc','PyInstaller','defusedxml','truststore'):
    try:distribution=metadata.distribution(package)
    except metadata.PackageNotFoundError:continue
    for item in distribution.files or []:
        if ('license' in str(item).lower() or 'copying' in str(item).lower()) and distribution.locate_file(item).is_file():
            data.append((str(distribution.locate_file(item)),'licenses/'+package))
a = Analysis([str(root/'tools/mod_manager.py')], pathex=[str(root)], datas=data,
             hiddenimports=['pyodbc','PIL.Image','PIL.ImageDraw','truststore'],
             excludes=['playwright','numpy','scipy','matplotlib','tkinter','pytest'], noarchive=False)
pyz = PYZ(a.pure)
exe = EXE(pyz,a.scripts,a.binaries,a.datas,[],name='SDGO Mod Manager',console=False,
          debug=False,strip=False,upx=False,disable_windowed_traceback=False,
          icon=str(root/'manager/static/brand.ico'))
