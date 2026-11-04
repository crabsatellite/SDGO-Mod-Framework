"""Native activity machine numbers, separate from pinned and legacy IDs."""
from __future__ import annotations

STANDARD_NUMBER_FIRST=1
STANDARD_NUMBER_LAST=9899
LEGACY_MACHINE_FIRST=1_990_001
LEGACY_MACHINE_LAST=1_990_999


def is_supported_machine_id(value):
    if type(value) is not int:return False
    return (STANDARD_NUMBER_FIRST<=value//100<=STANDARD_NUMBER_LAST and value%100==1
            or LEGACY_MACHINE_FIRST<=value<=LEGACY_MACHINE_LAST)


def machine_id_from_number(number):
    if type(number) is not int or not STANDARD_NUMBER_FIRST<=number<=STANDARD_NUMBER_LAST:
        raise ValueError('普通活动机编号须为1..9899；9900及以上保留为置顶号段')
    return number*100+1


def allocate_activity_id(occupied,*,listed=()):
    """Append to listed ordinary machines, ignoring pinned/legacy display IDs."""
    occupied=set(occupied)
    ordinary=[mid//100 for mid in listed if type(mid) is int and mid%100==1
              and STANDARD_NUMBER_FIRST<=mid//100<=STANDARD_NUMBER_LAST]
    for number in range(max(ordinary,default=0)+1,STANDARD_NUMBER_LAST+1):
        identity=machine_id_from_number(number)
        if identity not in occupied:return identity
    raise ValueError('普通活动机编号已用满；不会占用置顶号段')
