"""Pair editor publications with the existing directed-graph server importer.

These requests describe records and inventory entries, never host authority.
The server controller derives authority from independently installed SQL/ledgers.
"""
from __future__ import annotations

import hashlib
import json
import struct


def requires_graph(family):
    return len(family['forms'])>2 or family.get('behavior_profile') in (
        'native-directed-forms-v1','native-three-stage-forms-v1','native-entry-forms-v1')


def build(families, plans, catalog, revision):
    """Return portable server-publication members, or no members for native pairs."""
    if not any(requires_graph(family) for family in families):return {}
    from .indexes import catalog_records
    from . import batch_server
    records={identity:raw for identity,(kind,raw) in catalog_records(catalog).items() if kind==2}
    by_key={family['family_key']:family for family in families}
    if len(by_key)!=len(families):raise ValueError('形态图机体族标识重复')
    by_id={}
    for key,family in by_key.items():
        for form in family['forms']:
            identity=form['id']
            if identity in by_id:raise ValueError('形态图记录被多个机体族占用')
            by_id[identity]=key
    neighbors={identity:set() for identity in records}
    targets={identity:struct.unpack_from('<I',raw,316)[0] for identity,raw in records.items()}
    for identity,target in targets.items():
        if target in records:neighbors[identity].add(target);neighbors[target].add(identity)
    pending=set(records);requests=[]
    while pending:
        members=set();stack=[min(pending)]
        while stack:
            identity=stack.pop()
            if identity not in members:members.add(identity);stack.extend(neighbors[identity]-members)
        pending-=members
        keys=sorted({by_id[identity] for identity in members})
        if not any(requires_graph(by_key[key]) for key in keys):continue
        if any(targets[identity] and targets[identity] not in members for identity in members):
            raise ValueError('请把形态图引用的全部形态放入同一工作区并发布；不能从未配对的外部记录推断服务端授权')
        if any({form['id'] for form in by_key[key]['forms']}-members for key in keys):
            raise ValueError('同一机体族包含不连通的形态，不能拆成隐式库存入口')
        requests.append({'schema':'sdgo.unit-form-graphs/1','owner':keys[0],'families':keys,
            'revision':revision,'roots':sorted(by_key[key]['root_unit_id'] for key in keys),
            'records':[{'id':identity,'client_sha256':hashlib.sha256(records[identity]).hexdigest()}
                       for identity in sorted(members)]})
    owner='unit-import.editor'
    plan={'schema':batch_server.PLAN_SCHEMA,'owner':owner,'revision':revision,
        'families':[{key:value for key,value in plan.items() if key!='revision'} for plan in plans],
        'skills':{'schema':'sdgo.unit-server-dependencies/1','owner':owner,'revision':revision,'skills':[]},
        'funnels':{'schema':'sdgo.unit-server-funnels/1','owner':owner,'revision':revision,'funnels':[]},
        'affiliations':[]}
    graphs={'schema':batch_server.GRAPH_SCHEMA,'owner':owner,'revision':revision,'requests':requests}
    # Reuse the actual consumer, rather than maintain an editor-only graph format.
    batch_server._validate_plan(plan)
    batch_server._validate_graphs(graphs,plan,{2:records})
    def encode(value):return json.dumps(value,ensure_ascii=False,indent=2).encode('utf-8')
    return {'server-plan.json':encode(plan),'form-graph-requests.json':encode(graphs),
        batch_server.CATALOG:catalog,
        'package/manifest.json':encode({'schema_version':1,'revision':revision,
            'files':{'client/sdgo/mods/unit_catalog/catalog.bin':hashlib.sha256(catalog).hexdigest()}})}
