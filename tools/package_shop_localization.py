"""Build a script-free client overlay for SDGO Mod Manager import."""
from __future__ import annotations
import argparse
import configparser
import hashlib
import json
from pathlib import Path
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def build_overlay(binary: Path, output: Path, root: Path = ROOT) -> dict:
    folder = root / 'mods/shop_localization'
    config = configparser.ConfigParser(interpolation=None)
    config.read(folder / 'mod.ini', encoding='utf-8-sig')
    native = binary.read_bytes()
    if len(native) < 256 or native[:2] != b'MZ':
        raise ValueError('Build the native Win32 localization DLL first')
    pe = struct.unpack_from('<I', native, 0x3c)[0]
    if native[pe:pe+4] != b'PE\0\0' or struct.unpack_from('<H', native, pe+4)[0] != 0x14c:
        raise ValueError('Localization DLL must be native x86')
    prefix = 'client/sdgo/mods/shop_localization/'
    payload = {prefix + 'shop_localization.dll': native}
    for name in ('mod.ini', 'manager.json', 'zh-CN.ini', 'symbols.fragment.ini'):
        payload[prefix + name] = (folder / name).read_bytes()
    payload['symbols.fragment.ini'] = (folder / 'symbols.fragment.ini').read_bytes()
    payload['README.zh-CN.md'] = (root / 'docs/shop-localization.zh-CN.md').read_bytes()
    payload['README.md'] = (root / 'docs/shop-localization.md').read_bytes() + b'\n\n' + (root / 'LICENSE').read_bytes()
    manifest = {'schema_version': 1, 'id': 'shop_localization', 'version': config['mod']['version'],
                'activation': 'restart_required', 'files': {name: hashlib.sha256(data).hexdigest() for name, data in payload.items()}}
    payload['manifest.json'] = json.dumps(manifest, ensure_ascii=False, indent=2).encode('utf-8')
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(payload.items()):
            archive.writestr(name, data)
    with zipfile.ZipFile(output) as archive:
        if archive.testzip() is not None or set(archive.namelist()) != set(payload):
            raise ValueError('Localization overlay readback failed')
    return {'path': str(output.resolve()), 'sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
            'files': len(payload), 'version': config['mod']['version']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'build/Release/shop_localization.dll')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(build_overlay(args.binary, args.output)))


if __name__ == '__main__':
    main()
