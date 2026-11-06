"""Read immutable KCAP/ATAD source bytes; never execute the source application."""
from __future__ import annotations

from dataclasses import asdict, dataclass
import hashlib
import mmap
from pathlib import Path
import struct
from typing import Iterator

from .codec import MAX_OUTPUT, DecodedStream, decode_stream, read_varuint


ATAD_PREFIX_SIZE = 18
# The 16/32/48-byte opaque identity extent is stored in the header itself.
# Its meaning (e.g. encrypted path versus hash) belongs to the native lookup
# recovery; the storage extent does not require guessing that meaning.
ATAD_IDENTITY_SIZES = (16, 32, 48)


@dataclass(frozen=True)
class Block:
    index: int
    offset: int
    flags: int
    expanded_size: int
    compressed_size: int
    extra: int
    identity_hash_hex: str

    @property
    def header_size(self) -> int:
        return ATAD_PREFIX_SIZE + self.flags

    @property
    def end(self) -> int:
        return self.offset + self.header_size + self.compressed_size


@dataclass(frozen=True)
class DecodedBlock:
    block: Block
    data: bytes
    compressed_sha256: str
    expanded_sha256: str
    consumed_size: int
    trailer_hex: str

    def provenance(self) -> dict:
        return {**asdict(self.block), "compressed_sha256": self.compressed_sha256,
                "expanded_sha256": self.expanded_sha256, "consumed_size": self.consumed_size,
                "trailer_hex": self.trailer_hex}


class Archive:
    def __init__(self, path: str | Path):
        self.path = Path(path).resolve(strict=True)
        if not self.path.is_file():
            raise ValueError("KCAP input must be a regular file")
        self._stat = self.path.stat()
        with self.path.open("rb") as stream:
            stream.seek(0xA00)
            self.header = stream.read(40)
        if self.header[:4] != b"KCAP":
            raise ValueError("KCAP source header absent at 0xA00")

    def verify_unchanged(self) -> None:
        now = self.path.stat()
        if (now.st_size, now.st_mtime_ns) != (self._stat.st_size, self._stat.st_mtime_ns):
            raise ValueError("KCAP source changed during recovery")

    def sha256(self) -> str:
        h = hashlib.sha256()
        with self.path.open("rb") as stream:
            for data in iter(lambda: stream.read(8*1024*1024), b""):
                h.update(data)
        self.verify_unchanged()
        return h.hexdigest()

    @staticmethod
    def _block(index: int, offset: int, header: bytes) -> Block:
        if len(header) < ATAD_PREFIX_SIZE or header[:4] != b"ATAD":
            raise ValueError("ATAD header mismatch")
        flags, expanded, compressed, extra = struct.unpack_from("<HIII", header, 4)
        if flags not in ATAD_IDENTITY_SIZES or len(header) != ATAD_PREFIX_SIZE+flags:
            raise ValueError("ATAD identity extent mismatch")
        return Block(index, offset, flags, expanded, compressed, extra, header[18:].hex())

    def scan(self) -> Iterator[Block]:
        """Find non-overlapping stream-validated blocks, independent of old extraction numbering.

        Source payloads are skipped in their entirety. A magic string without
        matching inner size declarations is not promoted to an archive entry.
        The 16-byte identity is retained opaque until its native consumer is
        recovered; it is not claimed to be a filename or content MD5.
        """
        with self.path.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
            position, index = 0xA28, 0
            while position + ATAD_PREFIX_SIZE <= len(data):
                position = data.find(b"ATAD", position)
                if position < 0:
                    break
                try:
                    identity_size = struct.unpack_from("<H", data, position+4)[0]
                    block = self._block(index, position, data[position:position+ATAD_PREFIX_SIZE+identity_size])
                    if not (0 < block.compressed_size <= MAX_OUTPUT and 0 <= block.expanded_size <= MAX_OUTPUT
                            and block.end <= len(data)):
                        raise ValueError("ATAD size budget or file range")
                    start = position+block.header_size
                    prefix = data[start:min(block.end,start+18)]
                    expanded, cursor = read_varuint(prefix, 0)
                    compressed, _ = read_varuint(prefix, cursor)
                    if (expanded, compressed) != (block.expanded_size, block.compressed_size):
                        raise ValueError("inner stream sizes differ from ATAD header")
                except (ValueError, struct.error):
                    position += 4
                    continue
                yield block
                position, index = block.end, index+1
        self.verify_unchanged()

    def compressed(self, block: Block) -> bytes:
        self.verify_unchanged()
        with self.path.open("rb") as stream:
            stream.seek(block.offset)
            actual = self._block(block.index, block.offset, stream.read(block.header_size))
            if actual != block:
                raise ValueError("ATAD block identity changed")
            data = stream.read(block.compressed_size)
        if len(data) != block.compressed_size:
            raise ValueError("truncated ATAD stream")
        return data

    def read(self, block: Block, *, tail_policy: str = "exact", decoder=None) -> DecodedBlock:
        compressed = self.compressed(block)
        result: DecodedStream = (decoder or decode_stream)(compressed, tail_policy=tail_policy)
        if len(result.data) != block.expanded_size:
            raise ValueError("decoded size differs from ATAD header")
        return DecodedBlock(block, result.data, hashlib.sha256(compressed).hexdigest(),
                            hashlib.sha256(result.data).hexdigest(), result.consumed_size, result.trailer.hex())
