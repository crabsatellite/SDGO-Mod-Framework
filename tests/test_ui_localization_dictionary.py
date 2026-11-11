from pathlib import Path
import json
import tempfile
import unittest

from tools import build_ui_localization_dictionary as catalog


class UiPhraseCatalogTests(unittest.TestCase):
    def file(self, root, name, language, rows):
        path=root/name
        path.write_text(json.dumps({'schema_version':1,'language':language,'entries':rows}),encoding='utf-8')
        return path

    def test_generator_matches_authored_sources(self):
        root=Path(__file__).resolve().parents[1]/'mods/ui_localization'
        result=catalog.render(catalog.load(sorted((root/'translations').glob('*.json'))))
        self.assertEqual((root/'phrases.generated.h').read_bytes(),result)
        self.assertEqual((root/'acquisition.generated.h').read_bytes(),
                         catalog.render_acquisition(catalog.load_acquisition(root/'acquisition.json')))

    def test_acquisition_requires_exact_id_unit_and_bounded_text(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'acquisition.json'
            valid={'id':1,'unit_id':2,'source':'native','target':'中文','status':'ready'}
            for rows in [[valid,valid], [{**valid,'unit_id':0}], [{**valid,'target':'中'*200}],
                         [{**valid,'source':'Count %u','target':'数量 %s'}]]:
                path.write_text(json.dumps({'schema_version':1,'entries':rows}),encoding='utf-8')
                with self.assertRaises(ValueError):catalog.load_acquisition(path)

    def test_review_and_kept_labels_are_not_rewritten(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=self.file(Path(tmp),'phrases.json','en',[
                {'source':'Start','target':'开始','status':'ready'},
                {'source':'NPC','target':'NPC','status':'keep'},
                {'source':'Unclear','target':'未确认','status':'review'}])
            phrases,system=catalog.load([path])
            self.assertEqual([('Start','开始')],phrases)
            self.assertEqual([],system)

    def test_same_system_label_keeps_distinct_record_identities(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=self.file(Path(tmp),'system.json','system',[
                {'id':1,'source':'All','target':'全部','status':'ready'},
                {'id':2,'source':'All','target':'全部','status':'ready'}])
            phrases,system=catalog.load([path])
            self.assertEqual([],phrases)
            self.assertEqual([1,2],[identity for identity,_ in system])

    def test_changed_format_arguments_are_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=self.file(Path(tmp),'phrases.json','en',[
                {'source':'Count %u','target':'数量 %s','status':'ready'}])
            with self.assertRaisesRegex(ValueError,'format arguments'):
                catalog.load([path])

    def test_native_text_extent_and_conflicting_identity_are_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for rows in [
                [{'id':1,'source':'A','target':'中'*100,'status':'ready'}],
                [{'id':1,'source':'A','target':'甲','status':'ready'},
                 {'id':1,'source':'B','target':'乙','status':'ready'}]]:
                path=self.file(root,'system.json','system',rows)
                with self.assertRaises(ValueError):catalog.load([path])


if __name__=='__main__':unittest.main()
