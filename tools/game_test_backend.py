"""Prepare and run a bounded, loopback-only native GameTest backend.

Private SQL credentials remain under the selected work directory. Reusing a
verified backup source avoids any write, including backup history, to the
source SQL instance.
"""
from pathlib import Path
import argparse, configparser, ctypes, datetime, ipaddress, json, os, re
import secrets, shutil, socket, subprocess, sys, time
import xml.etree.ElementTree as ET
import traceback
from ctypes import wintypes

REPO=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(REPO))
sys.path.insert(0,str(REPO/'tools'))
import game_test
from game_test_windows import WindowsBackgroundProcess
from manager.database import connect as source_connect
from manager.storage import atomic as atomic_bytes
from manager.storage import ini_values, patch_ini

DATABASES=('GameRank','GDClan','GDCommon','GDGameLogView','GDLogDB','GDLongTermGameLogDB',
           'GDStoreBilling','Goldensoftstat','GOnlineGame','JobMonitoring','MonitorDB')
DEFAULT_IMAGE='mcr.microsoft.com/mssql/server@sha256:cf8f0ab2ebe61feec2eed2629061a07d247f791c2a8ddda74a4ac9f06a291718'


class BackendBackgroundProcess(WindowsBackgroundProcess):
    """Private-desktop server owner with an explicit locked-session audit."""

    def __init__(self, *args, **kwargs):
        kwargs['allow_named_desktop_fallback'] = True
        super().__init__(*args, **kwargs)

    def verify_background(self, target_pid=None, *, allowed_input_windows=()):
        selected_pid = self.pid if target_pid is None else int(target_pid)
        receipt = {
            'verified': False,
            'run_id': self.run_id,
            'entry_pid': self.pid,
            'target_pid': selected_pid,
            'desktop_name': self.desktop_name,
            'desktop_path': self.desktop_path,
            'background_window_count': 0,
            'target_background_window_count': 0,
            'private_desktop_verified': False,
            'foreground_pid': 0,
            'foreground_safe': False,
            'input_desktop_observed':
                self._input_desktop_at_launch is not None,
            'input_desktop_observation_error':
                'access_denied' if self._input_desktop_launch_error else None,
            'named_desktop_name': None,
            'named_desktop_target_window_count': None,
            'switch_desktop_called': False,
            'error': None,
        }
        if selected_pid <= 0:
            receipt['error'] = 'target_pid must be positive'
            return receipt
        if self._desktop_handle is None:
            receipt['error'] = 'background desktop is closed'
            return receipt
        try:
            background = self._api.desktop_state(
                self._desktop_handle, selected_pid
            )
            default = self._api.named_desktop_state(
                'Default', selected_pid
            )
            foreground_pid = self._api.foreground_pid()
            private_verified = (
                background.name.casefold() == self.desktop_name.casefold()
            )
            receipt.update(
                background_window_count=background.window_count,
                target_background_window_count=background.target_window_count,
                private_desktop_verified=private_verified,
                named_desktop_name=default.name,
                named_desktop_target_window_count=default.target_window_count,
                foreground_pid=foreground_pid,
                foreground_safe=foreground_pid != selected_pid,
            )
            receipt['verified'] = (
                receipt['private_desktop_verified']
                and receipt['foreground_safe']
                and default.name.casefold() == 'default'
                and default.target_window_count == 0
            )
        except Exception as error:
            receipt['error'] = f'{type(error).__name__}: {error}'
        return receipt

def emit(phase,**values): print(json.dumps({'phase':phase,**values}),flush=True)
def write(path,value):
    # Readers/antivirus can briefly hold Windows JSON files. A private temp
    # name also avoids colliding with a previous controller's interrupted write.
    for attempt in range(20):
        try:
            atomic_bytes(path,json.dumps(value,indent=2).encode('utf-8'))
            return
        except PermissionError:
            if attempt==19:raise
            time.sleep(.05)
def literal(value): return "N'"+value.replace("'","''")+"'"
def docker(*args,env=None,input=None,timeout=180):
    r=subprocess.run(['docker',*args],env=env,input=input if input is not None else '',text=True,capture_output=True,
                     timeout=timeout,creationflags=subprocess.CREATE_NO_WINDOW)
    if r.returncode: raise RuntimeError('Docker operation failed: '+args[0])
    return r.stdout.strip()
def sql(private,statement):
    env=dict(os.environ,SQLCMDPASSWORD=private['sa_password'],GAME_TEST_APP_PASSWORD=private['app_password'])
    return docker('exec','-e','SQLCMDPASSWORD','-e','GAME_TEST_APP_PASSWORD',private['container'],
        '/opt/mssql-tools18/bin/sqlcmd','-S','tcp:127.0.0.1,1433','-U','sa','-C','-I','-b','-l','5','-t','120','-h','-1','-W',
        '-Q',statement,env=env)
def drain(cursor):
    while cursor.nextset(): pass

def owned_labels(private):
    return {
        'sdgo.gametest.owner': private['owner'],
        'sdgo.gametest.token': private['owner_token'],
    }

def container_names():
    return set(filter(None,docker('ps','--all','--format','{{.Names}}').splitlines()))

def volume_names():
    return set(filter(None,docker('volume','ls','--quiet').splitlines()))

def identity(private):
    d=json.loads(docker('inspect',private['container']))[0]
    labels=d['Config'].get('Labels') or {}
    bindings=d['HostConfig'].get('PortBindings') or {}
    expected_binding=[{'HostIp':'127.0.0.1','HostPort':str(private['port'])}]
    mounts=d.get('Mounts') or []
    mount_ok=(len(mounts)==1 and mounts[0].get('Type')=='volume'
        and mounts[0].get('Name')==private['volume']
        and mounts[0].get('Destination')=='/var/opt/mssql')
    if (any(labels.get(key)!=value for key,value in owned_labels(private).items())
            or d['Id']!=private['container_id']
            or d['Name'].lstrip('/')!=private['container']
            or d['HostConfig'].get('NetworkMode')!='bridge'
            or set(bindings)!= {'1433/tcp'}
            or bindings.get('1433/tcp')!=expected_binding
            or not mount_ok):
        raise RuntimeError('Owned container identity mismatch')
    return d

def volume_identity(private):
    value=json.loads(docker('volume','inspect',private['volume']))[0]
    labels=value.get('Labels') or {}
    if (value.get('Name')!=private['volume']
            or any(labels.get(key)!=item for key,item in owned_labels(private).items())):
        raise RuntimeError('Owned SQL volume identity mismatch')
    return value
def sql_connect(private):
    import pyodbc
    return pyodbc.connect('DRIVER={SQL Server};SERVER=127.0.0.1,'+str(private['port'])+
        ';DATABASE=GOnlineGame;UID='+private['app_user']+';PWD='+private['app_password']+';',
        timeout=15,autocommit=True)
def child_environment():
    env=os.environ.copy()
    env.update(CommonProgramFiles=r'C:\Program Files (x86)\Common Files',
        CommonProgramW6432=r'C:\Program Files\Common Files',__COMPAT_LAYER='RunAsInvoker')
    env['CommonProgramFiles(x86)']=r'C:\Program Files (x86)\Common Files'
    return env
def tcp_listener_rows():
    size=wintypes.DWORD(65536);data=ctypes.create_string_buffer(size.value)
    result=ctypes.windll.iphlpapi.GetExtendedTcpTable(data,ctypes.byref(size),False,2,3,0)
    if result: raise RuntimeError('Listener inventory unavailable')
    import struct
    rows=[]
    for i in range(struct.unpack_from('<I',data.raw)[0]):
        state,ip,port,_,_,owner=struct.unpack_from('<IIIIII',data.raw,4+i*24)
        if state==2:
            rows.append((socket.inet_ntoa(struct.pack('<I',ip)),socket.ntohs(port&65535),owner))
    return rows

def listeners(pid):
    return [(ip,port) for ip,port,owner in tcp_listener_rows() if owner==pid]

def assert_listener_endpoints_free(target_ip,ports):
    occupied={(ip,port) for ip,port,_ in tcp_listener_rows()}
    blocked=sorted(port for port in set(ports)
        if (target_ip,port) in occupied or ('0.0.0.0',port) in occupied)
    if blocked:
        raise RuntimeError('Owned backend endpoint is already in use: '+','.join(map(str,blocked)))
def specs():
    return [('StoreServer',9801),('WorldServer',9101),('ChattingServer',9901)]+[
        (f'BattleServer{i}',9300+i) for i in range(1,8)]+[(f'AgentServer{i}',9000+i) for i in range(1,4)]

def loopback_ipv4(value):
    parsed=ipaddress.ip_address(value)
    if parsed.version!=4 or not parsed.is_loopback:
        raise RuntimeError('GameTest backend target must be loopback IPv4')
    return str(parsed)

def free_loopback_port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1',0))
        return int(probe.getsockname()[1])

def paths_overlap(left,right):
    left=left.resolve();right=right.resolve()
    return left==right or left.is_relative_to(right) or right.is_relative_to(left)

def is_reparse_point(path):
    try:
        attributes=getattr(path.lstat(),'st_file_attributes',0)
    except FileNotFoundError:
        return False
    return bool(attributes & 0x400)

def backup_row(row):
    if not isinstance(row,dict) or row.get('name') not in DATABASES:
        raise RuntimeError('Backup source contains an invalid database entry')
    filename=row.get('file')
    if (not isinstance(filename,str) or not re.fullmatch(r'[A-Za-z0-9_.-]+',filename)
            or Path(filename).name!=filename):
        raise RuntimeError('Backup source contains an unsafe filename')
    digest=row.get('sha256')
    if not isinstance(digest,str) or not re.fullmatch(r'[0-9a-fA-F]{64}',digest):
        raise RuntimeError('Backup source contains an invalid hash')
    files=row.get('files')
    if not isinstance(files,list) or not files:
        raise RuntimeError('Backup source file list is missing')
    logical=[];types=[]
    for file in files:
        if (not isinstance(file,dict) or not isinstance(file.get('name'),str)
                or not 1<=len(file['name'])<=128
                or any(character in file['name'] for character in '\x00\r\n')
                or file.get('type') not in ('D','L')):
            raise RuntimeError('Backup source contains an invalid logical file')
        logical.append(file['name'].casefold());types.append(file['type'])
    if len(logical)!=len(set(logical)) or 'D' not in types or 'L' not in types:
        raise RuntimeError('Backup source logical file set is invalid')
    return {
        'name':row['name'],'file':filename,'sha256':digest.lower(),
        'files':[{'name':file['name'],'type':file['type']} for file in files],
    }

def rebase_owned_path(value,source_server,owned_server):
    if not re.match(r'(?i)^[a-z]:[\\/]',value):
        return value
    source_server=source_server.resolve();owned_server=owned_server.resolve()
    source_path=Path(value).resolve()
    if source_path.is_relative_to(source_server):
        return str(owned_server/source_path.relative_to(source_server))
    source_package=source_server.parent
    owned_package=owned_server.parent
    if source_path.is_relative_to(source_package):
        return str(owned_package/source_path.relative_to(source_package))
    raise RuntimeError('Native configuration contains an external absolute path')

def rebase_owned_ini(path,source_server,owned_server):
    raw=path.read_bytes()
    try:
        parsed=ini_values(raw)
    except UnicodeError:
        raise RuntimeError('Copied Mod configuration encoding is unknown') from None
    changes=[]
    for section in parsed.sections():
        for key,value in parsed[section].items():
            updated=rebase_owned_path(value,source_server,owned_server)
            if updated!=value:
                changes.append((section,key,updated))
    if changes:
        try:
            atomic_bytes(path,patch_ini(raw,changes))
        except ValueError:
            raise RuntimeError('Copied Mod absolute path could not be safely rebased') from None
    return len(changes)

def load_backup_source(source,backups):
    source=source.resolve()
    manifest=json.loads((source/'backend.json').read_text(encoding='utf-8-sig'))
    rows=manifest.get('backups',[])
    if not isinstance(rows,list) or len(rows)!=len(DATABASES):
        raise RuntimeError('Backup source does not contain the required database set')
    checked=[backup_row(row) for row in rows]
    if len({row['name'] for row in checked})!=len(DATABASES) or {row['name'] for row in checked}!=set(DATABASES):
        raise RuntimeError('Backup source does not contain the required database set')
    if len({row['file'].casefold() for row in checked})!=len(checked):
        raise RuntimeError('Backup source contains duplicate filenames')
    source_backups=(source/'backups').resolve()
    if not source_backups.is_dir():
        raise RuntimeError('Backup source directory is missing')
    result=[]
    for row in checked:
        source_file=(source_backups/row['file']).resolve()
        if (not source_file.is_relative_to(source_backups) or not source_file.is_file()
                or game_test.sha256(source_file)!=row['sha256']):
            raise RuntimeError('Backup source hash mismatch: '+row['name'])
        destination=backups/row['file']
        shutil.copy2(source_file,destination)
        copied={**row,'bytes':destination.stat().st_size}
        result.append(copied)
        emit('backup_reused',database=row['name'],bytes=copied['bytes'])
    return result

def create_live_backups(source_server,backups):
    source_ini=source_server/'WorldServer/WorldServer.ini'
    db=source_connect(source_ini,autocommit=True)
    result=[];source_identity={}
    try:
        db.timeout=180;cur=db.cursor()
        row=cur.execute(
            "SELECT CAST(SERVERPROPERTY('ProductVersion') AS nvarchar(128)),"
            "CAST(SERVERPROPERTY('ProcessID') AS int),"
            "CAST(SERVERPROPERTY('ServerName') AS nvarchar(128))"
        ).fetchone()
        source_identity={'product_version':str(row[0]),'process_id':int(row[1]),'server_name':str(row[2])}
        for name in DATABASES:
            file=backups/(name+'.bak')
            drain(cur.execute(
                f'BACKUP DATABASE [{name}] TO DISK=? WITH COPY_ONLY,CHECKSUM,'
                'COMPRESSION,BUFFERCOUNT=8,MAXTRANSFERSIZE=65536',str(file)
            ))
            drain(cur.execute('RESTORE VERIFYONLY FROM DISK=? WITH CHECKSUM',str(file)))
            cur.execute('RESTORE FILELISTONLY FROM DISK=?',str(file))
            names=[column[0] for column in cur.description]
            files=[dict(zip(names,item)) for item in cur.fetchall()];drain(cur)
            result.append({'name':name,'file':file.name,'bytes':file.stat().st_size,
                'sha256':game_test.sha256(file),
                'files':[{'name':item['LogicalName'],'type':item['Type']} for item in files]})
            emit('backup_verified',database=name,bytes=file.stat().st_size)
    finally:
        db.close()
    return result,source_identity

def application_sql_probe(private):
    import pyodbc
    checked=[]
    for database in ('GDCommon','GDStoreBilling','GOnlineGame'):
        connection=pyodbc.connect(
            'DRIVER={SQL Server};SERVER=127.0.0.1,'+str(private['port'])+
            ';DATABASE='+database+';UID='+private['app_user']+
            ';PWD='+private['app_password']+';',
            timeout=15,autocommit=True,
        )
        try:
            value=connection.cursor().execute('SELECT DB_NAME()').fetchval()
            if str(value).casefold()!=database.casefold():
                raise RuntimeError('Application SQL probe selected the wrong database')
            checked.append(database)
        finally:
            connection.close()
    return checked

def validate_backend_inputs(stage,source_server):
    required=['server/winmm.dll','server/sdgo/core.dll','server/sdgo/sdgo.ini',
        'server/sdgo/symbols/BattleServer-548A891D-0046C000.ini']
    for module in ('pvp_ai','game_test_server'):
        required.extend('server/sdgo/mods/'+module+'/'+name
            for name in (module+'.dll','mod.ini','manager.json'))
    missing=[name for name in required if not (stage/name).is_file()]
    if missing:
        raise RuntimeError('Stage server runtime is incomplete: '+', '.join(missing))
    for folder,_ in specs():
        exe='BattleServer.exe' if folder.startswith('Battle') else 'AgentServer.exe' if folder.startswith('Agent') else folder+'.exe'
        if not (source_server/folder/exe).is_file() or not (source_server/folder/'sip.txt').is_file():
            raise RuntimeError('Source native service is incomplete: '+folder)

def prepare(work,stage,source_server,target_ip,backup_source=None,allow_live_copy_only=False,image=DEFAULT_IMAGE):
    work=work.resolve();stage=stage.resolve();source_server=source_server.resolve()
    backup_source=backup_source.resolve() if backup_source is not None else None
    target_ip=loopback_ipv4(target_ip)
    if work.exists(): raise RuntimeError('Need a new owned backend work directory')
    for label,path in (('source server',source_server),('stage',stage),('backup source',backup_source)):
        if path is not None and paths_overlap(work,path):
            raise RuntimeError('Owned work directory overlaps '+label)
    if not (source_server/'WorldServer/WorldServer.ini').is_file():
        raise RuntimeError('Source server root is incomplete')
    if not (stage/'server/sdgo/core.dll').is_file():
        raise RuntimeError('Stage server runtime is incomplete')
    if backup_source is None and not allow_live_copy_only:
        raise RuntimeError('Live source backup requires --allow-live-copy-only')
    # Validate all later copy dependencies before creating SQL resources or
    # restoring databases. A core DLL alone is not a complete server stage.
    validate_backend_inputs(stage,source_server)
    raise RuntimeError('Docker backend creation is retired. Reuse the isolated local native test. See docs/game-test.md.')
    docker('info','--format','{{.ServerVersion}}',timeout=30)
    work.mkdir(parents=True);backups=work/'backups';backups.mkdir()
    private={'owner':str(work),'container':'sdgo-gametest-'+secrets.token_hex(6),
        'owner_token':secrets.token_hex(16),
        'sa_password':'Aa1!'+secrets.token_hex(24),'app_user':'gt_app_'+secrets.token_hex(5),
        'app_password':'Aa1!'+secrets.token_hex(12),'port':free_loopback_port()}
    private_path=work/'private.json';write(private_path,private)
    manifest={'schema':1,'scope':'copied native services and isolated restored databases',
        'target_ip':target_ip,'source_server':str(source_server),'stage':str(stage),
        'backup_origin':'reused' if backup_source else 'live_copy_only',
        'backups':[],'services':[],'prepared':False}
    write(work/'backend.json',manifest)
    if backup_source is not None:
        manifest['backups']=load_backup_source(backup_source,backups)
    else:
        live_rows,manifest['source_sql_identity']=create_live_backups(source_server,backups)
        manifest['backups']=[
            {**backup_row(row),'bytes':row['bytes']} for row in live_rows
        ]
    write(work/'backend.json',manifest)
    private['volume']=private['container']+'-data'
    write(private_path,private)
    docker('volume','create',
        '--label','sdgo.gametest.owner='+private['owner'],
        '--label','sdgo.gametest.token='+private['owner_token'],
        private['volume'])
    env=dict(os.environ,MSSQL_SA_PASSWORD=private['sa_password'])
    container_id=docker('create','--name',private['container'],
        '--label','sdgo.gametest.owner='+private['owner'],
        '--label','sdgo.gametest.token='+private['owner_token'],
        '--network','bridge',
        '--publish','127.0.0.1:'+str(private['port'])+':1433',
        '--cpus','1.5','--memory','4g','--env','ACCEPT_EULA=Y','--env','MSSQL_PID=Developer',
        '--env','MSSQL_SA_PASSWORD','--env','MSSQL_COLLATION=Chinese_PRC_CI_AS',
        '--env','MSSQL_MEMORY_LIMIT_MB=2048','--mount',
        'type=volume,source='+private['volume']+',target=/var/opt/mssql',image,env=env)
    private['container_id']=container_id;write(private_path,private)
    started=False
    try:
        docker('start',private['container']);started=True;deadline=time.monotonic()+180
        while True:
            try:
                if sql(private,'SET NOCOUNT ON;SELECT 1;').strip().endswith('1'): break
            except RuntimeError: pass
            if time.monotonic()>deadline: raise RuntimeError('Owned SQL startup timed out')
            time.sleep(1)
        emit('owned_sql_ready',container=private['container'])
        restore_copy(work,stage,source_server,private,manifest)
    finally:
        if started:
            identity(private)
            docker('stop','--time','15',private['container'],timeout=30)

def restore_copy(work,stage,source_server,private,manifest):
    backups=work/'backups'
    owned_server=work/'Server'
    sql(private,"IF SUSER_ID("+literal(private['app_user'])+") IS NULL CREATE LOGIN ["+private['app_user']+"] WITH PASSWORD='$(GAME_TEST_APP_PASSWORD)',CHECK_POLICY=OFF;")
    sql(private,'GRANT VIEW SERVER STATE TO ['+private['app_user']+'];GRANT VIEW SERVER PERFORMANCE STATE TO ['+private['app_user']+'];')
    docker('exec',private['container'],'mkdir','-p','/var/opt/mssql/backup/gametest')
    for entry in manifest['backups']:
        name=entry['name'];source=backups/entry['file']
        if game_test.sha256(source)!=entry['sha256']: raise RuntimeError('Backup changed')
        destination='/var/opt/mssql/backup/gametest/'+entry['file']
        docker('cp',str(source),private['container']+':'+destination)
        moves=[]
        for i,f in enumerate(entry['files']):
            target=f'/var/opt/mssql/data/gametest_{name}_{i}.'+('ldf' if f['type']=='L' else 'mdf')
            moves.append('MOVE '+literal(f['name'])+' TO '+literal(target))
        sql(private,f'IF DB_ID('+literal(name)+f') IS NULL RESTORE DATABASE [{name}] FROM DISK='+literal(destination)+' WITH CHECKSUM,RECOVERY,'+','.join(moves)+';\n'+
            f'ALTER AUTHORIZATION ON DATABASE::[{name}] TO [{private["app_user"]}];\n'+
            f'ALTER DATABASE [{name}] SET TRUSTWORTHY OFF;ALTER DATABASE [{name}] SET DB_CHAINING OFF;')
        emit('database_restored',database=name)
    target_ip=manifest['target_ip']
    sql(private,'USE GOnlineGame;UPDATE dbo.tbl_ChannelInfo SET AddrIP='+literal(target_ip)+';')
    manifest['application_sql_probe']=application_sql_probe(private)
    manifest['isolation']={'container_id':private['container_id'],'network':'bridge',
        'published_endpoint':'127.0.0.1:'+str(private['port']),'host_mounts':False,
        'original_db_writes':'none' if manifest['backup_origin']=='reused' else 'COPY_ONLY backup history only',
        'gameplay_rows_seeded':False}
    for folder,port in specs():
        src=source_server/folder;dest=owned_server/folder
        exe='BattleServer.exe' if folder.startswith('Battle') else 'AgentServer.exe' if folder.startswith('Agent') else folder+'.exe'
        if not (src/exe).is_file() or not (src/'sip.txt').is_file():
            raise RuntimeError('Source native service is incomplete: '+folder)
        dest.mkdir(parents=True,exist_ok=True)
        shutil.copy2(src/exe,dest/exe)
        for f in src.iterdir():
            if f.is_file() and f.suffix.lower() in ('.dll','.cfg') and f.name!='winmm.dll': shutil.copy2(f,dest/f.name)
        sip=(src/'sip.txt').read_text(encoding='utf-8-sig')
        addresses=re.findall(r'\b(?:\d+\.){3}\d+\b',sip)
        if any(not ipaddress.ip_address(address).is_loopback for address in addresses):
            raise RuntimeError('Source SIP has unreviewed external routes')
        rewritten=re.sub(r'\b(?:\d+\.){3}\d+\b',target_ip,sip)
        (dest/'sip.txt').write_text(rewritten,encoding='ascii')
        for f in src.glob('*.ini'):
            cfg=configparser.ConfigParser(interpolation=None,strict=False);cfg.optionxform=str
            raw=f.read_bytes()
            try: decoded=raw.decode('utf-16' if raw.startswith((b'\xff\xfe',b'\xfe\xff')) else 'utf-8-sig')
            except UnicodeError: decoded=raw.decode('cp936')
            cfg.read_string(decoded)
            for section in cfg.sections():
                for k,v in list(cfg[section].items()):
                    if k.casefold()=='dbservername': cfg[section][k]='127.0.0.1,'+str(private['port'])
                    elif section.casefold().startswith('db ') and k.casefold() in ('id','pwd'):
                        cfg[section][k]=private['app_user'] if k.casefold()=='id' else private['app_password']
                    else: cfg[section][k]=rebase_owned_path(v,source_server,owned_server)
            with (dest/f.name).open('w',encoding='cp936') as stream: cfg.write(stream,space_around_delimiters=False)
        if (src/'sdgo').is_dir():
            shutil.copytree(src/'sdgo',dest/'sdgo',ignore=game_test.copy_ignore)
            for copied_ini in (dest/'sdgo').rglob('*.ini'):
                manifest.setdefault('rebased_mod_paths',0)
                manifest['rebased_mod_paths']+=rebase_owned_ini(
                    copied_ini,source_server,owned_server
                )
        (dest/'sdgo/symbols').mkdir(parents=True,exist_ok=True)
        shutil.copy2(stage/'server/winmm.dll',dest/'winmm.dll')
        shutil.copy2(stage/'server/sdgo/core.dll',dest/'sdgo/core.dll')
        for f in (stage/'server/sdgo/symbols').glob('*.ini'): shutil.copy2(f,dest/'sdgo/symbols'/f.name)
        if not (dest/'sdgo/sdgo.ini').exists(): shutil.copy2(stage/'server/sdgo/sdgo.ini',dest/'sdgo/sdgo.ini')
        if folder.startswith('Battle'):
            for mod in ('pvp_ai','game_test_server'):
                target=dest/'sdgo/mods'/mod
                shutil.copytree(stage/'server/sdgo/mods'/mod,target,dirs_exist_ok=True)
                config=game_test.read_ini(target/'mod.ini');config['mod']['enabled']='1';game_test.write_ini(target/'mod.ini',config)
        manifest['services'].append({'folder':folder,'port':port,'exe':str(dest/exe),'sha256':game_test.sha256(dest/exe)})
    manifest['prepared']=True;write(work/'backend.json',manifest);emit('backend_prepared',directory=str(work))


def native_error_category(folder):
    logs=sorted((folder/'LogData').glob('*.txt'),key=lambda path:path.stat().st_mtime,reverse=True)
    if not logs:return None
    text=logs[0].read_bytes()[-131072:].decode('cp936',errors='replace')
    if 'g_oleGameDB.Initialize() fail' in text:return 'ole_game_db_initialize_failed'
    if 'OLE DB ERROR' in text:return 'ole_db_error'
    return None

def stop_owned_process(process):
    if process.poll() is None:
        try:
            process.kill()
        except TimeoutError:
            # TerminateProcess is asynchronous. Native COM/network teardown can
            # outlast the launcher's short initial wait; retain ownership while
            # waiting for the original process handle to signal.
            process.wait(timeout=30)
        except OSError:
            if process.poll() is None:
                raise
        if process.poll() is None:
            process.wait(timeout=10)
    if process.poll() is None:
        raise RuntimeError('Owned process did not stop')
    process.close()

def observer_host_id(fps_suite,value):
    if fps_suite:
        if value is not None:raise RuntimeError('FPS suites do not use an account or host pilot ID')
        return 0
    if type(value) is not int or not 1<=value<=0xffffffff:
        raise RuntimeError('Authenticated suites require --host-pilot-id from the actual reused test account')
    return value

def validate_local_native_sql(private, work):
    """Verify the selected copied native instance without touching production."""
    root=work.parent.resolve()
    if work.name!='server' or not (root/'TEST-ONLY.txt').is_file():
        raise RuntimeError('Native backend must belong to a marked local test/server copy')
    instance=json.loads((root/'database/native-sql.json').read_text())
    if instance['root']!=str(root) or int(instance['port'])!=int(private['port']):
        raise RuntimeError('Native SQL binding belongs to another test copy')
    db=sql_connect(private)
    try:
        cursor=db.cursor()
        pid=int(cursor.execute("SELECT CAST(SERVERPROPERTY('ProcessID') AS int)").fetchone()[0])
        if pid!=instance.get('pid'):
            raise RuntimeError('Native test SQL process identity changed')
        for row in cursor.execute('SELECT physical_name FROM sys.master_files').fetchall():
            if not Path(row[0]).resolve().is_relative_to(root):
                raise RuntimeError('Native SQL still references data outside its test copy')
    finally:db.close()


def serve(work,client_run,lifetime,stage,startup_xml,startup_timeout,host_pilot_id=None,mission_respawn_probe=None):
    work=work.resolve();stage=stage.resolve();startup_xml=startup_xml.resolve()
    private=json.loads((work/'private.json').read_text())
    manifest=json.loads((work/'backend.json').read_text())
    if not manifest['prepared'] or private['owner']!=str(work): raise RuntimeError('Prepared backend identity invalid')
    client=game_test.validate_metadata(client_run.resolve())
    target_ip=manifest['target_ip']
    if client['plan']['target_host']!=target_ip: raise RuntimeError('Client must target this isolated backend')
    if not (stage/'server/sdgo/core.dll').is_file(): raise RuntimeError('Stage server runtime is incomplete')
    if not startup_xml.is_file(): raise RuntimeError('Server startup XML is missing')
    suite=client.get('suite') or client['plan'].get('suite','')
    fps_suite=suite=='native-fps-limit'
    host_pilot_id=observer_host_id(fps_suite,host_pilot_id)
    native_local=private.get('kind')=='native-local-test'
    respawn_policy=client['plan'].get('mission_respawns')
    if respawn_policy and (not native_local or mission_respawn_probe is None or not mission_respawn_probe.is_file()):
        raise RuntimeError('Mission respawn acceptance requires the explicit isolated native combat probe DLL')
    if mission_respawn_probe is not None and not respawn_policy:
        raise RuntimeError('Native combat probe requires a sealed mission respawn plan')
    if native_local:validate_local_native_sql(private,work)
    else:raise RuntimeError('Docker service startup is retired; use a copied native-local-test backend. Legacy cleanup remains available.')
    lock,lock_token=game_test.acquire_lock(work)
    owned_processes=[];ready_services=[];started=False
    state={'phase':'starting','pid':os.getpid(),'services':[],'client_run':str(client_run.resolve())}
    def publish(phase):
        state['phase']=phase;state['heartbeat']=time.time();write(work/'status.json',state)
    try:
        (work/'stop.request').unlink(missing_ok=True)
        publish('starting_sql')
        if not native_local:
            docker('start',private['container']);started=True
        deadline=time.monotonic()+180
        while True:
            try:
                checked=application_sql_probe(private)
                if checked==['GDCommon','GDStoreBilling','GOnlineGame']:break
            except Exception:
                if time.monotonic()>deadline: raise RuntimeError('Owned application SQL databases not ready') from None
                time.sleep(.5)
        stage_server=stage/'server'
        payload_receipt=[]
        for owned_spec in manifest['services']:
            owned_folder=Path(owned_spec['exe']).parent
            if not owned_folder.is_relative_to(work/'Server'):raise RuntimeError('Owned payload path escaped')
            for core_file in ('winmm.dll','sdgo/core.dll'):
                source=stage_server/core_file
                if source.is_file():
                    destination=owned_folder/core_file
                    destination.parent.mkdir(parents=True,exist_ok=True)
                    shutil.copy2(source,destination)
            if not (owned_folder/'sdgo/sdgo.ini').is_file():
                shutil.copy2(stage_server/'sdgo/sdgo.ini',owned_folder/'sdgo/sdgo.ini')
            manifests=[]
            for source_manifest in (stage_server/'sdgo/mods').glob('*/mod.ini'):
                target_folder=owned_folder/'sdgo/mods'/source_manifest.parent.name
                target_manifest=target_folder/'mod.ini'
                if not target_manifest.is_file():continue
                updated=game_test.read_ini(source_manifest);previous=game_test.read_ini(target_manifest)
                if previous.has_option('mod','enabled'):updated['mod']['enabled']=previous['mod']['enabled']
                if source_manifest.parent.name in ('game_test_server','pvp_ai') and owned_spec['folder'].startswith('Battle'):
                    updated['mod']['enabled']='0' if fps_suite else '1'
                if previous.has_section('settings'):
                    if not updated.has_section('settings'):updated.add_section('settings')
                    for key,value in previous.items('settings',raw=True):updated['settings'][key]=value
                game_test.write_ini(target_manifest,updated)
                for file in source_manifest.parent.iterdir():
                    if file.is_file() and (file.suffix.lower() in ('.dll','.lua') or file.name=='symbols.fragment.ini'):
                        shutil.copy2(file,target_folder/file.name)
                manifests.append(target_manifest)
            symbols=owned_folder/'sdgo/symbols'
            for symbol in (stage_server/'sdgo/symbols').glob('*.ini'):
                if (symbols/symbol.name).is_file():
                    applicable=[]
                    for module_manifest in manifests:
                        module_hosts=game_test.read_ini(module_manifest).get('mod','hosts',fallback='').split(',')
                        if symbol.stem in module_hosts or '*' in module_hosts:applicable.append(module_manifest)
                    game_test.merge_required_symbols(symbol,symbols/symbol.name,applicable,include_all=True)
            payload_receipt.append({'folder':owned_spec['folder'],'core':game_test.sha256(owned_folder/'sdgo/core.dll'),
                'mods':{m.parent.name:game_test.sha256(m.parent/(m.parent.name+'.dll')) for m in manifests if (m.parent/(m.parent.name+'.dll')).is_file()}})
        write(work/'runtime-overlay.json',{'source':game_test.git_source_ref(REPO),'hosts':payload_receipt})
        from manager.storage import ini_values
        for copied_config in (work/'Server').glob('*/sdgo/mods/*/*.ini'):
            if copied_config.name.endswith('.example.ini'):continue
            try:parsed=ini_values(copied_config.read_bytes())
            except UnicodeError:raise RuntimeError('Copied Mod configuration encoding is unknown') from None
            for section in parsed.sections():
                for key,value in parsed[section].items():
                    if re.match(r'(?i)^[a-z]:[\\/]',value) and not Path(value).resolve().is_relative_to(work):
                        raise RuntimeError('Copied Mod still points outside its owned backend: '+copied_config.parent.name+'/'+key)

        # A copied folder is not an enabled native channel. Keep the original
        # Unit Support Info count check and launch only configured hosts.
        configured={Path(row.attrib['Path'].replace('\\','/')).parent.name
            for row in ET.parse(startup_xml).getroot().findall('Process')
            if row.attrib.get('Run','').lower()=='true'}
        db=sql_connect(private)
        try:
            channels=[{'port':int(r[0]),'server_id':int(r[1])} for r in db.cursor().execute(
                'SELECT AddrPT,ServerID FROM dbo.tbl_ChannelInfo ORDER BY AddrPT').fetchall()]
        finally:db.close()
        enabled_ports={row['port'] for row in channels}
        selected=[spec for spec in manifest['services'] if spec['folder'] in configured and
            (not spec['folder'].startswith('Battle') or spec['port'] in enabled_ports)]
        if native_local:
            # World actively connects to its peers while initializing. Starting
            # it last lets those native listeners accept its first table stream
            # instead of racing World initialization and losing that stream.
            selected.sort(key=lambda spec: spec['folder']=='WorldServer')
        skipped=[spec['folder'] for spec in manifest['services'] if spec not in selected]
        login_port=int(client['plan']['login_port'])
        assert_listener_endpoints_free(
            target_ip,[spec['port'] for spec in selected]+[login_port]
        )
        write(work/'launch-selection.json',{'native_xml':str(startup_xml),'database_channels':channels,
            'selected':[{'folder':x['folder'],'port':x['port']} for x in selected],'skipped':skipped})
        emit('launch_selection',selected=len(selected),skipped=skipped)
        for spec in selected:
            exe=Path(spec['exe']);folder=exe.parent
            if not exe.is_relative_to(work/'Server') or game_test.sha256(exe)!=spec['sha256']: raise RuntimeError('Copied service identity changed')
            if native_local and spec['folder']=='WorldServer':
                # Creating peer processes is not enough: World can connect and
                # begin its initial stream before their native listeners exist.
                peers_deadline=time.monotonic()+startup_timeout
                for peer,peer_row in ready_services:
                    while (target_ip,peer_row['port']) not in listeners(peer.pid):
                        if (work/'stop.request').exists():raise InterruptedError('Owned backend stop requested')
                        if peer.poll() is not None or time.monotonic()>peers_deadline:
                            raise RuntimeError('Native peer listener not ready before World: '+peer_row['folder'])
                        time.sleep(.2)
                emit('native_peer_listeners_ready',count=len(ready_services))
            env=child_environment()
            if spec['folder'].startswith('Battle') and not fps_suite:
                job=folder/'sdgo/gametest'
                if job.exists():
                    archive=job.with_name('gametest.previous-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ'))
                    if not job.resolve().is_relative_to(work/'Server') or not archive.resolve().is_relative_to(work/'Server'):
                        raise RuntimeError('Unsafe owned receipt archive')
                    job.rename(archive)
                job.mkdir()
                cfg=configparser.ConfigParser(interpolation=None)
                cfg['run']={'id':client['run_id'],'role':'server','suite':'pvp-ai'}
                cfg['observer']={'nonce':client['plan']['title_nonce'],'host_pilot_id':str(host_pilot_id),
                    'mode':'0','minimum_ai':str(client['plan'].get('expected_ai',client['plan'].get('mission_ai_count',3))),'require_end':'1'}
                if respawn_policy:
                    cfg['mission_respawn_probe']={
                        'output':str(Path(client['client']['root'])/'sdgo/gametest'/('mission-respawn-server-'+client['run_id']+'.ini')),
                        'infinite':'1' if respawn_policy=='infinite' else '0'}
                    probe=folder/'sdgo/mods/mission_respawn_server_probe'
                    probe.mkdir(parents=True,exist_ok=True)
                    shutil.copy2(mission_respawn_probe,probe/'mission_respawn_server_probe.dll')
                    (probe/'mod.ini').write_text('[mod]\ndistribution=public\nid=mission_respawn_server_probe\nname=Native mission respawn combat fixture\ncategory=development\nfeature=game-test\nversion=1.0.0\napi=1\ndll=mission_respawn_server_probe.dll\norder=900\nhosts=BattleServer-548A891D-0046C000\n')
                    symbols=folder/'sdgo/symbols/BattleServer-548A891D-0046C000.ini'
                    text=symbols.read_text(encoding='utf-8-sig')
                    if '[gametest.mission_respawn_tick]' not in text:
                        symbols.write_text(text+'\n[gametest.mission_respawn_tick]\nkind=code\nrva=0x000884D9\nexpect=C7 45 F4 00 00 00 00\n',encoding='utf-8')
                game_test.write_ini(job/'run.ini',cfg)
                env.update(SDGO_GAME_TEST_RUN=client['run_id'],SDGO_GAME_TEST_ROLE='server')
                settings=game_test.read_ini(folder/'sdgo/sdgo.ini')
                for section in ('categories','features','mods'):
                    if not settings.has_section(section): settings.add_section(section)
                settings['categories']['development']='1';settings['features']['game-test']='1'
                settings['mods']['game_test_server']='1';settings['mods']['pvp_ai']='1'
                if respawn_policy:
                    settings['mods']['mission_respawn_server_probe']='1'
                    settings['mods']['coop_ai']='1'
                    settings['features']['coop-wingmen']='1'
                else:
                    settings['mods']['mission_respawn_server_probe']='0'
                game_test.write_ini(folder/'sdgo/sdgo.ini',settings)
            for attempt in range(1,3):
                console=work/(spec['folder']+'.console.log')
                log_offsets={str(p):p.stat().st_size for p in (folder/'LogData').glob('*.txt')}
                with console.open('ab') as output:
                    process=BackendBackgroundProcess([str(exe)],cwd=folder,env=env,stdout=output,stderr=subprocess.STDOUT,run_id=client['run_id'])
                owned_processes.append(process)
                state_row={'folder':spec['folder'],'pid':process.pid,'exe':str(exe),
                    'desktop':process.desktop_path,'attempt':attempt}
                state['services'].append(state_row);publish('starting_'+spec['folder'])
                if native_local:
                    # Original hosts connect to each other asynchronously.
                    # Start the copied set first, then verify every listener;
                    # waiting for each host before starting its peers causes
                    # minutes of avoidable startup delay under concurrent load.
                    ready_services.append((process,state_row))
                    state_row['port']=spec['port']
                    state_row['startup_log_offsets']=log_offsets
                    break
                deadline=time.monotonic()+startup_timeout
                while (target_ip,spec['port']) not in listeners(process.pid):
                    if (work/'stop.request').exists(): raise InterruptedError('Owned backend stop requested')
                    if process.poll() is not None or time.monotonic()>deadline:break
                    time.sleep(.2)
                if (target_ip,spec['port']) in listeners(process.pid):
                    proof=process.verify_background()
                    if not (proof['verified'] and proof['private_desktop_verified'] and proof['foreground_safe']):
                        raise RuntimeError('Owned server background desktop policy failed: '+spec['folder'])
                    state_row['quiet_proof']=proof;ready_services.append((process,state_row))
                    emit('service_ready',folder=spec['folder'],pid=process.pid,port=spec['port'],attempt=attempt)
                    break
                state_row['failure_category']=native_error_category(folder)
                stop_owned_process(process);state['services'].pop()
                if attempt==2:
                    raise RuntimeError('Owned native service not ready: '+spec['folder'])
                emit('service_retry',folder=spec['folder'],category=state_row['failure_category'])
                time.sleep(3)
        if native_local:
            deadline=time.monotonic()+startup_timeout
            for process,row in ready_services:
                while (target_ip,row['port']) not in listeners(process.pid):
                    if (work/'stop.request').exists():raise InterruptedError('Owned backend stop requested')
                    if process.poll() is not None or time.monotonic()>deadline:
                        raise RuntimeError('Copied native service not ready: '+row['folder'])
                    time.sleep(.2)
                proof=process.verify_background()
                if not (proof['verified'] and proof['private_desktop_verified'] and proof['foreground_safe']):
                    raise RuntimeError('Copied native service background policy failed: '+row['folder'])
                row['quiet_proof']=proof
                emit('service_ready',folder=row['folder'],pid=process.pid,port=row['port'],attempt=1)
            # Native Battle opens TCP before its asynchronous table loader
            # enables Time Procedure. A socket alone can yield 0FBC on Start.
            # Bind the readiness marker to bytes written by this launch only.
            tables_deadline=time.monotonic()+600
            for process,row in ready_services:
                if not row['folder'].startswith('Battle'):continue
                folder=Path(row['exe']).parent
                offsets=row['startup_log_offsets']
                while True:
                    loaded=False
                    for log in (folder/'LogData').glob('*.txt'):
                        with log.open('rb') as stream:
                            stream.seek(offsets.get(str(log),0))
                            loaded=loaded or b'Table Download OK.. Time Procedure Ready..' in stream.read()
                    if loaded:break
                    if (work/'stop.request').exists():raise InterruptedError('Owned backend stop requested')
                    if process.poll() is not None or time.monotonic()>tables_deadline:
                        raise RuntimeError('Native Battle table loading did not complete: '+row['folder'])
                    time.sleep(.5)
                row['native_tables_ready']=True
                publish('tables_ready_'+row['folder'])
                emit('native_tables_ready',folder=row['folder'],pid=process.pid)
        deadline=time.monotonic()+30
        while not any((target_ip,login_port) in listeners(process.pid) for process,_ in ready_services):
            if any(process.poll() is not None for process,_ in ready_services) or time.monotonic()>deadline:
                raise RuntimeError('Owned native client login listener was not ready')
            time.sleep(.2)
        if respawn_policy:
            current_client=game_test.validate_metadata(client_run.resolve())
            if (current_client['run_id']!=client['run_id'] or
                    current_client['plan']['room_title']!=client['plan']['room_title']):
                raise RuntimeError('Mission client capsule changed during backend startup; finish preparation before starting its backend')
        publish('ready');emit('backend_ready',ip=target_ip,lifetime=lifetime,client_login_port=login_port)
        until=time.monotonic()+lifetime
        while time.monotonic()<until and not (work/'stop.request').exists():
            failed=[{'folder':row['folder'],'pid':process.pid,'exit_code':process.poll()}
                for process,row in ready_services if process.poll() is not None]
            if failed:
                state['early_exit']=failed;publish('failure_detected')
                write(work/'early-exit.json',{'client_run':str(client_run.resolve()),'failed':failed})
                raise RuntimeError('Owned native service stopped: '+','.join(row['folder']+'='+str(row['exit_code']) for row in failed))
            publish('ready');time.sleep(1)
    finally:
        cleanup_errors=[];remaining=[];sql_failed=False
        for process in reversed(owned_processes):
            try:
                stop_owned_process(process)
            except Exception as error:cleanup_errors.append({'pid':process.pid,'step':'stop','error':type(error).__name__})
            try:
                if process.poll() is None:remaining.append(process.pid)
            except Exception:remaining.append(process.pid)
        if started:
            try:identity(private);docker('stop','--time','15',private['container'],timeout=30)
            except Exception as error:
                sql_failed=True;cleanup_errors.append({'step':'owned_sql_stop','error':type(error).__name__})
        state['cleanup_errors']=cleanup_errors;state['remaining_owned_pids']=remaining
        complete=not remaining and not sql_failed and not cleanup_errors
        try:
            publish('stopped' if complete else 'cleanup_incomplete')
        finally:
            if complete:game_test.release_owned_lock(lock,lock_token)
        emit('backend_stopped',remaining_owned_pids=remaining,cleanup_errors=cleanup_errors)

def stop(work,reason):
    work=work.resolve();status_path=work/'status.json'
    private=json.loads((work/'private.json').read_text(encoding='utf-8-sig'))
    if private.get('owner')!=str(work):
        raise RuntimeError('Owned backend identity is invalid')
    if not status_path.is_file():raise RuntimeError('Backend status is missing')
    state=json.loads(status_path.read_text(encoding='utf-8-sig'))
    if state.get('phase') not in ('starting','starting_sql','ready') and not str(state.get('phase','')).startswith('starting_'):
        raise RuntimeError('Backend is not active')
    write(work/'stop.request',{'requested_at':datetime.datetime.now(datetime.timezone.utc).isoformat(),'reason':reason})
    emit('stop_requested',pid=state.get('pid'))

def _pid_exists(pid):
    if type(pid) is not int or pid<=0:return False
    if os.name=='nt':
        # Signal zero is CTRL_C_EVENT on Windows, not a POSIX existence probe.
        # A synchronizable handle can only observe exit; it cannot signal or kill.
        kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        kernel.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD]
        kernel.OpenProcess.restype=wintypes.HANDLE
        kernel.WaitForSingleObject.argtypes=[wintypes.HANDLE,wintypes.DWORD]
        kernel.WaitForSingleObject.restype=wintypes.DWORD
        kernel.CloseHandle.argtypes=[wintypes.HANDLE]
        kernel.CloseHandle.restype=wintypes.BOOL
        process=kernel.OpenProcess(0x00100000,False,pid) # SYNCHRONIZE only
        if not process:
            # Access denied or an unknown query failure cannot justify cleanup.
            return ctypes.get_last_error() not in (87,1168)
        try:
            return kernel.WaitForSingleObject(process,0)!=0 # WAIT_OBJECT_0 proves exit
        finally:kernel.CloseHandle(process)
    try:
        os.kill(pid,0)
    except ProcessLookupError:return False
    except PermissionError:return True
    return True

def cleanup(work,remove_runtime):
    work=work.resolve()
    private_path=work/'private.json'
    if not private_path.is_file():
        raise RuntimeError('Owned backend private marker is missing')
    private=json.loads(private_path.read_text(encoding='utf-8-sig'))
    if (private.get('owner')!=str(work)
            or not re.fullmatch(r'[0-9a-f]{32}',str(private.get('owner_token','')))
            or not re.fullmatch(r'sdgo-gametest-[0-9a-f]{12}',str(private.get('container','')))):
        raise RuntimeError('Owned backend ownership is invalid')
    state_path=work/'status.json'
    if state_path.is_file():
        state=json.loads(state_path.read_text(encoding='utf-8-sig'))
        pid=int(state.get('pid') or 0)
        if pid and state.get('phase')!='stopped' and _pid_exists(pid):
            raise RuntimeError('Backend controller is still active')
        service_pids={int(row.get('pid') or 0) for row in state.get('services',[])}
        service_pids.update(int(pid or 0) for pid in state.get('remaining_owned_pids',[]))
        if any(pid>0 and _pid_exists(pid) for pid in service_pids):
            raise RuntimeError('Owned native service is still active')
    runtime_targets=[]
    if remove_runtime:
        for name in ('Server','backups','.local_runtime'):
            raw_target=work/name
            if is_reparse_point(raw_target):
                raise RuntimeError('Unsafe runtime cleanup reparse point')
            target=raw_target.resolve()
            if target.parent!=work:raise RuntimeError('Unsafe runtime cleanup target')
            runtime_targets.append((name,target))
    console_logs=[work/(folder+'.console.log') for folder,_ in specs()]
    if any(is_reparse_point(path) or path.resolve().parent!=work for path in console_logs):
        raise RuntimeError('Unsafe owned console log cleanup target')
    container_removed=False
    if private.get('container') in container_names():
        if not private.get('container_id') or not private.get('volume'):
            raise RuntimeError('Owned container marker is incomplete')
        details=identity(private)
        if details['State']['Running']:raise RuntimeError('Owned SQL container is still running')
        docker('rm',private['container_id']);container_removed=True
    volume_removed=False
    if private.get('volume') in volume_names():
        volume_identity(private)
        docker('volume','rm',private['volume']);volume_removed=True
    removed=[]
    for name,target in runtime_targets:
        if target.exists():shutil.rmtree(target);removed.append(name)
    removed_logs=[]
    for path in console_logs:
        if path.is_file():
            path.unlink();removed_logs.append(path.name)
    private_path.unlink()
    write(work/'cleanup.json',{'container_removed':container_removed,'volume_removed':volume_removed,
        'private_config_removed':True,'runtime_removed':removed,'console_logs_removed':removed_logs})
    emit('cleanup_complete',runtime_removed=removed,console_logs_removed=len(removed_logs))

def public_status(work):
    work=work.resolve();manifest={}
    if (work/'backend.json').is_file():
        manifest=json.loads((work/'backend.json').read_text(encoding='utf-8-sig'))
    state={}
    if (work/'status.json').is_file():
        state=json.loads((work/'status.json').read_text(encoding='utf-8-sig'))
    result={'work':str(work),'prepared':bool(manifest.get('prepared')),
        'target_ip':manifest.get('target_ip'),'backup_origin':manifest.get('backup_origin'),
        'phase':state.get('phase','stopped'),'controller_pid':state.get('pid'),
        'service_count':len(state.get('services',[]))}
    print(json.dumps(result,indent=2))

def build_parser():
    parser=argparse.ArgumentParser(description=__doc__)
    commands=parser.add_subparsers(dest='action',required=True)
    prepare_cmd=commands.add_parser('prepare')
    prepare_cmd.add_argument('--work',type=Path,required=True)
    prepare_cmd.add_argument('--source-server',type=Path,required=True)
    prepare_cmd.add_argument('--stage',type=Path,required=True)
    prepare_cmd.add_argument('--target-ip',required=True)
    prepare_cmd.add_argument('--backup-source',type=Path)
    prepare_cmd.add_argument('--allow-live-copy-only',action='store_true')
    prepare_cmd.add_argument('--image',default=DEFAULT_IMAGE)
    serve_cmd=commands.add_parser('serve')
    serve_cmd.add_argument('--work',type=Path,required=True)
    serve_cmd.add_argument('--client-run',type=Path,required=True)
    serve_cmd.add_argument('--stage',type=Path,required=True)
    serve_cmd.add_argument('--startup-xml',type=Path,required=True)
    serve_cmd.add_argument('--lifetime',type=int,default=1800)
    serve_cmd.add_argument('--startup-timeout',type=int,default=240)
    serve_cmd.add_argument('--host-pilot-id',type=int,
        help='actual reused account pilot ID for authenticated suites; omit for FPS')
    serve_cmd.add_argument('--mission-respawn-probe',type=Path,help='development-only native combat fixture for a sealed mission respawn plan')
    stop_cmd=commands.add_parser('stop')
    stop_cmd.add_argument('--work',type=Path,required=True)
    stop_cmd.add_argument('--reason',default='bounded GameTest backend stop')
    cleanup_cmd=commands.add_parser('cleanup')
    cleanup_cmd.add_argument('--work',type=Path,required=True)
    cleanup_cmd.add_argument('--remove-runtime',action='store_true')
    status_cmd=commands.add_parser('status')
    status_cmd.add_argument('--work',type=Path,required=True)
    return parser

def main(argv=None):
    args=build_parser().parse_args(argv)
    try:
        if args.action=='prepare':
            prepare(args.work,args.stage,args.source_server,args.target_ip,args.backup_source,
                args.allow_live_copy_only,args.image)
        elif args.action=='serve':
            if not 60<=args.lifetime<=7200:raise RuntimeError('Lifetime must be between 60 and 7200 seconds')
            if not 30<=args.startup_timeout<=600:raise RuntimeError('Startup timeout must be between 30 and 600 seconds')
            serve(args.work,args.client_run,args.lifetime,args.stage,args.startup_xml,args.startup_timeout,args.host_pilot_id,args.mission_respawn_probe)
        elif args.action=='stop':
            stop(args.work,args.reason)
        elif args.action=='cleanup':
            cleanup(args.work,args.remove_runtime)
        else:
            public_status(args.work)
        return 0
    except Exception as error:
        emit('failed',error_type=type(error).__name__,reason=str(error) if isinstance(error,RuntimeError) else 'local execution error',path=getattr(error,'filename',None),errno=getattr(error,'errno',None),winerror=getattr(error,'winerror',None),stack=[{'function':f.name,'line':f.lineno,'file':Path(f.filename).name} for f in traceback.extract_tb(error.__traceback__)])
        return 1

if __name__=='__main__':
    raise SystemExit(main())
