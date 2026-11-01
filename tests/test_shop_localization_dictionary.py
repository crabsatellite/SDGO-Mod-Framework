import importlib.util
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('shop_word_compiler',ROOT/'tools/build_shop_localization_dictionary.py')
compiler=importlib.util.module_from_spec(spec);spec.loader.exec_module(compiler)


class WordCompiler(unittest.TestCase):
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


if __name__=='__main__':unittest.main()
