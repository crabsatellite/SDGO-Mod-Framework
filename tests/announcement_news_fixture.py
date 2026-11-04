"""Loopback-only content fixture; game login/battle still use native services."""
from __future__ import annotations
import argparse,datetime as dt,json,threading,time
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path


def payloads(now=None):
    now=int(time.time() if now is None else now);date=dt.datetime.fromtimestamp(now,dt.timezone(dt.timedelta(hours=8))).date()
    return {
      '/v2/60s':{'code':200,'data':{'date':date.isoformat(),'news':[f'原生每日新闻验证 {n}：日期与来源保持一致' for n in range(1,7)],'link':'https://example.com/daily'}},
      '/v2/it-news':{'code':200,'data':[{'title':f'原生科技资讯验证 {n}：大厅提示按已读去重','link':f'https://example.com/tech/{n}','created_at':(now-60)*1000} for n in range(1,6)]},
      '/v2/epic':{'code':200,'data':[
          {'title':'原生有效限免验证','link':'https://example.com/game/live','original_price':100,'is_free_now':True,'free_start_at':(now-5*86400)*1000,'free_end_at':(now+86400)*1000},
          {'title':'不可展示的未来限免','link':'https://example.com/game/future','original_price':100,'is_free_now':True,'free_start_at':(now+86400)*1000,'free_end_at':(now+2*86400)*1000}]},
      '/v2/today_in_history':{'code':200,'data':{'month':date.month,'day':date.day,'items':[{'year':'1950','title':'原生科学发现验证','link':'https://example.com/history'}]}}
    }


def server(port=0,log=None):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self,*args):pass
        def do_GET(self):
            status=200;data=payloads().get(self.path)
            if self.path.startswith('/unavailable/'):status=503;data={'code':503}
            elif data is None:status=404;data={'code':404}
            raw=json.dumps(data,ensure_ascii=False).encode()
            if log:
                with Path(log).open('a',encoding='utf-8') as stream:stream.write(json.dumps({'path':self.path,'status':status,'time':time.time()})+'\n')
            self.send_response(status);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
    return ThreadingHTTPServer(('127.0.0.1',port),Handler)


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--port',type=int,default=0);parser.add_argument('--log',type=Path);args=parser.parse_args()
    service=server(args.port,args.log);print(json.dumps({'loopback_fixture_port':service.server_port}),flush=True)
    try:service.serve_forever()
    finally:service.server_close()
