"""Persist the verified native ledger identity independently of worker uptime."""
from pathlib import Path
import configparser
import uuid
from .runtime_control import _atomic_write

PROTOCOL='sdgo-native-wallet-scope-v1'


def publish(runtime, scope):
    root=Path(runtime)
    if root.is_symlink():raise ValueError('Linked wallet runtime is unsupported')
    root=root.resolve();path=root/'wallet-binding.ini'
    expected=str(uuid.UUID(str(scope)))
    if path.is_symlink():raise ValueError('Linked wallet binding is unsupported')
    if path.exists():
        parser=configparser.ConfigParser(interpolation=None,strict=True)
        parser.optionxform=str
        parser.read_string(path.read_text(encoding='ascii'))
        if parser.sections()!=['binding'] or dict(parser['binding'])!={
                'SchemaVersion':'1','Protocol':PROTOCOL,'ScopeId':expected}:
            raise ValueError('Wallet binding differs from the verified native ledger; choose its original runtime')
        return path
    root.mkdir(parents=True,exist_ok=True)
    _atomic_write(path,('[binding]\r\nSchemaVersion=1\r\nProtocol='+PROTOCOL+'\r\nScopeId='+expected+'\r\n').encode('ascii'))
    return path
