import http.client
import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import Mock, patch
from contextlib import nullcontext
from manager.calendar_gifts import CalendarGiftService
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
        self.assertEqual(len(state['festivals']),14)
        self.assertEqual(next(e for e in state['festivals'] if e['value']=='christmas')['month_day'],'12-25')
        service=self.call('GET','/api/services/calendar.gifts')[1]
        self.assertEqual(service['owner'],'manager');self.assertFalse(service['desired']);self.assertFalse(service['running'])

    def test_old_policy_can_add_international_presets_through_existing_revision_api(self):
        from owner.calendar_gifts import model
        state=self.call('GET',PREFIX)[1];old=state['policy']
        old['events']=[e for e in old['events'] if e['id'] not in model.ADDITIONAL_FESTIVALS]
        old['events'][0]['enabled']=False
        status,saved=self.call('POST',PREFIX+'/policy',{'value':old,'revision':state['policy_revision']})
        self.assertEqual(status,200);self.assertEqual(len(saved['missing_festivals']),7)
        new=saved['policy'];new['events']+=saved['missing_festivals']
        with patch('manager.database.connect',side_effect=AssertionError('saving presets must not open SQL')):
            status,result=self.call('POST',PREFIX+'/policy',{'value':new,'revision':saved['policy_revision']})
        self.assertEqual(status,200);self.assertEqual(len(result['policy']['events']),14)
        self.assertEqual(result['missing_festivals'],[]);self.assertFalse(result['policy']['events'][0]['enabled'])

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

    def test_legacy_birthday_backup_import_ignores_email_and_preserves_registered_dates(self):
        backup=self.root/'old-private-birthdays.json'
        backup.write_text(json.dumps([
            {'AccountNumber':73491,'Birthday':'0304','Email':'never-export@example.invalid'},
            {'AccountNumber':73492,'Birthday':'0229'},
            {'AccountNumber':True,'Birthday':'0101'},
            {'AccountNumber':73493,'Birthday':'0230'}]),encoding='utf-8')
        self.provider.configure({'legacy_birthdays':str(backup)})
        initial=self.call('GET',PREFIX)[1]
        people=initial['private'];people['birthdays']=[{'account_id':73491,'month_day':'05-06','enabled':True}]
        self.call('POST',PREFIX+'/private',{'value':people,'revision':initial['private_revision']})
        initial=self.call('GET',PREFIX)[1]
        with patch('manager.database.connect',side_effect=AssertionError('backup import must not open SQL')):
            status,result=self.call('POST',PREFIX+'/import-birthdays',{'revision':initial['private_revision']})
        self.assertEqual(status,200);self.assertEqual(result['imported'],1)
        self.assertEqual(result['private']['birthdays'],[
            {'account_id':73491,'month_day':'05-06','enabled':True},
            {'account_id':73492,'month_day':'02-29','enabled':True}])
        self.assertNotIn('never-export',json.dumps(result))
        self.assertNotIn('never-export',self.provider.get_engine().people_file.read_text(encoding='utf-8'))


class CalendarWorkerContracts(unittest.TestCase):
    def run_batch(self, accounts, pending=False):
        service=CalendarGiftService()
        repository=Mock()
        repository.accounts.side_effect=accounts
        repository.deliver.return_value={'delivery_count':1}
        engine=Mock()
        engine.policy.return_value={'enabled':True}
        engine.window.return_value=(None,None)
        engine.occurrences.return_value=[{'key':str(i)} for i in range(len(accounts))]
        # Finish after one complete batch without a timing-dependent sleep.
        service.stop_event.wait=Mock(side_effect=lambda _:service.stop_event.set())
        with tempfile.TemporaryDirectory() as directory:
            engine.root=Path(directory)
            with patch('manager.database.connect') as connect, \
                 patch('owner.calendar_gifts.repository.Repository',return_value=repository), \
                 patch('manager.operations.pending',return_value=pending), \
                 patch('manager.operations.target_key',return_value='calendar-worker-fixture'), \
                 patch('manager.target_locks.lease',side_effect=lambda _:nullcontext()), \
                 patch('threading.excepthook') as thread_errors:
                service.start('selected-local-server.ini',engine)
                service.thread.join(timeout=5)
                self.assertFalse(service.thread.is_alive())
                thread_errors.assert_not_called()
                self.assertEqual(service.error,'')
                connect.return_value.close.assert_called_once()
        return service,repository

    def test_empty_recipient_list_does_not_stop_later_occurrences(self):
        service,repository=self.run_batch([[],[101,102,103]])
        self.assertEqual(repository.accounts.call_count,2)
        self.assertEqual([call.args[1] for call in repository.deliver.call_args_list],[101,102,103])
        self.assertEqual(service.delivered,3)

    def test_empty_batch_does_not_attempt_delivery(self):
        service,repository=self.run_batch([[]])
        repository.deliver.assert_not_called()
        self.assertEqual(service.delivered,0)

    def test_pending_catalog_defers_the_entire_batch_before_delivery(self):
        service,repository=self.run_batch([[101,102],[103]],pending=True)
        repository.deliver.assert_not_called()
        self.assertEqual(repository.accounts.call_count,1)
        self.assertEqual(service.delivered,0)

    def test_batch_budget_defers_remaining_accounts_and_occurrences(self):
        service,repository=self.run_batch([list(range(205)),[300]])
        self.assertEqual([call.args[1] for call in repository.deliver.call_args_list],list(range(200)))
        self.assertEqual(repository.accounts.call_count,1)
        self.assertEqual(service.delivered,200)


if __name__=='__main__':unittest.main()
