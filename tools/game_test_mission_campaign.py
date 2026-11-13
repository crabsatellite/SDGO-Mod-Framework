"""Sealed inputs for an ordinary imported-mission native campaign.

These fields select observations and finite normal input. They do not provide
unit positions, HP, phase state, packet injection or a synthetic mission result.
"""
from __future__ import annotations

import ipaddress
import json
from pathlib import Path
import re

DEFAULTS = {'schema': 1, 'guard_npc': 0, 'party_phase': 6, 'skip': False,
            'skip_delay_ms': 2000, 'capture': True, 'contact_ms': 20000,
            'tab_capture': True}
FIELDS = {'target_npc', *DEFAULTS}
RUN_FIELDS = {'mission_campaign', *('mission_campaign_' + key for key in FIELDS - {'schema'})}
BOOLS = {'skip', 'capture', 'tab_capture'}
LIMITS = {'target_npc': (1, 0x7FFFFFFF), 'guard_npc': (0, 0x7FFFFFFF),
          'party_phase': (1, 8), 'skip_delay_ms': (0, 5000), 'contact_ms': (0, 120000)}


def validate(value, context):
    if not isinstance(value, dict) or set(value) - FIELDS or 'target_npc' not in value:
        raise ValueError('mission_campaign requires target_npc and only declared campaign fields')
    result = {**DEFAULTS, **value}
    if type(result['schema']) is not int or result['schema'] != 1:
        raise ValueError('mission_campaign schema must be 1')
    for key in BOOLS:
        if type(result[key]) is not bool:
            raise ValueError('mission_campaign ' + key + ' must be boolean')
    for key, (minimum, maximum) in LIMITS.items():
        if type(result[key]) is not int or not minimum <= result[key] <= maximum:
            raise ValueError('mission_campaign ' + key + ' is outside its integer range')
    if result['guard_npc'] == result['target_npc']:
        raise ValueError('mission_campaign guard and target must differ')
    if ('fps_cap' in context or context.get('mission_mode') is None or not context.get('mission_game_mode_id')
            or context.get('mission_behavior_profile') or context.get('mission_respawns')):
        raise ValueError('mission_campaign requires an exact ordinary imported mission')
    if context.get('mission_ai_count') not in (0, 3):
        raise ValueError('mission_campaign requires zero or three registered wingmen')
    if any(context.get(key) for key in ('bootstrap_only', 'battleship_only', 'gift_requests_only',
            'deck_only', 'pages_only', 'friends_only', 'room_titles_only', 'collection_draw',
            'random_loadout', 'unit_scenario', 'operator_flow', 'review_footage')):
        raise ValueError('mission_campaign cannot combine another native scenario')
    try:
        local = ipaddress.IPv4Address(context['target_host']).is_loopback
    except (KeyError, ValueError):
        local = False
    if not local:
        raise ValueError('mission_campaign must use the isolated loopback backend')
    if result['tab_capture'] and (not result['capture'] or context['mission_ai_count'] != 3):
        raise ValueError('mission_campaign TAB capture requires capture and three wingmen')
    return result


def load(path):
    path = Path(path)
    if not path.is_file() or path.stat().st_size > 8192:
        raise ValueError('mission_campaign JSON must be a file of at most 8192 bytes')
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('duplicate mission_campaign JSON key')
            result[key] = value
        return result
    return json.loads(path.read_text(encoding='utf-8-sig'), object_pairs_hook=unique)


def run_fields(value, context):
    value = validate(value, context)
    result = {'mission_campaign': '1'}
    for key in FIELDS - {'schema'}:
        result['mission_campaign_' + key] = str(int(value[key]))
    return result


def read(run, context):
    present = {key for key in run if key.startswith('mission_campaign')}
    if not present:
        return None
    if present != RUN_FIELDS or run['mission_campaign'] != '1':
        raise ValueError('mission_campaign run fields must be complete and canonical')
    value = {'schema': 1}
    for key in FIELDS - {'schema'}:
        raw = run['mission_campaign_' + key]
        if not re.fullmatch(r'0|[1-9][0-9]*', raw):
            raise ValueError('mission_campaign run integers must be canonical')
        if key in BOOLS:
            if raw not in ('0', '1'):
                raise ValueError('mission_campaign run booleans must be 0 or 1')
            value[key] = raw == '1'
        else:
            value[key] = int(raw)
    return validate(value, context)


def settings(value):
    if value is None:
        return {}
    return {'BattleAiDeaths': '1', 'BattleAiTeleport': '0',
            'BattlePlayerCombat': '1', 'BattlePlayerCombatContinuous': '1',
            'BattlePlayerSecondary': '1',
            # Select the normal second weapon through native input. The probe
            # refuses a missing/melee slot and verifies the live binding/ammo.
            'BattlePlayerWeaponSlot': '1',
            'BattlePlayerMissionAdvance': '1' if value['contact_ms'] else '0',
            'BattlePlayerMissionAdvanceMs': str(value['contact_ms'])}
