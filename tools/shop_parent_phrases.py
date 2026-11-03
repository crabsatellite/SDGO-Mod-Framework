"""Exact translations for native parent shop records.

The parent table contains the strings rendered by the original shop UI.  This
module deliberately performs exact dictionary lookups: generation-time
normalization must never turn into a rule that guesses translations for future
custom products.
"""
from __future__ import annotations

import json
from collections.abc import Iterable, Mapping
from functools import lru_cache
from pathlib import Path
import re


PHRASES_PATH = (
    Path(__file__).resolve().parents[1]
    / "mods"
    / "shop_localization"
    / "parent-phrases.json"
)

_NUMBER = re.compile(r"\d+(?:[,.]\d+)*%*")
_AMOUNT_ONLY = re.compile(r"[\d., +%/\-\t]+")


def _no_duplicate_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate parent phrase key: {key!r}")
        result[key] = value
    return result


def _validate_numbers(source: str, translated: str) -> None:
    for token in _NUMBER.findall(source):
        if token not in translated:
            raise ValueError(
                f"parent translation drops numeric token {token!r}: {source!r}"
            )


@lru_cache(maxsize=1)
def _phrases() -> tuple[dict[str, str], dict[str, str]]:
    raw = json.loads(
        PHRASES_PATH.read_text(encoding="utf-8"),
        object_pairs_hook=_no_duplicate_object,
    )
    if raw.get("schema_version") != 1 or set(raw) != {
        "schema_version",
        "name",
        "description",
    }:
        raise ValueError("unsupported parent phrase schema")

    result: list[dict[str, str]] = []
    for field in ("name", "description"):
        entries = raw[field]
        if not isinstance(entries, dict):
            raise ValueError(f"parent {field} phrases must be an object")
        checked: dict[str, str] = {}
        for source, translated in entries.items():
            if (
                not isinstance(source, str)
                or not isinstance(translated, str)
                or not source
                or not translated
            ):
                raise ValueError(f"invalid parent {field} phrase")
            _validate_numbers(source, translated)
            checked[source] = translated
        result.append(checked)
    return result[0], result[1]


def translate_name(
    source: str,
    kinds: Iterable[int] | int | None = None,
    unit_words: Mapping[str, str] | None = None,
) -> str | None:
    """Translate a known native parent title without guessing custom stock."""
    if not isinstance(source, str) or not source:
        return None
    # The arguments are accepted for the parent builder interface.  Parent
    # titles are materialized after source-table review, so lookup is not
    # allowed to change with catalogue context.
    del kinds, unit_words
    names, _ = _phrases()
    return names.get(source)


def translate_description(
    source: str,
    unit_words: Mapping[str, str] | None = None,
) -> str | None:
    """Translate a known native description or usage string exactly."""
    if not isinstance(source, str) or not source:
        return None
    # The dictionary compiler deliberately owns raw amounts.  Treating a bare
    # number as prose would make it reject the whole localization build.
    if _AMOUNT_ONLY.fullmatch(source):
        return None
    del unit_words
    _, descriptions = _phrases()
    return descriptions.get(source)


__all__ = ["translate_name", "translate_description"]
