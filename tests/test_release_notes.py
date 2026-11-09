from pathlib import Path
import json,struct,tempfile,unittest,zipfile
from tools import release_notes as notes
from tools.game_test_release_demo import transition


def webp():
    def chunk(tag,data):return tag+struct.pack('<I',len(data))+data+(b'\0'if len(data)%2 else b'')
    frame=b'\0'*12+(500).to_bytes(3,'little')+b'\0'
    data=b'WEBP'+chunk(b'ANIM',b'\0'*6)+chunk(b'ANMF',frame)+chunk(b'ANMF',frame)
    return b'RIFF'+struct.pack('<I',len(data))+data


class ReleaseNotes(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name)
        docs=self.root/'docs';docs.mkdir();(docs/'demo.webp').write_bytes(webp())
        for name in('guide.md','guide.zh-CN.md'):(docs/name).write_text('Guide',encoding='utf-8')
        self.demo={'id':'example','path':'docs/demo.webp','since':'1.15.7','mods':['visible'],
                   'title_zh':'可见操作','title_en':'Visible action','guide_zh':'docs/guide.zh-CN.md','guide_en':'docs/guide.md'}
        self.registry={'schema_version':1,'demos':[self.demo]}
        self.manifest={'version':'1.15.7','channel':'stable','repository':'example/framework','source_commit':'a'*40}

    def test_notes_are_bilingual_and_link_only_assets_selected_for_the_package(self):
        body,assets=notes.render(self.root,self.manifest,{'visible'},self.registry)
        self.assertIn('## 中文',body);self.assertIn('## English',body)
        self.assertIn('/releases/download/v1.15.7/demo.webp',body)
        self.assertEqual(assets[0]['frames'],2);self.assertEqual(assets[0]['duration_ms'],1000)
        body,assets=notes.render(self.root,self.manifest,{'other'},self.registry)
        self.assertNotIn('demo.webp',body);self.assertEqual(assets,[])

    def test_future_demo_is_not_attached_to_an_older_release(self):
        self.manifest['version']='1.15.6';body,assets=notes.render(self.root,self.manifest,{'visible'},self.registry)
        self.assertNotIn('demo.webp',body);self.assertEqual(assets,[])

    def test_repository_escape_missing_language_and_bad_animation_fail_before_publication(self):
        for patch in({'path':'../outside.webp'},{'guide_zh':'../outside.md'},{'title_zh':''}):
            with self.subTest(patch=patch),self.assertRaises(ValueError):notes.select_demos(self.root,{'schema_version':1,'demos':[{**self.demo,**patch}]},'1.15.7',{'visible'})
        (self.root/'docs/demo.webp').write_bytes(webp()[:-1])
        with self.assertRaises(ValueError):notes.render(self.root,self.manifest,{'visible'},self.registry)

    def test_authored_notes_must_include_both_languages(self):
        p=self.root/'docs/releases/1.15.7.md';p.parent.mkdir();p.write_text('English only',encoding='utf-8')
        with self.assertRaises(ValueError):notes.render(self.root,self.manifest,{'visible'},self.registry)

    def test_owner_programmes_cannot_be_presented_as_public_framework(self):
        asset=self.root/'programme.zip'
        with zipfile.ZipFile(asset,'w')as z:
            z.writestr('release.json',json.dumps(self.manifest));z.writestr('client/sdgo/mods/private/mod.ini','[mod]\nid=private\ndistribution=owner\n')
        with self.assertRaises(ValueError):notes.package(asset)

    def test_recording_follows_real_sdk_case_events_and_rejects_foreign_runs(self):
        begin={'run_id':'mine','event':'case_begin','case':'pages.hangar.read','status':'running'}
        self.assertEqual(transition(begin,'mine','pages.hangar.read','pages.hangar.cancel'),'start')
        self.assertIsNone(transition(begin,'other','pages.hangar.read','pages.hangar.cancel'))
        self.assertIsNone(transition({**begin,'event':'case_start'},'mine','pages.hangar.read','pages.hangar.cancel'))
        end={**begin,'event':'case_end','case':'pages.hangar.cancel','status':'fail'}
        self.assertEqual(transition(end,'mine','pages.hangar.read','pages.hangar.cancel'),'stop')


if __name__=='__main__':unittest.main()
