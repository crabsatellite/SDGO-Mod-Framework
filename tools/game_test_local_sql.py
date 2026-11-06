"""Run the copied native SQL engine with per-process HKCU instance settings.

No Docker, installed Windows service, machine registry write or production
connection is needed. The caller supplies an already copied TEST-ONLY tree.
"""
from pathlib import Path
import argparse
import json
import os
import re
import subprocess
import sys
import socket
import time
import winreg

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))


def test_root(value):
    root = Path(value).resolve()
    if not (root / 'TEST-ONLY.txt').is_file():
        raise RuntimeError('Select an explicitly marked TEST-ONLY local copy.')
    if not (root / 'server/LiteSQL/MSSQL/Binn/sqlservr.exe').is_file():
        raise RuntimeError('The copied native SQL engine is missing.')
    return root


def configure(root, name, port):
    if not re.fullmatch(r'SDGO[A-Z0-9_]{1,24}', name) or not 1024 <= port <= 65535:
        raise RuntimeError('Use a distinct SDGO test instance name and unprivileged port.')
    hive = 'Software\\SDGO\\NativeTest\\' + name
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, hive) as key:
            registered_root = winreg.QueryValueEx(key, 'TestRoot')[0]
        if Path(registered_root).resolve() != root:
            raise RuntimeError('That test instance belongs to another copy; select a unique --name.')
    except FileNotFoundError:
        pass
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, hive) as key:
        winreg.SetValueEx(key, 'TestRoot', 0, winreg.REG_SZ, str(root))
    source = 'SOFTWARE\\Microsoft\\Microsoft SQL Server'
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, source + '\\Instance Names\\SQL') as key:
        original_id = winreg.QueryValueEx(key, 'MSSQLSERVER')[0]
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, source + '\\' + original_id + '\\Setup') as key:
        original_path = winreg.QueryValueEx(key, 'SQLPath')[0].rstrip('\\/')
    sql = root / 'server/LiteSQL'
    new_path = str(sql)

    def clone(parent, source_key, target_key):
        with winreg.OpenKey(parent, source_key, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
            with winreg.CreateKey(winreg.HKEY_CURRENT_USER, target_key) as target:
                children, values, _ = winreg.QueryInfoKey(key)
                for i in range(values):
                    label, value, kind = winreg.EnumValue(key, i)
                    if isinstance(value, str):
                        value = value.replace(original_path, new_path)
                    elif isinstance(value, list):
                        value = [v.replace(original_path, new_path) for v in value]
                    winreg.SetValueEx(target, label, 0, kind, value)
            for i in range(children):
                child = winreg.EnumKey(key, i)
                clone(parent, source_key + '\\' + child, target_key + '\\' + child)

    sql_key = hive + '\\Microsoft SQL Server'
    clone(winreg.HKEY_LOCAL_MACHINE, source, sql_key)
    legacy = 'SOFTWARE\\Microsoft\\MSSQLServer'
    try:
        clone(winreg.HKEY_LOCAL_MACHINE, legacy, hive + '\\MSSQLServer')
    except FileNotFoundError:
        pass
    instance = 'MSSQL12.' + name
    clone(winreg.HKEY_LOCAL_MACHINE, source + '\\' + original_id, sql_key + '\\' + instance)

    def put(subkey, label, value):
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, sql_key + '\\' + subkey) as key:
            kind = winreg.REG_DWORD if isinstance(value, int) else winreg.REG_SZ
            winreg.SetValueEx(key, label, 0, kind, value)

    put('Instance Names\\SQL', name, instance)
    prefix = instance + '\\MSSQLServer'
    for i, argument in enumerate(('-d' + str(sql / 'MSSQL/DATA/master.mdf'),
                                  '-e' + str(sql / 'MSSQL/Log/ERRORLOG'),
                                  '-l' + str(sql / 'MSSQL/DATA/mastlog.ldf'))):
        put(prefix + '\\Parameters', 'SQLArg' + str(i), argument)
    for label in ('DefaultData', 'DefaultLog', 'BackupDirectory'):
        put(prefix, label, str(sql / 'MSSQL/DATA'))
    net = prefix + '\\SuperSocketNetLib'
    tcp = net + '\\Tcp'
    put(tcp, 'Enabled', 1)
    put(tcp, 'ListenOnAllIPs', 0)
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, sql_key + '\\' + tcp) as key:
        ips = [winreg.EnumKey(key, i) for i in range(winreg.QueryInfoKey(key)[0])]
    for ip in ips:
        if ip != 'IPAll':
            put(tcp + '\\' + ip, 'Enabled', 0)
    for label, value in (('Enabled', 1), ('Active', 1), ('IpAddress', '127.0.0.1'),
                         ('TcpPort', str(port)), ('TcpDynamicPorts', '')):
        put(tcp + '\\IP1', label, value)
    put(tcp + '\\IPAll', 'TcpPort', str(port))
    put(tcp + '\\IPAll', 'TcpDynamicPorts', '')
    put(net + '\\Np', 'Enabled', 0)
    # The portable engine's SNI also reads legacy/shared SQL branches. Confine
    # all of those private registry views, not just the named instance branch.
    def configure_protocols(path):
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path) as key:
            children = [winreg.EnumKey(key, i) for i in range(winreg.QueryInfoKey(key)[0])]
        def set_value(key_path, label, value):
            with winreg.CreateKey(winreg.HKEY_CURRENT_USER, key_path) as key:
                winreg.SetValueEx(key, label, 0,
                    winreg.REG_DWORD if isinstance(value, int) else winreg.REG_SZ, value)
        if path.lower().endswith('\\supersocketnetlib\\tcp'):
            set_value(path, 'Enabled', 1)
            set_value(path, 'ListenOnAllIPs', 0)
            set_value(path, 'TcpPort', str(port))
            for ip in children:
                if ip != 'IPAll':
                    set_value(path + '\\' + ip, 'Enabled', 0)
            for label, value in (('Enabled', 1), ('Active', 1), ('IpAddress', '127.0.0.1'),
                                 ('TcpPort', str(port)), ('TcpDynamicPorts', '')):
                set_value(path + '\\IP1', label, value)
            set_value(path + '\\IPAll', 'TcpPort', str(port))
            set_value(path + '\\IPAll', 'TcpDynamicPorts', '')
        if path.lower().endswith(('\\supersocketnetlib\\np', '\\supersocketnetlib\\sm', '\\supersocketnetlib\\via')):
            set_value(path, 'Enabled', 0)
        for child in children:
            configure_protocols(path + '\\' + child)
    configure_protocols(hive)
    # SQL requests file security access in addition to normal read/write.
    # Grant only the current account, and only within this marked test copy.
    sid = subprocess.check_output(['whoami', '/user', '/fo', 'csv', '/nh'], text=True)
    sid = re.search(r'S-1-5-[0-9-]+', sid).group(0)
    subprocess.run(['icacls', str(sql / 'MSSQL'), '/grant', '*' + sid + ':(OI)(CI)F', '/T', '/Q'],
        check=True, stdout=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
    data = {'name': name, 'port': port, 'registry': hive, 'root': str(root)}
    (root / 'database').mkdir(exist_ok=True)
    (root / 'database/native-sql.json').write_text(json.dumps(data, indent=2))
    return data


def start(root, binaries, master_only=False):
    data = json.loads((root / 'database/native-sql.json').read_text())
    if data['root'] != str(root):
        raise RuntimeError('A copied test needs its own configure step before starting SQL.')
    with socket.socket() as probe:
        if probe.connect_ex(('127.0.0.1', int(data['port']))) == 0:
            raise RuntimeError('The selected test SQL endpoint is already running; reuse it.')
    engine = root / 'server/LiteSQL/MSSQL'
    (engine / 'Log').mkdir(exist_ok=True)
    environment = dict(os.environ, SDGO_TEST_SQL_REGISTRY=data['registry'],SDGO_TEST_SQL_DATA_ROOT=str(engine/'DATA'))
    executable = binaries / 'sql_local.exe'
    dll = binaries / 'sql_registry.dll'
    command = [str(executable), str(dll), str(engine / 'Binn/sqlservr.exe'),
        '-c', '-s' + data['name'], '-d' + str(engine / 'DATA/master.mdf'),
        '-l' + str(engine / 'DATA/mastlog.ldf'), '-e' + str(engine / 'Log/ERRORLOG'),
        '-T4022']
    if master_only:
        command.append('-T3608')
    result = subprocess.run(command, cwd=engine / 'Binn', env=environment,
        capture_output=True, text=True, timeout=75, check=True,
        creationflags=subprocess.CREATE_NO_WINDOW)
    data['pid'] = int(result.stdout.strip())
    (root / 'database/native-sql.json').write_text(json.dumps(data, indent=2))
    deadline = time.monotonic() + 45
    while True:
        with socket.socket() as probe:
            probe.settimeout(0.2)
            if probe.connect_ex(('127.0.0.1', int(data['port']))) == 0:
                break
        if time.monotonic() > deadline:
            raise RuntimeError('Native test SQL did not open its loopback endpoint; inspect its local ERRORLOG.')
        time.sleep(0.2)
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('configure', 'start'))
    parser.add_argument('--root', type=test_root, required=True)
    parser.add_argument('--name', default='SDGOTEST')
    parser.add_argument('--port', type=int, default=61433)
    parser.add_argument('--binaries', type=Path, default=ROOT / 'build/native-sql/Release')
    parser.add_argument('--master-only', action='store_true', help='first copied-catalog relocation only; never normal gameplay')
    args = parser.parse_args()
    result = configure(args.root, args.name, args.port) if args.command == 'configure' else start(args.root, args.binaries.resolve(), args.master_only)
    print(json.dumps(result))


if __name__ == '__main__':
    main()
