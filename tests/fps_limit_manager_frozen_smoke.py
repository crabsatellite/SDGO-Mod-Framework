"""Exercise FPS config storage through the real frozen Manager EXE."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import urllib.request


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='sdgo-fps-manager-exe-') as temporary:
        root = Path(temporary)
        state = root/'state'
        state.mkdir()
        client = root/'client'
        client.mkdir()
        (client/'GOnline.exe').write_bytes(b'fixture')
        mod = client/'sdgo/mods/fps_limit'
        mod.mkdir(parents=True)
        shutil.copyfile(repo/'mods/fps_limit/mod.ini', mod/'mod.ini')
        original = (
            '<?xml version="1.0" encoding="UTF-8" standalone="no" ?>\r\n'
            '<Config>\r\n'
            '  <Video><GameScreenRes level="13" /></Video>\r\n'
            '  <Framework>\r\n'
            '    <Timer VerticalSync="false" RenderSync="false" resolution="high" '
            'updatefps="160" Custom="preserve" />\r\n'
            '  </Framework>\r\n'
            '  <LoginData><CheckAutoLogin use="true" id="private-user" pass="private-pass" /></LoginData>\r\n'
            '</Config>\r\n'
        ).encode('utf-8')
        (client/'config.xml').write_bytes(original)
        (state/'manager.json').write_text(json.dumps({
            'schema_version': 1,
            'hosts': [{'id': 'client', 'name': 'isolated client', 'path': str(client), 'role': 'client'}],
            'modules': {},
        }), encoding='utf-8')
        environment = dict(os.environ)
        environment.pop('PYTHONPATH', None)
        flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
        process = subprocess.Popen(
            [str(args.exe.resolve()), '--workspace', str(state), '--no-browser', '--port', '0'],
            cwd=root,
            env=environment,
            creationflags=flags,
        )
        base = token = None
        try:
            for _ in range(300):
                if process.poll() is not None:
                    raise RuntimeError('Frozen Manager exited before publishing its session')
                session = state/'manager-session.json'
                if session.exists():
                    base = json.loads(session.read_text(encoding='utf-8'))['url'].rstrip('/')
                    break
                time.sleep(.1)
            if not base:
                raise RuntimeError('Frozen Manager did not start')

            def call(path, body=None):
                headers = {
                    'Content-Type': 'application/json',
                    'Origin': base,
                    'X-ModManager-Token': token or '',
                }
                request = urllib.request.Request(
                    base+path,
                    None if body is None else json.dumps(body).encode('utf-8'),
                    headers,
                )
                with urllib.request.urlopen(request, timeout=30) as response:
                    return json.load(response)

            state_value = call('/api/state')
            token = state_value['session_token']
            installed = state_value['hosts'][0]['mods']
            fps = next(item for item in installed if item['id'] == 'fps_limit')
            assert fps['registered'] and fps['settings_count'] == 1
            details = call('/api/settings?host=client&mod=fps_limit')
            assert details['values'] == {'frame_limit': 160}
            assert set(details['revisions']) == {'config.xml'}
            assert 'private-user' not in json.dumps(details)
            assert 'private-pass' not in json.dumps(details)
            saved = call('/api/settings', {
                'host': 'client',
                'mod': 'fps_limit',
                'values': {'frame_limit': 120},
                'revisions': details['revisions'],
            })
            assert saved['values'] == {'frame_limit': 120}
            assert saved['activation'] == 'restart_required'
            assert 'private-user' not in json.dumps(saved)
            assert 'private-pass' not in json.dumps(saved)
            assert (client/'config.xml').read_bytes() == original.replace(
                b'updatefps="160"', b'updatefps="120"')
            call('/api/exit', {})
            process.wait(timeout=30)
            assert process.returncode == 0
            print('PASS frozen FPS Manager: registered, read 160, saved 120, preserved other XML, no credential leak')
        finally:
            if process.poll() is None:
                if base and token:
                    try:
                        call('/api/exit', {})
                    except Exception:
                        pass
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(timeout=10)


if __name__ == '__main__':
    main()
