"""Compile an explicit owner reward mix to the original activity sequence."""
from __future__ import annotations

from fractions import Fraction
import hashlib
import json


POOL_SIZE=10000


def compile_pool(rewards,*,size=POOL_SIZE):
    if type(size) is not int or size!=POOL_SIZE:raise ValueError('原生活动机使用固定10000格序列，不能更改已有玩家的周期长度')
    if not isinstance(rewards,list) or not 2<=len(rewards)<=128:raise ValueError('活动奖池需要2..128个明确奖励')
    entries=[];seen=set()
    for row in rewards:
        if not isinstance(row,dict) or set(row)-{'category_id','amount','weight','name','role','source'}:
            raise ValueError('活动奖池字段无效')
        category=row.get('category_id');amount=row.get('amount')
        if type(category) is not int or not 0<category<=2_147_483_647 or type(amount) is not int or not 0<amount<=2_147_483_647:
            raise ValueError('活动奖励类别和数量必须为正整数')
        key=(category,amount)
        if key in seen:raise ValueError('相同类别与数量的奖励请合并权重')
        seen.add(key)
        weight=row.get('weight')
        if isinstance(weight,dict) and set(weight)=={'numerator','denominator'}:
            if any(type(weight[k]) is not int or weight[k]<=0 for k in weight):raise ValueError('概率有理数无效')
            weight=Fraction(weight['numerator'],weight['denominator'])
        elif type(weight) is int and weight>0:weight=Fraction(weight)
        else:raise ValueError('活动奖池权重须为正整数或精确有理数')
        entries.append({'category_id':category,'amount':amount,'weight':weight,
            'name':str(row.get('name',category)),'role':row.get('role','companion'),'source':row.get('source','authored')})
    total=sum((r['weight'] for r in entries),Fraction())
    quotas=[r['weight']*size/total for r in entries]
    counts=[q.numerator//q.denominator for q in quotas]
    remaining=size-sum(counts)
    order=sorted(range(len(entries)),key=lambda i:(-(quotas[i]-counts[i]),entries[i]['category_id'],entries[i]['amount']))
    for i in order[:remaining]:counts[i]+=1
    if any(c==0 for c in counts):raise ValueError('至少一个奖励的权重低于10000格可表达范围，请明确调整')
    # Smooth weighted round-robin spreads the configured rewards without
    # replacing native reset/random-start/repeat-avoidance or draw procedures.
    scores=[0]*len(entries);sequence=[]
    for index in range(size):
        for i,n in enumerate(counts):scores[i]+=n
        selected=max(range(len(entries)),key=lambda i:(scores[i],-i))
        scores[selected]-=size
        row=entries[selected]
        sequence.append({'Idx':index+1,'CategoryID':row['category_id'],'Amount':row['amount'],'used':1})
    totals={}
    for row,n in zip(entries,counts):totals[row['category_id']]=totals.get(row['category_id'],0)+n
    if max(totals.values())*2<=size:
        # Score ties can leave a few equal neighbors, including the cycle
        # boundary. Swap only when all four affected edges become valid.
        for i in range(size):
            position=(i+1)%size
            if sequence[i]['CategoryID']!=sequence[position]['CategoryID']:continue
            forbidden={position,(position-1)%size,(position+1)%size}
            changed=False
            for j in range(size):
                if j in forbidden:continue
                a=sequence[position]['CategoryID'];b=sequence[j]['CategoryID']
                if (b!=sequence[(position-1)%size]['CategoryID'] and b!=sequence[(position+1)%size]['CategoryID']
                    and a!=sequence[(j-1)%size]['CategoryID'] and a!=sequence[(j+1)%size]['CategoryID']):
                    sequence[position],sequence[j]=sequence[j],sequence[position];changed=True;break
            if not changed:
                # Equal-weight circular ties can require more than a local
                # swap. Rebuild a valid even/odd placement with exact counts.
                ordered=sorted(range(len(entries)),key=lambda k:(-totals[entries[k]['category_id']],entries[k]['category_id'],entries[k]['amount']))
                positions=[*range(0,size,2),*range(1,size,2)];sequence=[None]*size;cursor=0
                for k in ordered:
                    row=entries[k]
                    for unused in range(counts[k]):
                        at=positions[cursor];cursor+=1
                        sequence[at]={'Idx':at+1,'CategoryID':row['category_id'],'Amount':row['amount'],'used':1}
                break
        for i,row in enumerate(sequence):row['Idx']=i+1
    repeats=sum(sequence[i]['CategoryID']==sequence[(i+1)%size]['CategoryID'] for i in range(size))
    if max(totals.values())*2<=size and repeats:
        raise ValueError('无法在保留频率的条件下生成无相邻重复的原生活动序列')
    config=[]
    for row,n in zip(entries,counts):
        intended=row['weight']/total
        config.append({k:v for k,v in row.items() if k!='weight'}|{'entries':n,
            'intended_frequency':{'numerator':intended.numerator,'denominator':intended.denominator},
            'configuration_frequency':n/size,'quantization_error':float(Fraction(n,size)-intended)})
    checksum=hashlib.sha256(json.dumps(sequence,sort_keys=True,separators=(',',':')).encode('ascii')).hexdigest()
    return {'schema_version':1,'sequence_size':size,'sequence':sequence,'rewards':config,'adjacent_category_repeats':repeats,
        'sequence_sha256':checksum,'draw_policy':'native_item_machine_sequence',
        'limitations':['Configured frequency is not an independent random-draw promise.',
            'Original native reset, random start, repeat avoidance, limits, billing and delivery still apply.']}
