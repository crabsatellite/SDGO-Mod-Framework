"""Single loopback API. All providers share its origin, session and mode gates."""
from __future__ import annotations
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import secrets
import threading
from urllib.parse import parse_qs, urlsplit
from . import storage
from .providers import Binary
from .change_review import ReviewRequired


def make_server(workspace, port=0):
    token = secrets.token_urlsafe(32)
    static = Path(__file__).with_name('static')

    class Handler(BaseHTTPRequestHandler):
        server_version = 'SDGOModManager/1'
        def log_message(self, *_): pass

        def host_ok(self):
            return self.headers.get('Host') in (f'127.0.0.1:{self.server.server_port}', f'localhost:{self.server.server_port}')

        def response(self, status, data, mime='application/json; charset=utf-8', filename=None):
            self.send_response(status)
            for key, value in [('Content-Type', mime), ('Content-Length', str(len(data))), ('Cache-Control', 'no-store'),
                ('X-Content-Type-Options', 'nosniff'), ('Referrer-Policy', 'no-referrer'),
                ('Content-Security-Policy', "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' blob: data:; connect-src 'self'; frame-ancestors 'none'; object-src 'none'; base-uri 'none'")]:
                self.send_header(key, value)
            if filename:
                self.send_header('Content-Disposition', 'attachment; filename="' + filename + '"')
            self.end_headers()
            self.wfile.write(data)

        def send_json(self, status, value): self.response(status, storage.json_bytes(value))

        def execute(self, method):
            try:
                if not self.host_ok(): raise PermissionError('访问地址无效')
                split = urlsplit(self.path); path = split.path; query = parse_qs(split.query)
                body = {}
                if method != 'GET':
                    origins = (f'http://127.0.0.1:{self.server.server_port}', f'http://localhost:{self.server.server_port}')
                    if self.headers.get('Origin') not in origins or not secrets.compare_digest(self.headers.get('X-ModManager-Token', ''), token):
                        raise PermissionError('请求不属于当前 Manager 会话')
                    if self.headers.get('Content-Type', '').split(';')[0] != 'application/json': raise ValueError('需要 JSON 请求')
                    length = int(self.headers.get('Content-Length', '0'))
                    limit = 176 if path=='/api/providers/operators/api/operators/import-preview' else 46 if path.startswith('/api/providers/units/') else 64 if path.startswith('/api/providers/capsules/') else 352 if path == '/api/import' else 16
                    if not 0 < length <= limit * 1024 * 1024: raise ValueError('请求大小无效')
                    body = json.loads(self.rfile.read(length))
                    if not isinstance(body, dict): raise ValueError('请求必须是 JSON 对象')
                if method == 'GET' and path in ('/', '/ui-kit.html'):
                    return self.response(200, (static / ('index.html' if path == '/' else 'ui-kit.html')).read_bytes(), 'text/html; charset=utf-8')
                if method == 'GET' and path in ('/app.js', '/owner_updates.js', '/style.css', '/panel_helpers.js', '/ui.css', '/ui.js'):
                    return self.response(200, (static / path[1:]).read_bytes(), 'text/javascript; charset=utf-8' if path.endswith('.js') else 'text/css; charset=utf-8')
                if method=='GET' and path in ('/brand.svg','/favicon.ico'):
                    return self.response(200,(static/('brand.svg' if path=='/brand.svg' else 'brand.ico')).read_bytes(),
                                         'image/svg+xml' if path=='/brand.svg' else 'image/x-icon')
                if method == 'GET' and path.startswith('/api/art/'):
                    from .art import image
                    try:
                        return self.response(200, image(workspace, path[len('/api/art/'):], (query.get('host') or [None])[0]), 'image/jpeg')
                    except LookupError as error:
                        return self.send_json(404, {'error': str(error)})
                if method == 'GET' and path.startswith('/panels/'):
                    name = path[len('/panels/'):]
                    allowed={key + '.js' for key in workspace.providers}
                    if 'rewards' in workspace.providers:allowed.add('reward_social.js')
                    if 'announcements' in workspace.providers:allowed.add('announcement_news.js')
                    if 'operators' in workspace.providers:allowed.add('operator_protocol_ui.js')
                    if name not in allowed: raise ValueError('面板未登记')
                    return self.response(200, (static / 'panels' / name).read_bytes(), 'text/javascript; charset=utf-8')
                if method == 'GET' and path == '/api/state': return self.send_json(200, {**workspace.public(), 'session_token': token})
                if method=='GET'and path=='/api/safety':return self.send_json(200,workspace.change_reviews.state())
                if method=='GET'and path=='/api/runtime':
                    from .runtime_state import host_mod_states
                    host=workspace.host(query['host'][0])
                    return self.send_json(200,host_mod_states(host,workspace._installed(host)))
                if method == 'POST' and path == '/api/timezone':
                    return self.send_json(200,{**workspace.set_display_timezone(body.get('utc_offset_minutes')),'session_token':token})
                if method=='POST' and path=='/api/entrypoints':
                    executable=getattr(getattr(workspace,'updates',None),'manager_exe',None)
                    if executable is None:raise ValueError('请使用打包版 Manager 创建快捷入口')
                    from .entrypoints import create
                    rows=create(workspace,executable)
                    return self.send_json(200,{'created':sum(r['status']=='created' for r in rows),'kept':sum(r['status']=='kept' for r in rows)})
                if method == 'GET' and path == '/api/identity': return self.send_json(200, {'application': 'sdgo_mod_manager', 'workspace_id': storage.revision(str(workspace.root).encode())})
                if path=='/api/updates':
                    updates=getattr(workspace,'updates',None)
                    if updates is None:raise ValueError('当前管理器没有更新组件，请安装新版')
                    if method=='GET':return self.send_json(200,updates.state())
                    if method=='POST':
                        action=body.get('action')
                        if action=='source':return self.send_json(200,updates.source(body.get('source',body.get('repository'))))
                        if action=='runtime_plan':return self.send_json(200,updates.runtime_plan(body.get('host')))
                        if action=='adopt':return self.send_json(200,updates.adopt(body.get('host'),body.get('components'),body.get('digest')))
                        if action=='channel':return self.send_json(200,updates.channel(body.get('channel')))
                        if action=='automatic':
                            if type(body.get('enabled')) is not bool:raise ValueError('自动更新开关无效')
                            result=workspace.services.start('manager.updates') if body['enabled'] else workspace.services.stop('manager.updates')
                            return self.send_json(200,updates.state())
                        if action in ('check','install'):return self.send_json(200,updates.request(action=='install'))
                        if action=='restart':
                            from .update_restart import schedule
                            schedule(updates)
                            self.send_json(200,{'restarting':True})
                            threading.Thread(target=self.server.shutdown,daemon=True).start();return
                        raise ValueError('更新操作无效')
                if path=='/api/client-updates':
                    controller=getattr(workspace,'client_runtime',None)
                    if controller is None:raise ValueError('完整客户端更新尚未初始化')
                    if method=='GET':return self.send_json(200,controller.state())
                    if method=='POST':
                        if body.get('action')=='cancel':return self.send_json(200,controller.cancel(body.get('host')))
                        if body.get('action')=='profile_install':
                            return self.send_json(200,controller.bind_profile(body.get('host'),body.get('feeds',[]),body.get('offline')))
                        if body.get('action')=='profile_publish':
                            return self.send_json(200,controller.publish_profile(body.get('host'),body.get('directory'),body.get('feeds',[]),body.get('extras',[]),body.get('zip_payload',False),body.get('git_remote',''),body.get('branch','content')))
                        if body.get('action')=='restart':
                            controller.restart(body.get('host'));self.send_json(200,{'restarting':True})
                            threading.Thread(target=self.server.shutdown,daemon=True).start();return
                        return self.send_json(200,controller.request(body.get('host'),body.get('action'),body.get('digest')))
                if path=='/api/owner-updates':
                    controller=getattr(workspace,'owner_channels',None)
                    if controller is None:raise ValueError('服主发布组件尚未初始化')
                    if method=='GET':return self.send_json(200,controller.state())
                    if method=='POST':
                        action=body.get('action');identity=body.get('host')
                        if action=='resolve':return self.send_json(200,controller.resolve(body.get('feeds')))
                        if action=='automatic':return self.send_json(200,controller.automatic(identity,body.get('enabled')))
                        if action=='configure':return self.send_json(200,controller.configure(identity,body))
                        if action=='preview':return self.send_json(200,controller.preview(identity))
                        if action in ('publish','check','install','verify_publish'):
                            return self.send_json(200,controller.request(action,identity,body.get('digest')))
                        raise ValueError('服主发布操作无效')
                if method == 'GET' and path == '/api/developer':
                    workspace.require_developer()
                    host=workspace.host(query['host'][0]);identity=query['mod'][0]
                    root=storage.safe_child(Path(host['path'])/'sdgo/mods',identity)
                    manifest=storage.ini_values((root/'mod.ini').read_bytes())
                    return self.send_json(200, {'host_id':host['id'],'mod_id':identity,'directory':str(root),'manifest':dict(manifest['mod']),'contract':workspace.contracts.get(identity)})
                if method == 'POST' and path == '/api/ai-context':
                    from .ai_context import build
                    return self.send_json(200, build(workspace, 'http://127.0.0.1:'+str(self.server.server_port)+'/', host_id=body.get('host'),mod_id=body.get('mod'),provider_id=body.get('provider'),kind=body.get('kind','configure'),problem=body.get('problem','')))
                if method == 'POST' and path == '/api/mode': return self.send_json(200, workspace.set_mode(body['mode']))
                if path == '/api/services' and method == 'GET': return self.send_json(200, workspace.services.describe())
                if path.startswith('/api/services/'):
                    identity = path[len('/api/services/'):]
                    if method == 'GET': return self.send_json(200, workspace.services.describe(identity))
                    if method == 'POST' and body.get('action') == 'start': return self.send_json(200, workspace.services.start(identity))
                    if method == 'POST' and body.get('action') == 'stop': return self.send_json(200, workspace.services.stop(identity))
                    raise ValueError('后台任务操作无效')
                if method == 'POST' and path == '/api/exit':
                    self.send_json(200,{'stopping':'manager_only'})
                    threading.Thread(target=self.server.shutdown,daemon=True).start()
                    return
                if method == 'POST' and path == '/api/hosts': return self.send_json(200, workspace.add_host(body['path']))
                if method == 'POST' and path == '/api/pick':
                    from .picker import choose
                    return self.send_json(200,choose(body.get('kind','directory')))
                if method == 'POST' and path == '/api/import':
                    from .packages import import_client
                    return self.send_json(200, import_client(workspace, body['host'], body['package']))
                if method == 'POST' and path == '/api/toggle':
                    return self.send_json(200, workspace.toggle(body['host'], body['mod'], body['enabled'], body['revision']))
                if method == 'POST' and path == '/api/category':
                    return self.send_json(200,workspace.category(body['host'],body['category'],body['enabled'],body['revision']))
                if path == '/api/settings':
                    if method == 'GET':
                        return self.send_json(200, workspace.details(query['host'][0], query['mod'][0]))
                    if method == 'POST':
                        return self.send_json(200, workspace.configure(body['host'], body['mod'], body['values'], body['revisions']))
                if path.startswith('/api/providers/'):
                    rest = path[len('/api/providers/'):]; identity, _, local = rest.partition('/')
                    provider = workspace.providers.get(identity)
                    if provider is None: raise ValueError('模块未登记')
                    if local == 'config':
                        if method == 'GET': return self.send_json(200, provider.describe())
                        if method == 'POST': return self.send_json(200, provider.configure(body))
                    with provider.lock, workspace.change_reviews.execute(provider,method,'/'+local,body,self.headers):
                        result = provider.request(method, '/' + local + ('?' + split.query if split.query else ''), body, self.headers)
                    if isinstance(result, Binary): return self.response(200, result.data, result.mime, result.filename)
                    def urls(value):
                        if isinstance(value, list): return [urls(item) for item in value]
                        if isinstance(value, dict):
                            return {key: ('/api/providers/' + identity + item if key in ('url','asset_url','package_url') and isinstance(item,str) and item.startswith(('/api/','/assets/')) else urls(item)) for key,item in value.items()}
                        return value
                    if isinstance(result, dict): result = {**result, 'session_token': token}
                    return self.send_json(200, urls(result))
                return self.send_json(404, {'error': '此入口不存在'})
            except (BrokenPipeError,ConnectionAbortedError,ConnectionResetError): return
            except PermissionError as error: self.send_json(403, {'error': str(error)})
            except ReviewRequired as error:self.send_json(409,{'error':str(error),'change_review':error.plan})
            except storage.Conflict as error: self.send_json(409, {'error': str(error)})
            except (ValueError, KeyError, TypeError, FileNotFoundError) as error: self.send_json(400, {'error': str(error)})
            except Exception as error:
                # Capsule domain errors carry a bounded, owner-readable message.
                from .engines.capsules.capsule_ui import RequestError, ConflictError
                if isinstance(error, RequestError): return self.send_json(409 if isinstance(error, ConflictError) else 400, {'error': str(error)})
                import traceback
                workspace.root.mkdir(parents=True, exist_ok=True)
                with (workspace.root / 'manager-errors.log').open('a', encoding='utf-8') as stream: stream.write(traceback.format_exc() + '\n')
                self.send_json(500, {'error': '操作未完成，请查看本机 Manager 日志'})

        def do_GET(self): self.execute('GET')
        def do_POST(self): self.execute('POST')
        def do_PUT(self): self.execute('PUT')
        def do_DELETE(self): self.execute('DELETE')

    server = ThreadingHTTPServer(('127.0.0.1', port), Handler)
    server.daemon_threads = True
    server.workspace = workspace
    return server
