"""Read directed form relations from complete unit records, without inferring behavior.

A weak component may contain multiple entry variants which share later forms.
They are not necessarily all reachable from one inventory root. This report
preserves that distinction; it never adds a reverse edge or selects an entry.
"""
from __future__ import annotations

import hashlib
import struct


def inspect(records,root_unit_ids=()):
    if not isinstance(records,dict) or len(records)>4096:
        raise ValueError('unit transform record inventory exceeds its budget')
    if any(type(identity)is not int or not 1<=identity<=9999999 for identity in records):
        raise ValueError('unit transform record identity differs')
    if not isinstance(root_unit_ids,(list,tuple)) or len(root_unit_ids)>4096:
        raise ValueError('unit transform root inventory exceeds its budget')
    if any(type(n)is not int or n not in records for n in root_unit_ids) or len(set(root_unit_ids))!=len(root_unit_ids):
        raise ValueError('unit transform roots must name distinct supplied records')
    targets={};nodes=[]
    for identity,raw in sorted(records.items()):
        if (type(identity)is not int or not 1<=identity<=9999999 or not isinstance(raw,bytes) or len(raw)!=4976 or
                raw[4]!=0 or struct.unpack_from('<I',raw)[0]!=identity):
            raise ValueError('unit transform complete record identity/layout differs')
        target=struct.unpack_from('<I',raw,316)[0]
        targets[identity]=target
        nodes.append({'id':identity,'record_sha256':hashlib.sha256(raw).hexdigest(),'transform_target':target,
                      'field_offset':316,'field_bytes':4})
    neighbors={identity:set() for identity in targets}
    external=[]
    for identity,target in targets.items():
        if target in targets:neighbors[identity].add(target);neighbors[target].add(identity)
        elif target:external.append({'source':identity,'target':target})
    components=[];pending=set(targets)
    while pending:
        start=min(pending);stack=[start];members=set()
        while stack:
            identity=stack.pop()
            if identity in members:continue
            members.add(identity);stack.extend(neighbors[identity]-members)
        pending-=members
        components.append({'ids':sorted(members),'selected_roots':sorted(set(root_unit_ids)&members)})
    def trace(start):
        path=[];seen={};cursor=start
        while cursor in targets and cursor not in seen:
            seen[cursor]=len(path);path.append(cursor);cursor=targets[cursor]
        cycle=path[seen[cursor]:] if cursor in seen else []
        return {'root':start,'path':path,'end':'cycle' if cycle else 'external' if cursor else 'no-target',
                'cycle':cycle,'terminal_target':cursor}
    cycles=set();visited=set()
    for start in targets:
        if start in visited:continue
        route=trace(start);visited.update(route['path']);cycle=route['cycle']
        if cycle:
            smallest=cycle.index(min(cycle));cycles.add(tuple(cycle[smallest:]+cycle[:smallest]))
    routes=[]
    for root in root_unit_ids:
        route=trace(root)
        component=next(row['ids'] for row in components if root in row['ids'])
        route['other_component_ids']=sorted(set(component)-set(route['path']))
        routes.append(route)
    return {'schema':'sdgo.unit-transform-graph/1','nodes':nodes,'components':components,
            'root_routes':routes,'directed_cycles':[list(row) for row in sorted(cycles)],'external_targets':external,
            'record_references_closed':not external,'native_behavior_inferred':False,
            'native_registration_authorized':False,'player_inventory_inferred':False}
