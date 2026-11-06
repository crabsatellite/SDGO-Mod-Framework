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
import zlib

try:
    from . import game_test
except ImportError:
    import game_test

from manager.engines.mission_behavior import profile_codec


BATTLE_HOST = 'BattleServer-548A891D-0046C000'
STEPS = ('request_accepted', 'setup_complete', 'battle_active', 'battle_end')
DAMAGE_EVENT = 'native_effective_hp_loss'
DAMAGE_SOURCES = ('battle_rewards_bridge', 'direct_native_hook')
MISSION_PROFILE_EVENT = 'mission_profile_ready'
MISSION_CREATION_EVENT = 'mission_profile_creation'
MISSION_PROFILE_RELATIVE = 'sdgo/mods/mission_behavior/profile.bin'


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


def participants(event, pilot, expected_ai, setup,expected_allies=None,expected_humans=None,cooperative=False,layout=None):
    expected=human_roster(pilot,expected_humans)
    if len(expected)+expected_ai>8:raise ValueError('expected participants exceed native test room capacity')
    if layout not in (None,'allied') or (layout=='allied' and expected_ai!=6):
        raise ValueError('invalid sealed AI layout')
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
        allies=(5 if layout=='allied' else expected_ai//2) if expected_allies is None else expected_allies
        if teams[own][1]!=allies or teams[3-own][1]!=expected_ai-allies:
            raise ValueError('native ally/enemy placement disagrees with plan')
    return ids


def damage_diagnostic(event, ready, pilot, modes):
    if event.get('event') != DAMAGE_EVENT:
        return False
    source=event.get('source')
    if (ready.get('battle_ai_deaths') is not True or source not in DAMAGE_SOURCES or
        source!=ready.get('death_source')):
        raise ValueError('native damage diagnostic source does not match observer startup')
    integer(event.get('sequence'),'native damage sequence',1)
    integer(event.get('qpc'),'native damage timestamp',1)
    integer(event.get('thread_id'),'native damage thread',1)
    integer(event.get('room_cookie'),'native damage room cookie',1)
    if (integer(event.get('host_pilot_id'),'native damage room host',1)!=pilot or
        integer(event.get('mode'),'native damage room mode',1) not in modes):
        raise ValueError('native damage diagnostic room does not match the client plan')
    return True


def sealed_mission_profile(metadata, profile, mission_id):
    client=metadata.get('client')
    if not isinstance(client,dict):
        raise ValueError('sealed client metadata is missing')
    hashes=client.get('immutable_hashes')
    expected_hash=hashes.get(MISSION_PROFILE_RELATIVE) if isinstance(hashes,dict) else None
    if (not isinstance(expected_hash,str) or len(expected_hash)!=64 or
        any(c not in '0123456789abcdefABCDEF' for c in expected_hash)):
        raise ValueError('mission profile is not sealed by immutable metadata')
    root=client.get('root')
    if not isinstance(root,str) or not root:
        raise ValueError('sealed client root is missing')
    path=Path(root)/Path(MISSION_PROFILE_RELATIVE)
    raw=path.read_bytes()
    actual_hash=hashlib.sha256(raw).hexdigest()
    if actual_hash!=expected_hash.lower():
        raise ValueError('mission profile differs from the immutable client payload')
    decoded=profile_codec.decode_bundle(raw)
    selected_key='oc' if profile=='oc' else 'baseline'
    selected_rows=[
        entry[selected_key] for entry in decoded['rows']
        if entry['baseline']['MissionID']==mission_id
    ]
    all_rows=[entry[selected_key] for entry in decoded['rows']]
    packed=b''.join(profile_codec.ROW.pack(
        *(row[field] for field in profile_codec.FIELDS)) for row in all_rows)
    payload_crc32=zlib.crc32(raw[profile_codec.HEADER.size:]) & 0xffffffff
    return {
        'path':path,
        'sha256':actual_hash,
        'entry_count':decoded['entry_count'],
        'selected_mission_rows':len(selected_rows),
        'payload_crc32':payload_crc32,
        'native_rows_crc32':zlib.crc32(packed) & 0xffffffff,
        'selected_rows':{
            row['MissionNpcID']:row for row in selected_rows
        },
    }


def evaluate_mission_behavior(metadata: dict, paths: list[Path]) -> dict:
    plan=metadata['plan'];run_id=metadata['run_id']
    session=metadata.get('server_session')
    server_run_id=session['run_id'] if session else run_id
    profile=plan.get('mission_behavior_profile')
    profile_value={'oc':1,'gco':2}.get(profile)
    mission_id=plan.get('mission_game_mode_id')
    if profile_value is None or mission_id not in (88006,88009):
        raise ValueError('invalid sealed mission behavior plan')
    expected_state=1 if profile_value==1 else 2
    errors=[];sources=[];profiles=[];creations=[];seen_paths=set()
    sealed=None
    try:
        sealed=sealed_mission_profile(metadata,profile,mission_id)
    except (OSError,ValueError,TypeError,KeyError) as error:
        errors.append(f'{MISSION_PROFILE_RELATIVE}: {error}')
    for path in paths if sealed is not None else ():
        path=path.resolve()
        if path in seen_paths:continue
        seen_paths.add(path)
        try:
            raw=path.read_bytes()
            records=[json.loads(line) for line in raw.decode('utf-8').splitlines()
                     if line.strip()]
            if len(records)<2 or not all(isinstance(record,dict) for record in records):
                raise ValueError('mission server observer stream is incomplete')
            if any(record.get('run_id')!=server_run_id or
                   record.get('role')!='server' or
                   record.get('suite')!='native-mission-behavior'
                   for record in records):
                raise ValueError('mission observer stream belongs to another run or role')
            profile_event=records[0]
            ready=records[1]
            if (profile_event.get('event')!=MISSION_PROFILE_EVENT or
                integer(profile_event.get('profile'),'mission profile')!=profile_value or
                integer(profile_event.get('state'),'mission profile state')!=expected_state or
                integer(profile_event.get('entries'),'mission profile entries',1)!=sealed['entry_count'] or
                integer(profile_event.get('verified_rows'),'verified mission rows',1)!=sealed['entry_count'] or
                integer(profile_event.get('selected_mission_rows'),'selected mission rows')!=sealed['selected_mission_rows'] or
                integer(profile_event.get('baseline_matches'),'baseline mission rows',1)!=sealed['entry_count'] or
                integer(profile_event.get('applied'),'applied mission rows')!=(sealed['entry_count'] if profile_value==1 else 0)):
                raise ValueError('mission profile readiness does not match the sealed selection')
            native_crc=integer(
                profile_event.get('native_rows_crc32'),
                'native mission row CRC')
            if (native_crc!=sealed['native_rows_crc32'] or integer(
                    profile_event.get('verified_rows_crc32'),
                    'verified mission row CRC')!=sealed['native_rows_crc32']):
                raise ValueError('mission row CRC readback differs from production status')
            if integer(
                    profile_event.get('payload_crc32'),
                    'mission payload CRC')!=sealed['payload_crc32']:
                raise ValueError('mission payload CRC differs from the sealed profile')
            integer(profile_event.get('ready_epochs'),'mission ready epoch',1)
            if (ready.get('event')!='observer_ready' or
                ready.get('host_key')!=BATTLE_HOST or
                ready.get('normal_server_main') is not True):
                raise ValueError('mission observer startup follows profile verification')
            pid=integer(ready.get('pid'),'native server PID',1)
            if session and pid not in session['pids']:
                raise ValueError('reused mission server process differs from the sealed session')
            source={'path':str(path),'sha256':hashlib.sha256(raw).hexdigest(),
                    'pid':pid,'host_key':BATTLE_HOST}
            sources.append(source)
            profiles.append({
                'pid':pid,'profile':profile,'state':expected_state,
                'entries':sealed['entry_count'],
                'selected_mission_rows':sealed['selected_mission_rows'],
                'payload_crc32':sealed['payload_crc32'],
                'native_rows_crc32':native_crc,
                'profile_sha256':sealed['sha256'],
            })
            last_sequence=0
            for event in records[2:]:
                if event.get('event')!=MISSION_CREATION_EVENT:
                    raise ValueError('unexpected mission behavior observer event')
                sequence=integer(event.get('sequence'),'mission creation sequence',1)
                if sequence<=last_sequence:
                    raise ValueError('mission creation receipts are not monotonic')
                last_sequence=sequence
                mission_npc_id=integer(
                    event.get('mission_npc_id'),'mission NPC row',1)
                npc_id=integer(event.get('npc_id'),'created NPC',1)
                expected_row=sealed['selected_rows'].get(mission_npc_id)
                if (integer(event.get('profile'),'creation profile')!=profile_value or
                    integer(event.get('tracked'),'tracked creation')!=1 or
                    integer(event.get('profile_match'),'creation profile match')!=1 or
                    integer(event.get('original_result'),'creation original result',1)<1 or
                    integer(event.get('mission_id'),'created mission')!=mission_id or
                    expected_row is None or
                    npc_id!=expected_row['NpcID'] or
                    integer(event.get('native_pilot_id'),'created native pilot',1)<1 or
                    integer(event.get('native_unit_hp'),'created native unit HP',1)<1):
                    raise ValueError('native mission creation did not consume the sealed row')
                creations.append({
                    'pid':pid,'sequence':sequence,
                    'mission_npc_id':mission_npc_id,
                    'mission_id':mission_id,'npc_id':npc_id,
                })
        except (OSError,UnicodeError,json.JSONDecodeError,ValueError,TypeError,KeyError) as error:
            errors.append(f'{path}: {error}')
    if not sources:
        errors.append('no native BattleServer mission profile stream was accepted')
    if mission_id==88009 and not creations:
        errors.append('M2 requires a genuine native creation consuming one sealed profile row')
    return {
        'schema':1,'run_id':run_id,
        'scope':'passive_native_mission_profile',
        'status':'fail' if errors else 'pass',
        'mission_behavior_profile':profile,
        'mission_game_mode_id':mission_id,
        'sources':sources,'profiles':profiles,
        'creations':creations,'errors':errors,
    }


def evaluate(metadata: dict, paths: list[Path], pilot: int, *, expected_humans: list[int] | None = None,
             expected_rounds: int | None = None) -> dict:
    integer(pilot,'expected host pilot',1)
    plan=metadata['plan'];run_id=metadata['run_id']
    if 'mission_behavior_profile' in plan:
        if expected_humans is not None or expected_rounds is not None:
            raise ValueError('mission behavior server reports do not accept PvP overrides')
        return evaluate_mission_behavior(metadata,paths)
    rounds=2 if plan.get('same_room_rematch') else 1
    if expected_rounds is not None:
        if type(expected_rounds) is not int or expected_rounds not in (1,2) or expected_rounds<rounds:
            raise ValueError('expected_rounds must be one or two complete battles and cannot reduce sealed scope')
        rounds=expected_rounds
    session=metadata.get('server_session')
    server_run_id=session['run_id'] if session else run_id
    server_nonce=session['nonce'] if session else plan['title_nonce']
    modes=game_test.parse_mode_sequence(plan['mode_sequence'])
    expected_ai=integer(plan['expected_ai'],'expected AI',1)
    sealed_humans=plan.get('human_pilot_ids')
    if expected_humans is not None and sealed_humans is not None and human_roster(pilot,expected_humans)!=human_roster(pilot,sealed_humans):
        raise ValueError('declared human roster differs from the sealed plan')
    if expected_humans is None:expected_humans=sealed_humans
    human_roster(pilot,expected_humans)
    errors=[];battles=[];sources=[];seen_paths=set()
    for path in paths:
        path=path.resolve()
        if path in seen_paths:continue
        seen_paths.add(path)
        current={};last_sequence=0;last_qpc=0
        try:
            layout=plan.get('ai_layout')
            if 'ai_layout' in plan and (
                type(layout) is not str or layout!='allied' or expected_ai!=6 or
                plan.get('ai_difficulty')!='Hard' or 'ai_pool_level' in plan
            ):
                raise ValueError('sealed allied AI layout requires six Hard bots')
            raw=path.read_bytes()
            records=[json.loads(line) for line in raw.decode('utf-8').splitlines() if line.strip()]
            if not records:raise ValueError('empty server observer stream')
            if not all(isinstance(record,dict) for record in records):raise ValueError('server observer records must be objects')
            ready=records[0]
            if any(r.get('run_id')!=server_run_id or r.get('role')!='server' or r.get('suite')!='pvp-ai' for r in records):
                raise ValueError('server observer stream belongs to another run or role')
            minimum_ai=integer(ready.get('minimum_ai'),'observer AI count',1)
            if (ready.get('event')!='observer_ready' or ready.get('host_key')!=BATTLE_HOST or
                ready.get('normal_server_main') is not True or ready.get('require_end') is not True or
                ready.get('nonce')!=server_nonce or integer(ready.get('expected_host_pilot_id'),'observer host',1)!=pilot or
                (minimum_ai>expected_ai if session else minimum_ai!=expected_ai)):
                raise ValueError('server observer startup binding does not match the client plan')
            pid=integer(ready.get('pid'),'native server PID',1)
            if session and pid not in session['pids']:raise ValueError('reused server process differs from the sealed session')
            sources.append({'path':str(path),'sha256':hashlib.sha256(raw).hexdigest(),'pid':pid,'host_key':BATTLE_HOST})
            for event in records[1:]:
                # A reusable server keeps earlier rooms in its append-only
                # native log. The current sealed title contains a fresh nonce.
                if session and event.get('title')!=plan['room_title']:continue
                name=event.get('event')
                if damage_diagnostic(event,ready,pilot,modes):continue
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
                    expected_humans,plan.get('ai_teams')=='coop',layout=layout)
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
        expected=rounds
        if len(matches)!=expected or any(b['phase']!=4 for b in matches):
            errors.append(f'mode {mode} requires exactly {expected} complete four-event native battle lifecycle(s)')
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
