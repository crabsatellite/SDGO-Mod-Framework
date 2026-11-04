"""Server-local defaults, with explicit per-instance UTC-offset overrides."""
from __future__ import annotations
import datetime as dt

def local_offset_minutes():
    return int(dt.datetime.now().astimezone().utcoffset().total_seconds() // 60)

def validate_offset(value):
    if value is not None and (type(value) is not int or not -720 <= value <= 840):
        raise ValueError('时区偏移须为 -720..840 的整数，或留空跟随服务器本地')
    return value

def effective_offset(value=None):
    validate_offset(value)
    return local_offset_minutes() if value is None else value

