"""Bounded author-owned display fields; never rewrite source record bytes."""
from __future__ import annotations

# Offsets/capacities mirror ContentLocalizationBoundaryIndex. Capacities include
# the UTF-16 NUL, and weapon +105 is numeric data, not another name.
FIELDS = {
    2: ((5, 50, 'name'), (105, 50, 'secondary_name'), (205, 50, 'display_name')),
    10: ((5, 50, 'name'),),
    13: ((5, 50, 'name'), (0x213, 300, 'description')),
    34: ((5, 50, 'name'),),
}


def issues(kind, identity, raw):
    result = []
    for offset, capacity, field in FIELDS.get(kind, ()):
        data = raw[offset:offset + capacity * 2]
        code = ('truncated_display_text' if len(data) != capacity * 2 else
                'unterminated_display_text' if not any(
                    data[index:index + 2] == b'\0\0' for index in range(0, len(data), 2)) else None)
        if code:
            result.append({'code': code, 'kind': kind, 'id': identity,
                           'field': field, 'offset': offset, 'capacity': capacity,
                           'message': f'kind={kind} id={identity} field={field} offset={offset} '
                           f'capacity={capacity}: 文本字段缺少容量内的 UTF-16 NUL 结束符或已截断；'
                           f'容量包含结束符，正文最多 {capacity - 1} 个 UTF-16 单元。'
                           '请由内容作者修正源包并重新发布；不要直接修改已安装文件或安装回执。'})
    return result


def validate(kind, identity, raw):
    problems = issues(kind, identity, raw)
    if problems:
        raise ValueError(problems[0]['message'])
