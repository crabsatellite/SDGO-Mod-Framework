"""Cutscene camera checks: a shot must not start inside terrain or an object, nor look through one.

Package models are approximated by their bounds; models that already exist in the target client use their own
collision boxes, so an open structure (a gate frame, an arch) stays open. Only the first three quarters of
each view ray are tested: the camera may look *at* an object.
"""
from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Callable

from . import gltf, mod_format
from .package import Package

Box = tuple[tuple[float, float, float], tuple[tuple[float, float, float], ...], tuple[float, float, float]]
RAY_SAMPLES = 24
RAY_SPAN = 0.75
CAMERA_CLEARANCE = 30.0
OBJECT_MARGIN = 10.0


@dataclass
class ObjectOccluders:
    """Collision boxes split into fixed geometry and named destroyable blocks.

    Mission validation can remove a block only when that exact phase explicitly hides it. Placement checks
    continue to use the complete closed geometry through :func:`object_boxes`.
    """
    fixed: tuple[Box, ...]
    blocks: dict[str, tuple[Box, ...]]

    @property
    def boxes(self) -> list[Box]:
        return self.active()

    def active(self, hidden_blocks=()) -> list[Box]:
        hidden = set(hidden_blocks)
        boxes = list(self.fixed)
        for name, grouped in self.blocks.items():
            if name not in hidden:
                boxes.extend(grouped)
        return boxes


def _rotate_y(v, angle):
    s, c = math.sin(angle), math.cos(angle)
    # rotation_y turns clockwise seen from above: local +Z -> (sin a, 0, cos a), local +X -> (cos a, 0, -sin a)
    return (v[0] * c + v[2] * s, v[1], -v[0] * s + v[2] * c)


def _native_boxes(mod) -> list:
    """Collision boxes of a client model in model space. Some models store them in a smaller unit than the
    mesh (a uniform node scale); when every axis disagrees by the same factor the boxes are scaled to fit.
    Without usable boxes the mesh bounds are used."""
    nodes = [n for n in mod.nodes if n.mesh]
    if not nodes:
        return []
    lo = [min(n.bbox_min[k] for n in nodes) for k in range(3)]
    hi = [max(n.bbox_max[k] for n in nodes) for k in range(3)]
    bounds = [(tuple((lo[k] + hi[k]) / 2 for k in range(3)), ((1, 0, 0), (0, 1, 0), (0, 0, 1)),
               tuple((hi[k] - lo[k]) / 2 for k in range(3)))]
    if not mod.obbs:
        return bounds
    corners = [c for o in mod.obbs for c in o.corners()]
    ext = [max(c[k] for c in corners) - min(c[k] for c in corners) for k in range(3)]
    ratios = [(hi[k] - lo[k]) / ext[k] for k in range(3) if ext[k] > 1.0 and hi[k] - lo[k] > 1.0]
    if not ratios:
        return bounds
    factor = sum(ratios) / len(ratios)
    if any(abs(r / factor - 1.0) > 0.25 for r in ratios):
        return bounds
    if abs(factor - 1.0) <= 0.2:
        factor = 1.0
    return [(tuple(c * factor for c in o.center), o.axes, tuple(s * factor / 2 for s in o.size)) for o in mod.obbs]


def object_occluders(package: Package, variant: str,
                     read_native: Callable[[str], bytes] | None) -> ObjectOccluders:
    fixed: list[Box] = []
    block_boxes: dict[str, tuple[Box, ...]] = {}
    native_cache: dict[str, list] = {}
    for obj in package.objects:
        if obj.only and obj.only != variant or obj.collision == 'none':
            continue
        angle = math.radians(obj.rotation_y)
        local: list[tuple[tuple, tuple, tuple]] = []      # (center, axes, half) in model space
        if obj.native_model:
            if read_native is None:
                continue
            key = obj.native_model.replace('\\', '/').lower()
            if key not in native_cache:
                try:
                    mod = mod_format.parse(read_native(key))
                except (ValueError, KeyError, OSError):
                    mod = None
                native_cache[key] = _native_boxes(mod) if mod is not None else []
            local = native_cache[key]
        elif isinstance(obj.collision, list):
            local = [(tuple(b['center']),                    # authored in the game frame, like the compiler
                      (_rotate_y((1, 0, 0), math.radians(b.get('rotation_y', 0.0))), (0, 1, 0),
                       _rotate_y((0, 0, 1), math.radians(b.get('rotation_y', 0.0)))), b['half_extents'])
                     for b in obj.collision]
        else:
            lo, hi = gltf.bounds(gltf.load(package.read(obj.model), package.resolver(obj.model)))
            lo, hi = (lo[0], lo[1], -hi[2]), (hi[0], hi[1], -lo[2])     # glTF -Z forward -> game +Z
            local = [(tuple((lo[k] + hi[k]) / 2 for k in range(3)), ((1, 0, 0), (0, 1, 0), (0, 0, 1)),
                      tuple((hi[k] - lo[k]) / 2 for k in range(3)))]
        world: list[Box] = []
        for center, axes, half in local:
            scaled = tuple(center[k] * obj.scale[k] for k in range(3))
            world_center = tuple(obj.position[k] + r for k, r in enumerate(_rotate_y(scaled, angle)))
            world_axes, world_half = [], []
            for axis, h in zip(axes, half):
                stretched = tuple(axis[k] * obj.scale[k] for k in range(3))
                length = math.sqrt(sum(c * c for c in stretched)) or 1.0
                world_axes.append(_rotate_y(tuple(c / length for c in stretched), angle))
                world_half.append(h * length)
            world.append((world_center, tuple(world_axes), tuple(world_half)))
        if obj.block is not None:
            block_boxes[obj.object_id] = tuple(world)
        else:
            fixed.extend(world)
    return ObjectOccluders(tuple(fixed), block_boxes)


def object_boxes(package: Package, variant: str, read_native: Callable[[str], bytes] | None) -> list[Box]:
    return object_occluders(package, variant, read_native).boxes


def _inside(point, box: Box, margin: float) -> bool:
    center, axes, half = box
    d = [point[k] - center[k] for k in range(3)]
    return all(abs(sum(d[k] * axis[k] for k in range(3))) <= h + margin for axis, h in zip(axes, half))


def shot_problem(ground: Callable[[tuple], float], boxes: list[Box], camera, look) -> str | None:
    """None when the camera position and its view toward `look` are clear, else a short reason."""
    if camera[1] < ground(camera) + CAMERA_CLEARANCE:
        return '镜头低于地面'
    for box in boxes:
        if _inside(camera, box, OBJECT_MARGIN):
            return '镜头位于物件内部'
    for k in range(1, RAY_SAMPLES + 1):
        f = RAY_SPAN * k / RAY_SAMPLES
        p = tuple(camera[i] + (look[i] - camera[i]) * f for i in range(3))
        if p[1] < ground(p) - 5.0:
            return '视线被地形遮挡'
        for box in boxes:
            if _inside(p, box, 0.0):
                return '视线被物件遮挡'
    return None


def blocked(boxes: list[Box], point, margin: float = 20.0) -> bool:
    """True when `point` (a spawn, actor or warp point) lies inside an object's collision."""
    return any(_inside(point, box, margin) for box in boxes)


def placement_problems(package: Package, variant: str, boxes: list[Box]) -> list[str]:
    """Map spawn, sector and item points of `variant` that sit inside an object."""
    problems = []
    groups = list(package.spawns.items()) + [(f'sectors.{k}', v) for k, v in package.sectors.items()]
    for name, points in groups:
        for i, spawn in enumerate(points):
            if blocked(boxes, spawn.position):
                problems.append(f'{name}[{i}]')
    for team, patterns in package.item_patterns.items():
        for p, pattern in enumerate(patterns):
            for i, position in enumerate(pattern):
                if blocked(boxes, position, 0.0):
                    problems.append(f'item_patterns.{team}[{p}][{i}]')
    return problems
