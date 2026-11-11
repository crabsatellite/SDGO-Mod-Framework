import base64
import http.client
import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch

from owner.units.workspace import Workspace
from unit_import_support import Baseline,Repository,archive,png,publisher

from manager.workspace import Workspace as ManagerWorkspace
from manager.providers import attach
from manager.server import make_server


class HttpTests(unittest.TestCase):
    def setUp(self):
        # This HTTP fixture has no native artwork archive. The shared card
        # renderer is covered by the presentation tests, not a fake UI receipt.
        art=patch('owner.units.presentation.capsule_card',return_value=png());art.start();self.addCleanup(art.stop)
        self.tmp=tempfile.TemporaryDirectory()
        self.manager=ManagerWorkspace(Path(self.tmp.name)/'manager',Path(__file__).resolve().parents[1]/'mods');attach(self.manager)
        self.workspace=Workspace(Path(self.tmp.name)/'units',baseline=Baseline(),repository=Repository(),publisher=publisher)
        self.manager.providers['units'].engine=self.workspace
        self.server=make_server(self.manager);self.thread=threading.Thread(target=self.server.serve_forever,kwargs={'poll_interval':.01},daemon=True);self.thread.start()
        self.origin='http://127.0.0.1:'+str(self.server.server_port)
        self.token=self.request('GET','/api/state')[1]['session_token'];self.state=self.request('GET','/api/units')[1]
    def tearDown(self):
        self.server.shutdown();self.server.server_close();self.thread.join();self.workspace.close();self.tmp.cleanup()
    def request(self,method,path,value=None,headers=None):
        h={'Content-Type':'application/json'}
        if method!='GET':h.update({'Origin':self.origin,'X-ModManager-Token':getattr(self,'token',''),'X-Catalog-Revision':self.state['revision']})
        h.update(headers or {})
        c=http.client.HTTPConnection('127.0.0.1',self.server.server_port,timeout=5)
        c.request(method,('/api/providers/units'+path if path.startswith('/api/units') else path),None if value is None else json.dumps(value),h);r=c.getresponse();raw=r.read();kind=r.getheader('Content-Type','');status=r.status;c.close()
        return status,json.loads(raw) if kind.startswith('application/json') else raw
    def inspect(self):
        status,state=self.request('POST','/api/units/inspect',{'filename':'test.zip','data':base64.b64encode(archive()).decode()})
        self.assertEqual(status,200,state);self.state=state;return '/api/units/drafts/'+state['draft']['id']
    def test_real_html_and_module_route(self):
        status,raw=self.request('GET','/panels/units.js');self.assertEqual(status,200);self.assertIn('确认'.encode(),raw)
        self.assertIn('王牌驾驶员显示名'.encode(),raw)
        self.assertIn('整场替代机体名'.encode(),raw)
        status,result=self.request('GET','/api/state');self.assertIn('units',{r['id'] for r in result['panels']})
        self.assertEqual(self.request('GET','/units')[0],404)
    def test_wrong_origin_and_token_block_write(self):
        body={'filename':'x.zip','data':'YmFk'}
        for headers in ({'Origin':'https://example.invalid'},{'X-ModManager-Token':'wrong'}):
            self.assertEqual(self.request('POST','/api/units/inspect',body,headers)[0],403)
        self.assertEqual(self.workspace.public()['drafts'],[])
    def test_invalid_archive_rejected_without_draft(self):
        status,_=self.request('POST','/api/units/inspect',{'filename':'x.zip','data':'YmFk'})
        self.assertEqual(status,400);self.assertEqual(self.workspace.public()['drafts'],[])
    def test_missing_fields_and_stale_revision_block_confirmation(self):
        path=self.inspect();d=self.state['draft'];status,_=self.request('POST',path+'/confirm',{'preview_revision':d['content_revision'],'shop':True,'capsule':True})
        self.assertEqual(status,400)
        status,state=self.request('PUT',path,{'name':'修改'});self.assertEqual(status,200)
        self.assertEqual(self.request('PUT',path,{'name':'覆盖'})[0],409)
    def test_complete_preview_confirm_and_export(self):
        path=self.inspect();d=self.state['draft']
        status,self.state=self.request('PUT',path,{'item_template_id':1110001,'forms':[{'id':r['id'],'template_id':10001} for r in d['forms']],
                                                'weapons':[{'id':r['id'],'template_id':41001} for r in d['weapons']]})
        self.assertEqual(status,200,self.state)
        for role in ('shop','capsule'):
            status,self.state=self.request('POST',path+'/asset',{'filename':role+'.png','data':base64.b64encode(png()).decode(),'role':role})
            self.assertEqual(status,200,self.state)
        d=self.state['draft'];self.assertTrue(d['can_confirm'],d['requirements'])
        self.assertEqual(self.request('GET',d['previews']['shop'])[0],200)
        self.assertEqual(self.request('GET',path+'/export')[0],400)
        status,self.state=self.request('POST',path+'/confirm',{'preview_revision':d['content_revision'],'shop':True,'capsule':True,
            '_native_ui':{'method':'forged','shop':True,'capsule':True}})
        self.assertEqual(status,200,self.state);self.assertEqual(self.state['draft']['status'],'confirmed')
        confirmation=self.state['draft']['confirmation']
        self.assertNotIn('native_ui',confirmation)
        self.assertEqual(confirmation['presentation_review']['method'],'shared_asset_generators')
        self.assertEqual(len(confirmation['presentation_review']['generated_sha256']['shop']),64)
        status,raw=self.request('GET',path+'/export');self.assertEqual(status,200);self.assertTrue(raw.startswith(b'PK'))
        status,_=self.request('GET',path+'/server-plan');self.assertEqual(status,200)
    def test_raw_source_blob_cannot_be_downloaded_as_preview(self):
        self.inspect();sha=self.state['draft']['forms'][0]['source_sha']
        self.assertEqual(self.request('GET','/api/units/assets/'+sha)[0],400)

    def test_remove_missing_skill_is_explicit_and_uses_shared_revision_gate(self):
        path=self.inspect()
        status,self.state=self.request('PUT',path,{'forms':[{'id':90001,'server':{'SkillNumber_1':70331}}]})
        self.assertEqual(status,200);old=self.state['revision']
        self.assertEqual(self.state['draft']['removable_missing_skills'][0]['skill_id'],70331)
        status,self.state=self.request('POST',path+'/drop-missing-skills',{'skill_ids':[70331]})
        self.assertEqual(status,200,self.state)
        self.assertEqual(self.state['draft']['forms'][0]['server']['SkillNumber_1'],0)
        self.assertEqual(self.request('POST',path+'/drop-missing-skills',{'skill_ids':[70331]}, {'X-Catalog-Revision':old})[0],409)


if __name__=='__main__':unittest.main()
