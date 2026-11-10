"""Byte-exact reader/writer and builder for ZOA `.txr` textures.

A `.txr` is not a texture format of its own. CTexture::Load (zoa_graphics 0x10053120) reads the file and
hands it to the statically linked D3DXCreateTextureFromFileInMemoryEx, so the payload is any standard image
container D3DX understands. Shipped files (9955 packed `.txr`): PNG 5257, JPEG 4471, ZOATEX1 wrapper 223
(200 TGA, 23 BMP inside), BMP 2, raw TGA 1, DDS 1 (DXT1).

Optional wrapper (checked by CTexture::Load before handing the rest to D3DX):

    char[8] "ZOATEX1\\0", u32 format_code, then the image file.
    format_code is only used when the caller requests D3DFMT_UNKNOWN: 1 X8R8G8B8, 2 A1R5G5B5, 3 A4R4G4B4,
    4..8 DXT1..DXT5; any other value (all shipped files use 0) keeps the requested format.

How the engine loads model textures: CMaterial::SetTexture (Kudiz slots) and the CModel material table call
CTexture::Load(path, mip_levels=0 (full chain), D3DFMT_DXT3, D3DPOOL_MANAGED, D3DX_DEFAULT filters,
colour key 0xFFFF00FF). D3DX therefore converts every model texture to DXT3 with a generated mip chain;
opaque pure magenta (255, 0, 255) becomes transparent black. Other callers request A8R8G8B8 (0x15) or
A8B8G8R8 (0x20); the container never decides the GPU format.

Each container is parsed into its structural pieces (PNG chunks, JPEG segments, BMP/TGA headers and
pixel data, DDS header and per-level surfaces) whose concatenation is the original file, so
`serialize(parse(data)) == data` for every shipped texture. `info` summarises width, height, pixel
format, mip levels and alpha for validation.

Builder: `build_png_txr` (standard library only; RGBA8 PNG, the most common shipped container) and
`build_dds_txr` (uncompressed A8R8G8B8 DDS with an optional box-filtered mip chain). Dimensions should be
powers of two (D3DX rounds others up and resamples); 1..2048 is the safe range on D3D9 hardware.
"""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass, field

ZOATEX_MAGIC = b'ZOATEX1\0'
PNG_SIGNATURE = b'\x89PNG\r\n\x1a\n'
TGA_FOOTER = b'TRUEVISION-XFILE.\0'
COLOR_KEY = (255, 0, 255, 255)
MAX_DIMENSION = 2048
ZOATEX_FORMATS = {1: 'X8R8G8B8', 2: 'A1R5G5B5', 3: 'A4R4G4B4', 4: 'DXT1', 5: 'DXT2', 6: 'DXT3', 7: 'DXT4',
                  8: 'DXT5'}


class TxrFormatError(ValueError):
    pass


@dataclass
class ImageInfo:
    width: int
    height: int
    pixel_format: str          # e.g. 'RGBA8', 'RGB8', 'L8', 'DXT1', 'A8R8G8B8', 'BGR24', 'BGRA32'
    has_alpha: bool
    mip_levels: int = 1

    @property
    def power_of_two(self) -> bool:
        return self.width > 0 and self.height > 0 and not self.width & (self.width - 1) and not self.height & (self.height - 1)


# --------------------------------------------------------------------------- containers

@dataclass
class PngImage:
    chunks: list[tuple[bytes, bytes, int]] = field(default_factory=list)   # (type, data, stored crc)
    trailing: bytes = b''

    def info(self) -> ImageInfo:
        ihdr = next((d for t, d, _ in self.chunks if t == b'IHDR'), None)
        if ihdr is None or len(ihdr) != 13:
            raise TxrFormatError('PNG without IHDR')
        width, height, depth, color = struct.unpack('>IIBB', ihdr[:10])
        names = {0: 'L', 2: 'RGB', 3: 'P', 4: 'LA', 6: 'RGBA'}
        alpha = color in (4, 6) or any(t == b'tRNS' for t, _, _ in self.chunks)
        return ImageInfo(width, height, f'{names.get(color, "?")}{depth}', alpha)

    def to_bytes(self) -> bytes:
        out = bytearray(PNG_SIGNATURE)
        for kind, data, crc in self.chunks:
            out += struct.pack('>I', len(data)) + kind + data + struct.pack('>I', crc)
        return bytes(out + self.trailing)


@dataclass
class JpegImage:
    segments: list[tuple[int, bytes]] = field(default_factory=list)   # (marker, exact bytes incl. marker)
    trailing: bytes = b''

    def info(self) -> ImageInfo:
        for marker, raw in self.segments:
            if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
                body = raw[raw.index(bytes([0xFF, marker])) + 4:]
                depth, height, width, comps = struct.unpack('>BHHB', body[:6])
                return ImageInfo(width, height, {1: 'L8', 3: 'YCbCr8', 4: 'CMYK8'}.get(comps, '?'), False)
        raise TxrFormatError('JPEG without SOF')

    def to_bytes(self) -> bytes:
        return b''.join(raw for _, raw in self.segments) + self.trailing


@dataclass
class BmpImage:
    file_header: tuple[int, int, int, int]      # file size, reserved1, reserved2, pixel offset
    dib: bytes                                  # DIB header (first u32 = its size)
    body: bytes                                 # palette, gap and pixel array up to the end

    def info(self) -> ImageInfo:
        if len(self.dib) < 16:
            raise TxrFormatError('BMP core headers are not supported')
        width, height, planes, bpp = struct.unpack_from('<iiHH', self.dib, 4)
        compression = struct.unpack_from('<I', self.dib, 16)[0] if len(self.dib) >= 20 else 0
        return ImageInfo(width, abs(height), f'BGR{bpp}' if bpp != 32 else 'BGRA32', bpp == 32 or compression == 3)

    def to_bytes(self) -> bytes:
        return b'BM' + struct.pack('<IHHI', *self.file_header) + self.dib + self.body


@dataclass
class TgaImage:
    header: tuple[int, ...]       # id_length, cmap_type, image_type, cmap_first, cmap_length, cmap_entry_bits,
                                  # x, y, width, height, bits, descriptor
    image_id: bytes = b''
    colormap: bytes = b''
    pixels: bytes = b''
    tail: bytes = b''             # extension/developer area and TGA 2.0 footer

    def info(self) -> ImageInfo:
        width, height, bits, descriptor = self.header[8], self.header[9], self.header[10], self.header[11]
        return ImageInfo(width, height, f'BGR{bits}' if bits != 32 else 'BGRA32', bits == 32 or bool(descriptor & 0x0F))

    def to_bytes(self) -> bytes:
        return struct.pack('<BBBHHBHHHHBB', *self.header) + self.image_id + self.colormap + self.pixels + self.tail


@dataclass
class DdsImage:
    header: tuple[int, ...]       # 31 u32 of DDS_HEADER (dwSize .. dwReserved2)
    dx10: bytes = b''             # DDS_HEADER_DXT10 when FourCC is 'DX10'
    surfaces: list[bytes] = field(default_factory=list)   # face-major, then mip level
    trailing: bytes = b''

    @property
    def fourcc(self) -> bytes:
        return struct.pack('<I', self.header[20])

    def info(self) -> ImageInfo:
        height, width = self.header[2], self.header[3]
        mips = max(1, self.header[6]) if self.header[1] & 0x20000 else 1
        return ImageInfo(width, height, _dds_format_name(self.header), _dds_alpha(self.header), mips)

    def to_bytes(self) -> bytes:
        return b'DDS ' + struct.pack('<31I', *self.header) + self.dx10 + b''.join(self.surfaces) + self.trailing


Container = PngImage | JpegImage | BmpImage | TgaImage | DdsImage


@dataclass
class TxrFile:
    image: Container
    kind: str                         # 'png', 'jpeg', 'bmp', 'tga', 'dds'
    zoatex_code: int | None = None    # None: no ZOATEX1 wrapper

    @property
    def info(self) -> ImageInfo:
        return self.image.info()


# --------------------------------------------------------------------------- parsing

def _parse_png(data: bytes) -> PngImage:
    image = PngImage()
    pos = 8
    while pos < len(data):
        if pos + 12 > len(data):
            break
        length = struct.unpack_from('>I', data, pos)[0]
        kind = data[pos + 4:pos + 8]
        if pos + 12 + length > len(data):
            raise TxrFormatError('truncated PNG chunk')
        image.chunks.append((kind, data[pos + 8:pos + 8 + length], struct.unpack_from('>I', data, pos + 8 + length)[0]))
        pos += 12 + length
        if kind == b'IEND':
            break
    image.trailing = data[pos:]
    return image


def _parse_jpeg(data: bytes) -> JpegImage:
    image = JpegImage()
    pos = 0
    n = len(data)
    while pos < n:
        start = pos
        if data[pos] != 0xFF:
            break
        while pos < n and data[pos] == 0xFF:      # fill bytes belong to the following marker
            pos += 1
        if pos >= n:
            pos = start
            break
        marker = data[pos]
        pos += 1
        if marker in (0xD8, 0x01) or 0xD0 <= marker <= 0xD7:
            image.segments.append((marker, data[start:pos]))
            continue
        if marker == 0xD9:
            image.segments.append((marker, data[start:pos]))
            break
        if pos + 2 > n:
            pos = start
            break
        length = struct.unpack_from('>H', data, pos)[0]
        pos = min(n, pos + length)
        if marker == 0xDA:                        # entropy-coded data runs to the next real marker
            while pos < n:
                if data[pos] == 0xFF and pos + 1 < n and data[pos + 1] != 0 and not 0xD0 <= data[pos + 1] <= 0xD7 \
                        and data[pos + 1] != 0xFF:
                    break
                pos += 1
            # keep fill bytes in front of the next marker with that marker
            while pos > start and data[pos - 1] == 0xFF and pos < n:
                pos -= 1
        image.segments.append((marker, data[start:pos]))
    image.trailing = data[pos:]
    return image


def _parse_bmp(data: bytes) -> BmpImage:
    if len(data) < 18:
        raise TxrFormatError('truncated BMP')
    file_header = struct.unpack_from('<IHHI', data, 2)
    dib_size = struct.unpack_from('<I', data, 14)[0]
    if not 12 <= dib_size <= len(data) - 14:
        raise TxrFormatError('invalid BMP DIB header')
    return BmpImage(file_header, data[14:14 + dib_size], data[14 + dib_size:])


def _tga_pixel_span(data: bytes, pos: int, header: tuple[int, ...]) -> int:
    image_type, width, height, bits = header[2], header[8], header[9], header[10]
    pixel = (bits + 7) // 8
    count = width * height
    if image_type in (1, 2, 3):
        return min(len(data), pos + count * pixel)
    if image_type in (9, 10, 11):
        done = 0
        while done < count and pos < len(data):
            packet = data[pos]
            run = (packet & 0x7F) + 1
            pos += 1 + (pixel if packet & 0x80 else run * pixel)
            done += run
        return min(len(data), pos)
    return pos


def _parse_tga(data: bytes) -> TgaImage:
    if len(data) < 18:
        raise TxrFormatError('truncated TGA')
    header = struct.unpack_from('<BBBHHBHHHHBB', data, 0)
    pos = 18
    image_id = data[pos:pos + header[0]]
    pos += header[0]
    cmap_size = header[4] * ((header[5] + 7) // 8) if header[1] else 0
    colormap = data[pos:pos + cmap_size]
    pos += cmap_size
    end = _tga_pixel_span(data, pos, header)
    return TgaImage(header, image_id, colormap, data[pos:end], data[end:])


_DDS_FOURCC_BLOCK = {b'DXT1': 8, b'DXT2': 16, b'DXT3': 16, b'DXT4': 16, b'DXT5': 16, b'ATI1': 8, b'ATI2': 16}


def _dds_format_name(h: tuple[int, ...]) -> str:
    flags, fourcc, bits, r, g, b, a = h[19], h[20], h[21], h[22], h[23], h[24], h[25]
    if flags & 0x4:
        return struct.pack('<I', fourcc).decode('latin-1')
    masks = {(32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000): 'A8R8G8B8', (32, 0xFF0000, 0xFF00, 0xFF, 0): 'X8R8G8B8',
             (32, 0xFF, 0xFF00, 0xFF0000, 0xFF000000): 'A8B8G8R8', (24, 0xFF0000, 0xFF00, 0xFF, 0): 'R8G8B8',
             (16, 0x7C00, 0x3E0, 0x1F, 0x8000): 'A1R5G5B5', (16, 0xF00, 0xF0, 0xF, 0xF000): 'A4R4G4B4',
             (16, 0xF800, 0x7E0, 0x1F, 0): 'R5G6B5'}
    return masks.get((bits, r, g, b, a if flags & 0x1 else 0), f'RGB{bits}')


def _dds_alpha(h: tuple[int, ...]) -> bool:
    flags, fourcc = h[19], h[20]
    if flags & 0x4:
        return struct.pack('<I', fourcc) != b'DXT1'
    return bool(flags & 0x1)


def _dds_level_size(h: tuple[int, ...], width: int, height: int) -> int:
    flags, fourcc, bits = h[19], h[20], h[21]
    if flags & 0x4:
        block = _DDS_FOURCC_BLOCK.get(struct.pack('<I', fourcc))
        if block is None:
            return -1
        return max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * block
    return ((width * bits + 7) // 8) * height


def _parse_dds(data: bytes) -> DdsImage:
    if len(data) < 128:
        raise TxrFormatError('truncated DDS')
    header = struct.unpack_from('<31I', data, 4)
    if header[0] != 124 or header[18] != 32:
        raise TxrFormatError('invalid DDS header')
    pos = 128
    dx10 = b''
    if header[19] & 0x4 and struct.pack('<I', header[20]) == b'DX10':
        dx10 = data[pos:pos + 20]
        pos += 20
    image = DdsImage(header, dx10)
    height, width = header[2], header[3]
    mips = max(1, header[6]) if header[1] & 0x20000 else 1
    faces = 1
    if header[27] & 0x200:                         # DDSCAPS2_CUBEMAP
        faces = bin(header[27] & 0xFC00).count('1') or 6
    if not dx10:
        for _ in range(faces):
            w, h = width, height
            for _ in range(mips):
                size = _dds_level_size(header, w, h)
                if size < 0 or pos + size > len(data):
                    break
                image.surfaces.append(data[pos:pos + size])
                pos += size
                w, h = max(1, w // 2), max(1, h // 2)
    image.trailing = data[pos:]
    return image


def detect_kind(data: bytes) -> str:
    if data[:8] == PNG_SIGNATURE:
        return 'png'
    if data[:2] == b'\xff\xd8':
        return 'jpeg'
    if data[:2] == b'BM':
        return 'bmp'
    if data[:4] == b'DDS ':
        return 'dds'
    if len(data) >= 18 and data[1] in (0, 1) and data[2] in (1, 2, 3, 9, 10, 11) and data[16] in (8, 15, 16, 24, 32):
        return 'tga'
    raise TxrFormatError(f'unknown texture container (magic {data[:8].hex()})')


_PARSERS = {'png': _parse_png, 'jpeg': _parse_jpeg, 'bmp': _parse_bmp, 'tga': _parse_tga, 'dds': _parse_dds}


def parse(data: bytes) -> TxrFile:
    data = bytes(data)
    code = None
    if data[:8] == ZOATEX_MAGIC:
        if len(data) < 12:
            raise TxrFormatError('truncated ZOATEX1 header')
        code = struct.unpack_from('<I', data, 8)[0]
        data = data[12:]
    kind = detect_kind(data)
    return TxrFile(_PARSERS[kind](data), kind, code)


def serialize(txr: TxrFile) -> bytes:
    body = txr.image.to_bytes()
    if txr.zoatex_code is None:
        return body
    return ZOATEX_MAGIC + struct.pack('<I', txr.zoatex_code) + body


def validate(txr: TxrFile, *, for_model: bool = True) -> list[str]:
    """Problems for a texture used by a map model or terrain (empty list = fine)."""
    problems = []
    try:
        info = txr.info
    except TxrFormatError as error:
        return [str(error)]
    if not (1 <= info.width <= MAX_DIMENSION and 1 <= info.height <= MAX_DIMENSION):
        problems.append(f'size {info.width}x{info.height} outside 1..{MAX_DIMENSION}')
    if not info.power_of_two:
        problems.append(f'size {info.width}x{info.height} is not a power of two (D3DX resamples it)')
    if isinstance(txr.image, PngImage):
        for kind, data, crc in txr.image.chunks:
            if zlib.crc32(kind + data) & 0xFFFFFFFF != crc:
                problems.append(f'PNG chunk {kind!r} has a bad CRC')
    if txr.zoatex_code not in (None, 0) and for_model:
        problems.append('ZOATEX1 format code is ignored for model textures (DXT3 is forced)')
    return problems


def decode_rgba(txr: TxrFile) -> tuple[int, int, bytes]:
    """Decode to RGBA8 with Pillow (optional dependency; used for previews and checks only)."""
    import io
    from PIL import Image   # noqa: PLC0415 - optional
    with Image.open(io.BytesIO(txr.image.to_bytes())) as image:
        rgba = image.convert('RGBA')
        return rgba.width, rgba.height, rgba.tobytes()


# --------------------------------------------------------------------------- builder

def _check_rgba(width: int, height: int, rgba: bytes, require_pow2: bool) -> None:
    if not (1 <= width <= MAX_DIMENSION and 1 <= height <= MAX_DIMENSION):
        raise TxrFormatError(f'texture size {width}x{height} outside 1..{MAX_DIMENSION}')
    if require_pow2 and (width & (width - 1) or height & (height - 1)):
        raise TxrFormatError(f'texture size {width}x{height} is not a power of two')
    if len(rgba) != width * height * 4:
        raise TxrFormatError('RGBA buffer does not match width * height * 4')


def avoid_color_key(rgba: bytes) -> tuple[bytes, int]:
    """Nudge opaque pure magenta (the engine's colour key) to (254, 0, 255); returns (pixels, changed)."""
    key = bytes(COLOR_KEY)
    if key not in rgba:
        return rgba, 0
    out = bytearray(rgba)
    changed = 0
    for i in range(0, len(out), 4):
        if out[i:i + 4] == key:
            out[i] = 254
            changed += 1
    return bytes(out), changed


def _png_chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF)


def encode_png(width: int, height: int, rgba: bytes) -> bytes:
    """RGBA8 non-interlaced PNG using only the standard library (filter 0 per row)."""
    stride = width * 4
    raw = b''.join(b'\0' + rgba[y * stride:(y + 1) * stride] for y in range(height))
    return (PNG_SIGNATURE + _png_chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
            + _png_chunk(b'IDAT', zlib.compress(raw, 9)) + _png_chunk(b'IEND', b''))


def build_png_txr(width: int, height: int, rgba: bytes, *, require_pow2: bool = True,
                  fix_color_key: bool = True, zoatex_code: int | None = None) -> bytes:
    """Encode RGBA8 pixels (row-major, top row first) as a `.txr` holding a PNG."""
    _check_rgba(width, height, rgba, require_pow2)
    if fix_color_key:
        rgba, _ = avoid_color_key(rgba)
    return serialize(TxrFile(_parse_png(encode_png(width, height, rgba)), 'png', zoatex_code))


def build_jpeg_txr(width: int, height: int, rgba: bytes, *, quality: int = 92, require_pow2: bool = True) -> bytes:
    """Encode opaque RGBA8 pixels as a `.txr` holding a baseline JPEG (as many shipped opaque textures are).
    The alpha channel is dropped, so callers use this only for fully opaque images."""
    import io
    from PIL import Image
    _check_rgba(width, height, rgba, require_pow2)
    rgb = Image.frombytes('RGBA', (width, height), rgba).convert('RGB')
    out = io.BytesIO()
    rgb.save(out, format='JPEG', quality=quality, subsampling=0, optimize=True)
    return serialize(TxrFile(_parse_jpeg(out.getvalue()), 'jpeg', None))


def _half(width: int, height: int, rgba: bytes) -> tuple[int, int, bytes]:
    w2, h2 = max(1, width // 2), max(1, height // 2)
    out = bytearray(w2 * h2 * 4)
    for y in range(h2):
        y0 = min(height - 1, y * 2)
        y1 = min(height - 1, y * 2 + 1)
        for x in range(w2):
            x0 = min(width - 1, x * 2)
            x1 = min(width - 1, x * 2 + 1)
            for c in range(4):
                total = (rgba[(y0 * width + x0) * 4 + c] + rgba[(y0 * width + x1) * 4 + c]
                         + rgba[(y1 * width + x0) * 4 + c] + rgba[(y1 * width + x1) * 4 + c])
                out[(y * w2 + x) * 4 + c] = (total + 2) // 4
    return w2, h2, bytes(out)


def _rgba_to_bgra(rgba: bytes) -> bytes:
    out = bytearray(rgba)
    out[0::4] = rgba[2::4]
    out[2::4] = rgba[0::4]
    return bytes(out)


def build_dds_txr(width: int, height: int, rgba: bytes, *, mipmaps: bool = True, require_pow2: bool = True,
                  fix_color_key: bool = True) -> bytes:
    """Encode RGBA8 pixels as an uncompressed A8R8G8B8 DDS `.txr` (box-filtered mip chain when requested)."""
    _check_rgba(width, height, rgba, require_pow2)
    if fix_color_key:
        rgba, _ = avoid_color_key(rgba)
    levels = [_rgba_to_bgra(rgba)]
    w, h, current = width, height, rgba
    while mipmaps and (w > 1 or h > 1):
        w, h, current = _half(w, h, current)
        levels.append(_rgba_to_bgra(current))
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x8          # CAPS | HEIGHT | WIDTH | PIXELFORMAT | PITCH
    caps = 0x1000                                   # DDSCAPS_TEXTURE
    if len(levels) > 1:
        flags |= 0x20000                            # MIPMAPCOUNT
        caps |= 0x8 | 0x400000                      # COMPLEX | MIPMAP
    header = [124, flags, height, width, width * 4, 0, len(levels) if len(levels) > 1 else 0] + [0] * 11
    header += [32, 0x41, 0, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000]   # DDPF_RGB | DDPF_ALPHAPIXELS
    header += [caps, 0, 0, 0, 0]
    return serialize(TxrFile(DdsImage(tuple(header), b'', levels), 'dds'))


def build_txr(width: int, height: int, rgba: bytes, container: str = 'png', **options) -> bytes:
    """Builder entry point: container 'png' (default) or 'dds' (A8R8G8B8)."""
    if container == 'png':
        return build_png_txr(width, height, rgba, **options)
    if container == 'dds':
        return build_dds_txr(width, height, rgba, **options)
    raise TxrFormatError(f'unsupported container {container!r}')
