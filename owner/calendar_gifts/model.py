"""Validated reusable gift rules and separately stored private audiences."""
from __future__ import annotations
import copy
import datetime as dt
import json
from pathlib import Path
import re

HERE=Path(__file__).parent
GRADES=((4,3),(4,4),(4,2),(3,3),(3,4),(3,2),(2,3),(2,4),(2,2),(1,3),(1,4),(1,2))
GRADE_NAMES={(1,2):'C',(1,3):'CS',(1,4):'CR',(1,5):'CU',(2,2):'B',(2,3):'BS',(2,4):'BR',
             (2,5):'BU',(3,2):'A',(3,3):'AS',(3,4):'AR',(3,5):'AU',(4,2):'S',(4,3):'SS',(4,4):'SR'}
FESTIVALS={'new_year':'元旦','spring_festival':'春节','qingming':'清明',
           'labour_day':'劳动节','dragon_boat':'端午','mid_autumn':'中秋','national_day':'国庆',
           'valentines_day':'情人节','womens_day':'国际妇女节','childrens_day':'六一儿童节',
           'halloween':'万圣节','christmas_eve':'平安夜','christmas':'圣诞节','new_years_eve':'跨年夜'}
FIXED_FESTIVALS={'new_year':'01-01','labour_day':'05-01','national_day':'10-01',
                 'valentines_day':'02-14','womens_day':'03-08','childrens_day':'06-01',
                 'halloween':'10-31','christmas_eve':'12-24','christmas':'12-25','new_years_eve':'12-31'}
ADDITIONAL_FESTIVALS=('valentines_day','womens_day','childrens_day','halloween',
                      'christmas_eve','christmas','new_years_eve')
ID=re.compile(r'[a-z][a-z0-9_-]{0,47}\Z')


def default_rewards(each_grade=False):
    return {'mode':'missing_each_grade' if each_grade else 'missing_one',
            'grades':[list(x) for x in GRADES] if each_grade else [],'fallback':True,'unit_ids':[],'items':[]}


def defaults():
    return {'schema':1,'enabled':False,'utc_offset_minutes':480,'catchup_days':7,
            'birthday':{'enabled':True,'feb29':'feb28','rewards':default_rewards(True)},
            'events':[{'id':key,'title':name,'enabled':True,'calendar':'festival','festival':key,
                       'month_day':'','date':'','group':'all','rewards':default_rewards()} for key,name in FESTIVALS.items()]}


def missing_festivals(policy):
    """Offer only the new presets, preserving IDs and custom/disabled events."""
    ids={e['id'] for e in policy['events']}
    festivals={e['festival'] for e in policy['events'] if e['calendar']=='festival'}
    return [e for e in defaults()['events'] if e['id'] in ADDITIONAL_FESTIVALS
            and e['id'] not in ids and e['festival'] not in festivals]


def fields(value,expected,label):
    if not isinstance(value,dict) or set(value)!=set(expected):raise ValueError(label+'字段不完整或含未登记字段')


def integer(value,low,high,label):
    if type(value) is not int or not low<=value<=high:raise ValueError(label+'超出允许范围')
    return value


def boolean(value,label):
    if type(value) is not bool:raise ValueError(label+'必须是开关')


def month_day(value):
    if not isinstance(value,str) or not re.fullmatch(r'\d{2}-\d{2}',value):raise ValueError('日期必须为 MM-DD')
    dt.date.fromisoformat('2000-'+value)
    return value


def rewards(value):
    fields(value,default_rewards(),'礼物规则')
    if value['mode'] not in ('none','missing_one','missing_each_grade','fixed'):raise ValueError('机体选择规则无效')
    boolean(value['fallback'],'等级回退')
    grades=value['grades']
    if not isinstance(grades,list) or len(grades)>15 or any(not isinstance(g,list) or len(g)!=2 or any(type(x)is not int for x in g) or tuple(g) not in GRADE_NAMES for g in grades):raise ValueError('机体等级无效')
    if len({tuple(x) for x in grades})!=len(grades):raise ValueError('机体等级重复')
    if value['mode']=='missing_each_grade' and not grades:raise ValueError('每档一台需要选择等级')
    units=value['unit_ids'];items=value['items']
    if not isinstance(units,list) or len(units)>32:raise ValueError('固定机体清单无效')
    for uid in units:integer(uid,1,2147483647,'机体编号')
    if len(set(units))!=len(units):raise ValueError('固定机体清单重复')
    if value['mode']=='fixed' and not units:raise ValueError('请选择固定机体')
    if not isinstance(items,list) or len(items)>32:raise ValueError('道具清单过长')
    seen=set()
    for item in items:
        fields(item,('category_id','amount'),'道具')
        integer(item['category_id'],1,2147483647,'道具编号');integer(item['amount'],1,9999,'道具数量')
        if item['category_id'] in seen:raise ValueError('道具编号重复')
        seen.add(item['category_id'])
    return value


def validate(value):
    fields(value,defaults(),'日期赠礼')
    if type(value['schema'])is not int or value['schema']!=1:raise ValueError('日期赠礼配置版本无效')
    boolean(value['enabled'],'日期赠礼');integer(value['utc_offset_minutes'],-720,840,'时区')
    integer(value['catchup_days'],0,31,'补发天数')
    fields(value['birthday'],('enabled','feb29','rewards'),'生日规则');boolean(value['birthday']['enabled'],'生日赠礼')
    if value['birthday']['feb29'] not in ('feb28','mar01','leap_only'):raise ValueError('闰日生日规则无效')
    rewards(value['birthday']['rewards'])
    events=value['events']
    if not isinstance(events,list) or len(events)>100:raise ValueError('日期活动最多 100 项')
    seen=set()
    for event in events:
        fields(event,('id','title','enabled','calendar','festival','month_day','date','group','rewards'),'日期活动')
        if not isinstance(event['id'],str) or not ID.fullmatch(event['id']) or event['id']=='birthday' or event['id'] in seen:raise ValueError('活动编号无效或重复')
        seen.add(event['id']);boolean(event['enabled'],'活动开关')
        if not isinstance(event['title'],str) or not 1<=len(event['title'])<=64 or any(ord(c)<32 for c in event['title']):raise ValueError('活动名称无效')
        if not isinstance(event['group'],str) or not ID.fullmatch(event['group']):raise ValueError('接收分组无效')
        if event['calendar']=='festival':
            if not isinstance(event['festival'],str) or event['festival'] not in FESTIVALS:raise ValueError('节日无效')
        elif event['calendar']=='annual':month_day(event['month_day'])
        elif event['calendar']=='once':dt.date.fromisoformat(event['date'])
        else:raise ValueError('日期规则无效')
        rewards(event['rewards'])
    return copy.deepcopy(value)


def private_defaults():return {'schema':1,'birthdays':[],'groups':{}}


def validate_private(value):
    fields(value,private_defaults(),'本服私人名单')
    if type(value['schema'])is not int or value['schema']!=1 or not isinstance(value['birthdays'],list) or len(value['birthdays'])>10000:raise ValueError('私人名单版本或数量无效')
    seen=set()
    for row in value['birthdays']:
        fields(row,('account_id','month_day','enabled'),'生日登记')
        integer(row['account_id'],1,2147483647,'玩家编号');month_day(row['month_day']);boolean(row['enabled'],'生日开关')
        if row['account_id'] in seen:raise ValueError('玩家生日重复')
        seen.add(row['account_id'])
    if not isinstance(value['groups'],dict) or len(value['groups'])>100:raise ValueError('玩家分组无效')
    for name,ids in value['groups'].items():
        if not isinstance(name,str) or not ID.fullmatch(name) or name=='all' or not isinstance(ids,list) or len(ids)>10000:raise ValueError('私人分组无效')
        for account in ids:integer(account,1,2147483647,'玩家编号')
        if len(set(ids))!=len(ids):raise ValueError('私人分组玩家重复')
    return copy.deepcopy(value)


def event_date(event,year):
    if event['calendar']=='festival':
        if event['festival'] in FIXED_FESTIVALS:
            return dt.date.fromisoformat(str(year)+'-'+FIXED_FESTIVALS[event['festival']])
        dates=json.loads((HERE/'festival_dates.json').read_text(encoding='utf-8'))['years']
        if str(year) not in dates:raise ValueError('缺少 '+str(year)+' 年节日日历，请先更新日历或改用指定日期')
        return dt.date.fromisoformat(dates[str(year)][event['festival']])
    if event['calendar']=='once':return dt.date.fromisoformat(event['date'])
    try:return dt.date.fromisoformat(str(year)+'-'+event['month_day'])
    except ValueError:return None # annual February 29 in a common year


def birthday_date(monthday,year,feb29):
    if monthday=='02-29':
        try:return dt.date(year,2,29)
        except ValueError:
            return None if feb29=='leap_only' else dt.date(year,2,28) if feb29=='feb28' else dt.date(year,3,1)
    return dt.date.fromisoformat(str(year)+'-'+monthday)


def occurrences(policy,people,start,end):
    """Stable annual identities survive date/config edits; no grant is executed."""
    result=[]
    for year in range(start.year,end.year+1):
        for event in policy['events']:
            if not event['enabled']:continue
            date=event_date(event,year)
            if date and start<=date<=end and (event['calendar']!='once' or date.year==year):
                result.append({'key':event['id']+':'+str(year),'date':date.isoformat(),'title':event['title'],
                               'group':event['group'],'account_id':None,'rewards':event['rewards']})
        if policy['birthday']['enabled']:
            for person in people['birthdays']:
                if not person['enabled']:continue
                date=birthday_date(person['month_day'],year,policy['birthday']['feb29'])
                if date and start<=date<=end:
                    result.append({'key':'birthday:'+str(year),'date':date.isoformat(),'title':'生日快乐',
                                   'group':None,'account_id':person['account_id'],'rewards':policy['birthday']['rewards']})
    return sorted(result,key=lambda x:(x['date'],x['key'],x['account_id'] or 0))


def choose_missing(rule,units,owned,choose):
    """Legacy same-base-grade fallback, with one choice per original grade."""
    eligible=[u for u in units if u['unit_id'] not in owned];selected=[]
    if rule['mode']=='fixed':return [u for u in eligible if u['unit_id'] in rule['unit_ids']]
    if rule['mode']=='none':return []
    if rule['mode']=='missing_one':
        pool=[u for u in eligible if not rule['grades'] or list(u['grade']) in rule['grades']]
        return [choose(pool)] if pool else []
    for grade in rule['grades']:
        order=[tuple(grade)]
        if rule['fallback']:order += [(grade[0],g) for g in range(grade[1]+1,6)]+[(grade[0],g) for g in range(grade[1]-1,1,-1)]
        for candidate in order:
            pool=[u for u in eligible if tuple(u['grade'])==candidate and u['unit_id'] not in {s['unit_id'] for s in selected}]
            if pool:selected.append(choose(pool));break
    return selected


def legacy_birthdays(value,today):
    """Read only the historical completed birthday map, never other old state."""
    done=value.get('birthday_done') if isinstance(value,dict) else None
    if not isinstance(done,dict) or len(done)>10000:raise ValueError('旧生日回执文件缺少有效 birthday_done')
    records=[]
    for account,date in done.items():
        if not isinstance(account,str) or not account.isdigit() or not 0<int(account)<=2147483647:continue
        if not isinstance(date,str) or date=='pending':continue
        try:gift_date=dt.date.fromisoformat(date)
        except ValueError:continue
        if gift_date>today:raise ValueError('旧生日回执包含未来日期')
        records.append({'account_id':int(account),'date':gift_date.isoformat()})
    return sorted(records,key=lambda r:(r['account_id'],r['date']))
