"""Native backend implementation for the managed GameTest environment entrypoint."""
from __future__ import annotations
import argparse
import configparser
import contextlib
import hashlib
import json
import ipaddress
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys
import time
import uuid
import xml.etree.ElementTree as ET

from tools import game_test as gt
from tools.game_test_environment import Registry, ManagedEnvironment, atomic_json, process_identity, no_reparse_tree, ROOT
from tools.game_test_sql_bridge import SqlBridge
from tools.game_test_fixtures import build_accounts, verify_accounts

IMAGE = 'mcr.microsoft.com/mssql/server@sha256:cf8f0ab2ebe61feec2eed2629061a07d247f791c2a8ddda74a4ac9f06a291718'
DATABASES = ('GameRank','GDClan','GDCommon','GDGameLogView','GDLogDB','GDLongTermGameLogDB',
             'GDStoreBilling','Goldensoftstat','GOnlineGame','JobMonitoring','MonitorDB')
LABEL = 'sdgo.gametest.environment'


def quoted(value):
    return "N'" + str(value).replace("'", "''") + "'"


def identifier(value):
    return '[' + value.replace(']', ']]') + ']'


def docker(*args, env=None, missing=False, timeout=180):
    result = subprocess.run(['docker', *args], env=env, input='', capture_output=True, text=True,
        creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0), timeout=timeout)
    if result.returncode:
        if missing and any(s in result.stderr.lower() for s in ('no such object', 'no such container', 'no such volume')):
            return None
        # Credentials are never put into a diagnostic exception or printed argv.
        details=(result.stderr+'\n'+result.stdout).strip()
        if env:
            for key in ('SQLCMDPASSWORD','MSSQL_SA_PASSWORD'):
                if env.get(key):details=details.replace(env[key],'[redacted]')
        for value in re.findall(r"PASSWORD=N?'([^']+)'",' '.join(args),re.I):
            details=details.replace(value,'[redacted]')
        raise RuntimeError(f'Docker {args[0]} failed ({result.returncode}): '+details[-1000:])
    return result.stdout.strip()


def labels_for(lease):
    return {'sdgo.gametest.framework': '2', LABEL: lease.id,
            'sdgo.gametest.owner': hashlib.sha256(lease.token.encode()).hexdigest()}


def check_labels(description, lease):
    labels = description.get('Config', {}).get('Labels') if 'Config' in description else description.get('Labels')
    if any((labels or {}).get(k) != v for k, v in labels_for(lease).items()):
        raise RuntimeError('Refusing an unowned Docker resource.')


def child_environment():
    env = os.environ.copy()
    env.update(CommonProgramFiles=r'C:\Program Files (x86)\Common Files',
               CommonProgramW6432=r'C:\Program Files\Common Files', __COMPAT_LAYER='RunAsInvoker')
    env['CommonProgramFiles(x86)'] = r'C:\Program Files (x86)\Common Files'
    return env


def executable_path(pid):
    if os.name != 'nt':
        with contextlib.suppress(OSError):
            return Path(f'/proc/{pid}/exe').resolve()
        return None
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.QueryFullProcessImageNameW.argtypes = (wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD))
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    handle = kernel.OpenProcess(0x1000, False, pid)
    if not handle:
        return None
    try:
        size = wintypes.DWORD(32768)
        value = ctypes.create_unicode_buffer(size.value)
        return Path(value.value).resolve() if kernel.QueryFullProcessImageNameW(handle, 0, value, ctypes.byref(size)) else None
    finally:
        kernel.CloseHandle(handle)


class NativeDriver:
    def __init__(self, registry):
        self.registry = registry
        self.bridge = None
        self.processes = []
        self.streams = []
        self.private = {}
        self.environment = None
        self.connections = []

    def record_process(self, lease, process, exe, kind='native_process'):
        self.record_pid(lease, process.pid, exe, kind)

    def record_pid(self, lease, pid, exe, kind='native_process'):
        birth = process_identity(pid)
        if birth:
            self.registry.record(lease, {'kind': kind, 'pid': pid, 'birth': birth,
                'exe': str(Path(exe).resolve())})

    def progress(self, phase, **details):
        value = {'phase': phase, 'environment_id': self.environment.id, **details}
        atomic_json(self.environment.root / 'progress.json', value)
        print(json.dumps(value), file=sys.stderr, flush=True)

    def sql(self, statement):
        env = os.environ.copy()
        env['SQLCMDPASSWORD'] = self.private['sa_password']
        return docker('exec', '-e', 'SQLCMDPASSWORD', self.private['container'],
            '/opt/mssql-tools18/bin/sqlcmd', '-S', 'tcp:127.0.0.1,1433', '-U', 'sa', '-C', '-I',
            '-b', '-l', '5', '-t', '120', '-h', '-1', '-W', '-Q', statement, env=env)

    def start_sql(self, lease):
        self.environment = lease
        self.progress('sql_starting')
        prefix = 'sdgo-gt-' + lease.id.replace('-', '')[:20]
        self.private = {'container': prefix, 'volume': prefix + '-data',
                        'sa_password': 'Native+' + secrets.token_urlsafe(24),
                        'app_user': 'native_game_test', 'app_password': 'Native+' + secrets.token_urlsafe(24)}
        atomic_json(lease.root / 'private/instance.json', self.private)
        labels = labels_for(lease)
        flags = [x for k, v in labels.items() for x in ('--label', k+'='+v)]
        self.registry.record(lease, {'kind': 'volume', 'name': self.private['volume']})
        docker('volume', 'create', *flags, self.private['volume'])
        self.registry.record(lease, {'kind': 'container', 'name': prefix})
        env = os.environ.copy()
        env['MSSQL_SA_PASSWORD'] = self.private['sa_password']
        cid = docker('create', '--name', prefix, *flags, '--network', 'none', '--cpus', '1.5', '--memory', '4g',
            '--env', 'ACCEPT_EULA=Y', '--env', 'MSSQL_PID=Developer', '--env', 'MSSQL_SA_PASSWORD',
            '--env', 'MSSQL_COLLATION=Chinese_PRC_CI_AS',
            '--env', 'MSSQL_MEMORY_LIMIT_MB=2048', '--mount',
            'type=volume,source='+self.private['volume']+',target=/var/opt/mssql', IMAGE, env=env)
        self.private['container_id'] = cid
        atomic_json(lease.root / 'private/instance.json', self.private)
        description = json.loads(docker('inspect', prefix))[0]
        check_labels(description, lease)
        if description['HostConfig']['NetworkMode'] != 'none' or any(x['Type'] != 'volume' for x in description['Mounts']):
            raise RuntimeError('Private SQL must have network=none and no host bind mounts.')
        docker('start', prefix)
        deadline = time.monotonic() + 150
        while True:
            try:
                if self.sql('SET NOCOUNT ON;SELECT 1;').strip().endswith('1'):
                    break
            except RuntimeError:
                pass
            if time.monotonic() > deadline:
                raise RuntimeError('Owned private SQL startup timed out.')
            time.sleep(1)
        self.bridge = SqlBridge(prefix, on_process=lambda p: self.record_process(lease, p, shutil.which('docker'), 'sql_transport'))
        self.private['port'] = self.bridge.port
        atomic_json(lease.root / 'private/instance.json', self.private)
        self.progress('sql_ready')

    def connect(self):
        import pyodbc
        result = pyodbc.connect('DRIVER={SQL Server};SERVER=127.0.0.1,'+str(self.private['port'])+
            ';DATABASE=GOnlineGame;UID=sa;PWD='+self.private['sa_password']+';', timeout=5)
        result.timeout = 120
        self.connections.append(result)
        return result

    def restore(self, manifest, snapshots):
        if {b['name'] for b in manifest['backups']} != set(DATABASES):
            raise RuntimeError('A complete native database snapshot set is required.')
        docker('exec', self.private['container'], 'mkdir', '-p', '/var/opt/mssql/backup/gametest')
        for entry in manifest['backups']:
            self.progress('restoring', database=entry['name'])
            if Path(entry['file']).name != entry['file']:
                raise RuntimeError('Snapshot path must be a single local filename.')
            source = snapshots / entry['file']
            if gt.sha256(source) != entry['sha256']:
                raise RuntimeError('Native snapshot bytes differ from the frozen manifest.')
            destination = '/var/opt/mssql/backup/gametest/' + entry['file']
            docker('cp', str(source), self.private['container']+':'+destination)
            moves = ','.join('MOVE '+quoted(f['name'])+' TO '+quoted(
                '/var/opt/mssql/data/gt_'+entry['name']+'_'+str(i)+('.ldf' if f['type']=='L' else '.mdf'))
                for i, f in enumerate(entry['files']))
            name = identifier(entry['name'])
            self.sql('RESTORE DATABASE '+name+' FROM DISK='+quoted(destination)+' WITH CHECKSUM,RECOVERY,'+moves+';')
        user = self.private['app_user']
        self.sql('CREATE LOGIN '+identifier(user)+' WITH PASSWORD='+quoted(self.private['app_password'])+
                 ',CHECK_POLICY=OFF;GRANT VIEW SERVER STATE TO '+identifier(user)+
                 ';GRANT VIEW SERVER PERFORMANCE STATE TO '+identifier(user)+';')
        for db in DATABASES:
            self.sql('USE '+identifier(db)+';IF USER_ID('+quoted(user)+') IS NULL CREATE USER '+identifier(user)+
                ' FOR LOGIN '+identifier(user)+';ALTER USER '+identifier(user)+' WITH LOGIN='+identifier(user)+
                ';ALTER ROLE db_owner ADD MEMBER '+identifier(user)+';ALTER DATABASE '+identifier(db)+
                ' SET TRUSTWORTHY OFF;ALTER DATABASE '+identifier(db)+' SET DB_CHAINING OFF;')
        self.progress('databases_ready')

    def cleanup(self, lease, resources):
        errors = []
        # Use captured handles first. A failed client/server never owns a
        # global process name, another environment or an original service.
        for process in reversed(self.processes):
            try:
                if process.poll() is None:
                    process.kill(); process.wait(timeout=10)
                process.close()
            except Exception as error:
                errors.append(type(error).__name__)
        for connection in self.connections:
            with contextlib.suppress(Exception):connection.close()
        if self.bridge:
            try:
                self.bridge.close()
            except Exception as error:
                errors.append(type(error).__name__)
        for stream in self.streams:
            stream.close()
        seen = set()
        for resource in reversed(resources):
            if resource['kind'] in ('native_process', 'sql_transport'):
                key = (resource['pid'], resource['birth'])
                if key in seen:
                    continue
                seen.add(key)
                if process_identity(resource['pid']) != resource['birth']:
                    continue
                exe = executable_path(resource['pid'])
                expected = Path(resource['exe']).resolve()
                allowed = expected.is_relative_to(lease.root) if resource['kind']=='native_process' else expected.name.casefold() in ('docker','docker.exe')
                if exe != expected or not allowed:
                    errors.append('ProcessOwnershipMismatch')
                    continue
                try:
                    self.kill_recorded_process(resource)
                except Exception as error:
                    errors.append(type(error).__name__)
        for kind, noun in (('container', 'container'), ('volume', 'volume')):
            names = {r['name'] for r in resources if r['kind'] == kind}
            for name in names:
                try:
                    output = docker(noun, 'inspect', name, missing=True)
                    if output is None:
                        continue
                    description = json.loads(output)[0]
                    check_labels(description, lease)
                    if kind == 'container':
                        docker('stop','--time','15',name,timeout=30)
                        docker('rm',name)
                    else:
                        docker('volume','rm',name)
                    if docker(noun,'inspect',name,missing=True) is not None:
                        raise RuntimeError('Owned Docker resource remains.')
                except Exception as error:
                    errors.append(type(error).__name__)
        return {'status': 'clean' if not errors else 'cleanup_required', 'errors': errors,
                'environment_id': lease.id, 'original_services_touched': False}

    @staticmethod
    def kill_recorded_process(resource):
        if os.name != 'nt':
            import signal
            os.kill(resource['pid'], signal.SIGTERM)
            return
        import ctypes
        from ctypes import wintypes
        kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        kernel.OpenProcess.argtypes=(wintypes.DWORD,wintypes.BOOL,wintypes.DWORD);kernel.OpenProcess.restype=wintypes.HANDLE
        kernel.TerminateProcess.argtypes=(wintypes.HANDLE,wintypes.UINT)
        kernel.WaitForSingleObject.argtypes=(wintypes.HANDLE,wintypes.DWORD)
        kernel.CloseHandle.argtypes=(wintypes.HANDLE,)
        handle=kernel.OpenProcess(0x100001,False,resource['pid'])
        if not handle:
            raise RuntimeError('Cannot close the exact owned process.')
        try:
            if not kernel.TerminateProcess(handle,1) or kernel.WaitForSingleObject(handle,10000)!=0:
                raise RuntimeError('Owned process did not stop.')
        finally:
            kernel.CloseHandle(handle)

    def archive(self, lease, archive):
        if not lease.root.exists():
            return
        private_file = lease.root / 'private/instance.json'
        secret_values = list(self.private.get(k, '') for k in ('sa_password', 'app_password'))
        if private_file.is_file():
            private = json.loads(private_file.read_text(encoding='utf-8'))
            secret_values += [private.get(k, '') for k in ('sa_password', 'app_password')]
        for profile in lease.root.glob('private/accounts/*.ini'):
            secret_values.append(gt.read_ini(profile).get('account', 'password', fallback=''))
        def copy_evidence(source):
            target=archive/source.relative_to(lease.root)
            target.parent.mkdir(parents=True,exist_ok=True)
            if source.suffix.lower()=='.dmp':
                # Binary dumps can contain credentials. Retain only a digest.
                atomic_json(target.with_suffix('.dmp.json'), {'sha256':gt.sha256(source),'bytes':source.stat().st_size})
                return
            raw=source.read_bytes()
            for secret in filter(None, secret_values):
                for encoding in ('utf-8','utf-16-le'):
                    raw=raw.replace(secret.encode(encoding),'[redacted]'.encode(encoding))
            target.write_bytes(raw)
        for relative in ('result.json','fixture-binding.json','progress.json','launch-selection.json','native-network.json','client-run/run.json'):
            source=lease.root/relative
            if source.is_file():
                copy_evidence(source)
        for source in lease.root.rglob('*'):
            if not source.is_file() or source.suffix.lower() not in ('.jsonl','.log','.dmp','.xml'):
                continue
            if 'private' in source.relative_to(lease.root).parts:
                continue
            if source.suffix.lower()=='.xml' and 'receipts' not in source.parts:
                continue
            copy_evidence(source)
        for report in lease.root.glob('client-run/receipts/*.json'):
            copy_evidence(report)


def copy_tree(source, target):
    no_reparse_tree(source)
    def ignored(folder,names):
        return gt.copy_ignore(folder,names) | {n for n in names if n.lower()=='logdata' or n.lower().endswith('.dmp')}
    shutil.copytree(source,target,ignore=ignored)


def rebase_configuration(root, old_root, new_root, ip, private, *, old_ip, old_sql_port=None, old_password=None):
    old_forms=(str(old_root),old_root.as_posix(),json.dumps(str(old_root))[1:-1])
    new_forms=(str(new_root),new_root.as_posix(),json.dumps(str(new_root))[1:-1])
    for file in root.rglob('*'):
        if not file.is_file() or file.suffix.lower() not in ('.ini','.json','.txt','.cfg','.sql'):
            continue
        raw=file.read_bytes();updated=raw
        for encoding in ('utf-8','utf-16-le','cp936'):
            for old,new in zip(old_forms,new_forms):updated=updated.replace(old.encode(encoding),new.encode(encoding))
        old_prefix=old_ip.rsplit('.',1)[0]
        new_prefix=ip.rsplit('.',1)[0]
        updated=re.sub(re.escape(old_prefix.encode())+rb'\.([0-9]{1,3})(?![0-9])',
            lambda m:new_prefix.encode()+b'.'+m.group(1),updated)
        updated=re.sub(re.escape(old_prefix.encode('utf-16-le'))+rb'\.\x00((?:[0-9]\x00){1,3})',
            lambda m:new_prefix.encode('utf-16-le')+b'.\x00'+m.group(1),updated)
        for encoding in ('utf-8','utf-16-le'):
            if old_sql_port:
                updated=updated.replace(('127.0.0.1,'+str(old_sql_port)).encode(encoding),
                    ('127.0.0.1,'+str(private['port'])).encode(encoding))
            if old_password:
                updated=updated.replace(old_password.encode(encoding),private['app_password'].encode(encoding))
        if updated!=raw:file.write_bytes(updated)
    from manager.storage import ini_values
    for folder in (root/'Server').iterdir():
        for path in folder.glob('*.ini'):
            cfg=ini_values(path.read_bytes())
            for section in cfg.sections():
                if cfg.has_option(section,'DBServerName'):
                    cfg[section]['DBServerName']='127.0.0.1,'+str(private['port'])
                    cfg[section]['ID']=private['app_user'];cfg[section]['PWD']=private['app_password']
            with path.open('w',encoding='cp936') as stream:cfg.write(stream,space_around_delimiters=False)


def template_description(path):
    path=path.resolve()
    no_reparse_tree(path)
    data=json.loads((path/'template.json').read_text(encoding='utf-8'))
    if data.get('schema')!=1 or data.get('kind')!='native-gametest-template' or len(data.get('accounts',[]))!=5:
        raise RuntimeError('A sealed five-account native template is required.')
    expected={item['path'] for item in data['files']}|{'template.json'}
    if {file.relative_to(path).as_posix() for file in path.rglob('*') if file.is_file()}!=expected:
        raise RuntimeError('Sealed template file set changed.')
    for item in data['files']:
        relative=Path(item['path'])
        if relative.is_absolute() or '..' in relative.parts:
            raise RuntimeError('Template manifest path escapes its directory.')
        file=path/relative
        if file.resolve().is_relative_to(path) is False or gt.sha256(file)!=item['sha256']:
            raise RuntimeError('Sealed template input changed: '+relative.as_posix())
    return data


def build_template(prototype, destination, client_template, registry, stage):
    prototype=prototype.resolve();destination=destination.resolve();client_template=client_template.resolve()
    if destination.exists():raise RuntimeError('Template destination already exists; templates are immutable.')
    allowed=(ROOT/'build/game-test-templates').resolve()
    private_cache=(Path(os.environ.get('LOCALAPPDATA',Path.home()/'.local/share'))/'SDGO-GameTest/templates').resolve()
    if not destination.is_relative_to(allowed) and not destination.is_relative_to(private_cache):
        raise RuntimeError('Template destination must stay in the project build cache or private GameTest template cache.')
    status=prototype/'status.json'
    if status.exists() and json.loads(status.read_text(encoding='utf-8')).get('phase')!='stopped':
        raise RuntimeError('Import a stopped, frozen prototype; never copy another active test backend.')
    original=json.loads((prototype/'backend.json').read_text(encoding='utf-8'))
    private_file=prototype/'private.fps-session.json'
    if not private_file.exists():private_file=prototype/'private.json'
    source_private=json.loads(private_file.read_text(encoding='utf-8'))
    if source_private['owner']!=str(prototype) or len(source_private['app_password'])<16 or source_private['app_user'].lower()=='sa':
        raise RuntimeError('Import a verified private native prototype, not a live player database.')
    if not original.get('prepared') or not original.get('isolation') or original['isolation'].get('gameplay_rows_seeded'):
        raise RuntimeError('Prototype must contain complete real, unseeded COPY_ONLY snapshots.')
    lease=registry.allocate(ROOT/'build/game-test-environments','template-build')
    driver=NativeDriver(registry)
    output=lease.root/'template-output';output.mkdir()
    template_id=str(uuid.uuid4())
    with ManagedEnvironment(registry,lease,driver,ROOT/'build/game-test-receipts'):
        driver.start_sql(lease)
        driver.restore(original,prototype/'backups')
        with driver.connect() as connection:
            driver.progress('preparing_five_accounts')
            accounts=build_accounts(connection,output/'private/accounts',template_id,isolated_template=True)
        driver.progress('five_accounts_verified', accounts=len(accounts))
        # Keep the proven native runtime, not active processes or historical
        # logs. All copied references are moved into the immutable template.
        for name in ('Server','reward-owner','reward-runtime','ServerStartUp'):
            if (prototype/name).is_dir():copy_tree(prototype/name,output/name)
        if not (output/'ServerStartUp/ServerStartUp.xml').is_file():
            raise RuntimeError('The prototype needs its authoritative native startup XML.')
        normalized={'port':driver.private['port'],'app_user':source_private['app_user'],
                    'app_password':source_private['app_password']}
        rebase_configuration(output,prototype,output,original['ip'],normalized,
            old_ip=original['ip'],old_sql_port=source_private['port'])
        snapshots=output/'backups';snapshots.mkdir()
        driver.sql('CHECKPOINT;')
        for entry in original['backups']:
            driver.progress('sealing_snapshot', database=entry['name'])
            name=entry['name'];remote='/var/opt/mssql/backup/gametest/fixture_'+entry['file']
            driver.sql('BACKUP DATABASE '+identifier(name)+' TO DISK='+quoted(remote)+' WITH COPY_ONLY,CHECKSUM,INIT;')
            driver.sql('RESTORE VERIFYONLY FROM DISK='+quoted(remote)+' WITH CHECKSUM;')
            target=snapshots/entry['file'];docker('cp',driver.private['container']+':'+remote,str(target))
            entry['sha256']=gt.sha256(target);entry['bytes']=target.stat().st_size
        # Rebase once more from the temporary builder path to the durable
        # cache location. Runtime instances never write into this cache.
        rebase_configuration(output,output,destination,original['ip'],
            {'port':source_private['port'],'app_user':source_private['app_user'],'app_password':source_private['app_password']},
            old_ip=original['ip'],old_sql_port=driver.private['port'])
        data={'schema':1,'kind':'native-gametest-template','id':template_id,'base_ip':original['ip'],
            'source_root':str(destination),'native_db_user':source_private['app_user'],
            'native_sql_port':source_private['port'],'native_db_password':source_private['app_password'],
            'backups':original['backups'],'services':original['services'],'accounts':accounts,
            'client_template':str(client_template),'fixture_provenance':'explicit five-account template fixtures; native registration/user/unit procedures',
            'original_databases_written':False,'files':[]}
        for service in data['services']:
            service['exe']=Path(service['exe']).relative_to(prototype).as_posix()
        for file in output.rglob('*'):
            if file.is_file():data['files'].append({'path':file.relative_to(output).as_posix(),'sha256':gt.sha256(file)})
        # Native connection credentials stay in a private file, never CLI JSON.
        native_secret={'native_db_password':data.pop('native_db_password')}
        atomic_json(output/'private/native.json',native_secret)
        data['files'].append({'path':'private/native.json','sha256':gt.sha256(output/'private/native.json')})
        atomic_json(output/'template.json',data)
        destination.parent.mkdir(parents=True,exist_ok=True)
        shutil.move(str(output),str(destination))
        registry.register_template(destination,template_id)
        atomic_json(lease.root/'result.json',{'status':'template_ready','template':str(destination),'accounts':5})
    return {'status':'template_ready','template':str(destination),'template_id':template_id,'accounts':5,
            'runtime_cleanup':'clean'}


def tcp_listeners(pid):
    import ctypes,struct,socket
    from ctypes import wintypes
    size=wintypes.DWORD(65536);data=ctypes.create_string_buffer(size.value)
    value=ctypes.windll.iphlpapi.GetExtendedTcpTable(data,ctypes.byref(size),False,2,3,0)
    if value:raise RuntimeError('Cannot observe native service listeners.')
    rows=[]
    for i in range(struct.unpack_from('<I',data.raw)[0]):
        state,ip,port,_,_,owner=struct.unpack_from('<IIIIII',data.raw,4+i*24)
        if owner==pid and state==2:rows.append((socket.inet_ntoa(struct.pack('<I',ip)),socket.ntohs(port&65535)))
    return rows


def udp_endpoints(pid):
    import ctypes,struct,socket
    from ctypes import wintypes
    size=wintypes.DWORD(65536);data=ctypes.create_string_buffer(size.value)
    value=ctypes.windll.iphlpapi.GetExtendedUdpTable(data,ctypes.byref(size),False,2,1,0)
    if value:raise gt.GameTestError('Cannot observe native service UDP endpoints.')
    rows=[]
    for i in range(struct.unpack_from('<I',data.raw)[0]):
        ip,port,owner=struct.unpack_from('<III',data.raw,4+i*12)
        if owner==pid:rows.append((socket.inet_ntoa(struct.pack('<I',ip)),socket.ntohs(port&65535)))
    return rows


def verify_native_loopback(driver,lease):
    if time.monotonic()<getattr(driver,'next_network_check',0):return
    driver.next_network_check=time.monotonic()+1
    observations=[]
    for process in driver.processes:
        if process.poll() is not None:raise gt.GameTestError('An owned native service exited during GameTest.')
        tcp=tcp_listeners(process.pid);udp=udp_endpoints(process.pid)
        if any(not ipaddress.ip_address(ip).is_loopback for ip,_ in tcp+udp):
            raise gt.GameTestError('An owned native TCP/UDP listener escaped loopback.')
        observations.append({'pid':process.pid,'tcp':tcp,'udp':udp})
    atomic_json(lease.root/'native-network.json',{'status':'loopback_only','services':observations})


def start_native_services(driver,lease,template,client,account):
    from tools.game_test_windows import WindowsBackgroundProcess
    configured={Path(p.attrib['Path'].replace('\\','/')).parent.name
                for p in ET.parse(lease.root/'native/ServerStartUp/ServerStartUp.xml').getroot().findall('Process')
                if p.attrib.get('Run','').lower()=='true'}
    with driver.connect() as connection:
        ports={int(r[0]) for r in connection.cursor().execute('SELECT DISTINCT AddrPT FROM dbo.tbl_ChannelInfo').fetchall()}
    selected=[s for s in template['services'] if s['folder'] in configured and
              (not s['folder'].startswith('Battle') or s['port'] in ports)]
    atomic_json(lease.root/'launch-selection.json',{'selected':selected,'enabled_database_ports':sorted(ports)})
    for service in selected:
        executable=lease.root/'native'/service['exe'];folder=executable.parent
        if gt.sha256(executable)!=service['sha256']:
            raise RuntimeError('Native service identity changed: '+service['folder'])
        env=child_environment()
        if service['folder'].startswith('Battle'):
            job=folder/'sdgo/gametest'
            if job.exists():
                no_reparse_tree(job);shutil.rmtree(job)
            job.mkdir(parents=True)
            cfg=gt.ini_parser()
            cfg['run']={'id':client['run_id'],'role':'server','suite':'pvp-ai'}
            cfg['observer']={'nonce':client['plan']['title_nonce'],'host_pilot_id':str(account['account_id']),
                'mode':'0','minimum_ai':str(client['plan'].get('expected_ai',3)),'require_end':'1'}
            gt.write_ini(job/'run.ini',cfg)
            env.update(SDGO_GAME_TEST_RUN=client['run_id'],SDGO_GAME_TEST_ROLE='server')
            settings=gt.read_ini(folder/'sdgo/sdgo.ini')
            for section in ('categories','features','mods'):
                if not settings.has_section(section):settings.add_section(section)
            settings['categories']['development']='1';settings['features']['game-test']='1'
            settings['mods']['game_test_server']='1';settings['mods']['pvp_ai']='1'
            gt.write_ini(folder/'sdgo/sdgo.ini',settings)
        stream=(lease.root/(service['folder']+'.console.log')).open('wb');driver.streams.append(stream)
        process=WindowsBackgroundProcess([str(executable)],cwd=folder,env=env,
            stdout=stream,stderr=subprocess.STDOUT,run_id=lease.id)
        driver.processes.append(process);driver.record_process(lease,process,executable)
        deadline=time.monotonic()+120
        while (lease.ip,service['port']) not in tcp_listeners(process.pid):
            if process.poll() is not None or time.monotonic()>deadline:
                raise RuntimeError('Native service failed readiness: '+service['folder'])
            time.sleep(.2)
        proof=process.verify_background()
        if not(proof['input_desktop_safe'] and proof['input_desktop_unchanged'] and proof['foreground_safe'] and proof['input_target_window_count']==0):
            raise RuntimeError('Native service escaped its private desktop.')
        listeners=tcp_listeners(process.pid)
        if any(not ipaddress.ip_address(ip).is_loopback for ip,_ in listeners):
            raise RuntimeError('Native service listener escaped loopback.')
        driver.progress('service_ready', service=service['folder'], pid=process.pid, listeners=listeners)


def execute_environment(template_path,stage,slot,include_mods,bootstrap_only,registry,apsalus_roundtrip=False):
    template_path=template_path.resolve();template=template_description(template_path)
    account=next((a for a in template['accounts'] if a['slot']==slot),None)
    if account is None:raise RuntimeError('Choose one of the five prepared account slots.')
    gt.validate_observer_stage(Path(template['client_template']),stage.resolve(),tuple(include_mods))
    lease=registry.allocate(ROOT/'build/game-test-environments',template['id'])
    driver=NativeDriver(registry);outcome=None
    managed=ManagedEnvironment(registry,lease,driver,ROOT/'build/game-test-receipts')
    with managed:
        driver.start_sql(lease);driver.private['app_user']=template['native_db_user']
        atomic_json(lease.root/'private/instance.json',driver.private)
        # Restore creates this service login using the current generated secret.
        driver.restore(template,template_path/'backups')
        native=lease.root/'native';native.mkdir()
        for name in ('Server','reward-owner','reward-runtime','ServerStartUp'):
            if (template_path/name).is_dir():copy_tree(template_path/name,native/name)
        secret=json.loads((template_path/'private/native.json').read_text(encoding='utf-8'))
        rebase_configuration(native,template_path,native,lease.ip,driver.private,
            old_ip=template['base_ip'],old_sql_port=template['native_sql_port'],old_password=secret['native_db_password'])
        with driver.connect() as connection:
            verify_accounts(connection,template['accounts'])
            connection.cursor().execute('UPDATE dbo.tbl_ChannelInfo SET AddrIP=?',lease.ip);connection.commit()
        for i in range(1,4):
            path=native/f'Server/AgentServer{i}/sdgo/mods/agent_udp_pool/config.ini'
            if path.is_file():
                cfg=gt.read_ini(path);cfg['settings']['FirstPort']=str(lease.udp_first+(i-1)*8);gt.write_ini(path,cfg)
        profiles=lease.root/'private/accounts';copy_tree(template_path/'private/accounts',profiles)
        run=lease.root/'client-run'
        meta=gt.prepare(Path(template['client_template']),stage,run,target_host=lease.ip,network_policy='direct',
            mode_sequence='1',account_profile=profiles/account['profile'],
            play_unit_uid=None if bootstrap_only else account['primary_uid'],bootstrap_only=bootstrap_only,
            extra_mods=tuple(include_mods),apsalus_roundtrip=apsalus_roundtrip)
        atomic_json(lease.root/'fixture-binding.json',{'template_id':template['id'],'account_slot':slot,
            'account_id':account['account_id'],'units':account['units'],'primary_uid':account['primary_uid'],
            'new_accounts_created_during_run':0,'global_account_profile_used':False})
        start_native_services(driver,lease,template,meta,account)
        registry.update(lease,'running')
        driver.progress('client_running', account_slot=slot)
        outcome=gt.run_prepared(run,600,30,on_process=lambda pid,exe:driver.record_pid(lease,pid,exe),
            on_poll=lambda:verify_native_loopback(driver,lease))
        if not bootstrap_only:
            from tools.game_test_server_report import evaluate
            report=evaluate(meta,list((native/'Server').glob('BattleServer*/sdgo/gametest/server-observer-events.jsonl')),account['account_id'])
            atomic_json(run/'receipts/server-report.json',report)
            outcome['server_observer']=report
            if report['status']!='pass':outcome['status']='fail'
        atomic_json(lease.root/'result.json',outcome)
    return {'status':outcome['status'],'environment_id':lease.id,'evidence':str(managed.archive),
            'account_slot':slot,'cleanup':'clean','new_accounts_created_during_run':0}


def native_main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    commands=parser.add_subparsers(dest='command',required=True)
    commands.add_parser('list',help='show active owners and outstanding cleanup')
    commands.add_parser('templates',help='show reusable existing five-account templates; never create accounts')
    commands.add_parser('recover',help='recover only abandoned environments; never stop a live owner')
    create=commands.add_parser('template',help='build one reusable native template with five prepared accounts')
    create.add_argument('--prototype',type=Path,required=True)
    create.add_argument('--output',type=Path,required=True)
    create.add_argument('--client-template',type=Path,required=True)
    create.add_argument('--stage',type=Path,default=ROOT/'build/stage/Release')
    run=commands.add_parser('run',help='create, test and delete one private loopback environment')
    run.add_argument('--template',type=Path,help='existing sealed template; defaults to latest registered template')
    run.add_argument('--stage',type=Path,default=ROOT/'build/stage/Release')
    run.add_argument('--account',type=int,choices=range(1,6),default=1)
    run.add_argument('--include-mod',action='append',default=[])
    run.add_argument('--bootstrap-only',action='store_true')
    run.add_argument('--apsalus-roundtrip',action='store_true',help='verify two original transform requests and releases during a full battle')
    args=parser.parse_args(argv)
    registry=Registry()
    if args.command=='templates':
        print(json.dumps(registry.templates(),indent=2));return 0
    if args.command in ('template','run'):
        try:
            if args.command=='template':
                result=build_template(args.prototype,args.output,args.client_template,registry,args.stage)
            else:
                existing=registry.templates()
                template=args.template or (Path(existing[0]['path']) if existing else None)
                if template is None:raise RuntimeError('No prepared template is registered. Restore the shared template; ordinary tests never create accounts.')
                result=execute_environment(template,args.stage,args.account,args.include_mod,args.bootstrap_only,registry,args.apsalus_roundtrip)
            print(json.dumps(result,indent=2));return 0 if result['status'] in ('pass','template_ready') else 1
        except Exception as error:
            print(json.dumps({'status':'fail','reason':str(error),'error_type':type(error).__name__}));return 1
    if args.command=='list':
        print(json.dumps(registry.list(),indent=2));return 0
    results=[]
    for row in registry.list():
        if row['state']=='closed' or row['owner_alive']:continue
        try:
            lease=registry.claim_recovery(row['id']);driver=NativeDriver(registry)
            ManagedEnvironment(registry,lease,driver,Path(row['parent']).parent/'game-test-receipts').close()
            results.append({'id':lease.id,'status':'clean'})
        except Exception as error:results.append({'id':row['id'],'status':'cleanup_required','reason':type(error).__name__})
    print(json.dumps(results,indent=2));return 1 if any(r['status']!='clean' for r in results) else 0
