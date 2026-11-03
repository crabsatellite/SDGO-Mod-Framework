import copy
import base64
import io
import json
from pathlib import Path
import tempfile
import unittest
import zipfile
from unittest.mock import patch
from types import SimpleNamespace

from manager import announcements, storage


def rule():
    return dict(id='notice_one', enabled=True, title='公告 <标题>', text='正文 & 内容\n第二行',
                surfaces=['login', 'game'], triggers=['enter', 'timer'], weekdays=list(range(7)),
                start_time='22:00', end_time='02:00', start_date='', end_date='',
                duration_seconds=5, interval_seconds=60, max_session=2, max_day=3,
                max_total=0, priority=10)


class AnnouncementsTest(unittest.TestCase):
    def policy(self):
        return {**copy.deepcopy(announcements.DEFAULT), 'rules': [rule()]}

    def test_reuses_the_only_registered_client_without_replacing_a_choice(self):
        from manager.workspace import Workspace
        source=Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);client=root/'client';client.mkdir();(client/'GOnline.exe').touch()
            workspace=Workspace(root/'manager',source/'mods');workspace.add_host(client)
            provider=announcements.Announcements(workspace)
            try:
                self.assertEqual(provider.describe()['config']['client'],str(client))
                provider.configure({'client':str(root/'explicit-choice')})
                self.assertEqual(provider.describe()['config']['client'],str(root/'explicit-choice'))
            finally:workspace.close()

    def test_compile_unicode_escape_and_overnight(self):
        ini, pages = announcements.compile_rules(self.policy())
        text = ini.decode('utf-16')
        self.assertIn('start=1320\r\nend=120', text)
        self.assertIn('surfaces=3\r\ntriggers=3', text)
        page = pages['notice_one.html'].decode('utf-8')
        self.assertIn('&lt;标题&gt;', page)
        self.assertIn('正文 &amp; 内容<br>第二行', page)

    def test_chinese_body_wraps_to_the_native_panel_width(self):
        value=self.policy();value['rules'][0]['text']='公'*960
        ini,_=announcements.compile_rules(value)
        self.assertIn('lines=48',ini.decode('utf-16'))
        self.assertEqual(announcements.lines('公'*42),['公'*20,'公'*20,'公'*2])

    def test_invalid_rules_fail_before_files_are_written(self):
        for key, value in [('id', '../escape'), ('start_time', '24:00'),
                           ('duration_seconds', 0), ('triggers', ['unknown']),
                           ('weekdays', [True]), ('text', 'x\0y'),
                           ('max_day', -1), ('end_date', '2026-02-30')]:
            with self.subTest(key=key):
                policy = self.policy();policy['rules'][0][key] = value
                with self.assertRaises(ValueError):announcements.compile_rules(policy)

    def test_revision_and_runtime_commit(self):
        with tempfile.TemporaryDirectory() as temp:
            client = Path(temp);(client/'GOnline.exe').touch()
            target=client/'sdgo/mods/announcements';target.mkdir(parents=True)
            (target/'announcements.dll').touch()
            workspace=SimpleNamespace(settings={'modules': {'announcements': {'client': str(client)}}})
            provider=announcements.Announcements(workspace)
            old=provider.request('GET','/api/announcements',{}, {})
            body={'revision':old['revision'], 'policy':self.policy()}
            saved=provider.request('POST','/api/announcements',body,{})
            with self.assertRaises(storage.Conflict):provider.request('POST','/api/announcements',body,{})
            self.assertEqual(json.loads((target/'rules.json').read_bytes()),saved['policy'])
            text=(target/'runtime.ini').read_bytes().decode('utf-16')
            page=next(line.split('=',1)[1] for line in text.splitlines() if line.startswith('page='))
            self.assertTrue((target/'pages'/page).is_file())
            self.assertNotIn('local-state',text)

    def test_export_import_keeps_pages_and_receiver_counts(self):
        from manager.workspace import Workspace
        from manager.packages import import_client
        source=Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);client=root/'sender';client.mkdir();(client/'GOnline.exe').touch()
            target=client/'sdgo/mods/announcements';target.mkdir(parents=True)
            for name in ('mod.ini','manager.json','symbols.fragment.ini'):
                (target/name).write_bytes((source/'mods/announcements'/name).read_bytes())
            (target/'announcements.dll').write_bytes(b'public test module fixture')
            (target/'rules.json').write_bytes(storage.json_bytes(self.policy()))
            workspace=SimpleNamespace(settings={'modules':{'announcements':{'client':str(client)}}})
            package=announcements.Announcements(workspace).request('GET','/api/announcements/export',{},{}).data
            with zipfile.ZipFile(io.BytesIO(package)) as archive:
                names=archive.namelist()
                self.assertTrue(any(n.endswith('.html') for n in names))
                self.assertFalse(any('local-state' in n or 'counts' in n for n in names))
            receiver=root/'receiver';receiver.mkdir();(receiver/'GOnline.exe').touch()
            counts=receiver/'sdgo/local-state/announcement-counts.ini';counts.parent.mkdir(parents=True)
            counts.write_bytes(b'keep recipient counts')
            owner=Workspace(root/'manager',source/'mods');owner.add_host(receiver)
            try:
                with patch('manager.packages._running',return_value=False):
                    import_client(owner,owner.settings['hosts'][0]['id'],base64.b64encode(package).decode())
                self.assertEqual(counts.read_bytes(),b'keep recipient counts')
                self.assertTrue(list((receiver/'sdgo/mods/announcements/pages').glob('*.html')))
            finally:owner.close()


if __name__ == '__main__': unittest.main()
