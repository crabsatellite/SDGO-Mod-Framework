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


def announce(url):
    # A detached GUI can inherit a closed or invalid redirected stdout handle.
    # Publishing manager-session.json is authoritative; console output must
    # never prevent the already-bound HTTP server from entering its loop.
    if sys.stdout:
        try:print(url,flush=True)
        except (OSError,ValueError):pass


def main(argv=None):
    effective=sys.argv[1:] if argv is None else argv
    if effective[:1]==['--launch-game']:
        from manager import launch,launch_ui
        parser=argparse.ArgumentParser(description='Verify the bound runtime before starting the game')
        parser.add_argument('--launch-game',type=Path,required=True);parser.add_argument('--launch-request',required=True)
        parser.add_argument('--launch-worker',action='store_true');args=parser.parse_args(effective)
        try:
            launch.accept(args.launch_game,args.launch_request)
            if getattr(sys,'frozen',False)and not args.launch_worker:launch_ui.detach(args.launch_game,args.launch_request)
            else:launch_ui.run(args.launch_game,args.launch_request)
            return 0
        except Exception as error:
            if os.name=='nt':
                import ctypes
                ctypes.windll.user32.MessageBoxW(None,str(error),'SDGO Mod Manager',0x10)
            else:announce(str(error))
            return 1
    if effective[:1]==['--finish-client-update']:
        from manager.client_runtime import finish
        if len(effective)!=2:raise ValueError('客户端更新交接参数无效')
        finish(effective[1]);return
    if effective[:1]==['--finish-update']:
        from manager.update_restart import finish
        if len(effective)!=2:raise ValueError('更新交接参数无效')
        finish(effective[1]);return
    if effective[:1] in (['--sync-owner-pack'],['--check-owner-pack']):
        from manager.owner_channel import sync_client
        if len(effective)!=2:return 2
        try:return sync_client(effective[1],effective[0]=='--check-owner-pack')
        except Exception as error:
            announce('Owner update stopped: '+str(error));return 1
    parser = argparse.ArgumentParser(description=__doc__)
    sibling=Path(sys.executable).parent if getattr(sys,'frozen',False)else None
    local_client=bool(sibling and((sibling/'GOnline.exe').is_file()or(sibling/'sdgo/client-source.json').is_file()))
    default_workspace=sibling/'sdgo/local-state/manager'if local_client else Path(os.environ.get('LOCALAPPDATA',str(Path.home())))/'SDGO/ModManager'
    parser.add_argument('--workspace', type=Path, default=default_workspace)
    parser.add_argument('--port', type=int, default=0)
    parser.add_argument('--no-browser', action='store_true')
    parser.add_argument('--startup-client',type=Path,help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if not 0 <= args.port <= 65535: parser.error('port must be 0..65535')
    lease=Lease(args.workspace)
    if not lease.file:
        url=lease.existing_url()
        if not args.no_browser:webbrowser.open(url)
        announce(url)
        return
    workspace = Workspace(args.workspace, ROOT / 'mods')
    if args.startup_client:
        root=args.startup_client.resolve(strict=True)
        if args.workspace.resolve()!=root/'sdgo/local-state/manager':raise ValueError('启动工作区不属于这个客户端')
        if not any(Path(h['path']).resolve()==root for h in workspace.settings['hosts']):workspace.add_host(root)
    if local_client and not any(Path(h['path']).resolve()==sibling.resolve()for h in workspace.settings['hosts']):
        workspace.add_host(sibling)
    attach(workspace)
    from manager.updates import attach as attach_updates
    executable=Path(sys.executable) if getattr(sys,'frozen',False) else None
    if executable and args.startup_client:executable=args.startup_client.resolve()/'SDGO Mod Manager.exe'
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
    announce(url)
    try: server.serve_forever()
    finally:
        server.server_close()
        workspace.close()
        lease.close()


if __name__ == '__main__': raise SystemExit(main())
