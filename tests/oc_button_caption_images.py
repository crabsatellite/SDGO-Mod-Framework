"""Check optical button alignment in real native caption-test screenshots.

Usage: python tests/oc_button_caption_images.py <native gametest directory>
Requires Pillow. Reads the native fixture's observed control rectangles and
actual D3D captures; no authored UI coordinates or generated images are used.
"""
import argparse
import json
import re
from pathlib import Path

from PIL import Image


def bounds(points):
    if not points:
        raise AssertionError('native button border or caption was not visible')
    xs, ys = zip(*points)
    return min(xs), min(ys), max(xs), max(ys)


def measure(path, rect):
    image = Image.open(path).convert('RGB')
    x, y, width, height = rect
    # Cyan button border/skin stands out from the darker blue modal panel.
    border = []
    for yy in range(y, y + height):
        for xx in range(x, x + width):
            r, g, b = image.getpixel((xx, yy))
            if b >= 170 and g >= 160 and b-r >= 45 and g-r >= 35:
                border.append((xx, yy))
    left, top, right, bottom = bounds(border)
    # Exclude the bright rim so only the white caption contributes to its box.
    text = []
    for yy in range(top+3, bottom-2):
        for xx in range(left+8, right-7):
            pixel = image.getpixel((xx, yy))
            if min(pixel) >= 220 and max(pixel)-min(pixel) <= 30:
                text.append((xx, yy))
    caption = bounds(text)
    dx = (caption[0]+caption[2]-left-right)/2
    dy = (caption[1]+caption[3]-top-bottom)/2
    return {'border': (left, top, right, bottom), 'caption': caption,
            'offset_x': dx, 'offset_y': dy}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    pattern = re.compile(r'button state=(\d+) x=(-?\d+) y=(-?\d+) width=(\d+) height=(\d+) root_x=(-?\d+) root_y=(-?\d+)')
    rectangles = {}
    for line in (args.directory/'events.jsonl').read_text(encoding='utf-8-sig').splitlines():
        match = pattern.fullmatch(json.loads(line).get('message', ''))
        if match:
            state, x, y, width, height, root_x, root_y = map(int, match.groups())
            rectangles[state] = x+root_x, y+root_y, width, height
    assert set(rectangles) == {1, 2, 3, 4}, 'all four actual native states are required'
    results = {state: measure(args.directory/f'oc-button-state-{state}.bmp', rect)
               for state, rect in rectangles.items()}
    print(json.dumps(results, ensure_ascii=False, indent=2))
    assert all(abs(row['offset_x']) <= 1 and abs(row['offset_y']) <= 1
               for row in results.values()), 'caption must be visually centered inside the rendered button'
    assert len({tuple(row['caption']) for row in results.values()}) == 1, 'caption must not move between native states'


if __name__ == '__main__':
    main()
