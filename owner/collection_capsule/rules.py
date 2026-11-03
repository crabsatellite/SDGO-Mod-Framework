"""Shared public price/quota contract for SQL, native client data and posters."""
def defaults():return {'price_gp':100000,'limit_period':'week','draw_limit':15,'utc_offset_minutes':480}

def validate(value):
    if not isinstance(value,dict) or set(value)!=set(defaults()):raise ValueError('收藏扭蛋规则字段不完整')
    for key,low,high in [('price_gp',1,1000000000),('draw_limit',0,100000),('utc_offset_minutes',-720,840)]:
        if type(value[key])is not int or not low<=value[key]<=high:raise ValueError('收藏扭蛋 '+key+' 超出允许范围')
    if value['limit_period'] not in ('none','day','week'):raise ValueError('收藏扭蛋限额周期无效')
    if value['limit_period']=='none' and value['draw_limit']!=0:raise ValueError('不限次时请将次数设为0')
    return value

def from_config(config):
    value={**defaults(),**{k:(v if k=='limit_period' else int(v)) for k,v in config.items() if k in defaults() and v!=''}}
    return validate(value)
