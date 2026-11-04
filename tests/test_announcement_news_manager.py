import copy
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from manager import announcement_news as news
from manager.workspace import Workspace
from manager.providers import attach
from manager.server import make_server
from announcement_news_fixture import server as feed_server


class NewsManagerTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)
        client=self.root/'client';client.mkdir();(client/'GOnline.exe').touch()
        mod=client/'sdgo/mods/announcements';mod.mkdir(parents=True);(mod/'announcements.dll').touch()
        self.workspace=Workspace(self.root/'manager',Path(__file__).resolve().parents[1]/'mods');self.workspace.add_host(client);attach(self.workspace)
        self.server=make_server(self.workspace,0);self.base='http://127.0.0.1:'+str(self.server.server_port)
        self.thread=threading.Thread(target=self.server.serve_forever,daemon=True);self.thread.start()
        self.token=self.call('/api/state')['session_token'];self.feeds=feed_server();self.feed_thread=threading.Thread(target=self.feeds.serve_forever,daemon=True);self.feed_thread.start()
    def tearDown(self):
        self.workspace.close();self.server.shutdown();self.server.server_close();self.thread.join()
        self.feeds.shutdown();self.feeds.server_close();self.feed_thread.join();self.temp.cleanup()
    def call(self,path,body=None):
        headers={}
        if body is not None:headers={'Origin':self.base,'X-ModManager-Token':self.token,'Content-Type':'application/json'}
        request=urllib.request.Request(self.base+path,None if body is None else json.dumps(body).encode(),headers=headers)
        with urllib.request.urlopen(request,timeout=20) as response:
            raw=response.read();return json.loads(raw) if response.headers.get('Content-Type','').startswith('application/json') else raw
    def test_settings_preview_and_supervised_worker_use_existing_manager_contracts(self):
        prefix='/api/providers/announcements/api/announcements/news'
        self.call('/api/providers/announcements/config')
        before=self.call(prefix);settings=copy.deepcopy(before['settings'])
        settings['sources']=['http://127.0.0.1:'+str(self.feeds.server_port)]
        saved=self.call(prefix+'/settings',{'settings':settings,'revision':before['revision']})
        with self.assertRaises(urllib.error.HTTPError) as error:self.call(prefix+'/settings',{'settings':settings,'revision':before['revision']})
        self.assertEqual(error.exception.code,409)
        error.exception.close()
        preview=self.call(prefix+'/refresh',{})
        self.assertGreater(preview['available_count'],5)
        self.assertEqual({i['kind'] for i in preview['items']},set(news.KINDS))
        self.assertFalse(any('未来限免' in i['title'] for i in preview['items']))
        self.assertIn(b'mountNews',self.call('/panels/announcement_news.js'))
        started=self.call('/api/services/announcements.news',{'action':'start'})
        self.assertTrue(started['running']);self.assertTrue(started['desired']);self.assertEqual(started['owner'],'manager')
        stopped=self.call('/api/services/announcements.news',{'action':'stop'})
        self.assertFalse(stopped['running']);self.assertFalse(stopped['desired'])
        manual=self.call('/api/providers/announcements/api/announcements')
        self.assertEqual(manual['policy']['rules'],[])


if __name__=='__main__':unittest.main()
