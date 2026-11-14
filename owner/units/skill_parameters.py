"""Resolve typed client skill-parameter dependencies before server writes."""
from __future__ import annotations
import struct
from .profiles.skill_parameters_89 import SKILL_REFERENCE_OFFSET,validate_record


def validate_publication(publication,baseline=None):
    records=publication.records
    parameters=records.get(79,{})
    for identity,raw in parameters.items():
        validate_record(raw,identity)
        if baseline is not None:
            original=baseline.record('skill_parameter',identity)
            if original is not None and original!=raw:
                raise ValueError('skill parameter record conflicts with the compatible original; allocate an independent ID: '+str(identity))
            if original is None and callable(getattr(baseline,'any_id',None)) and baseline.any_id(identity):
                raise ValueError('skill parameter ID is occupied by another compatible-original record kind: '+str(identity))
    resolved={};references=[];missing=set()
    occupied={identity:kind for kind,rows in records.items()for identity in rows}
    for skill,raw in records.get(13,{}).items():
        if not isinstance(raw,bytes)or len(raw)!=1132:raise ValueError('skill parameter reference requires a complete skill record')
        identity=struct.unpack_from('<I',raw,SKILL_REFERENCE_OFFSET)[0]
        if not identity:continue
        if identity>9_999_999:raise ValueError('skill parameter reference exceeds the native ID domain')
        references.append({'skill':skill,'kind':79,'id':identity,'offset':SKILL_REFERENCE_OFFSET})
        if identity in resolved or identity in missing:continue
        if identity in occupied and occupied[identity]!=79:
            raise ValueError('skill parameter ID is occupied by another published record kind: '+str(identity))
        value=parameters.get(identity);source='published'
        if value is None and baseline is not None:
            value=baseline.record('skill_parameter',identity);source='compatible-original'
        if value is None:missing.add(identity);continue
        resolved[identity]={**validate_record(value,identity),'source':source}
    if missing:raise ValueError('skill parameter records (kind 79) are missing: '+', '.join(map(str,sorted(missing)[:16]))+
                                '; provide complete records or a compatible original before application')
    return {'records':resolved,'references':references,'complete':True,'actor_accepted':False}
