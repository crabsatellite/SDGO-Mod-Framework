"""Manager-owned bounded native bridge and single-flight DeepSeek worker."""
from __future__ import annotations

import json
import hashlib
import os
from pathlib import Path
import struct
import threading
import time

from . import storage

HEADER = struct.Struct('<8sIIQIIQ13I')
ROW = struct.Struct('<10Ii3I64s128s')
REQUEST_SIZE = HEADER.size + ROW.size * 24
REPLY = struct.Struct('<8sQIII')
KIND = {1: 'ai_kill', 2: 'ai_defeated', 3: 'ally_focus', 4: 'ally_support'}
WEAPON = {0: '未知', 1: '近战', 2: '射击', 3: '必杀'}


def ticks():
    if os.name == 'nt':
        import ctypes
        fn = ctypes.windll.kernel32.GetTickCount64
        fn.restype = ctypes.c_ulonglong
        return fn()
    return int(time.monotonic() * 1000)


def wide(raw):
    ends = [i for i in range(0, len(raw), 2) if raw[i:i+2] == b'\0\0']
    if not ends:
        raise ValueError('native dialogue string is not terminated')
    return raw[:ends[0]].decode('utf-16-le', errors='strict')


def decode(raw, now=None):
    if len(raw) != REQUEST_SIZE:
        raise ValueError('native dialogue request size mismatch')
    (magic, size, pid, session, sequence, battle, created, kind, local, speaker, target,
     killer, victim, focus, weapon, slot, count, mode, elapsed, limited) = HEADER.unpack_from(raw)
    now = ticks() if now is None else now
    if (magic != b'SDGODLG1' or size != REQUEST_SIZE or not pid or not session or not sequence
            or not battle or kind not in KIND or not 2 <= count <= 24 or not 0 <= now-created <= 10000):
        raise ValueError('native dialogue request expired or invalid')
    roster = []
    for index in range(count):
        pilot, obj, unit, team, ai, alive, aim, selected, deaths, life, stock, cost, respawn, targeted, name, unit_name = ROW.unpack_from(raw, HEADER.size + ROW.size * index)
        if ai not in (0, 1) or alive not in (0, 1):
            raise ValueError('native dialogue flags invalid')
        roster.append(dict(pilot_id=pilot, object_id=obj, unit_id=unit, team=team,
                           is_ai=bool(ai), alive=bool(alive), name=wide(name), unit_name=wide(unit_name),
                           deaths=deaths, current_life=life, stock_points=stock, life_cost=cost,
                           respawn_state={0:'unknown',1:'alive',2:'waiting_respawn',3:'eliminated'}.get(respawn,'unknown'),
                           targeted_by_ai_count=targeted))
    return dict(pid=pid, session=session, sequence=sequence, battle=battle, created=created,
                kind=KIND[kind], local_pilot=local, speaker=speaker, target=target,
                killer=killer, victim=victim, focus=focus,
                weapon={'category': WEAPON.get(weapon, '未知'), 'name': '', 'evidence':'selected_at_death_notice'},
                room_mode=mode, observed_elapsed_seconds=elapsed, limited_lives={0:False,1:True,2:None}.get(limited), roster=roster)


def encode_reply(event, value):
    from .ai_dialogue import line
    raw = line(value).encode('utf-16-le')
    if len(raw) > 96:
        raise ValueError('dialogue exceeds native UTF-16 budget')
    return REPLY.pack(b'SDGODLR1', event['session'], event['sequence'], event['battle'], len(raw)//2) + raw


def claim(folder, raw):
    """Reserve an event before HTTP, including across Manager restarts."""
    from .target_locks import lease
    digest = hashlib.sha256(raw).digest()
    try:
        with lease('ai-dialogue-event:' + str(folder.resolve()).casefold()):
            marker = folder/'claim.bin'
            if marker.is_symlink():
                return False
            if marker.is_file() and marker.stat().st_size == 32 and marker.read_bytes() == digest:
                return False
            storage.atomic(marker, digest)
            return True
    except storage.Conflict:
        return False


class DialogueService:
    def __init__(self, provider):
        self.provider = provider
        self.thread = None
        self.stop_event = threading.Event()
        self.error = ''
        self.healthy = False
        self.completed = self.skipped = 0
        self.seen = set()
        self.sessions = {}
        self.histories = {}
        self.retry_at = 0

    def state(self):
        running = bool(self.thread and self.thread.is_alive())
        return {'running': running, 'healthy': running and self.healthy, 'error': self.error,
                'recovering': bool(running and self.error), 'completed': self.completed,
                'skipped': self.skipped, 'ownership': 'manager_process'}

    def start(self):
        if self.thread and self.thread.is_alive():
            return self.state()
        self.stop_event.clear(); self.error = ''; self.healthy = True
        self.thread = threading.Thread(target=self.run, name='sdgo-ai-dialogue', daemon=True)
        self.thread.start()
        return self.state()

    def stop(self):
        self.stop_event.set()
        if self.thread:
            self.thread.join(timeout=12)
        self.healthy = False
        return self.state()

    def run(self):
        try:
            while not self.stop_event.is_set():
                for host in self.provider.clients():
                    if self.stop_event.is_set():
                        break
                    try:
                        self.poll(host)
                    except Exception:
                        self.error = 'AI 台词配置或本机密钥不可用，请检查设置'
                self.stop_event.wait(.2)
        finally:
            self.healthy = False

    def poll(self, host):
        from .ai_dialogue import complete, event_context, messages
        details, personas, prompt, revisions = self.provider.runtime_inputs(host)
        settings = details['values']
        if not settings['enabled'] or not details.get('enabled', True):
            return
        key = self.provider.secrets.get(host['path'],settings['endpoint'])
        if not key:
            self.error = '请为所选客户端填写本机 DeepSeek API 密钥'
            return
        if time.monotonic() < self.retry_at:
            return
        directory = Path(host['path']) / 'sdgo/local-state/ai-dialogue'
        if not directory.is_dir() or directory.is_symlink():
            return
        # Successful native sessions leave empty folders. Old empty folders
        # must not hide the current session after many game launches.
        candidates = sorted(directory.iterdir(), key=lambda p: p.stat().st_mtime, reverse=True)[:64]
        for folder in candidates:
            if not folder.is_dir() or folder.is_symlink() or not __import__('re').fullmatch(r'[0-9]+-[0-9a-f]{16}', folder.name):
                continue
            request = folder / 'request.bin'
            if not request.is_file() or request.is_symlink() or request.stat().st_size != REQUEST_SIZE:
                continue
            raw = request.read_bytes()
            try:
                event = decode(raw)
            except (ValueError, UnicodeError):
                continue
            if folder.name != str(event['pid']) + '-' + format(event['session'], '016x'):
                continue
            identity = (str(directory), event['session'], event['battle'], event['sequence'])
            if identity in self.seen:
                continue
            # Count/reserve before contacting the provider: retries cannot
            # duplicate charges, and an HTTP timeout cannot produce a backlog.
            self.seen.add(identity)
            if len(self.seen) > 2048:
                self.seen = {identity}
            lease = (str(directory), event['session'], event['battle'])
            state = self.sessions.setdefault(lease, {'count': 0, 'last': 0})
            if len(self.sessions) > 64:
                self.sessions = {lease: state}
                self.histories.clear()
            if state['count'] >= settings['max_lines'] or time.monotonic()-state['last'] < settings['cooldown_seconds']:
                self.skipped += 1
                continue
            context = event_context(event, personas)
            if event['kind'].startswith('ally_') and not settings['team_talk']:
                continue
            history_key = (lease, event['speaker'])
            history = self.histories.get(history_key, [])
            if not claim(folder, raw):
                continue
            state['count'] += 1; state['last'] = time.monotonic()
            try:
                effective_prompt = prompt + ('\n本次发到战斗频道，台词最多 18 个汉字或同等短句。' if settings['visibility'] == 'room' else '')
                result = complete(settings, key, messages(context, effective_prompt, settings['language'], history))
            except ValueError as error:
                self.error = str(error)
                self.retry_at = time.monotonic()+30
                self.skipped += 1
                continue
            except (TimeoutError, OSError):
                self.error = '台词服务超时，已跳过本次发言'
                self.retry_at = time.monotonic()+30
                self.skipped += 1
                continue
            # Refuse an answer after leaving the battle, disabling the Mod,
            # changing a prompt or superseding the one-slot native request.
            current, _, _, current_revisions = self.provider.runtime_inputs(host)
            heartbeat = folder / 'heartbeat.bin'
            try:
                fresh = heartbeat.is_file() and 0 <= time.time()-heartbeat.stat().st_mtime < 3
                same_request = request.is_file() and request.read_bytes() == raw
            except OSError:
                fresh = same_request = False
            if (self.stop_event.is_set() or not fresh or ticks()-event['created'] > 10000
                    or current['revisions'] != details['revisions'] or revisions != current_revisions
                    or not current['values']['enabled'] or not same_request):
                self.skipped += 1
                continue
            storage.atomic(folder / 'reply.bin', encode_reply(event, result))
            self.histories[history_key] = (history + [result])[-4:]
            self.completed += 1; self.error = ''; self.healthy = True
