import importlib.util
import configparser
import json
import re
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('shop_word_compiler',ROOT/'tools/build_shop_localization_dictionary.py')
compiler=importlib.util.module_from_spec(spec);spec.loader.exec_module(compiler)


class WordCompiler(unittest.TestCase):
    def test_shipped_stock_bank_has_no_untranslated_Thai_or_Korean_targets(self):
        parser=configparser.ConfigParser(interpolation=None)
        parser.read(ROOT/'mods/shop_localization/zh-CN.ini',encoding='utf-8')
        self.assertGreater(len(parser.sections()),10000)
        for section in parser.sections():
            self.assertEqual(set(parser[section]),{'kind','source','text'})
            self.assertNotRegex(
                parser[section]['text'],
                r'[\u0e00-\u0e7f\u1100-\u11ff\u3130-\u318f'
                r'\ua960-\ua97f\uac00-\ud7af\ud7b0-\ud7ff]',
            )
        coverage=json.loads((ROOT/'docs/investigations/shop-localization-coverage.json').read_text(encoding='utf-8'))
        self.assertEqual(coverage['missing_known_names'],0)
        self.assertEqual(coverage['missing_known_prose'],0)
        self.assertEqual(coverage['native_category_names'],3299)
    def terms(self):
        return {'schema_version':1,'unit_records':[
            {'unit_id':10001,'chinese':{'value':'高达'},'original_0011':{'slots':['GUNDAM']},'alias_0237_slot2':'GUNDAM'},
            {'unit_id':10002,'chinese':{'value':'高达（变形后）'},'original_0011':{'slots':['GUNDAM']},'alias_0237_slot2':'GUNDAM'}],
            'categories':[{'catalogue_id':1120001,'kind':11,'ref_index':10001,'source':{'name':'Gundam (Sale)'},'resolved_reference':{'type':'unit','chinese_name':'高达'}},
                          {'catalogue_id':1520001,'kind':15,'ref_index':200001,'source':{'name':'GUNDAM Plan Mix'},'resolved_reference':{'type':'blueprint','target_unit_chinese_name':'高达'}}]}

    def test_native_reference_join_keeps_blueprint_semantics(self):
        words,report=compiler.compile_words(self.terms())
        self.assertEqual(words['GUNDAM Plan Mix']['text'],'高达设计图')
        self.assertEqual(words['Gundam (Sale)']['text'],'高达（销售）')
        self.assertEqual(words['GUNDAM']['text'],'高达')
        self.assertNotIn('WorldDirectory',str(words))

    def test_unreferenced_transformation_alias_cannot_override_shop_unit(self):
        words,_=compiler.compile_words(self.terms())
        self.assertEqual(words['GUNDAM']['text'],'高达')

    def test_ambiguous_specific_variants_are_left_untranslated(self):
        self.assertIsNone(compiler.choose('GUNDAM (CUSTOM)',[(100,'高达（甲）','name'),(100,'高达（乙）','name')]))
        self.assertEqual(compiler.choose('GUNDAM',[(100,'高达','name'),(100,'高达（变形后）','name')]),('高达','name'))

    def test_dictionary_escapes_preserve_original_lines_and_paths(self):
        output=compiler.encode({'Name\\path\nline':{'text':'名称\n第二行','kind':'description'}}).decode('utf-8')
        self.assertIn('Source=Name\\\\path\\nline',output)
        self.assertIn('Text=名称\\n第二行',output)
        self.assertEqual(compiler.escape('truncated '),'truncated\\s')

    def test_literal_line_markers_gain_exact_runtime_lf_aliases(self):
        words,report=compiler.compile_words(
            self.terms(),
            extra={'description':{'บรรทัดหนึ่ง\\nบรรทัดสอง':'第一行\\n第二行'}},
        )
        self.assertEqual(words['บรรทัดหนึ่ง\\nบรรทัดสอง'],words['บรรทัดหนึ่ง\nบรรทัดสอง'])
        self.assertEqual(report['runtime_newline_aliases'],1)
        output=compiler.encode(words).decode('utf-8')
        self.assertIn('Source=บรรทัดหนึ่ง\\\\nบรรทัดสอง',output)
        self.assertIn('Source=บรรทัดหนึ่ง\\nบรรทัดสอง',output)

    def test_runtime_lf_alias_refuses_a_different_authored_translation(self):
        with self.assertRaisesRegex(ValueError,'Conflicting runtime newline alias'):
            compiler.compile_words(
                self.terms(),
                extra={'description':{
                    'บรรทัดหนึ่ง\\nบรรทัดสอง':'第一行',
                    'บรรทัดหนึ่ง\nบรรทัดสอง':'不同译文',
                }},
            )


if __name__=='__main__':unittest.main()
