"""Plan imported roots against proven configuration reachability.

The capsule catalog remains the owner of prices, weights, positions and native
grade patterns. This module only chooses the required acquisition route and
returns reviewable intents; it never mutates machines or player inventories.
"""
from __future__ import annotations

import copy


def _identity(value, label):
    if type(value) is not int or not 0 < value <= 2_147_483_647:
        raise ValueError(label+' must be a positive native integer')
    return value


def route_roots(registrations, reachability, recipes=(), *, family_revision):
    """Return one route for each imported inventory root.

    ``reachability`` is the existing capsule acquisition() result, built from
    the selected host's sources. Recipe presence alone does not make a recipe
    usable: its actual blueprint item and every material need a witness.
    Planned event machines stay separate from active-source witnesses.
    """
    if not isinstance(family_revision,str) or not family_revision:
        raise ValueError('A reviewed content revision is required')
    if not isinstance(reachability,dict) or not isinstance(reachability.get('known'),dict):
        raise ValueError('A host acquisition analysis is required')
    known={int(k):v for k,v in reachability['known'].items()}
    plans={int(k):v for k,v in reachability.get('blueprints',{}).items()}
    if any(not isinstance(v,dict) or not v.get('source') for v in [*known.values(),*plans.values()]):
        raise ValueError('Acquisition sources must retain their witnesses')
    by_output={}
    for recipe in recipes:
        recipe_id=_identity(recipe.get('Idx'),'recipe ID')
        output=_identity(recipe.get('OutUnitID'),'recipe output')
        materials=[]
        for slot in range(1,8):
            unit=recipe.get('UnitID'+str(slot))
            if type(unit) is not int or unit<0:raise ValueError('Invalid recipe material')
            if unit:materials.append(unit)
        if not materials:raise ValueError('A blueprint needs at least one material')
        by_output.setdefault(output,[]).append({
            'recipe_id':recipe_id,'materials':materials,
            'blueprint_available':recipe_id in plans,
            'missing_materials':sorted(set(materials)-known.keys()),
            'blueprint_source':copy.deepcopy(plans.get(recipe_id)),
        })
    result=[];seen=set()
    for entry in registrations:
        root=_identity(entry.get('unit_id'),'inventory root')
        if root in seen:raise ValueError('Duplicate inventory root')
        seen.add(root)
        rank=entry.get('rank')
        if type(rank) is not int or rank not in (1,2,3,4):
            raise ValueError('Unsupported native unit rank')
        if entry.get('server_applied') is not True:
            raise ValueError('Only confirmed host definitions can be routed')
        choices=sorted(by_output.get(root,[]),key=lambda r:r['recipe_id'])
        closed=[r for r in choices if r['blueprint_available'] and not r['missing_materials']]
        route={'unit_id':root,'rank':rank,'subgrade':entry.get('subgrade'),
               'name':entry.get('display_name',str(root)),
               'content_revision':entry.get('revision'),
               'current_source':copy.deepcopy(known.get(root)),
               'recipe_analysis':choices,'changes_applied':False}
        if rank>=4:
            route.update(route='dedicated_activity',dedicated_root=root,
                         artwork_state='required',artwork_minimum=[361,322],
                         reason='Each imported S-rank-or-higher root owns a separate activity machine')
        elif closed:
            chosen=closed[0]
            route.update(route='blueprint',recipe_id=chosen['recipe_id'],
                         material_sources={str(uid):copy.deepcopy(known[uid]) for uid in chosen['materials']},
                         reason='The blueprint item and all materials have configured acquisition paths')
        else:
            route.update(route='ordinary_capsule',reuse_existing=bool(known.get(root) and
                         known[root].get('source') in ('fixed_machines','coin_capsule')),
                         pattern_owner='capsules',
                         reason='No complete obtainable blueprint route; use native grade patterns and weights')
        result.append(route)
    return {'schema_version':1,'policy':'imported-root-acquisition-v1',
            'family_revision':family_revision,'routes':result,'changes_applied':False,
            'limitations':['Configuration reachability is not gameplay acceptance.',
                'Images, prices, probability weights and machine publication require review.',
                'No planned machine is treated as an already active acquisition source.']}
