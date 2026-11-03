"""Manager adapter for the trusted, receipt-aware unit package installer."""
from __future__ import annotations
import io
import json
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile


def install_package(client, raw):
    from manager.windows import powershell_environment,powershell_executable
    if len(raw) > 160 * 1024 * 1024: raise ValueError('机体客户端包超过 160 MiB')
    script = Path(__file__).resolve().parents[2]/'tools/install_unit_catalog.ps1'
    with tempfile.TemporaryDirectory(prefix='sdgo-unit-install-',ignore_cleanup_errors=True) as directory:
        root = Path(directory)
        seen = set(); total = 0
        with zipfile.ZipFile(io.BytesIO(raw)) as archive:
            if len(archive.infolist()) > 4096: raise ValueError('机体包文件过多')
            for entry in archive.infolist():
                if entry.is_dir(): continue
                name = entry.filename.replace('\\', '/')
                parts = name.split('/')
                if (name.casefold() in seen or any(p in ('', '.', '..') or p.endswith((' ', '.'))
                    or re.search(r'[\x00-\x1f:*?"<>|]', p)
                    or re.fullmatch(r'(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(?:\..*)?', p, re.I) for p in parts)
                    or (entry.external_attr >> 16) & 0o170000 == 0o120000):
                    raise ValueError('机体包包含重复或不安全路径')
                seen.add(name.casefold());total += entry.file_size
                if entry.file_size > 64 * 1024 * 1024 or total > 160 * 1024 * 1024:
                    raise ValueError('机体包解压内容超过限制')
                target = root.joinpath(*parts);target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(archive.read(entry))
        # Never execute Install-UnitCatalog.ps1 or any code supplied by the ZIP.
        result = subprocess.run([powershell_executable(), '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
            '-File', str(script), '-ClientRoot', str(Path(client).resolve()), '-Source', str(root)],
            capture_output=True, text=True, encoding='utf-8', errors='replace',
            env=powershell_environment(),
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        if result.returncode:
            raise ValueError('机体包安装未完成：'+(result.stderr or result.stdout)[-3000:])
        receipt = Path(client)/'sdgo/mods/unit_catalog/install-receipt.json'
        if not receipt.is_file(): raise ValueError('机体包安装没有返回收据，请检查客户端安装记录')
        return {'module': 'unit_catalog', 'activation': 'restart_required',
                'receipt': json.loads(receipt.read_text(encoding='utf-8-sig'))}
