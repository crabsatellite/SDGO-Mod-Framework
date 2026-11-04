"""Target GOnline kind-0x21 layout, preserving the donor's unknown bytes.

0029.gdt: 1197-byte records. Dword field widths and material count 3..7
are bound to the target's native preview/use consumers; this is not a
universal Union layout claim and does not enable registration by itself.
"""
from __future__ import annotations

import hashlib
import struct

from . import blueprints,codec


KIND=0x21
SIZE=1197
COUNT_OFFSET=105
OUTPUT_OFFSET=229
UNIT_OFFSET=233
LEVEL_OFFSET=261
MINIMUM_PLAYER_OFFSET=1189
REPAY_OFFSET=1193
RECIPE_SPRITE_OFFSET=169
PREVIEW_SPRITE_OFFSET=289


def decode(raw):
    identity=codec._validate_common(raw,SIZE,'blueprint record')
    name=codec._utf16(raw,5,100,'blueprint name',required=True)
    count=struct.unpack_from('<I',raw,COUNT_OFFSET)[0]
    if not 3<=count<=7:raise ValueError('当前目标设计图原生使用入口只支持3..7个材料')
    output=struct.unpack_from('<I',raw,OUTPUT_OFFSET)[0]
    if not 0<output<=codec.MAX_NATIVE_ID:raise ValueError('设计图产物编号无效')
    units=list(struct.unpack_from('<7I',raw,UNIT_OFFSET))
    levels=list(struct.unpack_from('<7I',raw,LEVEL_OFFSET))
    for uid,level in zip(units[:count],levels[:count]):
        if not 0<uid<=codec.MAX_NATIVE_ID or not 1<=level<=255:
            raise ValueError('设计图有效材料或等级超出目标范围')
    if any(units[count:]):raise ValueError('设计图材料数量与非空槽位不一致')
    if any(level>255 for level in levels[count:]):raise ValueError('设计图闲置等级字段超出已验证范围')
    if len(set(units[:count]))!=count:raise ValueError('当前设计图不支持重复材料编号')
    minimum=struct.unpack_from('<I',raw,MINIMUM_PLAYER_OFFSET)[0]
    repay=struct.unpack_from('<I',raw,REPAY_OFFSET)[0]
    if minimum>255 or repay>2_147_483_647:raise ValueError('设计图目标服务端数值超出范围')
    return {'id':identity,'name':name,'material_count':count,'output_unit_id':output,
        'materials':[{'unit_id':uid,'minimum_level':level} for uid,level in zip(units[:count],levels[:count])],
        'unused_slot_levels':levels[count:],'minimum_player_level':minimum,'repay_point':repay,
        'recipe_sprite':codec._virtual_path(codec._utf16(raw,RECIPE_SPRITE_OFFSET,60,'blueprint sprite'),'blueprint sprite'),
        'resource_refs':codec.scan_resource_references(raw,'.bin'),
        'source_sha256':hashlib.sha256(raw).hexdigest()}


def patch(template,declaration,*,root_unit_id,recipe_sprite=None,preview_sprite=None):
    original=decode(template)
    value=blueprints.normalize(declaration,root_unit_id=root_unit_id)
    blueprints.server_rows(value,root_unit_id=root_unit_id)
    if original['id']!=value['recipe_template_id']:raise ValueError('设计图模板身份与所选目标模板不一致')
    result=bytearray(template)
    codec._put_utf16(result,5,100,value['name'],'blueprint name')
    struct.pack_into('<I',result,0,value['id'])
    struct.pack_into('<I',result,COUNT_OFFSET,len(value['materials']))
    struct.pack_into('<I',result,OUTPUT_OFFSET,root_unit_id)
    for slot in range(7):
        material=value['materials'][slot] if slot<len(value['materials']) else None
        struct.pack_into('<I',result,UNIT_OFFSET+4*slot,material['unit_id'] if material else 0)
        if material:struct.pack_into('<I',result,LEVEL_OFFSET+4*slot,material['minimum_level'])
    struct.pack_into('<I',result,MINIMUM_PLAYER_OFFSET,value['minimum_player_level'])
    struct.pack_into('<I',result,REPAY_OFFSET,value['repay_point'])
    for offset,sprite in ((RECIPE_SPRITE_OFFSET,recipe_sprite),(PREVIEW_SPRITE_OFFSET,preview_sprite)):
        if sprite is None:continue
        path=codec._virtual_path(sprite,'blueprint sprite')
        if not path.startswith('sprs/') or not path.endswith('.spr'):raise ValueError('设计图图片必须为原生SPR')
        codec._put_utf16(result,offset,60,path.replace('/','\\'),'blueprint sprite')
    decode(bytes(result))
    return bytes(result)
