"""Source-bound averages for dedicated imported S-tier activity drafts."""
from __future__ import annotations

from collections import Counter,defaultdict
from fractions import Fraction


def recommend(root,*,units,categories,prices,pool,listed_machines,currency=None):
    if root.get('rank')!=4:raise ValueError('独立活动机建议只用于S级及以上机库根形态')
    unit_by_id={u['UnitNumber']:u for u in units}
    category_by_id={r['CatalogueID']:r for r in categories}
    price_by_id={r['MachineID']:r for r in prices}
    by_machine=defaultdict(Counter)
    for row in pool:
        count=row.get('ExpandedEntries',1)
        if type(count) is not int or count<=0:raise ValueError('活动机奖池频率无效')
        by_machine[row['MachineID']][(row['CategoryID'],row['Amount'])]+=count
    cohorts=defaultdict(list)
    for mid in listed_machines:
        price=price_by_id.get(mid)
        entries=by_machine.get(mid)
        if not price or price['Disable'] or not entries:continue
        rare=Counter();filler=Counter();total=sum(entries.values())
        for (category,amount),count in entries.items():
            item=category_by_id.get(category)
            if not item or type(amount) is not int or amount<=0:
                raise ValueError('原生活动奖池含未解析物品或数量')
            unit=unit_by_id.get(item['RefIndex']) if category//100000==11 else None
            if unit and unit['UnitGradeNumber']==4:
                if unit['UnitGradeNumber2']==root.get('subgrade'):rare[unit['UnitNumber']]+=count
            else:filler[(category,amount)]+=count
        if rare and filler:
            cohorts[price['Type']].append({'machine_id':mid,'price':price['Price'],
                'rare':rare,'filler':filler,'total':total})
    available=sorted(cohorts)
    chosen=currency if currency is not None else (0 if 0 in cohorts else available[0] if available else None)
    if chosen not in cohorts:
        return {'available':False,'changes_applied':False,'unit_id':root['unit_id'],
                'available_currencies':available,'message':'没有同等级、同货币的原生活动机参考；请明确选择模板或作者设定'}
    selected=cohorts[chosen]
    samples=[Fraction(count,r['total']) for r in selected for count in r['rare'].values()]
    rare_mean=sum(samples,Fraction())/len(samples)
    price_mean=Fraction(sum(r['price'] for r in selected),len(selected))
    filler_mean=defaultdict(Fraction)
    for row in selected:
        for key,count in row['filler'].items():filler_mean[key]+=Fraction(count,row['total'])/len(selected)
    filler_total=sum(filler_mean.values(),Fraction())
    normalized={key:value*(1-rare_mean)/filler_total for key,value in filler_mean.items()}
    def rational(value):return {'numerator':value.numerator,'denominator':value.denominator,'value':float(value)}
    return {'available':True,'changes_applied':False,'unit_id':root['unit_id'],
        'currency':chosen,'available_currencies':available,'machine_count':len(selected),'rare_sample_count':len(samples),
        'source_machine_ids':sorted(r['machine_id'] for r in selected),
        'rare_configuration_frequency':rational(rare_mean),
        'suggested_price':(2*price_mean.numerator+price_mean.denominator)//(2*price_mean.denominator),
        'price_average':rational(price_mean),
        'filler_rewards':[{'category_id':category,'amount':amount,'name':category_by_id[category]['Name'],
                           'configuration_frequency':rational(value)} for (category,amount),value in sorted(normalized.items())],
        'artwork_required':True,'probability_kind':'native-config-frequency',
        'method':'Arithmetic mean of matching rare unit frequencies; non-S rewards use per-machine mean shares.',
        'limitations':['This is a draft recommendation, not an applied pool.',
            'Native sequence, repeat avoidance and configured limits remain separate from independent random odds.']}
