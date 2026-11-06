"""Union KCAP archive-LOAD codec recovered from native VA 0x00CB32C0.

This profile includes the owning loader's behavior: a freshly initialized
dictionary context, a completely zeroed destination, and rejection of a zero
return value. The native command stream succeeds only through an A/B short
group. C/D commands therefore need a following A/B terminal marker when they
finish the declared output.
"""
from __future__ import annotations

from dataclasses import dataclass
import struct


MAX_OUTPUT = 512 * 1024 * 1024
CODEC_PROFILE = "union-native-load-cb32c0-zeroed-v2"


@dataclass(frozen=True)
class DecodedStream:
    data: bytes
    consumed_size: int
    trailer: bytes


def available(data: bytes, position: int, size: int) -> None:
    if position < 0 or size < 0 or position + size > len(data):
        raise ValueError(
            f"truncated KCAP stream at {position}: "
            f"need {size}, have {len(data) - position}"
        )


def read_varuint(data: bytes, position: int) -> tuple[int, int]:
    available(data, position, 1)
    tag = data[position]
    if tag < 0x40:
        return tag, position + 1
    width = (2, 4, 8)[(tag >> 6) - 1]
    available(data, position + 1, width)
    return (
        int.from_bytes(data[position + 1 : position + 1 + width], "little"),
        position + 1 + width,
    )


def _header(data: bytes) -> tuple[int, int, int]:
    """Read the two sizes and reproduce the native coupled header extent."""
    expanded, second = read_varuint(data, 0)
    compressed, _ = read_varuint(data, second)
    first_tag = data[0]
    if first_tag < 0x40:
        position = 2
    else:
        position = {1: 6, 2: 10, 3: 18}[first_tag >> 6]
    available(data, 0, position)
    return expanded, compressed, position


def hash32(value: int) -> int:
    return ((value * 0x9E3779B1) & 0xFFFFFFFF) >> 16


def hash64(low: int, high: int) -> int:
    return (
        (
            high * 0x7F4A7C15
            + ((low * 0x7F4A7C15) >> 32)
            + low * 0x9E3779B9
        )
        & 0xFFFFFFFF
    ) >> 16


def decode_stream(
    data: bytes,
    *,
    tail_policy: str = "exact",
    maximum_output_size: int = MAX_OUTPUT,
) -> DecodedStream:
    """Decode one archive block with the recovered native LOAD semantics.

    ``exact`` requires the terminal A/B command to end at the declared
    compressed boundary. ``native-output-boundary`` records bytes after that
    terminal command because the native routine returns as soon as output is
    complete and accounts the full declared compressed size in its context.
    """
    if tail_policy not in {"exact", "native-output-boundary"}:
        raise ValueError("unknown codec tail policy")
    if not isinstance(data, bytes):
        data = bytes(data)

    expanded, compressed, position = _header(data)
    if expanded > maximum_output_size:
        raise ValueError("KCAP expanded-size budget exceeded")
    if expanded == 0:
        raise ValueError("KCAP zero-length output is rejected by native LOAD")
    if compressed != len(data):
        raise ValueError(
            f"KCAP compressed-size mismatch: declared={compressed}, actual={len(data)}"
        )
    # Native 0xCB32C0 refuses implausibly large compressed streams before
    # dispatch. Archive values are 32-bit and do not approach multiplication
    # overflow under the public output budget.
    if compressed > ((expanded * 0x44) >> 6) + 100:
        raise ValueError("KCAP compressed-size expansion bound exceeded")

    d32 = [0] * 65536
    d64 = [(0, 0)] * 65536
    output = bytearray(expanded)
    output_position = 0

    def dictionary_group(width: int, count: int) -> None:
        nonlocal position, output_position
        available(data, position, 2)
        mask = struct.unpack_from("<H", data, position)[0]
        position += 2
        for _ in range(count):
            reference = bool(mask & 0x8000)
            mask = (mask << 1) & 0xFFFF
            if reference:
                available(data, position, 2)
                index = struct.unpack_from("<H", data, position)[0]
                position += 2
                if width == 4:
                    value = struct.pack("<I", d32[index])
                else:
                    value = struct.pack("<II", *d64[index])
            else:
                available(data, position, width)
                value = data[position : position + width]
                position += width
                if width == 4:
                    word = struct.unpack("<I", value)[0]
                    d32[hash32(word)] = word
                else:
                    low, high = struct.unpack("<II", value)
                    d64[hash64(low, high)] = (low, high)
            output[output_position : output_position + width] = value
            output_position += width

    while True:
        available(data, position, 1)
        command = data[position]
        position += 1

        if command == 0x43:
            count, position = read_varuint(data, position)
            available(data, position, count)
            if count > expanded - output_position:
                raise ValueError("KCAP literal output overrun")
            complete = count & ~7
            if complete:
                output[
                    output_position : output_position + complete
                ] = data[position : position + complete]
            # 0xCB3A26..0xCB3B1A copies complete qwords only, then advances
            # both cursors by the full declared count. The LOAD destination was
            # prezeroed, so the residual bytes remain zero.
            position += count
            output_position += count
            continue

        if command == 0x44:
            count, position = read_varuint(data, position)
            available(data, position, 8)
            if count > expanded - output_position:
                raise ValueError("KCAP pattern output overrun")
            pattern = data[position : position + 8]
            position += 8
            complete = count & ~7
            for offset in range(0, complete, 8):
                output[
                    output_position + offset : output_position + offset + 8
                ] = pattern
            # 0xCB3B85..0xCB3C49 emits complete qword repetitions and advances
            # the destination by the full count, preserving zero remainder.
            output_position += count
            continue

        if command not in (0x41, 0x42):
            raise ValueError(
                f"unknown KCAP command 0x{command:02x} at {position - 1}"
            )

        width = 4 if command == 0x41 else 8
        remaining = expanded - output_position
        if remaining < 16 * width:
            complete_words = remaining // width
            if complete_words:
                dictionary_group(width, complete_words)
            residual = expanded - output_position
            available(data, position, residual)
            if residual:
                output[
                    output_position : output_position + residual
                ] = data[position : position + residual]
                position += residual
                output_position += residual
            # This is the sole native success path. A full 16-word group must
            # dispatch another command instead of consuming a residual tail.
            break

        dictionary_group(width, 16)

    if output_position != expanded:
        raise ValueError("KCAP expanded-size mismatch")
    trailer = data[position:]
    if tail_policy == "exact" and trailer:
        raise ValueError(
            f"KCAP unread suffix: consumed={position}, "
            f"bytes={len(data)}, suffix={trailer[:16].hex()}"
        )
    return DecodedStream(bytes(output), position, trailer)
