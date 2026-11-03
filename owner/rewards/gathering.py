"""Optional owner-authored participation rules; no server-specific rewards."""
from __future__ import annotations
from datetime import datetime,timedelta,timezone
import copy,re

ID=re.compile(r'[a-z][a-z0-9_-]{0,47}\Z')

def defaults():
    return {'enabled':False,'utc_offset_minutes':480,'minimum_active_seconds':180,
            'require_contribution':True,'rules':[]}

def integer(value,low,high,label):
    if type(value)is not int or not low<=value<=high:raise ValueError(label+'超出允许范围')

def validate(value):
    if not isinstance(value,dict) or set(value)!=set(defaults()):raise ValueError('聚会奖励字段不完整')
    if type(value['enabled'])is not bool or type(value['require_contribution'])is not bool:raise ValueError('聚会奖励开关无效')
    integer(value['utc_offset_minutes'],-720,840,'聚会奖励时区')
    integer(value['minimum_active_seconds'],30,3600,'聚会最低参战秒数')
    rules=value['rules']
    if not isinstance(rules,list) or len(rules)>16:raise ValueError('聚会奖励最多16条规则')
    ids=set()
    for row in rules:
        if not isinstance(row,dict) or set(row)!={'id','title','enabled','minimum_humans','period','modes','gb','mb'}:raise ValueError('聚会规则字段不完整')
        if not isinstance(row['id'],str) or not ID.fullmatch(row['id']) or row['id'] in ids:raise ValueError('聚会规则编号无效或重复')
        ids.add(row['id'])
        if not isinstance(row['title'],str) or not 1<=len(row['title'])<=64 or any(ord(c)<32 for c in row['title']):raise ValueError('聚会规则名称无效')
        if type(row['enabled'])is not bool:raise ValueError('聚会规则开关无效')
        integer(row['minimum_humans'],2,12,'聚会真人数')
        if row['period'] not in ('match','day','week'):raise ValueError('聚会重置周期无效')
        if not isinstance(row['modes'],list) or not row['modes'] or len(set(row['modes']))!=len(row['modes']) or any(m not in ('pvp','pve') for m in row['modes']):raise ValueError('聚会适用模式无效')
        integer(row['gb'],0,1000000,'聚会GB');integer(row['mb'],0,100000,'聚会MB')
    return value

def period_key(rule,job,offset):
    date=datetime.fromtimestamp(job['completed_unix'],timezone(timedelta(minutes=offset))).date()
    if rule['period']=='match':return job['match_id']
    if rule['period']=='week':date-=timedelta(days=date.weekday())
    return date.isoformat()

def awards(job,value):
    validate(value)
    if not value['enabled'] or job.get('schema_version',0)<3:return []
    enough=lambda seconds,contributed:seconds>=value['minimum_active_seconds'] and (contributed or not value['require_contribution'])
    if not enough(job['active_seconds'],job['contributed']):return []
    humans=sum(enough(r['active_seconds'],r['contributed']) for r in job['participants'])
    return [{**copy.deepcopy(rule),'period_key':period_key(rule,job,value['utc_offset_minutes'])}
            for rule in value['rules'] if rule['enabled'] and job['mode'] in rule['modes']
            and humans>=rule['minimum_humans'] and (rule['gb'] or rule['mb'])]

def public(policy):
    return {'config':copy.deepcopy(validate(policy.get('gathering',defaults()))),
            'requires_native_v3':True,'fixed_rewards_not_multiplied':True,
            'deduplication':'account_rule_period_across_servers'}
