"""Lossless structural reader for LuaJIT 2.1 bytecode dumps.

The format and opcode tables are grounded in the official LuaJIT sources:

* ``lj_bcdump.h`` / ``lj_bcread.c`` at LuaJIT commit
  ``f4231949b5e14e1116452f9e9eea31e19a462c82`` for the historical version-2
  format used by G Union.
* ``lj_bc.h`` at that commit for the 97-opcode legacy profile.
* ``lj_bcdump.h`` / ``lj_bc.h`` at commit
  ``a2ce8114f107464473070427e69e84f4923bd08a`` for the optional BITOP flag
  and seven opcodes added in 2026.

This module inventories bytes and structure. It does not execute bytecode,
translate it to Lua source, or establish compatibility with either the native
SDGO host or a Minecraft content consumer.
"""
from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
import base64
import hashlib
import struct
from typing import Any, Iterable


MAGIC = b"\x1bLJ"
VERSION_2 = 2

FLAG_BIG_ENDIAN = 0x01
FLAG_STRIPPED = 0x02
FLAG_FFI = 0x04
FLAG_FR2 = 0x08
FLAG_BITOP = 0x10
FLAG_DETERMINISTIC = 0x80000000
KNOWN_FLAGS = (
    FLAG_BIG_ENDIAN
    | FLAG_STRIPPED
    | FLAG_FFI
    | FLAG_FR2
    | FLAG_BITOP
    | FLAG_DETERMINISTIC
)

PROTO_CHILD = 0x01
PROTO_VARARG = 0x02
PROTO_FFI = 0x04
KNOWN_PROTO_FLAGS = PROTO_CHILD | PROTO_VARARG | PROTO_FFI

UV_LOCAL = 0x8000
UV_IMMUTABLE = 0x4000
UV_INDEX_MASK = 0x3FFF

KGC_CHILD = 0
KGC_TABLE = 1
KGC_I64 = 2
KGC_U64 = 3
KGC_COMPLEX = 4
KGC_STRING = 5

KTAB_NIL = 0
KTAB_FALSE = 1
KTAB_TRUE = 2
KTAB_INT = 3
KTAB_NUMBER = 4
KTAB_STRING = 5

_SPECIAL_VARIABLE_NAMES = {
    1: "(for index)",
    2: "(for limit)",
    3: "(for step)",
    4: "(for generator)",
    5: "(for state)",
    6: "(for control)",
}


class LuaJITFormatError(ValueError):
    """The input cannot be parsed as a bounded LuaJIT bytecode dump."""


class UnsupportedLuaJITFeature(LuaJITFormatError):
    """The dump declares a format feature whose layout is unknown."""


@dataclass(frozen=True)
class ParseLimits:
    """Allocation and recursion limits for untrusted bytecode."""

    maximum_bytes: int = 512 * 1024 * 1024
    maximum_prototype_bytes: int = 128 * 1024 * 1024
    maximum_prototypes: int = 100_000
    maximum_instructions: int = 10_000_000
    maximum_constants: int = 10_000_000
    maximum_table_entries: int = 1_000_000
    maximum_string_bytes: int = 128 * 1024 * 1024
    maximum_depth: int = 4096

    def __post_init__(self) -> None:
        for name, value in vars(self).items():
            if not isinstance(value, int) or value < 0:
                raise ValueError(f"{name} must be a non-negative integer")


@dataclass(frozen=True)
class OpcodeSpec:
    name: str
    a_mode: str
    b_mode: str
    cd_mode: str

    @property
    def form(self) -> str:
        return "AD" if self.b_mode == "___" else "ABC"


def _opcode(name: str, a: str, b: str, cd: str) -> OpcodeSpec:
    return OpcodeSpec(name, a, b, cd)


# Exact order from the historical LuaJIT 2.1 BCDEF macro.
LEGACY_OPCODE_SPECS = (
    _opcode("ISLT", "var", "___", "var"),
    _opcode("ISGE", "var", "___", "var"),
    _opcode("ISLE", "var", "___", "var"),
    _opcode("ISGT", "var", "___", "var"),
    _opcode("ISEQV", "var", "___", "var"),
    _opcode("ISNEV", "var", "___", "var"),
    _opcode("ISEQS", "var", "___", "str"),
    _opcode("ISNES", "var", "___", "str"),
    _opcode("ISEQN", "var", "___", "num"),
    _opcode("ISNEN", "var", "___", "num"),
    _opcode("ISEQP", "var", "___", "pri"),
    _opcode("ISNEP", "var", "___", "pri"),
    _opcode("ISTC", "dst", "___", "var"),
    _opcode("ISFC", "dst", "___", "var"),
    _opcode("IST", "___", "___", "var"),
    _opcode("ISF", "___", "___", "var"),
    _opcode("ISTYPE", "var", "___", "lit"),
    _opcode("ISNUM", "var", "___", "lit"),
    _opcode("MOV", "dst", "___", "var"),
    _opcode("NOT", "dst", "___", "var"),
    _opcode("UNM", "dst", "___", "var"),
    _opcode("LEN", "dst", "___", "var"),
    _opcode("ADDVN", "dst", "var", "num"),
    _opcode("SUBVN", "dst", "var", "num"),
    _opcode("MULVN", "dst", "var", "num"),
    _opcode("DIVVN", "dst", "var", "num"),
    _opcode("MODVN", "dst", "var", "num"),
    _opcode("ADDNV", "dst", "var", "num"),
    _opcode("SUBNV", "dst", "var", "num"),
    _opcode("MULNV", "dst", "var", "num"),
    _opcode("DIVNV", "dst", "var", "num"),
    _opcode("MODNV", "dst", "var", "num"),
    _opcode("ADDVV", "dst", "var", "var"),
    _opcode("SUBVV", "dst", "var", "var"),
    _opcode("MULVV", "dst", "var", "var"),
    _opcode("DIVVV", "dst", "var", "var"),
    _opcode("MODVV", "dst", "var", "var"),
    _opcode("POW", "dst", "var", "var"),
    _opcode("CAT", "dst", "rbase", "rbase"),
    _opcode("KSTR", "dst", "___", "str"),
    _opcode("KCDATA", "dst", "___", "cdata"),
    _opcode("KSHORT", "dst", "___", "lits"),
    _opcode("KNUM", "dst", "___", "num"),
    _opcode("KPRI", "dst", "___", "pri"),
    _opcode("KNIL", "base", "___", "base"),
    _opcode("UGET", "dst", "___", "uv"),
    _opcode("USETV", "uv", "___", "var"),
    _opcode("USETS", "uv", "___", "str"),
    _opcode("USETN", "uv", "___", "num"),
    _opcode("USETP", "uv", "___", "pri"),
    _opcode("UCLO", "rbase", "___", "jump"),
    _opcode("FNEW", "dst", "___", "func"),
    _opcode("TNEW", "dst", "___", "lit"),
    _opcode("TDUP", "dst", "___", "tab"),
    _opcode("GGET", "dst", "___", "str"),
    _opcode("GSET", "var", "___", "str"),
    _opcode("TGETV", "dst", "var", "var"),
    _opcode("TGETS", "dst", "var", "str"),
    _opcode("TGETB", "dst", "var", "lit"),
    _opcode("TGETR", "dst", "var", "var"),
    _opcode("TSETV", "var", "var", "var"),
    _opcode("TSETS", "var", "var", "str"),
    _opcode("TSETB", "var", "var", "lit"),
    _opcode("TSETM", "base", "___", "num"),
    _opcode("TSETR", "var", "var", "var"),
    _opcode("CALLM", "base", "lit", "lit"),
    _opcode("CALL", "base", "lit", "lit"),
    _opcode("CALLMT", "base", "___", "lit"),
    _opcode("CALLT", "base", "___", "lit"),
    _opcode("ITERC", "base", "lit", "lit"),
    _opcode("ITERN", "base", "lit", "lit"),
    _opcode("VARG", "base", "lit", "lit"),
    _opcode("ISNEXT", "base", "___", "jump"),
    _opcode("RETM", "base", "___", "lit"),
    _opcode("RET", "rbase", "___", "lit"),
    _opcode("RET0", "rbase", "___", "lit"),
    _opcode("RET1", "rbase", "___", "lit"),
    _opcode("FORI", "base", "___", "jump"),
    _opcode("JFORI", "base", "___", "jump"),
    _opcode("FORL", "base", "___", "jump"),
    _opcode("IFORL", "base", "___", "jump"),
    _opcode("JFORL", "base", "___", "lit"),
    _opcode("ITERL", "base", "___", "jump"),
    _opcode("IITERL", "base", "___", "jump"),
    _opcode("JITERL", "base", "___", "lit"),
    _opcode("LOOP", "rbase", "___", "jump"),
    _opcode("ILOOP", "rbase", "___", "jump"),
    _opcode("JLOOP", "rbase", "___", "lit"),
    _opcode("JMP", "rbase", "___", "jump"),
    _opcode("FUNCF", "rbase", "___", "___"),
    _opcode("IFUNCF", "rbase", "___", "___"),
    _opcode("JFUNCF", "rbase", "___", "lit"),
    _opcode("FUNCV", "rbase", "___", "___"),
    _opcode("IFUNCV", "rbase", "___", "___"),
    _opcode("JFUNCV", "rbase", "___", "lit"),
    _opcode("FUNCC", "rbase", "___", "___"),
    _opcode("FUNCCW", "rbase", "___", "___"),
)

_BITOPS = (
    _opcode("BNOT", "dst", "___", "var"),
    _opcode("BAND", "dst", "var", "var"),
    _opcode("BOR", "dst", "var", "var"),
    _opcode("BXOR", "dst", "var", "var"),
    _opcode("BSHL", "dst", "var", "var"),
    _opcode("BSHR", "dst", "var", "var"),
    _opcode("BSAR", "dst", "var", "var"),
)

# The 2026 BITOP additions occur immediately before the function headers.
BITOP_OPCODE_SPECS = LEGACY_OPCODE_SPECS[:-8] + _BITOPS + LEGACY_OPCODE_SPECS[-8:]


@dataclass(frozen=True)
class Instruction:
    pc: int
    offset: int
    raw_bytes: bytes
    raw_word: int
    opcode: int
    opcode_name: str
    form: str
    a_mode: str
    b_mode: str
    cd_mode: str
    a: int
    b: int
    c: int
    d: int
    d_signed: int
    jump: int


@dataclass(frozen=True)
class UpvalueReference:
    index: int
    raw: int
    source: str
    source_index: int
    immutable: bool


@dataclass(frozen=True)
class TableValue:
    kind: str
    type_code: int
    value: Any = None
    raw_words: tuple[int, ...] = ()


@dataclass(frozen=True)
class TableConstant:
    array: tuple[TableValue, ...]
    hash_entries: tuple[tuple[TableValue, TableValue], ...]


@dataclass(frozen=True)
class GCConstant:
    serialized_index: int
    operand_index: int
    kind: str
    type_code: int
    value: Any = None
    raw_words: tuple[int, ...] = ()
    child_prototype_id: int | None = None


@dataclass(frozen=True)
class NumericConstant:
    index: int
    kind: str
    value: int | float
    raw_words: tuple[int, ...]


@dataclass(frozen=True)
class DebugVariable:
    name: bytes | None
    special_name: str | None
    start_pc: int
    end_pc: int


@dataclass(frozen=True)
class DebugInfo:
    raw: bytes
    first_line: int
    line_span: int
    line_deltas: tuple[int, ...]
    lines: tuple[int, ...]
    upvalue_names: tuple[bytes, ...]
    variables: tuple[DebugVariable, ...]


@dataclass(frozen=True)
class Prototype:
    prototype_id: int
    envelope_offset: int
    payload_offset: int
    payload_size: int
    raw_payload: bytes
    sha256: str
    flags: int
    num_parameters: int
    frame_size: int
    upvalues: tuple[UpvalueReference, ...]
    instructions: tuple[Instruction, ...]
    gc_constants: tuple[GCConstant, ...]
    numeric_constants: tuple[NumericConstant, ...]
    child_prototype_ids: tuple[int, ...]
    debug: DebugInfo | None
    depth: int
    issues: tuple[str, ...]

    def gc_constant_for_operand(self, operand_index: int) -> GCConstant | None:
        """Resolve a KGC operand, whose encoded order is reverse serialized order."""
        serialized_index = len(self.gc_constants) - 1 - operand_index
        if 0 <= serialized_index < len(self.gc_constants):
            return self.gc_constants[serialized_index]
        return None


@dataclass(frozen=True)
class HostAPIReference:
    name: bytes
    access: str
    prototype_id: int
    pc: int


@dataclass(frozen=True)
class LuaJITDump:
    raw: bytes
    sha256: str
    version: int
    flags: int
    flag_names: tuple[str, ...]
    chunk_name: bytes | None
    byte_order: str
    opcode_profile: str
    prototypes: tuple[Prototype, ...]
    root_prototype_id: int
    host_api_references: tuple[HostAPIReference, ...]
    host_requirements: tuple[str, ...]
    unsupported_features: tuple[str, ...]
    issues: tuple[str, ...]

    def all_strings(self) -> tuple[bytes, ...]:
        values: list[bytes] = []
        if self.chunk_name is not None:
            values.append(self.chunk_name)
        for prototype in self.prototypes:
            for constant in prototype.gc_constants:
                if constant.kind == "string":
                    values.append(constant.value)
                elif constant.kind == "table":
                    values.extend(_table_strings(constant.value))
            if prototype.debug is not None:
                values.extend(prototype.debug.upvalue_names)
                values.extend(
                    variable.name
                    for variable in prototype.debug.variables
                    if variable.name is not None
                )
        return tuple(values)

    def inventory(self, *, include_raw: bool = False) -> dict[str, Any]:
        """Return a JSON-safe structural inventory, optionally embedding source bytes."""
        parent_ids: dict[int, int] = {}
        for prototype in self.prototypes:
            for child_id in prototype.child_prototype_ids:
                parent_ids[child_id] = prototype.prototype_id
        result: dict[str, Any] = {
            "format": "LuaJIT-bytecode",
            "size": len(self.raw),
            "sha256": self.sha256,
            "version": self.version,
            "flags": self.flags,
            "flag_names": list(self.flag_names),
            "byte_order": self.byte_order,
            "stripped": bool(self.flags & FLAG_STRIPPED),
            "chunk_name": _bytes_inventory(self.chunk_name),
            "opcode_profile": self.opcode_profile,
            "root_prototype_id": self.root_prototype_id,
            "prototype_count": len(self.prototypes),
            "host_requirements": list(self.host_requirements),
            "unsupported_features": list(self.unsupported_features),
            "issues": list(self.issues),
            "host_api_references": [
                {
                    "name": _bytes_inventory(reference.name),
                    "access": reference.access,
                    "prototype_id": reference.prototype_id,
                    "pc": reference.pc,
                }
                for reference in self.host_api_references
            ],
            "prototypes": [],
        }
        for prototype in self.prototypes:
            opcode_counts = Counter(
                instruction.opcode_name for instruction in prototype.instructions
            )
            result["prototypes"].append(
                {
                    "id": prototype.prototype_id,
                    "parent_id": parent_ids.get(prototype.prototype_id),
                    "child_ids": list(prototype.child_prototype_ids),
                    "depth": prototype.depth,
                    "envelope_offset": prototype.envelope_offset,
                    "payload_offset": prototype.payload_offset,
                    "payload_size": prototype.payload_size,
                    "payload_sha256": prototype.sha256,
                    "flags": prototype.flags,
                    "num_parameters": prototype.num_parameters,
                    "frame_size": prototype.frame_size,
                    "upvalue_count": len(prototype.upvalues),
                    "instruction_count": len(prototype.instructions),
                    "opcode_counts": dict(sorted(opcode_counts.items())),
                    "gc_constant_count": len(prototype.gc_constants),
                    "numeric_constant_count": len(prototype.numeric_constants),
                    "strings": [
                        _bytes_inventory(constant.value)
                        for constant in prototype.gc_constants
                        if constant.kind == "string"
                    ],
                    "issues": list(prototype.issues),
                }
            )
        if include_raw:
            result["raw_base64"] = base64.b64encode(self.raw).decode("ascii")
        return result


@dataclass
class _Budget:
    limits: ParseLimits
    instructions: int = 0
    constants: int = 0
    table_entries: int = 0
    string_bytes: int = 0

    def add(self, field: str, amount: int, maximum: int) -> None:
        value = getattr(self, field) + amount
        if value > maximum:
            label = {
                "instructions": "instruction",
                "constants": "constant",
                "table_entries": "table-entry",
                "string_bytes": "string-byte",
            }.get(field, field.replace("_", "-"))
            raise LuaJITFormatError(f"LuaJIT {label} budget exceeded")
        setattr(self, field, value)


class _Reader:
    def __init__(self, data: bytes, start: int = 0, end: int | None = None):
        self.data = data
        self.start = start
        self.position = start
        self.end = len(data) if end is None else end
        if not 0 <= self.start <= self.end <= len(data):
            raise ValueError("invalid reader range")

    @property
    def remaining(self) -> int:
        return self.end - self.position

    def read(self, size: int, what: str) -> bytes:
        if size < 0 or size > self.remaining:
            raise LuaJITFormatError(
                f"truncated LuaJIT {what} at 0x{self.position:x}: "
                f"need {size}, have {self.remaining}"
            )
        start = self.position
        self.position += size
        return self.data[start:self.position]

    def byte(self, what: str) -> int:
        return self.read(1, what)[0]

    def peek(self) -> int:
        if self.remaining == 0:
            raise LuaJITFormatError(f"truncated LuaJIT data at 0x{self.position:x}")
        return self.data[self.position]

    def uleb32(self, what: str) -> int:
        value = 0
        for index in range(5):
            byte = self.byte(what)
            payload = byte & 0x7F
            if index == 4 and payload > 0x0F:
                raise LuaJITFormatError(f"overflowing LuaJIT ULEB128 for {what}")
            value |= payload << (7 * index)
            if byte < 0x80:
                return value
        raise LuaJITFormatError(f"unterminated LuaJIT ULEB128 for {what}")

    def uleb33_low(self, what: str) -> tuple[bool, int]:
        first = self.byte(what)
        is_number = bool(first & 1)
        value = (first >> 1) & 0x3F
        if first < 0x80:
            return is_number, value
        shift = 6
        for index in range(4):
            byte = self.byte(what)
            payload = byte & 0x7F
            if index == 3 and payload > 0x1F:
                raise LuaJITFormatError(f"overflowing LuaJIT 33-bit ULEB128 for {what}")
            value |= payload << shift
            if byte < 0x80:
                return is_number, value
            shift += 7
        raise LuaJITFormatError(f"unterminated LuaJIT 33-bit ULEB128 for {what}")

    def cstring(self, what: str) -> bytes:
        terminator = self.data.find(b"\0", self.position, self.end)
        if terminator < 0:
            raise LuaJITFormatError(f"unterminated LuaJIT {what}")
        value = self.data[self.position:terminator]
        self.position = terminator + 1
        return value


def opcode_number(name: str, *, bitop: bool = False) -> int:
    """Return an opcode number from the selected official version-2 profile."""
    specs = BITOP_OPCODE_SPECS if bitop else LEGACY_OPCODE_SPECS
    for number, spec in enumerate(specs):
        if spec.name == name:
            return number
    raise KeyError(name)


def _flag_names(flags: int) -> tuple[str, ...]:
    names = []
    for bit, name in (
        (FLAG_BIG_ENDIAN, "big-endian"),
        (FLAG_STRIPPED, "stripped"),
        (FLAG_FFI, "ffi"),
        (FLAG_FR2, "fr2"),
        (FLAG_BITOP, "bitop"),
        (FLAG_DETERMINISTIC, "deterministic"),
    ):
        if flags & bit:
            names.append(name)
    return tuple(names)


def _to_signed(value: int, bits: int) -> int:
    sign = 1 << (bits - 1)
    return value - (1 << bits) if value & sign else value


def _double(low: int, high: int) -> float:
    return struct.unpack("<d", struct.pack("<II", low, high))[0]


def _read_string(reader: _Reader, length: int, budget: _Budget, what: str) -> bytes:
    budget.add("string_bytes", length, budget.limits.maximum_string_bytes)
    return reader.read(length, what)


def _read_table_value(reader: _Reader, budget: _Budget) -> TableValue:
    type_code = reader.uleb32("table constant type")
    if type_code >= KTAB_STRING:
        length = type_code - KTAB_STRING
        return TableValue(
            "string",
            type_code,
            _read_string(reader, length, budget, "table string"),
        )
    if type_code == KTAB_INT:
        word = reader.uleb32("table integer")
        return TableValue("integer", type_code, _to_signed(word, 32), (word,))
    if type_code == KTAB_NUMBER:
        low = reader.uleb32("table number low word")
        high = reader.uleb32("table number high word")
        return TableValue("number", type_code, _double(low, high), (low, high))
    if type_code == KTAB_NIL:
        return TableValue("nil", type_code, None)
    if type_code == KTAB_FALSE:
        return TableValue("boolean", type_code, False)
    if type_code == KTAB_TRUE:
        return TableValue("boolean", type_code, True)
    raise LuaJITFormatError(f"unknown LuaJIT table constant type {type_code}")


def _read_table(reader: _Reader, budget: _Budget) -> TableConstant:
    array_count = reader.uleb32("table array count")
    hash_count = reader.uleb32("table hash count")
    total = array_count + hash_count
    budget.add("table_entries", total, budget.limits.maximum_table_entries)
    array = tuple(_read_table_value(reader, budget) for _ in range(array_count))
    entries = []
    for _ in range(hash_count):
        key = _read_table_value(reader, budget)
        if key.kind == "nil":
            raise LuaJITFormatError("nil LuaJIT table hash key")
        entries.append((key, _read_table_value(reader, budget)))
    return TableConstant(array, tuple(entries))


def _read_gc_constants(
    reader: _Reader,
    count: int,
    child_stack: list[int],
    budget: _Budget,
) -> tuple[tuple[GCConstant, ...], tuple[int, ...]]:
    constants = []
    children = []
    for serialized_index in range(count):
        type_code = reader.uleb32("GC constant type")
        operand_index = count - 1 - serialized_index
        if type_code >= KGC_STRING:
            length = type_code - KGC_STRING
            constants.append(
                GCConstant(
                    serialized_index,
                    operand_index,
                    "string",
                    type_code,
                    _read_string(reader, length, budget, "GC string"),
                )
            )
        elif type_code == KGC_CHILD:
            if not child_stack:
                raise LuaJITFormatError("LuaJIT child prototype stack underflow")
            child_id = child_stack.pop()
            children.append(child_id)
            constants.append(
                GCConstant(
                    serialized_index,
                    operand_index,
                    "child",
                    type_code,
                    child_prototype_id=child_id,
                )
            )
        elif type_code == KGC_TABLE:
            constants.append(
                GCConstant(
                    serialized_index,
                    operand_index,
                    "table",
                    type_code,
                    _read_table(reader, budget),
                )
            )
        elif type_code in (KGC_I64, KGC_U64):
            low = reader.uleb32("64-bit constant low word")
            high = reader.uleb32("64-bit constant high word")
            combined = (high << 32) | low
            if type_code == KGC_I64:
                combined = _to_signed(combined, 64)
            constants.append(
                GCConstant(
                    serialized_index,
                    operand_index,
                    "int64" if type_code == KGC_I64 else "uint64",
                    type_code,
                    combined,
                    (low, high),
                )
            )
        elif type_code == KGC_COMPLEX:
            words = tuple(
                reader.uleb32(f"complex constant word {index}") for index in range(4)
            )
            constants.append(
                GCConstant(
                    serialized_index,
                    operand_index,
                    "complex",
                    type_code,
                    complex(_double(words[0], words[1]), _double(words[2], words[3])),
                    words,
                )
            )
        else:
            raise LuaJITFormatError(f"unknown LuaJIT GC constant type {type_code}")
    return tuple(constants), tuple(children)


def _read_numeric_constants(reader: _Reader, count: int) -> tuple[NumericConstant, ...]:
    constants = []
    for index in range(count):
        is_number, low = reader.uleb33_low("numeric constant")
        if is_number:
            high = reader.uleb32("numeric constant high word")
            constants.append(
                NumericConstant(index, "number", _double(low, high), (low, high))
            )
        else:
            constants.append(
                NumericConstant(index, "integer", _to_signed(low, 32), (low,))
            )
    return tuple(constants)


def _read_debug(
    raw: bytes,
    instruction_count: int,
    upvalue_count: int,
    first_line: int,
    line_span: int,
    byte_order: str,
    budget: _Budget,
) -> DebugInfo:
    reader = _Reader(raw)
    width = 1 if line_span < 256 else 2 if line_span < 65536 else 4
    line_deltas = []
    for _ in range(instruction_count):
        encoded = reader.read(width, "debug line map")
        line_deltas.append(int.from_bytes(encoded, byte_order))
    names = []
    for _ in range(upvalue_count):
        name = reader.cstring("debug upvalue name")
        budget.add("string_bytes", len(name), budget.limits.maximum_string_bytes)
        names.append(name)
    variables = []
    last_start = 0
    while True:
        marker = reader.peek()
        if marker == 0:
            reader.byte("debug variable terminator")
            break
        if marker in _SPECIAL_VARIABLE_NAMES:
            reader.byte("special debug variable")
            name = None
            special_name = _SPECIAL_VARIABLE_NAMES[marker]
        elif marker < 7:
            raise LuaJITFormatError(f"unknown LuaJIT special variable marker {marker}")
        else:
            name = reader.cstring("debug variable name")
            budget.add("string_bytes", len(name), budget.limits.maximum_string_bytes)
            special_name = None
        last_start += reader.uleb32("debug variable start")
        length = reader.uleb32("debug variable length")
        variables.append(
            DebugVariable(name, special_name, last_start, last_start + length)
        )
    if reader.remaining:
        raise LuaJITFormatError(
            f"LuaJIT debug section has {reader.remaining} unread bytes"
        )
    return DebugInfo(
        raw,
        first_line,
        line_span,
        tuple(line_deltas),
        tuple(first_line + delta for delta in line_deltas),
        tuple(names),
        tuple(variables),
    )


def _decode_instructions(
    reader: _Reader,
    count: int,
    byte_order: str,
    specs: tuple[OpcodeSpec, ...],
) -> tuple[Instruction, ...]:
    instructions = []
    for pc in range(1, count + 1):
        offset = reader.position
        raw = reader.read(4, "bytecode instruction")
        word = int.from_bytes(raw, byte_order)
        opcode = word & 0xFF
        a = (word >> 8) & 0xFF
        c = (word >> 16) & 0xFF
        b = (word >> 24) & 0xFF
        d = (word >> 16) & 0xFFFF
        if opcode < len(specs):
            spec = specs[opcode]
            name = spec.name
        else:
            spec = OpcodeSpec(f"UNKNOWN_0x{opcode:02X}", "unknown", "unknown", "unknown")
            name = spec.name
        instructions.append(
            Instruction(
                pc,
                offset,
                raw,
                word,
                opcode,
                name,
                spec.form,
                spec.a_mode,
                spec.b_mode,
                spec.cd_mode,
                a,
                b,
                c,
                d,
                _to_signed(d, 16),
                d - 0x8000,
            )
        )
    return tuple(instructions)


def _validate_prototype(
    flags: int,
    instructions: tuple[Instruction, ...],
    gc_constants: tuple[GCConstant, ...],
    numeric_constants: tuple[NumericConstant, ...],
    children: tuple[int, ...],
) -> tuple[str, ...]:
    issues = []
    unknown_proto = flags & ~KNOWN_PROTO_FLAGS
    if unknown_proto:
        issues.append(f"unknown prototype flags 0x{unknown_proto:02x}")
    if bool(flags & PROTO_CHILD) != bool(children):
        issues.append("prototype child flag does not match child constants")
    has_cdata = any(
        constant.kind in {"int64", "uint64", "complex"} for constant in gc_constants
    )
    if has_cdata and not flags & PROTO_FFI:
        issues.append("prototype has cdata constants without its FFI flag")
    for instruction in instructions:
        if instruction.opcode_name.startswith("UNKNOWN_"):
            issues.append(
                f"unknown opcode 0x{instruction.opcode:02x} at pc {instruction.pc}"
            )
            continue
        value = instruction.d if instruction.form == "AD" else instruction.c
        if instruction.cd_mode in {"str", "tab", "cdata", "func"}:
            serialized_index = len(gc_constants) - 1 - value
            if not 0 <= serialized_index < len(gc_constants):
                issues.append(
                    f"{instruction.opcode_name} pc {instruction.pc} references "
                    f"missing GC constant {value}"
                )
        elif instruction.cd_mode == "num" and value >= len(numeric_constants):
            issues.append(
                f"{instruction.opcode_name} pc {instruction.pc} references "
                f"missing numeric constant {value}"
            )
    return tuple(dict.fromkeys(issues))


def _host_references(
    prototypes: Iterable[Prototype],
) -> tuple[tuple[HostAPIReference, ...], tuple[str, ...]]:
    references = []
    issues = []
    for prototype in prototypes:
        for instruction in prototype.instructions:
            if instruction.opcode_name not in {"GGET", "GSET"}:
                continue
            constant = prototype.gc_constant_for_operand(instruction.d)
            if constant is None or constant.kind != "string":
                issues.append(
                    f"{instruction.opcode_name} in prototype {prototype.prototype_id} "
                    f"pc {instruction.pc} does not resolve to a string"
                )
                continue
            references.append(
                HostAPIReference(
                    constant.value,
                    "read" if instruction.opcode_name == "GGET" else "write",
                    prototype.prototype_id,
                    instruction.pc,
                )
            )
    return tuple(references), tuple(issues)


def _table_strings(table: TableConstant) -> list[bytes]:
    values = list(table.array)
    for key, value in table.hash_entries:
        values.extend((key, value))
    return [value.value for value in values if value.kind == "string"]


def _bytes_inventory(value: bytes | None) -> dict[str, Any] | None:
    if value is None:
        return None
    try:
        text = value.decode("utf-8")
        utf8 = True
    except UnicodeDecodeError:
        text = value.decode("utf-8", "replace")
        utf8 = False
    return {
        "size": len(value),
        "sha256": hashlib.sha256(value).hexdigest(),
        "hex": value.hex(),
        "text": text,
        "valid_utf8": utf8,
    }


def parse_dump(data: bytes | bytearray | memoryview, *, limits: ParseLimits | None = None) -> LuaJITDump:
    """Parse a complete LuaJIT version-2 dump without executing it."""
    raw = bytes(data)
    limits = limits or ParseLimits()
    if len(raw) > limits.maximum_bytes:
        raise LuaJITFormatError("LuaJIT input byte budget exceeded")
    reader = _Reader(raw)
    if reader.read(3, "header magic") != MAGIC:
        raise LuaJITFormatError("LuaJIT bytecode magic mismatch")
    version = reader.byte("dump version")
    if version != VERSION_2:
        raise UnsupportedLuaJITFeature(
            f"unsupported LuaJIT dump version {version}; version 2 is required"
        )
    flags = reader.uleb32("dump flags")
    unknown_flags = flags & ~KNOWN_FLAGS
    if unknown_flags:
        raise UnsupportedLuaJITFeature(
            f"unsupported LuaJIT dump flags 0x{unknown_flags:08x}"
        )
    byte_order = "big" if flags & FLAG_BIG_ENDIAN else "little"
    specs = BITOP_OPCODE_SPECS if flags & FLAG_BITOP else LEGACY_OPCODE_SPECS
    profile = "luajit-2.1-bitop-2026" if flags & FLAG_BITOP else "luajit-2.1-legacy"
    budget = _Budget(limits)

    chunk_name = None
    if not flags & FLAG_STRIPPED:
        name_length = reader.uleb32("chunk name length")
        chunk_name = _read_string(reader, name_length, budget, "chunk name")

    prototypes: list[Prototype] = []
    child_stack: list[int] = []
    while True:
        if reader.remaining == 0:
            raise LuaJITFormatError("LuaJIT dump is missing its prototype terminator")
        envelope_offset = reader.position
        payload_size = reader.uleb32("prototype length")
        if payload_size == 0:
            break
        if len(prototypes) >= limits.maximum_prototypes:
            raise LuaJITFormatError("LuaJIT prototype budget exceeded")
        if payload_size > limits.maximum_prototype_bytes:
            raise LuaJITFormatError("LuaJIT prototype byte budget exceeded")
        payload_offset = reader.position
        payload_end = payload_offset + payload_size
        if payload_end > reader.end:
            raise LuaJITFormatError(
                f"truncated LuaJIT prototype at 0x{payload_offset:x}: "
                f"need {payload_size}, have {reader.remaining}"
            )
        prototype_reader = _Reader(raw, payload_offset, payload_end)
        proto_flags = prototype_reader.byte("prototype flags")
        num_parameters = prototype_reader.byte("prototype parameter count")
        frame_size = prototype_reader.byte("prototype frame size")
        upvalue_count = prototype_reader.byte("prototype upvalue count")
        gc_count = prototype_reader.uleb32("prototype GC constant count")
        numeric_count = prototype_reader.uleb32("prototype numeric constant count")
        instruction_count = prototype_reader.uleb32("prototype instruction count")
        budget.add(
            "instructions", instruction_count, limits.maximum_instructions
        )
        budget.add(
            "constants", gc_count + numeric_count, limits.maximum_constants
        )

        debug_size = 0
        first_line = 0
        line_span = 0
        if not flags & FLAG_STRIPPED:
            debug_size = prototype_reader.uleb32("prototype debug size")
            if debug_size:
                first_line = prototype_reader.uleb32("prototype first line")
                line_span = prototype_reader.uleb32("prototype line span")

        instructions = _decode_instructions(
            prototype_reader, instruction_count, byte_order, specs
        )
        upvalues = []
        for index in range(upvalue_count):
            raw_reference = int.from_bytes(
                prototype_reader.read(2, "upvalue reference"), byte_order
            )
            upvalues.append(
                UpvalueReference(
                    index,
                    raw_reference,
                    "local" if raw_reference & UV_LOCAL else "parent-upvalue",
                    raw_reference & UV_INDEX_MASK,
                    bool(raw_reference & UV_IMMUTABLE),
                )
            )

        gc_constants, children = _read_gc_constants(
            prototype_reader, gc_count, child_stack, budget
        )
        numeric_constants = _read_numeric_constants(prototype_reader, numeric_count)
        debug = None
        if debug_size:
            debug_raw = prototype_reader.read(debug_size, "debug section")
            debug = _read_debug(
                debug_raw,
                instruction_count,
                upvalue_count,
                first_line,
                line_span,
                byte_order,
                budget,
            )
        if prototype_reader.remaining:
            raise LuaJITFormatError(
                f"LuaJIT prototype at 0x{payload_offset:x} has "
                f"{prototype_reader.remaining} unread bytes"
            )

        child_depths = [prototypes[child_id].depth for child_id in children]
        depth = 1 + (max(child_depths) if child_depths else 0)
        if depth > limits.maximum_depth:
            raise LuaJITFormatError("LuaJIT prototype nesting budget exceeded")
        prototype_id = len(prototypes)
        payload = raw[payload_offset:payload_end]
        issues = _validate_prototype(
            proto_flags, instructions, gc_constants, numeric_constants, children
        )
        prototypes.append(
            Prototype(
                prototype_id,
                envelope_offset,
                payload_offset,
                payload_size,
                payload,
                hashlib.sha256(payload).hexdigest(),
                proto_flags,
                num_parameters,
                frame_size,
                tuple(upvalues),
                instructions,
                gc_constants,
                numeric_constants,
                children,
                debug,
                depth,
                issues,
            )
        )
        child_stack.append(prototype_id)
        reader.position = payload_end

    if reader.remaining:
        raise LuaJITFormatError(
            f"LuaJIT dump has {reader.remaining} trailing bytes after terminator"
        )
    if len(child_stack) != 1:
        raise LuaJITFormatError(
            f"LuaJIT prototype graph has {len(child_stack)} roots; expected one"
        )
    if not prototypes:
        raise LuaJITFormatError("LuaJIT dump has no prototypes")

    references, reference_issues = _host_references(prototypes)
    issues = list(reference_issues)
    for prototype in prototypes:
        issues.extend(
            f"prototype {prototype.prototype_id}: {issue}"
            for issue in prototype.issues
        )
    requirements = ["LuaJIT 2.1 bytecode VM with the matching opcode profile"]
    if flags & FLAG_FFI:
        requirements.append("LuaJIT FFI/cdata support")
    if flags & FLAG_FR2:
        requirements.append("LuaJIT LJ_FR2 two-slot frame ABI")
    if references:
        requirements.append(
            "host globals referenced by GGET/GSET; names are inventoried separately"
        )
    unsupported = [
        "bytecode execution and native host ABI compatibility are not assessed",
        "dynamic table/member lookups and runtime-generated API names cannot be resolved statically",
        "translation to Lua source, native SDGO behavior, and Minecraft behavior is not implemented",
    ]
    if any(
        instruction.opcode_name.startswith("UNKNOWN_")
        for prototype in prototypes
        for instruction in prototype.instructions
    ):
        unsupported.append("unknown/private opcode semantics")

    return LuaJITDump(
        raw,
        hashlib.sha256(raw).hexdigest(),
        version,
        flags,
        _flag_names(flags),
        chunk_name,
        byte_order,
        profile,
        tuple(prototypes),
        child_stack[0],
        references,
        tuple(requirements),
        tuple(unsupported),
        tuple(dict.fromkeys(issues)),
    )
