"""Developer-only word-bank compiler from explicit, private configuration extracts.

The input is never bundled. Output contains text pairs only, no native records,
prices, account data, paths or connection settings. Day-to-day use is in Manager.
"""
from __future__ import annotations
import argparse
import collections
import hashlib
import importlib.util
import json
from pathlib import Path
import re
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]


def escape(value):
    value=value.replace('\\', '\\\\').replace('\r', '\\r').replace('\n', '\\n').replace('\t', '\\t')
    return re.sub(r'^ +| +$',lambda m:r'\s'*len(m[0]),value)


def display_suffix(source, target):
    if re.search(r'\(off\s*event\)', source, re.I):
        return target + '（活动已结束）'
    if re.search(r'\(event\)|\(이벤트\)', source, re.I):
        return target + '（活动）'
    if re.search(r'\(sale\)', source, re.I):
        return target + '（销售）'
    return target


def choose(source, rows):
    highest = max(priority for priority, text, kind in rows)
    candidates = {(text, kind) for priority, text, kind in rows if priority == highest}
    if len({text for text,kind in candidates})==1:
        text=next(iter(candidates))[0]
        return text, 'name' if any(kind=='name' for t,kind in candidates) else next(iter(candidates))[1]
    if len(candidates) == 1:
        return next(iter(candidates))
    # Native form tables reuse plain English family names. Collapse only an
    # identical Chinese base with parenthesized form annotations, and only if
    # the original name itself has no variant/pilot/custom qualifier.
    if not re.search(r'[()（）]|custom|pilot|type|\bCU\b', source, re.I) and all(kind=='name' for text,kind in candidates):
        bases = {re.split(r'[（(]', text, maxsplit=1)[0].strip() for text,kind in candidates}
        if len(bases)==1 and len(next(iter(bases)))>=2:
            return next(iter(bases)), 'name'
    return None


def add_runtime_newline_aliases(words):
    """Admit the LF form produced by native shop text preprocessing.

    Stock records encode line breaks as the two characters ``\n``. Some shop
    display paths expand that marker before the hooked string assignment, so
    retain the original key and add an exact LF alias for the same translation.
    """
    aliases = {}
    for source, value in words.items():
        alias = source.replace('\\n', '\n')
        if alias == source:
            continue
        existing = words.get(alias)
        if existing is not None:
            if existing != value:
                raise ValueError('Conflicting runtime newline alias: ' + source)
            continue
        previous = aliases.get(alias)
        if previous is not None and previous != value:
            raise ValueError('Conflicting runtime newline alias: ' + source)
        aliases[alias] = value
    words.update(aliases)
    return len(aliases)


def compile_words(terms, phrases=None, native_names=None, extra=None, unit_names=None):
    if terms.get('schema_version') != 1:
        raise ValueError('Unsupported private term-extract schema')
    candidates = collections.defaultdict(list)
    ignored = []
    identities = set()
    units = {u['unit_id']: u['chinese']['value'] for u in terms['unit_records']}
    renamed = {}
    if unit_names is not None:
        replacements = collections.defaultdict(set)
        for unit in terms['unit_records']:
            target = unit_names.get(str(unit['unit_id']), unit_names.get(unit['unit_id']))
            if target:
                units[unit['unit_id']] = target
                replacements[unit['chinese']['value']].add(target)
        renamed = {source: next(iter(values)) for source, values in replacements.items() if len(values) == 1}
    categories = terms['categories']
    refs = {c['ref_index'] for c in categories if c['kind']==11}

    def add(source, target, kind='name', priority=100):
        # Correct a known typo in the pre-existing Chinese unit-name resource.
        if target:target=target.replace('强袭尸装','（突击护甲）')
        if source and target and source==target:
            identities.add(source)
            return
        if (not source or not target or '\0' in source+target or '\ufffd' in target
                or len(source.encode('utf-16-le'))>16384 or len(target.encode('utf-16-le'))>16384):
            return
        if source in ('GB','MB','GP','GC') or re.fullmatch(r'[\d., +%/\-\t]+', source):
            raise ValueError('Amounts and currency codes cannot be translated')
        candidates[source].append((priority, target, kind))

    for c in categories:
        source = c['source']['name']
        ref = c.get('resolved_reference') or {}
        if ref.get('type') in ('unit','operator'):
            target = ref.get('chinese_name')
            if target: target = renamed.get(target, target)
            if target and ref['type']=='operator' and '副官' not in target:
                target += '副官'
            if target:
                translated=display_suffix(source,target)
                add(source,translated)
                add('@category:'+str(c['catalogue_id'])+':'+source,translated,priority=200)
        elif ref.get('type')=='blueprint' and ref.get('target_unit_chinese_name'):
            # Some stock recipe rows have only the bare unit name. Preserve
            # that literal meaning in the shared text bank; the product-scoped
            # title below still explicitly identifies a blueprint.
            suffix='设计图' if re.search(r'plan|mix|조합식|设计图',source,re.I) else ''
            target = renamed.get(ref['target_unit_chinese_name'], ref['target_unit_chinese_name'])
            add(source, target+suffix)
            add('@category:'+str(c['catalogue_id'])+':'+source,target+'设计图',priority=200)

    for unit in terms['unit_records']:
        if unit['unit_id'] not in refs:
            continue
        target = units[unit['unit_id']]
        original = unit.get('original_0011') or {}
        for slot, source in enumerate(original.get('slots',[])):
            if isinstance(source, dict): source=source.get('value','')
            add(source,target,priority=85-slot)
        add(unit.get('alias_0237_slot2',''),target,priority=40)

    preliminary = {source: selected[0] for source, rows in candidates.items() if (selected:=choose(source, rows)) is not None}
    if phrases:
        for c in categories:
            source=c['source']['name']
            target=phrases.translate_name(source,c['kind'],preliminary)
            if target:
                add(source,target,priority=95)
                add('@category:'+str(c['catalogue_id'])+':'+source,target,priority=100)
            for field in ('description','usage'):
                source=c['source'].get(field,'')
                target=phrases.translate_description(source,preliminary)
                if target:add(source,target,'description',priority=90)

    parent_misses=[]
    for item in native_names or []:
        source=item.get('sourceName',item.get('source_name',''))
        product=item.get('productId',item.get('product_id'))
        links=item.get('catalogueMappings',[])
        resolved=[]
        for link in links:
            ref=link.get('resolvedReference') or {}
            target=ref.get('chineseName',ref.get('chinese_name'))
            if ref.get('type')=='blueprint':target=ref.get('targetUnitChineseName',ref.get('target_unit_chinese_name',target))
            if target:
                target = renamed.get(target, target)
                if link['kind']==15:target+='设计图'
                elif link['kind']==12 and '副官' not in target:target+='副官'
                resolved.append(target)
        target=None
        if links and len(resolved)==len(links) and len(set(resolved))==1:
            target=display_suffix(source,resolved[0])
        kinds=sorted({link['kind'] for link in links})
        if not target and phrases:
            for kind in kinds:
                target=phrases.translate_name(source,kind,preliminary)
                if target:break
        if not target:target=preliminary.get(source)
        if target:
            add(source,target,priority=105)
            if product is not None:add('@product:'+str(product)+':'+source,target,priority=150)
        else:parent_misses.append({'field':'name','product':product,'source':source,'kinds':kinds})
        for field,key in [('description','sourceDescription'),('usage','sourceUsage')]:
            text=item.get(key,'')
            if not text:continue
            translated=phrases.translate_description(text,preliminary) if phrases else None
            if not translated and text in preliminary:translated=preliminary[text]
            if translated:add(text,translated,'description',priority=90)
            elif not re.search(r'[\u3400-\u9fff]',text):parent_misses.append({'field':field,'product':product,'source':text,'kinds':kinds})

    for kind,entries in (extra or {}).items():
        for source,text in entries.items():add(source,text,kind,priority=130)

    # Stable identifiers serve optional in-game Mod consumers. Never derive
    # these by fuzzy source-name matching: several forms share English aliases.
    indexed_units = unit_names if unit_names is not None else units
    for unit_id, target in indexed_units.items():
        if not str(unit_id).isdigit() or not target:
            raise ValueError('Invalid stable unit-name entry')
        add('@unit:'+str(unit_id), target, priority=200)
    for category in categories:
        source = category['source']['name']
        scoped = '@category:'+str(category['catalogue_id'])+':'+source
        choices = candidates.get(scoped) or candidates.get(source)
        selected = choose(scoped, choices) if choices else None
        if selected is not None and selected[1] == 'name':
            add('@item:'+str(category['catalogue_id']),selected[0],priority=200)

    unit_aliases=collections.defaultdict(list)
    for unit in terms['unit_records']:
        if unit['unit_id'] not in refs:continue
        for source in unit.get('original_0011',{}).get('slots',[])+[unit.get('alias_0237_slot2','')]:
            if isinstance(source,str) and source:unit_aliases[source].append((100,units[unit['unit_id']],'name'))
    unit_words={source:value[0] for source,rows in unit_aliases.items() if (value:=choose(source,rows)) is not None and len(source)>=5 and not re.fullmatch(r'[A-Z0-9 -]{1,6}',source)}
    # Translate already-reviewed proper-name aliases within Chinese prose as
    # well. Numeric model identifiers are retained when the canonical Chinese
    # resource name omits them (e.g. Banshee's "02"). This changes text only.
    technical={'vDUAL GUNDAM ASSAULT SHROUD':'v决斗高达（突击护甲）','DUAL GUNDAM ASSAULT SHROUD':'决斗高达（突击护甲）','Over Heat Booster':'推进器过热弹','New Type':'新人类','Custom':'自定义强化','Rank':'级别','Level':'等级','Days':'天','Day':'天','Operator Type':'副官型号','Duel Unit':'双机组合','Booster':'推进器','Point':'点数','ea':'个'}
    alias_candidates=collections.defaultdict(set)
    for source,target in {**technical,**unit_words}.items():alias_candidates[source.casefold()].add(target)
    aliases={s:t for s,t in {**technical,**unit_words}.items() if len(alias_candidates[s.casefold()])==1}
    pattern=re.compile(r'(?<![A-Za-z0-9])(?:'+ '|'.join(re.escape(x) for x in sorted(aliases,key=len,reverse=True))+r')(?![A-Za-z0-9])',re.I)
    folded={s.casefold():t for s,t in aliases.items()}
    def localize_prose(text):
        def replace(match):
            source=match[0];target=folded[source.casefold()]
            lost=[n for n in re.findall(r'\d+(?:[,.]\d+)*%*',source) if n not in target]
            return target+('（'+ ' / '.join(lost)+'）' if lost else '')
        return pattern.sub(replace,text)

    words={}
    for source,rows in sorted(candidates.items()):
        chosen=choose(source,rows)
        if chosen is None:
            ignored.append({'source':source,'targets':sorted({t for p,t,k in rows})})
        else: words[source]={'text':localize_prose(chosen[0]) if chosen[1]=='description' else chosen[0],'kind':chosen[1]}
    runtime_newline_aliases = add_runtime_newline_aliases(words)
    covered=collections.Counter();total=collections.Counter()
    for c in categories:
        total[str(c['kind'])]+=1
        if c['source']['name'] in words or '@category:'+str(c['catalogue_id'])+':'+c['source']['name'] in words or c['source']['name'] in identities or re.search(r'[\u3400-\u9fff]',c['source']['name']):covered[str(c['kind'])]+=1
    report={'schema_version':1,'dictionary_entries':len(words),'by_kind':dict(collections.Counter(v['kind'] for v in words.values())),
            'category_names':{'total_by_native_kind':dict(total),'translated_or_existing_Chinese_by_native_kind':dict(covered)},
            'retained_native_identifiers':sorted(identities),'unresolved_source_conflicts':ignored,
            'runtime_newline_aliases':runtime_newline_aliases,
            'parent_text_missing':parent_misses,
            'untranslated_names':[{'category':c['catalogue_id'],'kind':c['kind'],'source':c['source']['name']} for c in categories if c['source']['name'] not in words and '@category:'+str(c['catalogue_id'])+':'+c['source']['name'] not in words and c['source']['name'] not in identities and not re.search(r'[\u3400-\u9fff]',c['source']['name'])]}
    report['parent_text_missing']=[p for p in parent_misses if p['source'] not in words and p['source'] not in identities and '@product:'+str(p['product'])+':'+p['source'] not in words]
    return words,report


def encode(words):
    lines=['; Chinese shop localization dictionary. UTF-8. Unknown text stays original.',
           '; Native records, prices, currencies and account data are never included.']
    for source,value in sorted(words.items()):
        identity=hashlib.sha256(source.encode('utf-8')).hexdigest()[:24]
        lines+=['','[entry:'+identity+']','Kind='+value['kind'],'Source='+escape(source),'Text='+escape(value['text'])]
    return ('\n'.join(lines)+'\n').encode('utf-8')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--terms',type=Path,required=True)
    parser.add_argument('--native-names',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--report',type=Path,required=True)
    args=parser.parse_args()
    phrase_file=ROOT/'tools/shop_localization_phrases.py'
    phrases=None
    if phrase_file.is_file():
        spec=importlib.util.spec_from_file_location('shop_localization_phrases',phrase_file)
        phrases=importlib.util.module_from_spec(spec);spec.loader.exec_module(phrases)
    parent_file=ROOT/'tools/shop_parent_phrases.py'
    parent=None
    if parent_file.is_file():
        spec=importlib.util.spec_from_file_location('shop_parent_phrases',parent_file)
        parent=importlib.util.module_from_spec(spec);spec.loader.exec_module(parent)
    if parent:
        original=phrases
        phrases=SimpleNamespace(
            translate_name=lambda source,kind,words:original.translate_name(source,kind,words) or parent.translate_name(source,[kind],words),
            translate_description=lambda source,words:original.translate_description(source,words) or parent.translate_description(source,words))
    native=json.loads(args.native_names.read_text(encoding='utf-8')) if args.native_names else []
    if isinstance(native,dict):native=native.get('rows',native.get('records',native.get('names',[])))
    extra=collections.defaultdict(dict)
    for file in sorted((ROOT/'mods/shop_localization/translations').glob('*.json')):
        value=json.loads(file.read_text(encoding='utf-8'))
        if value.get('schema_version')!=1:raise ValueError('Unknown authored translation bank')
        for kind in ('name','description','option'):
            for source,text in value.get(kind,{}).items():
                if source in extra[kind] and extra[kind][source]!=text:raise ValueError('Conflicting authored translation: '+source)
                extra[kind][source]=text
    unit_names = None
    unit_file = ROOT/'mods/shop_localization/unit-names.json'
    if unit_file.is_file():
        bank = json.loads(unit_file.read_text(encoding='utf-8'))
        if bank.get('schema_version') != 1 or not isinstance(bank.get('units'),dict):
            raise ValueError('Unsupported stable unit-name bank')
        unit_names = bank['units']
    words,report=compile_words(json.loads(args.terms.read_text(encoding='utf-8')),phrases,native,extra,unit_names)
    args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_bytes(encode(words))
    args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k not in ('unresolved_source_conflicts','untranslated_names','parent_text_missing')},ensure_ascii=False))


if __name__=='__main__':main()
