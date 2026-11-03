"""Single desktop entry for the SDGO Mod Manager (also the frozen EXE entry)."""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import sys
import webbrowser

ROOT = Path(getattr(sys, '_MEIPASS', Path(__file__).resolve().parents[1]))
if str(ROOT) not in sys.path: sys.path.insert(0, str(ROOT))
from manager.workspace import Workspace
from manager.providers import attach
from manager.server import make_server
from manager import storage
from manager.instance import Lease


def main(argv=None):
    effective=sys.argv[1:] if argv is None else argv
    if effective[:1]==['--finish-update']:
        from manager.update_restart import finish
        if len(effective)!=2:raise ValueError('更新交接参数无效')
        finish(effective[1]);return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, default=Path(os.environ.get('LOCALAPPDATA', str(Path.home()))) / 'SDGO/ModManager')
    parser.add_argument('--port', type=int, default=0)
    parser.add_argument('--no-browser', action='store_true')
    args = parser.parse_args(argv)
    if not 0 <= args.port <= 65535: parser.error('port must be 0..65535')
    lease=Lease(args.workspace)
    if not lease.file:
        url=lease.existing_url()
        if not args.no_browser:webbrowser.open(url)
        if sys.stdout:print(url,flush=True)
        return
    workspace = Workspace(args.workspace, ROOT / 'mods')
    attach(workspace)
    from manager.updates import attach as attach_updates
    executable=Path(sys.executable) if getattr(sys,'frozen',False) else None
    attach_updates(workspace,ROOT,executable)
    if executable and not args.no_browser:
        from manager.entrypoints import create
        try:create(workspace,executable)
        except (OSError, ValueError):pass  # A read-only desktop must not prevent opening Manager.
    server = make_server(workspace, args.port)
    url = 'http://127.0.0.1:' + str(server.server_port) + '/'
    storage.atomic(workspace.root / 'manager-session.json', storage.json_bytes({'application': 'sdgo_mod_manager', 'url': url, 'pid': os.getpid()}))
    workspace.services.restore()
    if not args.no_browser: webbrowser.open(url)
    if sys.stdout: print(url, flush=True)
    try: server.serve_forever()
    finally:
        server.server_close()
        workspace.close()
        lease.close()


if __name__ == '__main__': main()
