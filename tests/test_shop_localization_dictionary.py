import importlib.util
import configparser
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
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
            if parser[section]['kind']=='description':
                self.assertNotIn('\\\\n',parser[section]['text'])
        coverage=json.loads((ROOT/'docs/investigations/shop-localization-coverage.json').read_text(encoding='utf-8'))
        self.assertEqual(coverage['missing_known_names'],0)
        self.assertEqual(coverage['missing_known_prose'],0)
        self.assertEqual(coverage['native_category_names'],3299)

    def test_audited_long_native_slots_have_exact_runtime_coverage(self):
        thai=(
            'รูปแบบที่ซ่อมแซมและเปลี่ยนแปลงของ DYNAMES '
            'ที่ถูกทำลายจากการต่อสู้กับ United Nations Army (国連軍)'
        )
        authored=json.loads((
            ROOT/'mods/shop_localization/translations/prose-native-gaps.json'
        ).read_text(encoding='utf-8'))
        self.assertEqual(
            authored,
            {
                'schema_version':1,
                'description':{
                    thai:'在与联合国军的战斗中被摧毁的力天使高达的修复改造型。',
                },
            },
        )

        parser=configparser.ConfigParser(interpolation=None)
        parser.read(ROOT/'mods/shop_localization/zh-CN.ini',encoding='utf-8')
        generated={}
        for section in parser.sections():
            source=parser[section]['source']
            self.assertNotIn(source,generated)
            generated[source]=(parser[section]['kind'],parser[section]['text'])

        # One Thai description had no exact dictionary key. The other eleven
        # source strings occur twice across native description/usage slots and
        # already share the exact product-name translation used by Lookup.
        expected={
            thai:('description','在与联合国军的战斗中被摧毁的力天使高达的修复改造型。'),
            'Happy Newyear 2012  Festival':('name','2012新年庆典'),
            'Happy Newyear 2012  Festival  Key':('name','2012新年庆典钥匙'),
            'Valentine 2012  Festival  Key':('name','2012情人节庆典钥匙'),
            'Celebrete 1 year Anniversery Key':('name','1周年庆钥匙'),
            'Celebrete 1 year Anniversery Box':('name','1周年庆礼盒'),
            'Harvest moon festival Key':('name','中秋节钥匙'),
            'Harvest moon festival Box':('name','中秋节礼盒'),
            "Funny 's Capsule Point Hammer":('name','趣味扭蛋点数锤'),
            'Celebrete 3 year Anniversery Hammer':('name','3周年庆锤'),
            'Celebrete 3 year Anniversery Capsule':('name','3周年庆扭蛋'),
            "New Year 2015 's Capsule Hammer":('name','2015新年扭蛋锤'),
        }
        self.assertEqual(sum(1 if source==thai else 2 for source in expected),23)
        for source,value in expected.items():
            self.assertEqual(generated.get(source),value)

    def test_stock_native_unit_aliases_are_authored_and_generated(self):
        authored=json.loads((
            ROOT/'mods/shop_localization/translations/stock-native-unit-names.json'
        ).read_text(encoding='utf-8'))
        self.assertEqual(authored['schema_version'],1)
        self.assertEqual(
            authored['comment'],
            'Original client unit-label aliases with one unambiguous Chinese '
            'display name. Ambiguous and unloaded sources are intentionally omitted.',
        )
        names=authored['name']
        self.assertEqual(len(names),432)
        self.assertEqual(
            names['ZAKU PHANTOM (SLASH WIZARD) (YZAK CUSTOM)'],
            '斩击型扎古幻影（伊扎克专用）',
        )
        self.assertEqual(
            names['BUSTER GUNDAM/DUEL GUNDAM ASSAULTSHROUD'],
            '暴风高达/决斗高达（突击护甲）',
        )

        parser=configparser.ConfigParser(interpolation=None)
        parser.read(ROOT/'mods/shop_localization/zh-CN.ini',encoding='utf-8')
        generated={}
        for section in parser.sections():
            source=parser[section]['source']
            self.assertNotIn(source,generated)
            generated[source]=(parser[section]['kind'],parser[section]['text'])
        for source,target in names.items():
            self.assertEqual(
                generated.get(compiler.escape(source)),
                ('name',compiler.escape(target)),
            )

        # Sources with conflicting stock meanings, unloaded-only sources and
        # custom-content aliases stay outside the global runtime dictionary.
        excluded=(
            'FULL ARMOR HYAKU SHIKI CUSTOM',
            'GUNDAM KYRIOS (TAIL UNIT)',
            'Homming Missile Pod (M7E)',
            '마라사이',
            'Big Zam (T1) - Stand',
            'Big Zam (T2) - Invincible',
            'Big Zam (T3) - Weak',
            'Twin GN Drive System',
            'Core Change',
            '自由敢达（HiMat模式）（武装变更后）변경 후) 스페셜',
            '건담 AGE-FX (버스터 모드) 特殊',
            'GN SwordIV (Rifle Mode)',
            'GN Gunblade Twin Edge',
        )
        for source in excluded:
            self.assertNotIn(source,names)
            self.assertNotIn(compiler.escape(source),generated)

    def test_room_title_fixed_notices_are_all_authored_and_generated(self):
        source=(ROOT/'mods/room_titles/room_titles.cpp').read_text(encoding='utf-8')
        prepare=source[source.index('bool Prepare('):source.index('\nvoid Submit(',source.index('bool Prepare('))]
        fixed=set(re.findall(r'ExplainFixed\(L"([^"\\]*(?:\\.[^"\\]*)*)"\)',source))
        fixed.update(re.findall(r'L"([^"\\]*(?:\\.[^"\\]*)*)"',prepare))
        self.assertGreater(len(fixed),10)

        authored=json.loads((
            ROOT/'mods/shop_localization/translations/mod-ui.json'
        ).read_text(encoding='utf-8'))['option']
        generated=configparser.ConfigParser(interpolation=None)
        generated.read(ROOT/'mods/shop_localization/zh-CN.ini',encoding='utf-8')
        generated_sources={
            generated[section]['source']:generated[section]['text']
            for section in generated.sections()
        }
        for notice in fixed:
            key='@ui:'+notice
            self.assertIn(key,authored)
            self.assertEqual(generated_sources.get(key),authored[key])

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

    def test_reviewed_orphan_unit_aliases_require_exact_native_identity(self):
        terms=self.terms()
        terms['unit_records'].append({
            'unit_id':10003,
            'chinese':{'value':'训练用MS-A'},
            'original_0011':{'slots':['Training MS-A','Training MS-A','Training MS-A']},
            'alias_0237_slot2':'TRAINING MS-A',
        })
        stable={'10003':'训练用MS-A'}
        reviewed={'10003':['Training MS-A','TRAINING MS-A']}
        words,_=compiler.compile_words(
            terms,
            unit_names=stable,
            reviewed_orphan_aliases=reviewed,
        )
        self.assertEqual(words['Training MS-A']['text'],'训练用MS-A')
        self.assertEqual(words['TRAINING MS-A']['text'],'训练用MS-A')
        self.assertEqual(words['@unit:10003']['text'],'训练用MS-A')
        with self.assertRaisesRegex(ValueError,'not native for unit 10003'):
            compiler.compile_words(
                terms,
                unit_names=stable,
                reviewed_orphan_aliases={'10003':['TRAINING MS-B']},
            )

    def test_shipped_training_preview_aliases_are_pinned_to_unit_10003(self):
        bank=json.loads((ROOT/'mods/shop_localization/unit-names.json').read_text(encoding='utf-8'))
        self.assertEqual(
            bank['reviewed_orphan_aliases'],
            {'10003':['Training MS-A','TRAINING MS-A']},
        )
        parser=configparser.ConfigParser(interpolation=None)
        parser.read(ROOT/'mods/shop_localization/zh-CN.ini',encoding='utf-8')
        by_source={parser[section]['source']:parser[section]['text'] for section in parser.sections()}
        self.assertEqual(by_source['Training MS-A'],'训练用MS-A')
        self.assertEqual(by_source['TRAINING MS-A'],'训练用MS-A')

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
        self.assertEqual(words['บรรทัดหนึ่ง\\nบรรทัดสอง']['text'],'第一行\n第二行')
        self.assertEqual(report['runtime_newline_aliases'],1)
        output=compiler.encode(words).decode('utf-8')
        self.assertIn('Source=บรรทัดหนึ่ง\\\\nบรรทัดสอง',output)
        self.assertIn('Source=บรรทัดหนึ่ง\\nบรรทัดสอง',output)
        self.assertIn('Text=第一行\\n第二行',output)
        self.assertNotIn('Text=第一行\\\\n第二行',output)

    def test_literal_line_marker_normalization_is_description_target_only(self):
        words,_=compiler.compile_words(
            self.terms(),
            extra={
                'name':{'Name\\path':'名称\\new'},
                'description':{'Description\\path':'第一行\\n第二行'},
            },
        )
        self.assertIn('Name\\path',words)
        self.assertIn('Description\\path',words)
        self.assertEqual(words['Name\\path']['text'],'名称\\new')
        self.assertEqual(words['Description\\path']['text'],'第一行\n第二行')
        output=compiler.encode(words).decode('utf-8')
        self.assertIn('Source=Name\\\\path',output)
        self.assertIn('Text=名称\\\\new',output)

    def test_real_cpp_parser_expands_encoded_description_linefeed(self):
        compiler_path=next((
            shutil.which(candidate)
            for candidate in (os.environ.get('CXX'),'c++','clang++','g++','cl')
            if candidate
        ),None)
        self.assertIsNotNone(compiler_path,'A C++ compiler is required for the dictionary parser regression')
        words,_=compiler.compile_words(
            self.terms(),
            extra={'description':{'line one\\nline two':'first line\\nsecond line'}},
        )
        with tempfile.TemporaryDirectory() as directory:
            directory=Path(directory)
            dictionary=directory/'dictionary.ini'
            executable=directory/('parse-test.exe' if os.name=='nt' else 'parse-test')
            dictionary.write_bytes(compiler.encode(words))
            source=ROOT/'tests/shop_localization_dictionary_parse_test.cpp'
            if Path(compiler_path).name.lower() in ('cl','cl.exe'):
                command=[
                    compiler_path,
                    '/nologo',
                    '/std:c++17',
                    '/W4',
                    '/EHsc',
                    '/I'+str(ROOT),
                    str(source),
                    '/Fe:'+str(executable),
                ]
            else:
                command=[
                    compiler_path,
                    '-std=c++17',
                    '-Wall',
                    '-Wextra',
                    '-pedantic',
                    '-I',
                    str(ROOT),
                    str(source),
                    '-o',
                    str(executable),
                ]
            subprocess.run(
                command,
                check=True,
                capture_output=True,
                text=True,
            )
            for input_file in (dictionary,ROOT/'mods/shop_localization/zh-CN.ini'):
                subprocess.run(
                    [str(executable),str(input_file)],
                    check=True,
                    capture_output=True,
                    text=True,
                )

    def test_nahal_runtime_composite_has_only_exact_raw_and_lf_keys(self):
        bank=json.loads((
            ROOT/'mods/shop_localization/translations/composite-battleship.json'
        ).read_text(encoding='utf-8'))
        self.assertEqual(bank['schema_version'],1)
        self.assertEqual(len(bank['description']),1)
        source,target=next(iter(bank['description'].items()))
        runtime_source=source.replace('\\n','\n')
        self.assertEqual(source.count('\\n'),5)
        self.assertEqual(runtime_source.count('\n'),5)
        self.assertEqual(len(runtime_source.encode('utf-16-le'))//2,348)
        self.assertEqual(
            hashlib.sha256(runtime_source.encode('utf-16-le')).hexdigest(),
            '118afdbbcc808f858a88fd485da1e16cf78ae6c0cae01308bc9375814f5dc4f4',
        )
        self.assertIn('原文截断',target)
        words,report=compiler.compile_words(
            self.terms(),
            extra={'description':{source:target}},
        )
        self.assertEqual(words[source],words[runtime_source])
        self.assertEqual(words[source]['text'].count('\n'),6)
        self.assertNotIn('\\n',words[source]['text'])
        self.assertEqual(report['runtime_newline_aliases'],1)

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
