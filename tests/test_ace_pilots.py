import contextlib
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest

from owner.units.ace_pilots import catalog_from_projects,encode_catalog,normalize_catalog
from tools.export_unit_ace_pilots import main as export_main


ROOT=Path(__file__).resolve().parents[1]


class AcePilotCatalog(unittest.TestCase):
    def test_public_base_catalog_is_complete_unique_and_contains_reviewed_associations(self):
        catalog=normalize_catalog(json.loads((ROOT/'mods/ace_ai/base-pilots.json').read_text(encoding='utf-8')))
        self.assertEqual(1073,len(catalog['pilots']))
        roster=('\n'.join(str(row['unit_id']) for row in catalog['pilots'])+'\n').encode()
        self.assertEqual('5da00f55408475d4ea5e2bd300f4a4ca95d2b1c6067952b3c7f5c074399f4436',hashlib.sha256(roster).hexdigest())
        rows={row['unit_id']:row for row in catalog['pilots']}
        self.assertEqual('阿姆罗·雷',rows[10001]['display_name'])
        self.assertEqual('凯·西登',rows[10007]['display_name'])
        self.assertEqual('克莉丝汀娜·麦肯吉',rows[10011]['display_name'])
        self.assertEqual('夏亚·阿兹纳布尔',rows[11047]['display_name'])
        self.assertEqual('伊织诚＆岭司',rows[24700]['display_name'])
        self.assertEqual('里卡德·费里尼',rows[24713]['display_name'])
        self.assertEqual('里朋斯·阿尔马克',rows[22073]['display_name'])
        # Similar-looking variants must keep their actual pilot or a conservative role.
        self.assertEqual('特列斯·克休里纳达',rows[17038]['display_name'])
        self.assertEqual('杰克斯·马吉斯',rows[17024]['display_name'])
        self.assertEqual('托比亚·阿罗纳克斯',rows[21027]['display_name'])
        self.assertEqual('穆·拉·弗拉达',rows[15039]['display_name'])
        self.assertEqual('隆德·蜜纳·萨哈克',rows[15041]['display_name'])
        self.assertEqual('卡莲·约书亚',rows[10046]['display_name'])
        self.assertEqual('普雷亚·雷腓力',rows[15100]['display_name'])
        self.assertEqual('神圣军事同盟王牌驾驶员',rows[10047]['display_name'])
        self.assertEqual('联邦测试驾驶员',rows[10202]['display_name'])
        self.assertEqual('米哈伊尔·卡明斯基',rows[10206]['display_name'])
        self.assertEqual('卡嘉莉·尤拉·阿斯哈',rows[15031]['display_name'])
        self.assertEqual('联邦老兵',rows[11075]['display_name'])
        for row in catalog['pilots']:
            self.assertLessEqual(len(row['display_name'].encode('utf-16-le'))//2,64)
            self.assertNotRegex(row['display_name'],r'[<>]')
        self.assertIn('role',{row['kind'] for row in catalog['pilots']})
        self.assertIn('named',{row['kind'] for row in catalog['pilots']})
        self.assertFalse((ROOT/'mods/ace_ai/owner-pilots.json').exists())

    def test_optional_project_metadata_compiles_all_family_forms_into_owner_overlay(self):
        catalog=catalog_from_projects([{'root_unit_id':90001,'forms':[
            {'server':{'UnitNumber':90001}},{'server':{'UnitNumber':90002}}],
            'metadata':{'ace_pilot':{'display_name':'测试驾驶员','kind':'named'}}},
            {'root_unit_id':90003,'metadata':{}}])
        self.assertEqual([90001,90002],[row['unit_id'] for row in catalog['pilots']])
        self.assertEqual('测试驾驶员',catalog['pilots'][0]['display_name'])
        self.assertEqual(catalog,normalize_catalog(json.loads(encode_catalog(catalog))))

    def test_portable_project_definitions_compile_all_declared_forms(self):
        catalog=catalog_from_projects([{'protocol':'sdgo.unit-project','schema_version':1,
            'metadata':{'ace_pilot':{'display_name':'导入驾驶员','kind':'role'}},
            'definitions':{'schema_version':1,'root_unit_id':91001,
                'units':[{'UnitNumber':91001},{'UnitNumber':91002}]}}])
        self.assertEqual([91001,91002],[row['unit_id'] for row in catalog['pilots']])

    def test_export_tool_accepts_portable_projects_and_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);projects=root/'projects.json';output=root/'owner-pilots.json'
            projects.write_text(json.dumps([{'metadata':{'ace_pilot':{'display_name':'导出驾驶员','kind':'named'}},
                'definitions':{'root_unit_id':92001,'units':[{'UnitNumber':92001}]}}]),encoding='utf-8')
            with contextlib.redirect_stdout(io.StringIO()):export_main(['--projects',str(projects),'--out',str(output)])
            self.assertEqual(92001,json.loads(output.read_text(encoding='utf-8'))['pilots'][0]['unit_id'])
            with self.assertRaises(FileExistsError):export_main(['--projects',str(projects),'--out',str(output)])

    def test_overlay_rejects_missing_identity_and_conflicting_duplicate(self):
        with self.assertRaisesRegex(ValueError,'缺少机体编号'):
            catalog_from_projects([{'metadata':{'ace_pilot':{'display_name':'测试','kind':'role'}}}])
        with self.assertRaisesRegex(ValueError,'不同'):
            catalog_from_projects([
                {'root_unit_id':90001,'metadata':{'ace_pilot':{'display_name':'甲','kind':'named'}}},
                {'root_unit_id':90001,'metadata':{'ace_pilot':{'display_name':'乙','kind':'role'}}}])

    def test_catalog_rejects_unknown_fields_duplicate_ids_and_control_text(self):
        with self.assertRaises(ValueError):normalize_catalog({'schema_version':1,'pilots':[{
            'unit_id':1,'display_name':'甲','kind':'named','machine':'x'}]})
        with self.assertRaises(ValueError):normalize_catalog({'schema_version':1,'pilots':[
            {'unit_id':1,'display_name':'甲','kind':'named'},{'unit_id':1,'display_name':'甲','kind':'named'}]})
        with self.assertRaises(ValueError):normalize_catalog({'schema_version':1,'pilots':[{
            'unit_id':1,'display_name':'甲\n乙','kind':'named'}]})
        with self.assertRaises(ValueError):normalize_catalog({'schema_version':1,'pilots':[{
            'unit_id':1,'display_name':'<王牌>','kind':'named'}]})
        with self.assertRaises(ValueError):normalize_catalog({'schema_version':1,'pilots':[{
            'unit_id':1,'display_name':'😀'*33,'kind':'named'}]})


if __name__=='__main__':unittest.main()
