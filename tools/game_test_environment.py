"""Owned native GameTest environments: cross-process leases and mandatory cleanup."""
from __future__ import annotations

import argparse
import contextlib
import ctypes
import dataclasses
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import sqlite3
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = 1
ACTIVE = ('allocated', 'starting', 'running', 'cleaning', 'cleanup_required')


class _Connection(sqlite3.Connection):
    def __exit__(self, *args):
        try:
            return super().__exit__(*args)
        finally:
            self.close()


def process_identity(pid: int) -> str | None:
    """PID plus creation time prevents a recycled PID from proving ownership."""
    if os.name != 'nt':
        try:
            return Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()[19]
        except (OSError, IndexError):
            return None
    from ctypes import wintypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.GetProcessTimes.argtypes = (wintypes.HANDLE,) + (ctypes.POINTER(wintypes.FILETIME),) * 4
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    handle = kernel.OpenProcess(0x1000, False, pid)
    if not handle:
        return None
    try:
        created, exited, system, user = (wintypes.FILETIME() for _ in range(4))
        if not kernel.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited), ctypes.byref(system), ctypes.byref(user)):
            return None
        if exited.dwLowDateTime or exited.dwHighDateTime:
            return None
        return str((created.dwHighDateTime << 32) | created.dwLowDateTime)
    finally:
        kernel.CloseHandle(handle)


def atomic_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.' + secrets.token_hex(6) + '.tmp')
    try:
        with temporary.open('w', encoding='utf-8', newline='\n') as stream:
            json.dump(value, stream, indent=2)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def no_reparse_tree(root: Path) -> None:
    for path in (root, *root.rglob('*')):
        if path.is_symlink() or getattr(path.lstat(), 'st_file_attributes', 0) & 0x400:
            raise RuntimeError('Owned cleanup refuses reparse points.')


@dataclasses.dataclass(frozen=True)
class Lease:
    id: str
    token: str
    root: Path
    parent: Path
    ip: str
    udp_first: int
    owner_pid: int
    owner_birth: str
    template: str

    def public(self) -> dict:
        return {k: str(v) if isinstance(v, Path) else v for k, v in dataclasses.asdict(self).items() if k != 'token'}


class Registry:
    def __init__(self, directory: Path | None = None):
        base = Path(os.environ.get('LOCALAPPDATA', Path.home() / '.local/share'))
        self.directory = (directory or base / 'SDGO-GameTest/runtime-registry').resolve()
        self.directory.mkdir(parents=True, exist_ok=True)
        self.path = self.directory / 'environments.sqlite3'
        with self.connection() as cn:
            cn.executescript('''
                CREATE TABLE IF NOT EXISTS environments(
                  id TEXT PRIMARY KEY, token TEXT NOT NULL, root TEXT NOT NULL,
                  parent TEXT NOT NULL, ip TEXT NOT NULL, udp_first INTEGER NOT NULL,
                  owner_pid INTEGER NOT NULL, owner_birth TEXT NOT NULL,
                  template TEXT NOT NULL, state TEXT NOT NULL,
                  created REAL NOT NULL, updated REAL NOT NULL, error TEXT);
                CREATE UNIQUE INDEX IF NOT EXISTS leased_ip ON environments(ip) WHERE state<>'closed';
                CREATE UNIQUE INDEX IF NOT EXISTS leased_udp ON environments(udp_first) WHERE state<>'closed';
                CREATE TABLE IF NOT EXISTS resources(
                  sequence INTEGER PRIMARY KEY AUTOINCREMENT,
                  environment_id TEXT NOT NULL, descriptor TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS templates(
                  id TEXT PRIMARY KEY, path TEXT UNIQUE NOT NULL, created REAL NOT NULL);
                CREATE INDEX IF NOT EXISTS owned_resources ON resources(environment_id);
            ''')

    def connection(self):
        cn = sqlite3.connect(self.path, timeout=30, factory=_Connection)
        cn.row_factory = sqlite3.Row
        return cn

    def allocate(self, parent: Path, template: str, *, probe=True) -> Lease:
        parent = parent.resolve()
        parent.mkdir(parents=True, exist_ok=True)
        if parent.is_symlink() or getattr(parent.stat(), 'st_file_attributes', 0) & 0x400:
            raise RuntimeError('Environment parent must not be a reparse point.')
        birth = process_identity(os.getpid())
        if birth is None:
            raise RuntimeError('Cannot establish the controller process identity.')
        cn = self.connection()
        try:
            cn.execute('BEGIN IMMEDIATE')
            used = list(cn.execute("SELECT ip,udp_first FROM environments WHERE state<>'closed'"))
            ips, pools = {r['ip'] for r in used}, {r['udp_first'] for r in used}
            for _ in range(4096):
                number = secrets.randbelow(16384)
                ip = f'127.{64 + number // 256}.{number % 256}.1'
                first = 20000 + secrets.randbelow(700) * 40
                if ip in ips or first in pools:
                    continue
                if probe:
                    checks = []
                    try:
                        for port in (5001, 9001, 9002, 9003, 9101, 9302, 9303, 9304, 9305, 9306, 9307, 9801, 9901):
                            channel = socket.socket()
                            checks.append(channel)
                            channel.bind((ip, port))
                        for port in range(first, first + 24):
                            channel = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                            checks.append(channel)
                            channel.bind((ip, port))
                    except OSError:
                        continue
                    finally:
                        for channel in checks:
                            channel.close()
                identifier = str(uuid.uuid4())
                lease = Lease(identifier, secrets.token_hex(32), parent / identifier, parent, ip, first,
                              os.getpid(), birth, template)
                lease.root.mkdir(exist_ok=False)
                try:
                    atomic_json(lease.root / '.environment.json', {**lease.public(), 'token': lease.token, 'schema': SCHEMA, 'resources': []})
                    now = time.time()
                    cn.execute('INSERT INTO environments VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)',
                        (lease.id, lease.token, str(lease.root), str(parent), ip, first, lease.owner_pid,
                         birth, template, 'allocated', now, now, None))
                    cn.commit()
                except BaseException:
                    (lease.root / '.environment.json').unlink(missing_ok=True)
                    lease.root.rmdir()
                    raise
                return lease
            raise RuntimeError('No unclaimed loopback address/UDP range is available.')
        finally:
            cn.close()

    def get(self, identifier: str) -> Lease:
        with self.connection() as cn:
            row = cn.execute('SELECT * FROM environments WHERE id=?', (identifier,)).fetchone()
        if row is None:
            raise RuntimeError('Unknown GameTest environment.')
        return Lease(row['id'], row['token'], Path(row['root']), Path(row['parent']), row['ip'],
                     row['udp_first'], row['owner_pid'], row['owner_birth'], row['template'])

    def validate(self, lease: Lease, *, include_resources=True) -> dict:
        current = self.get(lease.id)
        if current != lease or lease.root.parent.resolve() != lease.parent.resolve() or lease.root.name != lease.id:
            raise RuntimeError('Environment ownership or cleanup path mismatch.')
        with self.connection() as cn:
            resources = [json.loads(r[0]) for r in cn.execute(
                'SELECT descriptor FROM resources WHERE environment_id=? ORDER BY sequence', (lease.id,))] if include_resources else []
            state = cn.execute('SELECT state FROM environments WHERE id=?', (lease.id,)).fetchone()[0]
        # A controller can die after deleting its tree but before closing the
        # lease. The registry still proves the exact path and resource owner.
        if not lease.root.exists() and state in ('cleaning', 'cleanup_required', 'closed'):
            return {'resources': resources}
        marker = lease.root / '.environment.json'
        for path in (lease.root, marker):
            if path.is_symlink() or getattr(path.lstat(), 'st_file_attributes', 0) & 0x400:
                raise RuntimeError('Environment ownership files must not be reparse points.')
        data = json.loads(marker.read_text(encoding='utf-8'))
        if data.get('id') != lease.id or data.get('token') != lease.token or data.get('root') != str(lease.root):
            raise RuntimeError('Environment marker ownership mismatch.')
        data['resources'] = resources
        return data

    def update(self, lease: Lease, state: str, error: str | None = None):
        if state not in (*ACTIVE, 'closed'):
            raise ValueError('Unknown environment lifecycle state.')
        with self.connection() as cn:
            changed = cn.execute('UPDATE environments SET state=?,updated=?,error=? WHERE id=? AND token=?',
                                 (state, time.time(), error, lease.id, lease.token)).rowcount
            if changed != 1:
                raise RuntimeError('Environment ownership changed.')

    def claim_recovery(self, identifier: str) -> Lease:
        """Only one recovery controller may adopt an abandoned environment."""
        with self.connection() as cn:
            cn.execute('BEGIN IMMEDIATE')
            row=cn.execute('SELECT * FROM environments WHERE id=?',(identifier,)).fetchone()
            if row is None or row['state']=='closed':
                raise RuntimeError('Environment is already closed or unknown.')
            if process_identity(row['owner_pid'])==row['owner_birth']:
                raise RuntimeError('A live controller still owns this environment.')
            birth=process_identity(os.getpid())
            if birth is None:raise RuntimeError('Cannot establish recovery controller identity.')
            cn.execute('UPDATE environments SET owner_pid=?,owner_birth=?,updated=? WHERE id=?',
                       (os.getpid(),birth,time.time(),identifier))
        return self.get(identifier)

    def record(self, lease: Lease, resource: dict):
        self.validate(lease,include_resources=False)
        with self.connection() as cn:
            cn.execute('INSERT INTO resources(environment_id,descriptor) VALUES(?,?)',
                       (lease.id, json.dumps(resource)))

    def list(self) -> list[dict]:
        with self.connection() as cn:
            rows = list(cn.execute('SELECT * FROM environments ORDER BY created'))
        return [{k: row[k] for k in row.keys() if k != 'token'} | {
            'owner_alive': process_identity(row['owner_pid']) == row['owner_birth'],
            'cleanup_required': row['state'] != 'closed',
        } for row in rows]

    def register_template(self, path: Path, identifier: str):
        with self.connection() as cn:
            cn.execute('INSERT INTO templates VALUES(?,?,?)', (identifier,str(path.resolve()),time.time()))

    def templates(self) -> list[dict]:
        with self.connection() as cn:
            return [dict(row) for row in cn.execute('SELECT * FROM templates ORDER BY created DESC')]

    def forget_closed(self,lease: Lease):
        if lease.root.exists():raise RuntimeError('Cannot forget an environment with remaining runtime files.')
        with self.connection() as cn:
            cn.execute('BEGIN IMMEDIATE')
            row=cn.execute('SELECT state,token FROM environments WHERE id=?',(lease.id,)).fetchone()
            if row is None:return
            if row['state']!='closed' or row['token']!=lease.token:
                raise RuntimeError('Only the exact closed environment can be forgotten.')
            cn.execute('DELETE FROM resources WHERE environment_id=?',(lease.id,))
            cn.execute('DELETE FROM environments WHERE id=?',(lease.id,))


class ManagedEnvironment:
    """The only runtime owner: success, failure and interruption all close it."""
    def __init__(self, registry: Registry, lease: Lease, driver, archive_parent: Path):
        self.registry, self.lease, self.driver = registry, lease, driver
        self.archive = archive_parent.resolve() / lease.id
        if self.archive.is_relative_to(lease.root):
            raise ValueError('Evidence archive must be outside the disposable runtime.')
        self.closed = False
        self.evidence_deleted = False

    def __enter__(self):
        self.registry.validate(self.lease)
        self.registry.update(self.lease, 'starting')
        return self

    def __exit__(self, exception_type, exception, tb):
        if exception is not None:
            atomic_json(self.lease.root/'result.json', {'status':'fail','error_type':exception_type.__name__,
                'reason':str(exception)})
        self.close()
        return False

    def close(self, *, recovering=False):
        if self.closed:
            return
        lease = self.lease
        if recovering and process_identity(lease.owner_pid) == lease.owner_birth:
            raise RuntimeError('A live controller still owns this environment.')
        data = self.registry.validate(lease)
        self.registry.update(lease, 'cleaning')
        self.archive.mkdir(parents=True, exist_ok=True)
        try:
            result_path=lease.root/'result.json'
            result=json.loads(result_path.read_text(encoding='utf-8')) if result_path.is_file() else {}
            discard_success=result.get('status') in ('pass','template_ready')
            cleanup = self.driver.cleanup(lease, data['resources'])
            atomic_json(self.archive / 'cleanup.json', cleanup)
            if cleanup.get('status') != 'clean':
                raise RuntimeError('Owned runtime cleanup needs attention; lease retained.')
            self.driver.archive(lease, self.archive)
            atomic_json(self.archive / 'environment.json', {**lease.public(),'result_status':result.get('status')})
            # Delete only the verified temporary tree. Templates, source files,
            # reusable accounts and the final evidence archive are outside it.
            self.registry.validate(lease)
            if lease.root.exists():
                no_reparse_tree(lease.root)
            def remove_readonly(function, name, exc_info):
                error = exc_info[1]
                path = Path(name)
                if not path.resolve().is_relative_to(lease.root) or path.stat().st_nlink != 1:
                    raise RuntimeError('Refuse to change a foreign/shared file during cleanup.') from error
                path.chmod(0o700)
                function(name)
            if lease.root.exists():
                shutil.rmtree(lease.root, onerror=remove_readonly)
            if lease.root.exists():
                raise RuntimeError('Disposable runtime directory remains.')
            self.registry.update(lease, 'closed')
            (self.archive / 'agent-action.json').unlink(missing_ok=True)
            self.closed = True
            if discard_success:
                purge_success_evidence(self.registry,lease.id,self.archive)
                self.evidence_deleted=True
        except BaseException as error:
            self.registry.update(lease, 'cleanup_required', type(error).__name__)
            atomic_json(self.archive / 'agent-action.json', {
                'status': 'cleanup_required', 'environment_id': lease.id,
                'action': ['python', 'tools/game_test.py', 'environment', 'recover'],
                'reason': type(error).__name__, 'runtime_root': str(lease.root),
            })
            raise


def purge_success_evidence(registry: Registry,identifier: str,archive: Path) -> None:
    """Delete only a closed, successful, clean run's exact owned evidence tree."""
    lease=registry.get(identifier)
    row=next(r for r in registry.list() if r['id']==identifier)
    if row['state']!='closed' or lease.root.exists():
        raise RuntimeError('Evidence cleanup requires a closed environment with no remaining runtime.')
    # Tests and callers may choose a different archive parent; its child must
    # still carry this exact environment identity and clean-success markers.
    if archive.name!=identifier or archive.parent==lease.root or archive.resolve().is_relative_to(lease.root):
        raise RuntimeError('Evidence cleanup path or ownership mismatch.')
    if not archive.exists():return
    no_reparse_tree(archive)
    marker=json.loads((archive/'environment.json').read_text(encoding='utf-8'))
    cleanup=json.loads((archive/'cleanup.json').read_text(encoding='utf-8'))
    if (marker.get('id')!=identifier or marker.get('root')!=str(lease.root) or
        marker.get('result_status') not in ('pass','template_ready') or cleanup.get('status')!='clean'):
        raise RuntimeError('Only verified successful, clean evidence may be discarded.')
    shutil.rmtree(archive)
    registry.forget_closed(lease)


def main(argv=None):
    from tools.game_test_native_environment import native_main
    return native_main(argv)


if __name__ == '__main__':
    raise SystemExit(main())
