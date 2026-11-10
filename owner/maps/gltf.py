"""Minimal, strict glTF 2.0 reader for map packages (static meshes only).

Accepts binary `.glb` and `.gltf` with buffers/images in the same package (no URLs outside it, no data URIs
larger than the limits). Node hierarchies are flattened into world space of the asset. Only triangle
primitives with POSITION (required), NORMAL and TEXCOORD_0 (optional) are read; skins, morph targets and
animations are rejected because map props are static. Textures come from `baseColorTexture` and must be PNG.
"""
from __future__ import annotations

import base64
import json
import math
import struct
from dataclasses import dataclass, field
from pathlib import PurePosixPath
from typing import Callable

MAX_VERTICES = 65_535 * 16
MAX_INDICES = 3 * 1_000_000
MAX_JSON = 16 * 1024 * 1024
COMPONENT = {5120: ('b', 1), 5121: ('B', 1), 5122: ('h', 2), 5123: ('H', 2), 5125: ('I', 4), 5126: ('f', 4)}
WIDTH = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}


class GltfError(ValueError):
    pass


@dataclass
class Material:
    name: str
    texture_png: bytes | None          # PNG bytes of baseColorTexture, if any
    color: tuple[float, float, float, float] = (1.0, 1.0, 1.0, 1.0)
    double_sided: bool = False


@dataclass
class Mesh:
    """One material group in asset space: parallel vertex arrays and a triangle list."""
    node: str
    material: Material
    positions: list[tuple[float, float, float]] = field(default_factory=list)
    normals: list[tuple[float, float, float]] = field(default_factory=list)
    uvs: list[tuple[float, float]] = field(default_factory=list)
    triangles: list[tuple[int, int, int]] = field(default_factory=list)


def _mat_mul(a: list[float], b: list[float]) -> list[float]:
    """Column-major 4x4 multiply (glTF convention): a * b."""
    return [sum(a[k * 4 + r] * b[c * 4 + k] for k in range(4)) for c in range(4) for r in range(4)]


def _trs(node: dict) -> list[float]:
    if 'matrix' in node:
        m = [float(v) for v in node['matrix']]
        if len(m) != 16:
            raise GltfError('node matrix must have 16 numbers')
        return m
    tx, ty, tz = (float(v) for v in node.get('translation', (0, 0, 0)))
    qx, qy, qz, qw = (float(v) for v in node.get('rotation', (0, 0, 0, 1)))
    sx, sy, sz = (float(v) for v in node.get('scale', (1, 1, 1)))
    n = math.sqrt(qx * qx + qy * qy + qz * qz + qw * qw) or 1.0
    qx, qy, qz, qw = qx / n, qy / n, qz / n, qw / n
    r = [1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy + qz * qw), 2 * (qx * qz - qy * qw),
         2 * (qx * qy - qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz + qx * qw),
         2 * (qx * qz + qy * qw), 2 * (qy * qz - qx * qw), 1 - 2 * (qx * qx + qy * qy)]
    return [r[0] * sx, r[1] * sx, r[2] * sx, 0, r[3] * sy, r[4] * sy, r[5] * sy, 0,
            r[6] * sz, r[7] * sz, r[8] * sz, 0, tx, ty, tz, 1]


def _apply(m: list[float], p: tuple[float, float, float], w: float) -> tuple[float, float, float]:
    x, y, z = p
    return (m[0] * x + m[4] * y + m[8] * z + m[12] * w,
            m[1] * x + m[5] * y + m[9] * z + m[13] * w,
            m[2] * x + m[6] * y + m[10] * z + m[14] * w)


def _normal_matrix(m: list[float]) -> list[float]:
    a, b, c, d, e, f, g, h, i = m[0], m[4], m[8], m[1], m[5], m[9], m[2], m[6], m[10]
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    if abs(det) < 1e-12:
        raise GltfError('node transform is singular')
    inv = [(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det,
           (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det,
           (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det]
    # transpose of the inverse, stored column-major in a 4x4 frame
    return [inv[0], inv[1], inv[2], 0, inv[3], inv[4], inv[5], 0, inv[6], inv[7], inv[8], 0, 0, 0, 0, 1]


class _Document:
    def __init__(self, doc: dict, buffers: list[bytes], read_file: Callable[[str], bytes]):
        self.doc, self.buffers, self.read_file = doc, buffers, read_file

    def view(self, index: int) -> bytes:
        v = self.doc['bufferViews'][index]
        raw = self.buffers[v['buffer']]
        start = v.get('byteOffset', 0)
        end = start + v['byteLength']
        if end > len(raw):
            raise GltfError('bufferView exceeds its buffer')
        return raw[start:end]

    def accessor(self, index: int) -> list[tuple]:
        a = self.doc['accessors'][index]
        if a.get('sparse'):
            raise GltfError('sparse accessors are not supported')
        fmt, size = COMPONENT[a['componentType']]
        width = WIDTH[a['type']]
        count = int(a['count'])
        v = self.doc['bufferViews'][a['bufferView']]
        raw = self.view(a['bufferView'])
        stride = v.get('byteStride') or size * width
        base = a.get('byteOffset', 0)
        if count and base + stride * (count - 1) + size * width > len(raw):
            raise GltfError('accessor exceeds its bufferView')
        out = []
        unit = struct.Struct('<' + fmt * width)
        normalized = a.get('normalized', False)
        scale = {5121: 255.0, 5123: 65535.0, 5120: 127.0, 5122: 32767.0}.get(a['componentType'])
        for k in range(count):
            row = unit.unpack_from(raw, base + k * stride)
            if normalized and scale:
                row = tuple(max(x / scale, -1.0) for x in row)
            out.append(row)
        return out

    def image_png(self, index: int) -> bytes:
        img = self.doc['images'][index]
        if 'bufferView' in img:
            data = self.view(img['bufferView'])
        else:
            uri = img.get('uri', '')
            data = _uri_bytes(uri, self.read_file)
        if not (data.startswith(b'\x89PNG\r\n\x1a\n') or data.startswith(b'\xff\xd8\xff')):
            raise GltfError('textures must be PNG or JPEG images')
        return data


def _uri_bytes(uri: str, read_file: Callable[[str], bytes]) -> bytes:
    if uri.startswith('data:'):
        head, _, payload = uri.partition(',')
        if ';base64' not in head:
            raise GltfError('only base64 data URIs are supported')
        return base64.b64decode(payload, validate=True)
    if not uri or ':' in uri or uri.startswith(('/', '\\')) or '..' in PurePosixPath(uri.replace('\\', '/')).parts:
        raise GltfError('external resources must be relative files inside the package')
    return read_file(uri)


def load(data: bytes, read_file: Callable[[str], bytes] | None = None) -> list[Mesh]:
    """Parse a .glb or .gltf document; `read_file(relative_path)` resolves package-relative URIs."""
    read_file = read_file or (lambda name: (_ for _ in ()).throw(GltfError('external file not available: ' + name)))
    buffers: list[bytes] = []
    if data[:4] == b'glTF':
        magic, version, length = struct.unpack_from('<4sII', data)
        if version != 2 or length != len(data):
            raise GltfError('unsupported or truncated .glb')
        offset, chunks = 12, []
        while offset < length:
            size, kind = struct.unpack_from('<I4s', data, offset)
            chunks.append((kind, data[offset + 8:offset + 8 + size]))
            offset += 8 + size
        if not chunks or chunks[0][0] != b'JSON':
            raise GltfError('.glb has no JSON chunk')
        doc = json.loads(chunks[0][1].decode('utf-8'))
        binary = next((c for k, c in chunks[1:] if k == b'BIN\x00'), None)
        for index, buf in enumerate(doc.get('buffers', [])):
            buffers.append(binary if index == 0 and 'uri' not in buf and binary is not None else _uri_bytes(buf.get('uri', ''), read_file))
    else:
        if len(data) > MAX_JSON:
            raise GltfError('.gltf JSON is too large')
        doc = json.loads(data.decode('utf-8'))
        buffers = [_uri_bytes(buf.get('uri', ''), read_file) for buf in doc.get('buffers', [])]
    if str(doc.get('asset', {}).get('version', '')).split('.')[0] != '2':
        raise GltfError('only glTF 2.0 is supported')
    for key in ('skins', 'animations'):
        if doc.get(key):
            raise GltfError('map props must be static: %s are not supported' % key)
    d = _Document(doc, buffers, read_file)
    materials: dict[int, Material] = {}

    def material(index: int | None) -> Material:
        if index is None:
            return Material('default', None)
        if index not in materials:
            m = doc['materials'][index]
            pbr = m.get('pbrMetallicRoughness', {})
            tex = pbr.get('baseColorTexture')
            png = d.image_png(doc['textures'][tex['index']]['source']) if tex else None
            color = tuple(float(v) for v in pbr.get('baseColorFactor', (1, 1, 1, 1)))
            materials[index] = Material(m.get('name', 'material%d' % index), png, color, bool(m.get('doubleSided')))
        return materials[index]

    meshes: list[Mesh] = []
    total_vertices = total_indices = 0
    scene = doc.get('scenes', [{}])[doc.get('scene', 0)] if doc.get('scenes') else {'nodes': list(range(len(doc.get('nodes', []))))}

    def visit(node_index: int, parent: list[float], depth: int) -> None:
        nonlocal total_vertices, total_indices
        if depth > 64:
            raise GltfError('node hierarchy is too deep')
        node = doc['nodes'][node_index]
        world = _mat_mul(parent, _trs(node))
        if 'mesh' in node:
            nm = _normal_matrix(world)
            for prim in doc['meshes'][node['mesh']]['primitives']:
                if prim.get('mode', 4) != 4:
                    raise GltfError('only triangle primitives are supported')
                if prim.get('targets'):
                    raise GltfError('morph targets are not supported')
                attrs = prim['attributes']
                pos = d.accessor(attrs['POSITION'])
                nor = d.accessor(attrs['NORMAL']) if 'NORMAL' in attrs else None
                uv = d.accessor(attrs['TEXCOORD_0']) if 'TEXCOORD_0' in attrs else None
                idx = [i[0] for i in d.accessor(prim['indices'])] if 'indices' in prim else list(range(len(pos)))
                if len(idx) % 3:
                    raise GltfError('triangle index count is not a multiple of 3')
                total_vertices += len(pos)
                total_indices += len(idx)
                if total_vertices > MAX_VERTICES or total_indices > MAX_INDICES:
                    raise GltfError('model is too large for a map prop')
                mesh = Mesh(node.get('name', 'node%d' % node_index), material(prim.get('material')))
                mesh.positions = [_apply(world, p[:3], 1.0) for p in pos]
                if nor:
                    mesh.normals = []
                    for n in nor:
                        x, y, z = _apply(nm, n[:3], 0.0)
                        length = math.sqrt(x * x + y * y + z * z) or 1.0
                        mesh.normals.append((x / length, y / length, z / length))
                mesh.uvs = [(float(t[0]), float(t[1])) for t in uv] if uv else []
                for k in range(0, len(idx), 3):
                    a, b, c = idx[k], idx[k + 1], idx[k + 2]
                    if max(a, b, c) >= len(pos):
                        raise GltfError('triangle index out of range')
                    mesh.triangles.append((a, b, c))
                meshes.append(mesh)
        for child in node.get('children', []):
            visit(child, world, depth + 1)

    identity = [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0]
    for root in scene.get('nodes', []):
        visit(root, identity, 0)
    if not meshes:
        raise GltfError('model contains no triangle mesh')
    return meshes


def bounds(meshes: list[Mesh]) -> tuple[tuple[float, float, float], tuple[float, float, float]]:
    xs = [p for m in meshes for p in m.positions]
    return (tuple(min(p[k] for p in xs) for k in range(3)), tuple(max(p[k] for p in xs) for k in range(3)))
