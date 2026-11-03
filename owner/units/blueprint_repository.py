"""Guarded owner-scoped blueprint definitions inside the unit transaction."""
from __future__ import annotations

import copy
import hashlib
import json

from . import blueprints


RECIPE_COLUMNS=tuple(['Idx','OutUnitID']+[f'UnitID{i}' for i in range(1,8)]+
                     [f'UnitLevel{i}' for i in range(1,8)]+['MinUserLevel','RepayPoint'])
INFO_COLUMNS=('ItemID','Hash','OutUnitID')
LIST_COLUMNS=('ItemID','UnitID','UnitLv')


def schema():
    return {'tbl_MixTreeFile':tuple((name,'tinyint' if name.startswith('UnitLevel') else 'int',
        1 if name.startswith('UnitLevel') else 4,False) for name in RECIPE_COLUMNS),
        'tbl_MixTreeInfo':(('ItemID','int',4,False),('Hash','bigint',8,False),('OutUnitID','int',4,False)),
        'tbl_MixTreeList':(('ItemID','int',4,False),('UnitID','int',4,False),('UnitLv','tinyint',1,False))}


def load(cursor):
    from .repository import _object_exists,_select_rows,_index
    if not all(_object_exists(cursor,name) for name in schema()):return {}
    recipes=_index(_select_rows(cursor,'tbl_MixTreeFile',RECIPE_COLUMNS),'Idx','tbl_MixTreeFile')
    infos=_index(_select_rows(cursor,'tbl_MixTreeInfo',INFO_COLUMNS),'ItemID','tbl_MixTreeInfo')
    lists={}
    for row in _select_rows(cursor,'tbl_MixTreeList',LIST_COLUMNS):lists.setdefault(row['ItemID'],[]).append(row)
    return {'blueprints':recipes,'blueprint_infos':infos,'blueprint_materials':lists}


def material_hash(rows):
    return hashlib.sha256(json.dumps(sorted(rows,key=lambda r:(r['UnitID'],r['UnitLv'])),
        sort_keys=True,ensure_ascii=False,separators=(',',':')).encode('utf-8')).hexdigest()


def validate(declarations,*,root,unit_ids,catalog,ownership,family):
    from .repository import ITEM_COLUMNS,_row_hash
    if not isinstance(declarations,list) or len(declarations)>16:raise ValueError('设计图计划列表无效')
    result=[];seen=set();categories=set()
    for value in declarations:
        r=blueprints.normalize(value,root_unit_id=root)
        issues=blueprints.requirements(r,root_unit_id=root,unit_ids=unit_ids)
        if issues:raise ValueError(issues[0]['message'])
        # Disabled definitions stay present for existing inventory. A disabled
        # draft with missing fields cannot silently alter an owned definition.
        if not r['enabled']:
            r['enabled']=True
            issues=blueprints.requirements(r,root_unit_id=root,unit_ids=unit_ids)
            if issues:raise ValueError(issues[0]['message'])
            r['enabled']=False
        if not blueprints.RECIPE_FIRST<=r['id']<=blueprints.RECIPE_LAST:raise ValueError('目标设计图不在模块保留范围')
        if not blueprints.CATEGORY_FIRST<=r['item_category_id']<=blueprints.CATEGORY_LAST:raise ValueError('目标图纸类别不在模块保留范围')
        if r['id'] in seen or r['item_category_id'] in categories:raise ValueError('设计图或图纸类别重复')
        seen.add(r['id']);categories.add(r['item_category_id'])
        if not all(key in catalog for key in ('blueprints','blueprint_infos','blueprint_materials')):
            raise ValueError('设计图固定目录元数据缺失，不能登记')
        if r['recipe_template_id'] not in catalog['blueprints']:raise ValueError('服务端缺少所选配方模板')
        template=catalog['items'].get(r['item_template_id'])
        if not template or template['CatalogueID']//100000!=15 or template['TimeLimit']!=0:
            raise ValueError('请选择原生永久图纸物品模板')
        if template['RefIndex'] not in catalog['blueprints']:raise ValueError('所选图纸模板没有对应配方')
        item=copy.deepcopy(template);item.update(CatalogueID=r['item_category_id'],Name=r['internal_name'],RefIndex=r['id'])
        spec=copy.deepcopy(r);spec['enabled']=True
        rows=blueprints.server_rows(spec,root_unit_id=root)
        for kind,identity,values,existing,columns in (
            ('blueprint',r['id'],rows['recipe'],catalog['blueprints'],RECIPE_COLUMNS),
            ('blueprint_info',r['id'],rows['info'],catalog['blueprint_infos'],INFO_COLUMNS),
            ('item',r['item_category_id'],item,catalog['items'],ITEM_COLUMNS)):
            current=existing.get(identity);owned=ownership.get((kind,identity))
            if current is None:
                if owned:raise ValueError('设计图所有权存在但原生目录行缺失')
            elif not owned or owned[0]!=family or owned[1]!=_row_hash(current,columns):
                raise ValueError('设计图、派生记录或图纸物品被原生/其他作者占用或已外部修改')
        current_list=catalog['blueprint_materials'].get(r['id'],[]);owned=ownership.get(('blueprint_list',r['id']))
        if current_list:
            if not owned or owned[0]!=family or owned[1]!=material_hash(current_list):
                raise ValueError('设计图材料索引已在模块外修改或被占用')
        elif owned:raise ValueError('设计图材料账本存在但索引行缺失')
        result.append({'declaration':r,'item':item,**rows})
    return result


def validate_schema(cursor):
    for table,expected in schema().items():
        rows=cursor.execute('SELECT c.name,t.name,c.max_length,c.is_nullable FROM sys.columns c '
            'JOIN sys.types t ON c.user_type_id=t.user_type_id WHERE c.object_id=OBJECT_ID(?) ORDER BY c.column_id','dbo.'+table).fetchall()
        actual=tuple((r[0],r[1],r[2],bool(r[3])) for r in rows)
        if actual!=expected:raise ValueError('设计图表结构不匹配：'+table)


def apply_materials(cursor,rows,*,family,revision):
    from .repository import OWNER_KEY
    rid=rows[0]['ItemID'];current=[dict(zip(LIST_COLUMNS,r)) for r in cursor.execute(
        'SELECT ItemID,UnitID,UnitLv FROM dbo.tbl_MixTreeList WITH(UPDLOCK,HOLDLOCK) WHERE ItemID=?',rid).fetchall()]
    ledger=cursor.execute('SELECT OwnerKey,FamilyKey,OriginalJson,InstalledHash FROM dbo.sdgo_UnitImportDefinitions '
        'WITH(UPDLOCK,HOLDLOCK) WHERE RecordKind=? AND RecordID=?','blueprint_list',rid).fetchone()
    if current:
        if not ledger or ledger[0]!=OWNER_KEY or ledger[1]!=family or ledger[3]!=material_hash(current):
            raise ValueError('图纸材料索引的所有权或哈希不匹配')
    elif ledger:raise ValueError('图纸材料索引缺失，拒绝重建')
    if current:cursor.execute('DELETE FROM dbo.tbl_MixTreeList WHERE ItemID=?',rid)
    for row in rows:cursor.execute('INSERT dbo.tbl_MixTreeList(ItemID,UnitID,UnitLv) VALUES(?,?,?)',row['ItemID'],row['UnitID'],row['UnitLv'])
    actual=[dict(zip(LIST_COLUMNS,r)) for r in cursor.execute('SELECT ItemID,UnitID,UnitLv FROM dbo.tbl_MixTreeList WHERE ItemID=?',rid).fetchall()]
    digest=material_hash(rows)
    if material_hash(actual)!=digest:raise ValueError('图纸材料索引读回不同')
    if ledger:
        cursor.execute('UPDATE dbo.sdgo_UnitImportDefinitions SET RecordKey=?,Revision=?,InstalledHash=?,UpdatedAt=SYSUTCDATETIME() '
            'WHERE RecordKind=? AND RecordID=?',f'{family}/blueprint_list/{rid}',revision,digest,'blueprint_list',rid)
    else:
        cursor.execute('INSERT dbo.sdgo_UnitImportDefinitions(RecordKind,RecordID,OwnerKey,FamilyKey,RecordKey,Revision,OriginalJson,InstalledHash,UpdatedAt) '
            'VALUES(?,?,?,?,?,?,?,?,SYSUTCDATETIME())','blueprint_list',rid,OWNER_KEY,family,f'{family}/blueprint_list/{rid}',revision,'null',digest)
    return 'updated' if ledger else 'inserted',digest
