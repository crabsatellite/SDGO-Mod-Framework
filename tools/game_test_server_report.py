"""Validate passive native Battle observer evidence for every requested mode.

This reader never launches a process or sends a game request. The server's
single-scenario summary cannot substitute for the per-battle raw lifecycle in
a client matrix, so this report groups and validates all four native events.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

try:
    from . import game_test
except ImportError:
    import game_test


BATTLE_HOST = 'BattleServer-548A891D-0046C000'
STEPS = ('request_accepted', 'setup_complete', 'battle_active', 'battle_end')


def integer(value, name, minimum=0):
    if type(value) is not int or value < minimum:
        raise ValueError(f'invalid {name}')
    return value


def human_roster(pilot, expected_humans=None):
    roster=[pilot] if expected_humans is None else expected_humans
    if not isinstance(roster,(list,tuple)) or not 1<=len(roster)<=8:
        raise ValueError('invalid expected human roster')
    if any(type(who) is not int or not 0<who<0x80000000 for who in roster):
        raise ValueError('invalid expected human pilot')
    if len(set(roster))!=len(roster) or pilot not in roster:
        raise ValueError('expected human roster must uniquely include the host')
    return set(roster)


def participants(event, pilot, expected_ai, setup,expected_allies=None,expected_humans=None,cooperative=False):
    expected=human_roster(pilot,expected_humans)
    if len(expected)+expected_ai>8:raise ValueError('expected participants exceed native test room capacity')
    slots=event.get('slots')
    if not isinstance(slots,list):raise ValueError('missing native participant slots')
    ids=set(); seats=set(); humans=set(); bots=0; teams={1:[0,0],2:[0,0]}
    for slot in slots:
        who=integer(slot.get('pilot_id'),'participant pilot',1)
        seat=integer(slot.get('slot'),'participant slot')
        team=integer(slot.get('team'),'participant team',1)
        if who in ids or seat in seats or seat>=12 or team not in teams:
            raise ValueError('ambiguous native participant identity')
        if type(slot.get('is_ai')) is not bool:raise ValueError('invalid native AI flag')
        if who>0xFFFFFFFF or bool(who&0x80000000)!=slot['is_ai']:
            raise ValueError('native AI flag disagrees with pilot identity')
        integer(slot.get('unit_or_npc_id'),'participant unit',1)
        ids.add(who);seats.add(seat)
        if slot['is_ai']:bots+=1;teams[team][1]+=1
        else:
            if who not in expected:raise ValueError('unexpected human pilot in isolated battle')
            humans.add(who);teams[team][0]+=1
    if humans!=expected or bots!=(expected_ai if setup else 0):raise ValueError('native participant counts disagree with plan')
    if integer(event.get('active_slots'),'active slots')!=len(slots):raise ValueError('active-slot total mismatch')
    for team in (1,2):
        if integer(event.get(f'humans_team_{team}'),'human team count')!=teams[team][0] or integer(event.get(f'ai_team_{team}'),'AI team count')!=teams[team][1]:
            raise ValueError('native team totals disagree with slots')
    own=next(s['team'] for s in slots if s['pilot_id']==pilot and not s['is_ai'])
    if cooperative and any(s['team']!=own for s in slots if not s['is_ai']):
        raise ValueError('cooperative humans did not follow the host team')
    if setup:
        allies=expected_ai//2 if expected_allies is None else expected_allies
        if teams[own][1]!=allies or teams[3-own][1]!=expected_ai-allies:
            raise ValueError('native ally/enemy placement disagrees with plan')
    return ids


def evaluate(metadata: dict, paths: list[Path], pilot: int) -> dict:
    integer(pilot,'expected host pilot',1)
    plan=metadata['plan'];run_id=metadata['run_id']
    session=metadata.get('server_session')
    server_run_id=session['run_id'] if session else run_id
    server_nonce=session['nonce'] if session else plan['title_nonce']
    modes=game_test.parse_mode_sequence(plan['mode_sequence'])
    expected_ai=integer(plan['expected_ai'],'expected AI',1)
    expected_humans=plan.get('human_pilot_ids')
    human_roster(pilot,expected_humans)
    errors=[];battles=[];sources=[];seen_paths=set()
    for path in paths:
        path=path.resolve()
        if path in seen_paths:continue
        seen_paths.add(path)
        current={};last_sequence=0;last_qpc=0
        try:
            raw=path.read_bytes()
            records=[json.loads(line) for line in raw.decode('utf-8').splitlines() if line.strip()]
            if not records:raise ValueError('empty server observer stream')
            if not all(isinstance(record,dict) for record in records):raise ValueError('server observer records must be objects')
            ready=records[0]
            if any(r.get('run_id')!=server_run_id or r.get('role')!='server' or r.get('suite')!='pvp-ai' for r in records):
                raise ValueError('server observer stream belongs to another run or role')
            if (ready.get('event')!='observer_ready' or ready.get('host_key')!=BATTLE_HOST or
                ready.get('normal_server_main') is not True or ready.get('require_end') is not True or
                ready.get('nonce')!=server_nonce or integer(ready.get('expected_host_pilot_id'),'observer host',1)!=pilot or
                integer(ready.get('minimum_ai'),'observer AI count',1)!=expected_ai):
                raise ValueError('server observer startup binding does not match the client plan')
            pid=integer(ready.get('pid'),'native server PID',1)
            if session and pid not in session['pids']:raise ValueError('reused server process differs from the sealed session')
            sources.append({'path':str(path),'sha256':hashlib.sha256(raw).hexdigest(),'pid':pid,'host_key':BATTLE_HOST})
            for event in records[1:]:
                # A reusable server keeps earlier rooms in its append-only
                # native log. The current sealed title contains a fresh nonce.
                if session and event.get('title')!=plan['room_title']:continue
                name=event.get('event')
                if name not in STEPS:raise ValueError('unexpected or failed native observer event')
                sequence=integer(event.get('sequence'),'native sequence',1)
                qpc=integer(event.get('qpc'),'native timestamp',1)
                integer(event.get('thread_id'),'native thread',1)
                if sequence<=last_sequence or qpc<last_qpc:raise ValueError('native event ordering is not monotonic')
                last_sequence=sequence;last_qpc=qpc
                mode=integer(event.get('mode'),'native room mode',1)
                cookie=integer(event.get('room_cookie'),'native room cookie',1)
                if (mode not in modes or event.get('title')!=plan['room_title'] or
                    integer(event.get('host_pilot_id'),'room host',1)!=pilot or integer(event.get('status_code'),'native result')!=0):
                    raise ValueError('native room identity, result or mode does not match plan')
                if name==STEPS[0]:
                    if cookie in current and current[cookie]['phase']!=4:raise ValueError('room identity reused before battle ended')
                    battle={'mode':mode,'room_cookie':cookie,'pid':pid,'phase':0,'events':[], 'qpc_begin':qpc}
                    current[cookie]=battle;battles.append(battle)
                battle=current.get(cookie)
                if not battle or battle['mode']!=mode or battle['phase']>=4 or STEPS[battle['phase']]!=name:
                    raise ValueError('native battle lifecycle is missing a step or out of order')
                ids=participants(event,pilot,expected_ai,name!=STEPS[0],plan.get('ai_allies'),
                    expected_humans,plan.get('ai_teams')=='coop')
                roles=sorted((s['pilot_id'],s['team'],s['is_ai']) for s in event['slots'])
                if name==STEPS[1]:
                    battle['participant_ids']=sorted(ids)
                    battle['participant_roles']=roles
                if name in STEPS[2:] and sorted(ids)!=battle['participant_ids']:
                    raise ValueError('native participants changed across the active battle')
                if name in STEPS[2:] and roles!=battle['participant_roles']:
                    raise ValueError('native participant teams changed across the active battle')
                battle['events'].append(name);battle['phase']+=1
                if name==STEPS[3]:
                    battle['qpc_end']=qpc
                    battle['scores']=[integer(event.get(f'score_team_{team}'),'final native score') for team in (1,2)]
        except (OSError,UnicodeError,json.JSONDecodeError,ValueError,TypeError,KeyError) as error:
            errors.append(f'{path}: {error}')
    for mode in modes:
        matches=[b for b in battles if b['mode']==mode]
        if len(matches)!=1 or matches[0]['phase']!=4:
            errors.append(f'mode {mode} requires exactly one complete four-event native battle lifecycle')
    return {'schema':1,'run_id':run_id,'scope':'passive_native_server_lifecycle',
            'status':'fail' if errors else 'pass','expected_host_pilot_id':pilot,
            'expected_human_pilot_ids':sorted(human_roster(pilot,expected_humans)),
            'required_modes':list(modes),'sources':sources,'battles':battles,'errors':errors}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-dir',type=Path,required=True)
    parser.add_argument('--events',type=Path,action='append',required=True)
    parser.add_argument('--host-pilot',type=int,required=True)
    args=parser.parse_args()
    metadata=game_test.validate_metadata(args.run_dir)
    game_test.verify_hashes(metadata)
    report=evaluate(metadata,args.events,args.host_pilot)
    game_test.atomic_write(args.run_dir/'receipts/server-report.json',json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    return 0 if report['status']=='pass' else 1


if __name__=='__main__':raise SystemExit(main())
