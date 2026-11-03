"""Conservative phrase and template translations for SDGO shop text."""
from __future__ import annotations

import json
from collections.abc import Mapping
from functools import lru_cache
from pathlib import Path
import re


PHRASES_PATH = Path(__file__).resolve().parents[1] / "mods" / "shop_localization" / "phrases.json"

_NUMBER = re.compile(r"\d+(?:[,.]\d+)*%*")
_EVENT_KO = re.compile(r"^\(이벤트\)\s*")
_EVENT_EN = re.compile(r"^\(Event\)\s*", re.IGNORECASE)
_TRUNCATED_MISC = re.compile(r"(?:\bSyste|\bVouche|\btic|\bexchang|\bExchange C)$", re.IGNORECASE)

_KO_COLORS = {
    "어두운연두색": "深黄绿色",
    "아이보리색": "象牙色",
    "진한녹색": "深绿色",
    "밝은녹색": "亮绿色",
    "국방색": "军绿色",
    "연두색": "黄绿色",
    "보라색": "紫色",
    "주황색": "橙色",
    "분홍색": "粉色",
    "노란색": "黄色",
    "검정색": "黑色",
    "빨간색": "红色",
    "하늘색": "天蓝色",
    "청록색": "青绿色",
    "청회색": "蓝灰色",
    "파란색": "蓝色",
    "녹색": "绿色",
    "회색": "灰色",
    "흰색": "白色",
    "갈색": "棕色",
    "바다색": "海蓝色",
    "금색": "金色",
    "남색": "深蓝色",
    "옥색": "玉绿色",
}

_EN_COLORS = {
    "Dark Yellow-Green": "深黄绿色",
    "Dark Yello-Green": "深黄绿色",
    "Light Jade Green": "浅玉绿色",
    "Navy Purple": "深紫色",
    "Golden Yellow": "金黄色",
    "Light Green": "浅绿色",
    "Bright Green": "亮绿色",
    "Yellow Brown": "黄棕色",
    "Yellow-brown": "黄棕色",
    "Bright red": "鲜红色",
    "Bright Red": "鲜红色",
    "Light Red": "浅红色",
    "Dark Green": "深绿色",
    "Dark Red": "深红色",
    "Red Brown": "红棕色",
    "Blue Black": "蓝黑色",
    "Blue Gray": "蓝灰色",
    "Blue Grey": "蓝灰色",
    "Rouge Gray": "Rouge Gray",
    "Sky Blue": "天蓝色",
    "Skyblue": "天蓝色",
    "Light Blue": "浅蓝色",
    "Light  Blue": "浅蓝色",
    "Pure White": "纯白色",
    "Unique Black": "特制黑色",
    "Ultramarine": "群青色",
    "Turquoise": "青绿色",
    "Military": "军绿色",
    "Jade Green": "玉绿色",
    "Grey": "灰色",
    "Gray": "灰色",
    "Yellow": "黄色",
    "Purple": "紫色",
    "Orange": "橙色",
    "Black": "黑色",
    "White": "白色",
    "Green": "绿色",
    "Brown": "棕色",
    "Plum": "梅红色",
    "Pink": "粉色",
    "Gold": "金色",
    "Blue": "蓝色",
    "Navy": "深蓝色",
    "Ivory": "象牙色",
    "Red": "红色",
}

_KO_STICKERS = {
    "삼각형": "三角形",
    "표적": "标靶",
    "로캣": "火箭",
    "이펙트1": "特效1",
    "거미줄": "蜘蛛网",
    "섬광": "闪光",
    "드래곤": "龙",
    "사자 문장": "狮子纹章",
    "주먹 문장": "拳头纹章",
    "지구연방": "地球联邦",
    "지구연방군2": "地球联邦军2",
    "지구연방군3": "地球联邦军3",
    "지온공국": "吉翁公国",
    "네오지온": "新吉翁",
    "에우고": "奥古",
    "오브": "奥布",
    "좌절1": "沮丧1",
    "좌절2": "沮丧2",
    "스마일": "笑脸",
    "스크림 패러디": "《呐喊》戏仿",
    "기사 문장": "骑士纹章",
    "사자 자리": "狮子座",
    "늑대": "狼",
    "토끼": "兔子",
    "토끼1": "兔子1",
    "그리핀": "狮鹫",
    "투구": "头盔",
    "펜텀": "幻影",
    "팬텀": "幻影",
    "팬텀문장": "幻影纹章",
    "캐슬": "城堡",
    "자프트": "扎夫特",
    "자프트2": "扎夫特2",
    "티탄즈": "提坦斯",
    "호랑가시나무": "冬青",
    "검은삼연성": "黑色三连星",
    "산타모자 하로": "圣诞帽哈罗",
    "신년 복": "新年福",
    "뉴타입": "新人类",
    "하트": "爱心",
    "스페이드": "黑桃",
    "디아나 카운터": "迪安娜·反击",
    "아무로": "阿姆罗",
    "론도 벨": "隆德·贝尔",
}

_EN_STICKERS = {
    "Triangular mark": "三角标记",
    "Spider Web": "蜘蛛网",
    "Missile Seal": "导弹标记",
    "Federal global": "地球联邦",
    "Neo Zeon": "新吉翁",
    "White Base": "白色基地",
    "Celestial Being": "天人",
    "King of Hearts": "红心之王",
    "Cherry Blossoms": "樱花",
    "Butterfly": "蝴蝶",
    "Scorpion": "蝎子",
    "Frustration": "沮丧",
    "Unicorn": "独角兽",
    "Target": "标靶",
    "Rabbit": "兔子",
    "Falcon": "猎鹰",
    "Flower": "花朵",
    "Smile": "笑脸",
    "Heart": "爱心",
    "Spade": "黑桃",
    "Clover": "梅花",
    "Diamond": "方片",
    "Winter": "冬季",
    "Autumn": "秋季",
    "Summer": "夏季",
    "Eagle": "鹰",
    "Griffin": "狮鹫",
    "Lion": "狮子",
    "Wolf": "狼",
    "Night": "夜晚",
    "Rose": "玫瑰",
    "Star": "星星",
    "Stars": "星星",
    "Bug": "虫子",
}


def _no_duplicate_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate phrase key: {key!r}")
        result[key] = value
    return result


def _validate_numbers(source: str, translated: str) -> None:
    for token in _NUMBER.findall(source):
        if token not in translated:
            raise ValueError(f"translation drops numeric token {token!r}: {source!r}")


@lru_cache(maxsize=1)
def _phrases() -> tuple[dict[int, dict[str, str]], dict[str, str]]:
    raw = json.loads(
        PHRASES_PATH.read_text(encoding="utf-8"),
        object_pairs_hook=_no_duplicate_object,
    )
    if raw.get("schema_version") != 1 or set(raw) != {"schema_version", "name", "description"}:
        raise ValueError("unsupported shop phrase schema")
    names: dict[int, dict[str, str]] = {}
    for raw_kind, entries in raw["name"].items():
        kind = int(raw_kind)
        if not isinstance(entries, dict):
            raise ValueError("name phrase group must be an object")
        names[kind] = {}
        for source, translated in entries.items():
            if not isinstance(source, str) or not isinstance(translated, str) or not source or not translated:
                raise ValueError("invalid name phrase")
            _validate_numbers(source, translated)
            names[kind][source] = translated
    descriptions: dict[str, str] = {}
    for source, translated in raw["description"].items():
        if not isinstance(source, str) or not isinstance(translated, str) or not source or not translated:
            raise ValueError("invalid description phrase")
        _validate_numbers(source, translated)
        descriptions[source] = translated
    return names, descriptions


def _unit_label(source: str, unit_words: Mapping[str, str] | None) -> str:
    if unit_words:
        translated = unit_words.get(source)
        if isinstance(translated, str) and translated:
            return translated
    return source


def _replace_prefix(value: str, words: Mapping[str, str]) -> str:
    for source in sorted(words, key=len, reverse=True):
        if value == source:
            return words[source]
        if value.startswith(source):
            boundary = value[len(source):len(source) + 1]
            if not boundary or boundary.isspace() or boundary.isdigit() or boundary in "-(":
                return words[source] + value[len(source):]
    return value


def _paint_core(source: str) -> tuple[str, bool] | None:
    value = source.strip()
    is_set = bool(re.search(r"(?:^|\s)(?:Set|세트)(?:\s|$)", value, re.IGNORECASE))
    value = re.sub(r"(?:^|\s)(?:Set|세트)(?=\s|$)", " ", value, flags=re.IGNORECASE)
    value = re.sub(r"\s*(?:Paint|페인트)\s*$", "", value, flags=re.IGNORECASE).strip()
    if not value:
        return None
    value = _replace_prefix(value, _KO_COLORS)
    value = _replace_prefix(value, _EN_COLORS)
    value = re.sub(r"\s+", " ", value).strip()
    return value, is_set


def _translate_paint_name(source: str) -> str | None:
    event = bool(_EVENT_KO.match(source) or _EVENT_EN.match(source))
    value = _EVENT_KO.sub("", source)
    value = _EVENT_EN.sub("", value)
    polish = re.fullmatch(r"(?:Polish|광택제)\s*(.*)", value, re.IGNORECASE)
    if polish:
        suffix = polish.group(1).strip()
        translated = "抛光剂" + (f" {suffix}" if suffix else "")
    else:
        core = _paint_core(value)
        if core is None:
            return None
        label, is_set = core
        translated = f"{label}{'套装' if is_set else ''}涂料"
    return f"（活动）{translated}" if event else translated


def _sticker_core(source: str) -> str:
    value = source.strip()
    value = re.sub(r"\s+(?:Sticker|Stickers)(?:\s+Sticker)?(?:\s+SSG)?\s*$", "", value, flags=re.IGNORECASE)
    value = re.sub(r"\s*스티커\s*$", "", value)
    value = value.strip()
    if value in _KO_STICKERS:
        return _KO_STICKERS[value]
    value = _replace_prefix(value, _EN_STICKERS)
    return value


def _translate_sticker_name(source: str) -> str | None:
    event = bool(_EVENT_KO.match(source) or _EVENT_EN.match(source))
    value = _EVENT_KO.sub("", source)
    value = _EVENT_EN.sub("", value)
    if value in {"Sticker Eraser", "스티커 지우개"}:
        translated = "贴纸清除剂"
    else:
        label = _sticker_core(value)
        if not label:
            return None
        translated = f"{label}贴纸"
    return f"（活动）{translated}" if event else translated


def _translate_misc_name(
    source: str,
    unit_words: Mapping[str, str] | None,
) -> str | None:
    if _TRUNCATED_MISC.search(source):
        return None
    match = re.fullmatch(r"EXP Pack_(\d+)(?: \((Full)\))?", source, re.IGNORECASE)
    if match:
        suffix = "（Full）" if match.group(2) else ""
        return f"经验包_{match.group(1)}{suffix}"
    match = re.fullmatch(
        r"(.+?)\s+(?:Plan\s*Mix|Mix\s+Plan)\s+(?:Exchange Coupon|exchange ticket)",
        source,
        re.IGNORECASE,
    )
    if match:
        return f"{_unit_label(match.group(1).strip(), unit_words)}设计图兑换券"
    match = re.fullmatch(r"(.+?)\s+(?:Exchange Coupon|exchange ticket)", source, re.IGNORECASE)
    if match:
        return f"{_unit_label(match.group(1).strip(), unit_words)}兑换券"
    match = re.fullmatch(r"(.+?)\s+Combined System\s+Voucher Test", source, re.IGNORECASE)
    if match:
        return f"{_unit_label(match.group(1).strip(), unit_words)}合成系统兑换券（测试）"
    match = re.fullmatch(r"(.+?)\s+Combined(?: System)?", source, re.IGNORECASE)
    if match:
        return f"{_unit_label(match.group(1).strip(), unit_words)}合成系统"
    match = re.fullmatch(r"(.+?)\s+Voucher Test(?:\s+(\d+)\s+Piece)?", source, re.IGNORECASE)
    if match:
        count = f" {match.group(2)}件" if match.group(2) else ""
        return f"{_unit_label(match.group(1).strip(), unit_words)}兑换券（测试{count}）"
    match = re.fullmatch(r"(.+?)\s+Present Box", source, re.IGNORECASE)
    if match:
        return f"{match.group(1).strip()}礼物盒"
    match = re.fullmatch(r"(.+?)\s+Gold Key", source, re.IGNORECASE)
    if match:
        return f"{match.group(1).strip()}黄金钥匙"
    match = re.fullmatch(r"(.+?)\s+(Key|key)", source)
    if match:
        return f"{match.group(1).strip()}钥匙"
    match = re.fullmatch(r"(.+?)\s+Box", source, re.IGNORECASE)
    if match:
        return f"{match.group(1).strip()}礼盒"
    match = re.fullmatch(r"(.+?)\s+RARE Capsule Point Hammer", source, re.IGNORECASE)
    if match:
        return f"{match.group(1).strip()}稀有扭蛋点数锤"
    match = re.fullmatch(r"(.+?)\s+RARE Capsule Hammer", source, re.IGNORECASE)
    if match:
        return f"{match.group(1).strip()}稀有扭蛋锤"
    match = re.fullmatch(r"(.+?)\s+RARE Capsule", source, re.IGNORECASE)
    if match:
        return f"{match.group(1).strip()}稀有扭蛋"
    match = re.fullmatch(r"(.+?)\s+Capsule Point Hammer", source, re.IGNORECASE)
    if match:
        return f"{match.group(1).strip()}扭蛋点数锤"
    match = re.fullmatch(r"(.+?)\s+Capsule Hammer", source, re.IGNORECASE)
    if match:
        return f"{match.group(1).strip()}扭蛋锤"
    match = re.fullmatch(r"(.+?)\s+Capsule", source, re.IGNORECASE)
    if match:
        return f"{match.group(1).strip()}扭蛋"
    match = re.fullmatch(r"(.+?)\s+Festival", source, re.IGNORECASE)
    if match:
        return f"{match.group(1).strip()}庆典"
    match = re.fullmatch(r"(.+?)\s+Event", source, re.IGNORECASE)
    if match:
        return f"{match.group(1).strip()}活动"
    match = re.fullmatch(r"(.+?)\s+Hammer", source, re.IGNORECASE)
    if match:
        return f"{match.group(1).strip()}锤"
    return None


def translate_name(
    source: str,
    kind: int,
    unit_words: Mapping[str, str] | None = None,
) -> str | None:
    """Translate a known stock name without guessing future custom items."""
    if not isinstance(source, str) or not source:
        return None
    names, _ = _phrases()
    kind = int(kind)
    exact = names.get(kind, {}).get(source)
    if exact is None:
        return None
    if kind == 22 and unit_words:
        plain = _translate_misc_name(source, None)
        mapped = _translate_misc_name(source, unit_words)
        if mapped is not None and mapped != plain:
            return mapped
    return exact


def translate_description(
    source: str,
    unit_words: Mapping[str, str] | None = None,
) -> str | None:
    """Translate a known literal description; item-name context is never inferred."""
    if not isinstance(source, str) or not source:
        return None
    _, descriptions = _phrases()
    return descriptions.get(source)


__all__ = ["translate_name", "translate_description"]
