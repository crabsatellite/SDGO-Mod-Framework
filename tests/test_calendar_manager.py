import http.client
import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch
from manager.workspace import Workspace
from manager.providers import attach
from manager.server import make_server

ROOT=Path(__file__).resolve().parents[1]
PREFIX='/api/providers/calendar_gifts/api/calendar-gifts'


class CalendarManagerContracts(unittest.TestCase):
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory();self.root=Path(self.temporary.name)
        self.workspace=Workspace(self.root/'manager',ROOT/'mods');attach(self.workspace)
        self.provider=self.workspace.providers['calendar_gifts']
        self.provider.configure({'workspace':str(self.root/'calendar'),'server_ini':'selected-local-server.ini'})
        self.server=make_server(self.workspace);self.thread=threading.Thread(target=self.server.serve_forever,daemon=True);self.thread.start()
        self.token='';self.token=self.call('GET','/api/state')[1]['session_token']

    def tearDown(self):
        self.workspace.services.close();self.server.shutdown();self.server.server_close();self.thread.join();self.temporary.cleanup()

    def call(self,method,path,body=None,token=None):
        connection=http.client.HTTPConnection('127.0.0.1',self.server.server_port,timeout=10)
        headers={'Content-Type':'application/json','Origin':'http://127.0.0.1:'+str(self.server.server_port),'X-ModManager-Token':self.token if token is None else token}
        connection.request(method,path,None if body is None else json.dumps(body),headers)
        response=connection.getresponse();raw=response.read();status=response.status;connection.close()
        return status,json.loads(raw) if raw else None

    def test_registered_defaults_do_not_start_or_connect_a_worker(self):
        state=self.call('GET',PREFIX)[1]
        self.assertFalse(state['policy']['enabled']);self.assertEqual(state['private']['birthdays'],[])
        service=self.call('GET','/api/services/calendar.gifts')[1]
        self.assertEqual(service['owner'],'manager');self.assertFalse(service['desired']);self.assertFalse(service['running'])

    def test_authenticated_revision_writes_are_separate_and_stale_edits_rejected(self):
        state=self.call('GET',PREFIX)[1];policy=state['policy'];policy['catchup_days']=3
        status,_=self.call('POST',PREFIX+'/policy',{'value':policy,'revision':state['policy_revision']},token='foreign')
        self.assertEqual(status,403)
        status,first=self.call('POST',PREFIX+'/policy',{'value':policy,'revision':state['policy_revision']})
        self.assertEqual(status,200)
        self.assertEqual(self.call('POST',PREFIX+'/policy',{'value':policy,'revision':state['policy_revision']})[0],409)
        # A private save uses its independent revision, without overwriting policy.
        people={'schema':1,'birthdays':[{'account_id':73491,'month_day':'11-03','enabled':True}],'groups':{}}
        self.assertEqual(self.call('POST',PREFIX+'/private',{'value':people,'revision':state['private_revision']})[0],200)
        self.assertEqual(self.call('GET',PREFIX)[1]['policy']['catchup_days'],3)

    def test_public_export_and_ai_context_never_load_private_lists(self):
        state=self.call('GET',PREFIX)[1]
        people={'schema':1,'birthdays':[{'account_id':73491,'month_day':'11-03','enabled':True}],'groups':{'secret_group':[73491]}}
        self.call('POST',PREFIX+'/private',{'value':people,'revision':state['private_revision']})
        policy=state['policy'];policy['events'][0]['group']='secret_group'
        self.call('POST',PREFIX+'/policy',{'value':policy,'revision':state['policy_revision']})
        exported=self.call('GET',PREFIX+'/export')[1];raw=json.dumps(exported)
        self.assertFalse(exported['enabled']);self.assertNotIn('73491',raw);self.assertNotIn('11-03',raw);self.assertNotIn('secret_group',raw)
        with patch.object(self.provider.get_engine(),'people',side_effect=AssertionError('AI must not read private birthdays')):
            status,context=self.call('POST','/api/ai-context',{'provider':'calendar_gifts','kind':'configure'})
        self.assertEqual(status,200);self.assertNotIn('73491',context['text']);self.assertIn('calendar.gifts',context['text'])

    def test_disabled_policy_never_connects_and_keeps_lifecycle_in_supervisor(self):
        with patch('manager.database.connect',side_effect=AssertionError('disabled policy must not open SQL')):
            status,_=self.call('POST','/api/services/calendar.gifts',{'action':'start'})
        self.assertEqual(status,400)
        state=self.call('GET','/api/services/calendar.gifts')[1]
        self.assertFalse(state['running']);self.assertTrue(state['desired']);self.assertTrue(state['error'])
        with self.assertRaises(ValueError):self.provider.configure({'server_ini':'other.ini'})
        self.call('POST','/api/services/calendar.gifts',{'action':'stop'})
        self.provider.configure({'server_ini':'other.ini'})
        self.assertEqual(self.call('POST',PREFIX+'/start',{})[0],400)

    def test_public_policy_cannot_accept_private_rows_or_reserved_event_identity(self):
        state=self.call('GET',PREFIX)[1];policy=state['policy'];policy['birthdays']=[{'account_id':1,'month_day':'01-01'}]
        self.assertEqual(self.call('POST',PREFIX+'/policy',{'value':policy,'revision':state['policy_revision']})[0],400)
        del policy['birthdays'];policy['events'][0]['id']='birthday'
        self.assertEqual(self.call('POST',PREFIX+'/policy',{'value':policy,'revision':state['policy_revision']})[0],400)

    def test_private_workspace_cannot_be_inside_distributable_mod_tree(self):
        server=self.root/'server/WorldServer';server.mkdir(parents=True)
        with self.assertRaisesRegex(ValueError,'私人'):
            self.provider.configure({'server_ini':str(server/'WorldServer.ini'),'workspace':str(server/'sdgo/mods/calendar_gifts/private')})


if __name__=='__main__':unittest.main()
