"""Host-derived activity presets; no unit roster or private artwork is shipped."""
from __future__ import annotations

from collections import Counter, defaultdict
from fractions import Fraction

from .activity_balance import recommend
from .activity_pool import compile_pool


PATTERNS = (
    {'id': 'gp_rare', 'title': '普通 G 币', 'currency_type': 0,
     'description': '参考普通 G 币机台，按本服设置预填价格与目标频率。'},
    {'id': 'token_rare', 'title': '少量代币', 'currency_type': 2,
     'description': '参考少量代币机台，按本服设置预填价格与目标频率。'},
    {'id': 'token_99', 'title': '99 代币高阶', 'currency_type': 2,
     'description': '单独参考 99 代币机台的同档高阶奖池，需要明确选择。'},
)


def normalize_preferences(value=None):
    if value is None:return {'schema_version':1,'patterns':{}}
    if (not isinstance(value,dict) or set(value)!={'schema_version','patterns'} or value['schema_version']!=1
        or not isinstance(value['patterns'],dict) or set(value['patterns'])-{row['id'] for row in PATTERNS}):
        raise ValueError('本服活动模板设置格式无效')
    result={'schema_version':1,'patterns':{}}
    for pattern,settings in value['patterns'].items():
        if not isinstance(settings,dict) or set(settings)-{'price','featured_entries','s_grade_entries'}:
            raise ValueError('活动模板只接受本服价格、目标机体频率和 S 级合计频率设置')
        for key,number in settings.items():
            maximum=5000 if key in ('featured_entries','s_grade_entries') else 2147483647
            if type(number) is not int or not 1<=number<=maximum:raise ValueError('本服模板参数超出支持范围')
        if pattern=='token_99':
            if settings.get('price',99)!=99:raise ValueError('99 代币模板价格应为 99')
            if 's_grade_entries' in settings:raise ValueError('99 代币高阶模板保留独立奖池，不接受普通 S 级合计频率')
        result['patterns'][pattern]=dict(settings)
    return result


def _weight(row):
    value=row['weight']
    if isinstance(value,dict):return Fraction(value['numerator'],value['denominator'])
    return Fraction(value)


def _allocate_positive(rewards,total,label):
    """Allocate an exact integer slot budget while retaining every reward identity."""
    if not rewards:
        if total:raise ValueError(f'{label}没有可分配的参考奖励')
        return {}
    if total<len(rewards):raise ValueError(f'{label}目标格数不足以保留所有参考奖励')
    remaining=total-len(rewards);weight_sum=sum((_weight(row) for row in rewards.values()),Fraction())
    quotas={key:_weight(row)*remaining/weight_sum for key,row in rewards.items()}
    counts={key:1+quota.numerator//quota.denominator for key,quota in quotas.items()}
    missing=total-sum(counts.values())
    order=sorted(rewards,key=lambda key:(-(quotas[key]-(counts[key]-1)),key))
    for key in order[:missing]:counts[key]+=1
    return counts


def build_pattern(pattern_id, root, root_category, *, units, categories, prices, pool, listed_machines,preferences=None,excluded_machine_ids=()):
    pattern = next((r for r in PATTERNS if r['id'] == pattern_id), None)
    if pattern is None:
        raise ValueError('请选择 G 币、少量代币或 99 代币活动模板')
    premium = pattern_id == 'token_99'
    settings=normalize_preferences(preferences)['patterns'].get(pattern_id,{})
    # Dedicated imports and the dynamic collection pool are consumers of the
    # presets, not stock references for the next imported unit.
    listed_machines = {mid for mid in listed_machines
                       if not 1990001 <= mid <= 1990999 and mid != 990001 and mid not in excluded_machine_ids}
    reference_prices = [r for r in prices if not premium or r['Price'] == 99]
    balance = recommend(root, units=units, categories=categories, prices=reference_prices,
                        pool=pool, listed_machines=listed_machines,
                        currency=pattern['currency_type'], profile='same_tier' if premium else 'ordinary')
    if not balance['available']:
        raise ValueError(balance['message'])
    cats = {r['CatalogueID']: r for r in categories}
    unit_map = {r['UnitNumber']: r for r in units}
    price_map = {r['MachineID']: r for r in reference_prices}
    by_machine = defaultdict(Counter)
    for row in pool:
        by_machine[row['MachineID']][(row['CategoryID'], row['Amount'])] += row.get('ExpandedEntries', 1)
    common_price = min(Counter(price_map[mid]['Price'] for mid in balance['source_machine_ids']).items(),
                       key=lambda item: (-item[1], item[0]))[0]
    candidates = []
    for mid in balance['source_machine_ids']:
        entries = by_machine[mid]
        usable = {}; rare = []
        for (category, amount), count in entries.items():
            item = cats[category]
            if category == 1070001:
                continue  # Original native no-drop sentinel, not an inventory item.
            if category // 100000 not in (11, 15, 22) or item['TimeLimit'] != 0:
                usable = {}; break
            usable[(category, amount)] = {'category_id': category, 'amount': amount, 'weight': count,
                                'name': item['Name'], 'role': 'companion', 'source': 'native-pattern-reference'}
            unit = unit_map.get(item['RefIndex']) if category // 100000 == 11 else None
            if unit and unit['UnitGradeNumber'] == 4:
                rare.append((unit['UnitGradeNumber2'] != root['subgrade'], count, (category, amount)))
        if usable and rare and len(usable) <= 128:
            candidates.append((price_map[mid]['Price'] != common_price, -len(usable), mid, usable, rare))
    if not candidates:
        raise ValueError('参考机台包含尚未支持的奖品类型；请手动配置已支持的奖励')
    _, _, template_id, rewards, rare = min(candidates, key=lambda row: row[:3])
    replacement = min(rare)[2]
    reference_entries = round(Fraction(balance['rare_configuration_frequency']['numerator'],
                                      balance['rare_configuration_frequency']['denominator']) * 10000)
    target_entries = settings.get('featured_entries',reference_entries)
    if not 1 <= target_entries <= 5000:
        raise ValueError('参考目标频率超出原生奖池模板支持范围，请手动设定')
    rewards.pop(replacement)
    # Avoid two copies of the root if a host already added it to a reference.
    rewards = {key: row for key, row in rewards.items() if key[0] != root_category}
    fixed_rare = set()
    for category in rewards:
        item = cats[category[0]]
        unit = unit_map.get(item['RefIndex']) if category[0] // 100000 == 11 else None
        if unit and unit['UnitGradeNumber'] == 4:
            fixed_rare.add(category)
    flexible = set(rewards) - fixed_rare
    if premium or not flexible:
        flexible = set(rewards)
        fixed_rare = set()
    aggregate_s_entries=settings.get('s_grade_entries')
    root_is_s=root['rank']==4
    if aggregate_s_entries is not None:
        if not root_is_s:
            raise ValueError('S 级合计频率只适用于 S 级目标机体模板')
        if target_entries>aggregate_s_entries:
            raise ValueError('目标机体频率不能高于 S 级合计频率')
        other_s={key:rewards[key] for key in fixed_rare}
        ordinary={key:rewards[key] for key in set(rewards)-fixed_rare}
        allocations=_allocate_positive(other_s,aggregate_s_entries-target_entries,'S 级合计频率')
        allocations.update(_allocate_positive(ordinary,10000-aggregate_s_entries,'普通奖励频率'))
        for category,entries in allocations.items():rewards[category]['weight']=entries
    else:
        fixed_entries = sum(_weight(rewards[category]) for category in fixed_rare)
        budget = 10000 - target_entries - fixed_entries
        weight_sum = sum((_weight(rewards[category]) for category in flexible),Fraction())
        if budget <= 0 or weight_sum <= 0:
            raise ValueError('参考奖池没有足够的普通奖励空间，请手动配置')
        for category in flexible:
            weight = _weight(rewards[category]) * budget / weight_sum
            rewards[category]['weight'] = {'numerator': weight.numerator, 'denominator': weight.denominator}
    rewards[(root_category, 1)] = {'category_id': root_category, 'amount': 1, 'weight': target_entries,
                              'name': '目标机体', 'role': 'featured', 'source': 'owner-preset-default'}
    compiled = compile_pool(list(rewards.values()))
    category_totals = Counter()
    for row in compiled['rewards']:category_totals[row['category_id']] += row['entries']
    s_categories=set()
    for category,_amount in rewards:
        if category==root_category:
            if root_is_s:s_categories.add(category)
            continue
        item=cats.get(category);unit=unit_map.get(item['RefIndex']) if item and category//100000==11 else None
        if unit and unit['UnitGradeNumber']==4:s_categories.add(category)
    actual_s_entries=sum(category_totals[category] for category in s_categories)
    if aggregate_s_entries is not None and actual_s_entries!=aggregate_s_entries:
        raise ValueError('S 级合计频率无法精确编译为原生 10000 格序列')
    if max(category_totals.values()) > 5000:
        raise ValueError('参考奖池单项超过 50%，请拆分或手动调整')
    return {'pattern_id': pattern_id, 'currency_type': pattern['currency_type'],
            'price': settings.get('price',99 if premium else common_price), 'template_machine_id': template_id,
            'pool': [{key: r[key] for key in ('category_id', 'amount', 'entries')} for r in compiled['rewards']],
            'balance': balance, 'target_entries': target_entries, 's_grade_entries': actual_s_entries,
            'source_machine_id': template_id,
            'method': 'Native reference items; target and aggregate S-grade frequencies are configurable; ordinary rewards fill the remaining slots.'}
