"""A generated stand-in poster for a dedicated activity machine until the owner uploads real art.

The stand-in uses the unit's own published picture on a plain gradient with the unit name, at
the native 361x322 poster size. It is clearly a placeholder and is replaced, not edited, when the
owner uploads a poster.
"""
from __future__ import annotations

import io
import os
from pathlib import Path

SIZE = (361, 322)


def _font(size):
    from PIL import ImageFont
    folder = Path(os.environ.get('WINDIR', r'C:\Windows')) / 'Fonts'
    for name in ('msyhbd.ttc', 'msyh.ttc', 'simhei.ttf'):
        if (folder / name).is_file():
            return ImageFont.truetype(str(folder / name), size)
    return None


def placeholder(unit_png: bytes, name: str) -> bytes:
    from PIL import Image, ImageDraw, ImageFilter
    width, height = SIZE
    canvas = Image.new('RGBA', SIZE)
    draw = ImageDraw.Draw(canvas)
    for y in range(height):
        t = y / (height - 1)
        draw.line([(0, y), (width, y)], fill=(round(18 + 30 * t), round(22 + 40 * t), round(60 + 70 * t), 255))
    with Image.open(io.BytesIO(unit_png)) as source:
        unit = source.convert('RGBA')
    box = unit.getbbox()
    if box:
        unit = unit.crop(box)
    scale = min((width - 40) / unit.width, (height - 70) / unit.height)
    unit = unit.resize((max(1, round(unit.width * scale)), max(1, round(unit.height * scale))), Image.Resampling.LANCZOS)
    glow = Image.new('RGBA', unit.size, (150, 180, 255, 0))
    glow.putalpha(unit.getchannel('A').filter(ImageFilter.GaussianBlur(8)))
    position = ((width - unit.width) // 2, (height - 56 - unit.height) // 2 + 6)
    canvas.alpha_composite(glow, position)
    canvas.alpha_composite(unit, position)
    face = _font(22)
    if face is not None and name:
        text = name[:16]
        bounds = draw.textbbox((0, 0), text, font=face, stroke_width=3)
        draw.text(((width - (bounds[2] - bounds[0])) / 2, height - 44), text, font=face, fill=(255, 225, 140),
                  stroke_width=3, stroke_fill=(0, 0, 0))
    stream = io.BytesIO()
    canvas.convert('RGB').save(stream, 'PNG')
    return stream.getvalue()
