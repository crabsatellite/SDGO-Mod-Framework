"""Compile-only Lua 5.0.3 recovery for G Union plaintext SCP resources.

The original source chunks are parsed and dumped by the already verified
official Lua 5.0.3 compiler harness.  They are never called.  This module then
parses the dumped binary without loading it into a Lua VM and exports:

* every serialized prototype field, child, constant, upvalue and code word;
* exact CLOSURE-to-SETGLOBAL function bindings;
* the small JSON carrier consumed by CapsuleArena's current ranged VM; and
* explicit opcode/API compatibility gaps for that VM.

Compilation proves only that the recovered text is accepted by the official
Lua 5.0.3 parser and has a deterministic derived bytecode representation.  It
does not prove shipped program-counter identity or native/gameplay behavior.
"""
from __future__ import annotations

from collections import defaultdict, deque
from dataclasses import dataclass
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tarfile
from typing import Any, Iterable, Mapping, Sequence


LUA50_SIGNATURE = b"\x1bLua"
LUA50_VERSION = 0x50
LUA50_TEST_NUMBER = 3.14159265358979323846e7
LUA50_RK_CONSTANT = 250
LUA50_OPCODES = (
    "MOVE", "LOADK", "LOADBOOL", "LOADNIL", "GETUPVAL",
    "GETGLOBAL", "GETTABLE", "SETGLOBAL", "SETUPVAL", "SETTABLE",
    "NEWTABLE", "SELF", "ADD", "SUB", "MUL", "DIV", "POW", "UNM",
    "NOT", "CONCAT", "JMP", "EQ", "LT", "LE", "TEST", "CALL",
    "TAILCALL", "RETURN", "FORLOOP", "TFORLOOP", "TFORPREP",
    "SETLIST", "SETLISTO", "CLOSE", "CLOSURE",
)

# SourceSpecialCameraScriptVm.executeBytecode switch cases in the isolated
# Minecraft recovery worktree on 2026-11-02.
MINECRAFT_VM_SUPPORTED_OPCODES = frozenset({
    "MOVE", "LOADK", "LOADBOOL", "LOADNIL", "GETGLOBAL", "GETTABLE",
    "SETGLOBAL", "SETTABLE", "NEWTABLE", "SELF", "ADD", "SUB", "MUL",
    "DIV", "UNM", "JMP", "EQ", "LT", "LE", "TEST", "CALL", "RETURN",
    "FORLOOP",
})
MINECRAFT_VM_BUILTIN_GLOBALS = frozenset({"vector3"})
MINECRAFT_RANGED_GLOBALS = frozenset({
    "GetCamera", "ray3", "vec3_add", "vec3_cross", "vec3_normalize",
})
MINECRAFT_RANGED_METHODS = frozenset({
    "GetOrientation", "Right", "GetPos", "GetRight", "GetUp",
    "GetDirection", "GetCameraOrientation", "GetRangedAttackRay",
    "GetCurCombo", "AddCurCombo", "GetNodePos", "GetNodeDir", "GetNodeUp",
    "RangedAttackProc", "MissileAttackProc", "RangedMultiAttackProc",
    "CreateFang",
})

OFFICIAL_LUA_ARCHIVE_SHA256 = (
    "1193a61b0e08acaa6eee0eecf29709179ee49c71baebc59b682a25c3b5a45671"
)
OFFICIAL_COMPILER_HARNESS_SHA256 = (
    "f1f7188e038b424f3cd3d415562523c3fcf69270f4798ea49a86efd69dd708e7"
)
OFFICIAL_COMPILER_BINARY_SHA256 = (
    "724f70c9362324f9d983c84febede74be3315c4710e4c074227fa21ed8428978"
)

PRIORITY_FUNCTIONS = (
    {
        "weaponId": 3710606,
        "function": "baitudouxiandanqiang",
        "sourcePath": r"scp\2619.scp",
        "sourceOffset": 0xC7B9B8E6,
        "sourceSha256": (
            "4dc98b8ac295133c87e7a83fb8ff0c54542bf822befe3bfe5b046d38ed699916"
        ),
        "bodyCharacterRange": [6128, 21704],
        "bodyLineRange": [157, 560],
        "bodySha256": (
            "64bf3420ca89ce5558058ddad19dd73e7512e0fdf05d3d63239c305705de97b6"
        ),
    },
    {
        "weaponId": 3710608,
        "function": "oubeilongmgluanwu",
        "sourcePath": r"scp\2640.scp",
        "sourceOffset": 0xC7BBA9B0,
        "sourceSha256": (
            "eb8804207b22b9461d86b3280fbc10f1af6a741e07b6e6aeac517d392ee1e3d3"
        ),
        "bodyCharacterRange": [0, 4897],
        "bodyLineRange": [1, 120],
        "bodySha256": (
            "a7c9cfd8108dfccf679c218494db6890b129383a5c0d0ec8fcc38cb6abd38d1b"
        ),
    },
    {
        "weaponId": 3710609,
        "function": "oblmslkuosan",
        "sourcePath": r"scp\2640.scp",
        "sourceOffset": 0xC7BBA9B0,
        "sourceSha256": (
            "eb8804207b22b9461d86b3280fbc10f1af6a741e07b6e6aeac517d392ee1e3d3"
        ),
        "bodyCharacterRange": [4903, 5975],
        "bodyLineRange": [123, 160],
        "bodySha256": (
            "0b98184dcf2eb796f26d5a2d003c8d9e36e18f3b939bb75a6fb09df2ad154cfd"
        ),
    },
)


class Lua50FormatError(ValueError):
    """A binary chunk differs from the official 32-bit Lua 5.0.3 format."""


class Lua50CompileError(RuntimeError):
    """The compile-only toolchain or a plaintext parse failed."""


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def _string_info(value: bytes | None) -> dict[str, Any] | None:
    if value is None:
        return None
    try:
        text = value.decode("utf-8")
        valid_utf8 = True
    except UnicodeDecodeError:
        text = value.decode("utf-8", "replace")
        valid_utf8 = False
    return {
        "hex": value.hex(),
        "text": text,
        "validUtf8": valid_utf8,
        "size": len(value),
        "sha256": _sha256_bytes(value),
    }


def _json_number(value: float) -> float | str:
    if math.isfinite(value):
        return value
    if math.isnan(value):
        return "nan"
    return "positive-infinity" if value > 0 else "negative-infinity"


@dataclass(frozen=True)
class Lua50Header:
    version: int
    little_endian: bool
    int_size: int
    size_t_size: int
    instruction_size: int
    opcode_bits: int
    a_bits: int
    b_bits: int
    c_bits: int
    number_size: int
    test_number: float
    raw_hex: str

    def exact_json(self) -> dict[str, Any]:
        return {
            "signatureHex": LUA50_SIGNATURE.hex(),
            "version": self.version,
            "littleEndian": self.little_endian,
            "intSize": self.int_size,
            "sizeTSize": self.size_t_size,
            "instructionSize": self.instruction_size,
            "opcodeBits": self.opcode_bits,
            "aBits": self.a_bits,
            "bBits": self.b_bits,
            "cBits": self.c_bits,
            "numberSize": self.number_size,
            "testNumber": self.test_number,
            "rawHex": self.raw_hex,
        }


@dataclass(frozen=True)
class Lua50Constant:
    tag: int
    kind: str
    value: bytes | float | None
    raw_hex: str

    def carrier_value(self) -> str | float | None:
        if self.kind == "nil":
            return None
        if self.kind == "number":
            assert isinstance(self.value, float)
            if not math.isfinite(self.value):
                raise Lua50FormatError(
                    "non-finite Lua number cannot be represented by the JSON carrier"
                )
            return self.value
        assert self.kind == "string" and isinstance(self.value, bytes)
        try:
            return self.value.decode("utf-8")
        except UnicodeDecodeError as error:
            raise Lua50FormatError(
                "non-UTF-8 Lua string cannot be represented by the Minecraft carrier"
            ) from error

    def exact_json(self, index: int) -> dict[str, Any]:
        result: dict[str, Any] = {
            "index": index,
            "tag": self.tag,
            "kind": self.kind,
            "rawHex": self.raw_hex,
        }
        if self.kind == "number":
            assert isinstance(self.value, float)
            result["value"] = _json_number(self.value)
        elif self.kind == "string":
            assert isinstance(self.value, bytes)
            result["value"] = _string_info(self.value)
        else:
            result["value"] = None
        return result


@dataclass(frozen=True)
class Lua50Local:
    name: bytes | None
    start_pc: int
    end_pc: int

    def exact_json(self) -> dict[str, Any]:
        return {
            "name": _string_info(self.name),
            "startPc": self.start_pc,
            "endPc": self.end_pc,
        }


@dataclass(frozen=True)
class Lua50Instruction:
    pc: int
    raw: int
    opcode: int
    name: str
    a: int
    b: int
    c: int
    bx: int
    sbx: int

    def exact_json(self) -> dict[str, Any]:
        return {
            "pc": self.pc,
            "pcBasis": "zero",
            "raw": self.raw,
            "rawHex": f"{self.raw:08x}",
            "opcode": self.opcode,
            "name": self.name,
            "a": self.a,
            "b": self.b,
            "c": self.c,
            "bx": self.bx,
            "sbx": self.sbx,
        }


def decode_instruction(raw: int, pc: int = 0) -> Lua50Instruction:
    if not 0 <= raw <= 0xFFFFFFFF:
        raise Lua50FormatError(f"instruction is outside uint32: {raw}")
    opcode = raw & 0x3F
    if opcode >= len(LUA50_OPCODES):
        raise Lua50FormatError(f"unknown Lua 5.0 opcode {opcode} at pc {pc}")
    bx = (raw >> 6) & 0x3FFFF
    return Lua50Instruction(
        pc=pc,
        raw=raw,
        opcode=opcode,
        name=LUA50_OPCODES[opcode],
        a=(raw >> 24) & 0xFF,
        b=(raw >> 15) & 0x1FF,
        c=(raw >> 6) & 0x1FF,
        bx=bx,
        sbx=bx - 0x1FFFF,
    )


@dataclass(frozen=True)
class Lua50Prototype:
    path: tuple[int, ...]
    payload_offset: int
    payload_size: int
    payload_sha256: str
    source: bytes | None
    effective_source: bytes | None
    line_defined: int
    nups: int
    numparams: int
    is_vararg: int
    maxstacksize: int
    line_info: tuple[int, ...]
    locals: tuple[Lua50Local, ...]
    upvalue_names: tuple[bytes | None, ...]
    constants: tuple[Lua50Constant, ...]
    children: tuple["Lua50Prototype", ...]
    code: tuple[Lua50Instruction, ...]

    def carrier_constants(self) -> list[str | float | None]:
        return [constant.carrier_value() for constant in self.constants]

    def semantic_sha256(self) -> str:
        payload = {
            "constants": self.carrier_constants(),
            "codeWords": [instruction.raw for instruction in self.code],
            "parameterCount": self.numparams,
            "vararg": self.is_vararg,
            "maxStack": self.maxstacksize,
            "children": [child.semantic_sha256() for child in self.children],
        }
        encoded = json.dumps(
            payload,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
        return _sha256_bytes(encoded)

    def bytecode_program(self) -> dict[str, Any]:
        return {
            "constantPool": self.carrier_constants(),
            "codeWords": [instruction.raw for instruction in self.code],
            "parameterCount": self.numparams,
            "maxStackSize": self.maxstacksize,
        }

    def exact_json(self) -> dict[str, Any]:
        return {
            "path": list(self.path),
            "payloadOffset": self.payload_offset,
            "payloadSize": self.payload_size,
            "payloadSha256": self.payload_sha256,
            "semanticSha256": self.semantic_sha256(),
            "source": _string_info(self.source),
            "effectiveSource": _string_info(self.effective_source),
            "lineDefined": self.line_defined,
            "upvalueCount": self.nups,
            "parameterCount": self.numparams,
            "isVararg": self.is_vararg,
            "maxStackSize": self.maxstacksize,
            "lineInfo": list(self.line_info),
            "locals": [item.exact_json() for item in self.locals],
            "upvalueNames": [
                {"index": index, "name": _string_info(name)}
                for index, name in enumerate(self.upvalue_names)
            ],
            "constants": [
                item.exact_json(index)
                for index, item in enumerate(self.constants)
            ],
            "children": [child.exact_json() for child in self.children],
            "code": [instruction.exact_json() for instruction in self.code],
        }


@dataclass(frozen=True)
class Lua50Chunk:
    raw: bytes
    header: Lua50Header
    root: Lua50Prototype

    @property
    def sha256(self) -> str:
        return _sha256_bytes(self.raw)

    def exact_json(self) -> dict[str, Any]:
        return {
            "schemaVersion": 1,
            "format": "official-lua-5.0-32bit-little-endian",
            "chunkSize": len(self.raw),
            "chunkSha256": self.sha256,
            "header": self.header.exact_json(),
            "root": self.root.exact_json(),
        }


@dataclass(frozen=True)
class ParseLimits:
    maximum_depth: int = 128
    maximum_prototypes: int = 100000
    maximum_vector_items: int = 5000000
    maximum_string_bytes: int = 64 * 1024 * 1024


class _Reader:
    def __init__(self, data: bytes, limits: ParseLimits):
        self.data = data
        self.position = 0
        self.limits = limits
        self.prototype_count = 0

    def read(self, size: int, label: str) -> bytes:
        if size < 0 or self.position + size > len(self.data):
            raise Lua50FormatError(
                f"truncated {label} at 0x{self.position:x}: need {size} bytes"
            )
        result = self.data[self.position:self.position + size]
        self.position += size
        return result

    def byte(self, label: str) -> int:
        return self.read(1, label)[0]

    def uint32(self, label: str) -> int:
        return struct.unpack("<I", self.read(4, label))[0]

    def int32(self, label: str) -> int:
        value = struct.unpack("<i", self.read(4, label))[0]
        if value < 0:
            raise Lua50FormatError(f"negative official Lua count/value for {label}")
        return value

    def number(self, label: str) -> tuple[float, bytes]:
        raw = self.read(8, label)
        return struct.unpack("<d", raw)[0], raw

    def count(self, label: str) -> int:
        value = self.int32(label)
        if value > self.limits.maximum_vector_items:
            raise Lua50FormatError(f"{label} exceeds item limit: {value}")
        return value

    def string(self, label: str) -> bytes | None:
        size = self.uint32(f"{label} size")
        if size == 0:
            return None
        if size > self.limits.maximum_string_bytes:
            raise Lua50FormatError(f"{label} exceeds string limit: {size}")
        raw = self.read(size, label)
        if not raw.endswith(b"\0"):
            raise Lua50FormatError(f"{label} lacks serialized NUL terminator")
        return raw[:-1]


def parse_chunk(data: bytes, *, limits: ParseLimits | None = None) -> Lua50Chunk:
    """Parse one exact official Lua 5.0 binary chunk without executing it."""
    limits = limits or ParseLimits()
    reader = _Reader(data, limits)
    header_start = reader.position
    signature = reader.read(4, "signature")
    if signature != LUA50_SIGNATURE:
        raise Lua50FormatError(f"bad Lua signature {signature!r}")
    version = reader.byte("version")
    endian = reader.byte("endianness")
    int_size = reader.byte("int size")
    size_t_size = reader.byte("size_t size")
    instruction_size = reader.byte("instruction size")
    opcode_bits = reader.byte("opcode bits")
    a_bits = reader.byte("A bits")
    b_bits = reader.byte("B bits")
    c_bits = reader.byte("C bits")
    number_size = reader.byte("number size")
    test_number, _ = reader.number("test number")
    expected = (
        version == LUA50_VERSION
        and endian == 1
        and (int_size, size_t_size, instruction_size) == (4, 4, 4)
        and (opcode_bits, a_bits, b_bits, c_bits) == (6, 8, 9, 9)
        and number_size == 8
        and int(test_number) == int(LUA50_TEST_NUMBER)
    )
    if not expected:
        raise Lua50FormatError(
            "chunk is not the official 32-bit little-endian Lua 5.0 layout"
        )
    header = Lua50Header(
        version=version,
        little_endian=True,
        int_size=int_size,
        size_t_size=size_t_size,
        instruction_size=instruction_size,
        opcode_bits=opcode_bits,
        a_bits=a_bits,
        b_bits=b_bits,
        c_bits=c_bits,
        number_size=number_size,
        test_number=test_number,
        raw_hex=data[header_start:reader.position].hex(),
    )

    def prototype(
        path: tuple[int, ...],
        inherited_source: bytes | None,
        depth: int,
    ) -> Lua50Prototype:
        if depth > limits.maximum_depth:
            raise Lua50FormatError("prototype nesting exceeds limit")
        reader.prototype_count += 1
        if reader.prototype_count > limits.maximum_prototypes:
            raise Lua50FormatError("prototype count exceeds limit")
        start = reader.position
        source = reader.string(f"prototype {path} source")
        effective_source = inherited_source if source is None else source
        line_defined = reader.int32(f"prototype {path} lineDefined")
        nups = reader.byte(f"prototype {path} nups")
        numparams = reader.byte(f"prototype {path} numparams")
        is_vararg = reader.byte(f"prototype {path} is_vararg")
        maxstacksize = reader.byte(f"prototype {path} maxstacksize")

        line_count = reader.count(f"prototype {path} line count")
        line_info = tuple(
            reader.int32(f"prototype {path} line[{index}]")
            for index in range(line_count)
        )
        local_count = reader.count(f"prototype {path} local count")
        locals_: list[Lua50Local] = []
        for index in range(local_count):
            locals_.append(Lua50Local(
                name=reader.string(f"prototype {path} local[{index}] name"),
                start_pc=reader.int32(
                    f"prototype {path} local[{index}] startpc"),
                end_pc=reader.int32(
                    f"prototype {path} local[{index}] endpc"),
            ))
        upvalue_name_count = reader.count(
            f"prototype {path} upvalue name count")
        if upvalue_name_count not in (0, nups):
            raise Lua50FormatError(
                f"prototype {path} has {upvalue_name_count} upvalue names "
                f"but declares {nups} upvalues"
            )
        upvalue_names = tuple(
            reader.string(f"prototype {path} upvalue[{index}]")
            for index in range(upvalue_name_count)
        )

        constant_count = reader.count(f"prototype {path} constant count")
        constants: list[Lua50Constant] = []
        for index in range(constant_count):
            tag = reader.byte(f"prototype {path} constant[{index}] tag")
            if tag == 0:
                constants.append(Lua50Constant(tag, "nil", None, ""))
            elif tag == 3:
                value, raw = reader.number(
                    f"prototype {path} constant[{index}] number")
                constants.append(
                    Lua50Constant(tag, "number", value, raw.hex()))
            elif tag == 4:
                value = reader.string(
                    f"prototype {path} constant[{index}] string")
                if value is None:
                    raise Lua50FormatError(
                        f"prototype {path} string constant {index} is null"
                    )
                serialized = struct.pack("<I", len(value) + 1) + value + b"\0"
                constants.append(
                    Lua50Constant(tag, "string", value, serialized.hex()))
            else:
                # Official Lua 5.0.3 lundump accepts only nil, number and string.
                raise Lua50FormatError(
                    f"bad official Lua constant tag {tag} in {path}[{index}]"
                )

        child_count = reader.count(f"prototype {path} child count")
        children = tuple(
            prototype(path + (index,), effective_source, depth + 1)
            for index in range(child_count)
        )
        code_count = reader.count(f"prototype {path} code count")
        code = tuple(
            decode_instruction(
                reader.uint32(f"prototype {path} code[{index}]"), index)
            for index in range(code_count)
        )
        end = reader.position
        return Lua50Prototype(
            path=path,
            payload_offset=start,
            payload_size=end - start,
            payload_sha256=_sha256_bytes(data[start:end]),
            source=source,
            effective_source=effective_source,
            line_defined=line_defined,
            nups=nups,
            numparams=numparams,
            is_vararg=is_vararg,
            maxstacksize=maxstacksize,
            line_info=line_info,
            locals=tuple(locals_),
            upvalue_names=upvalue_names,
            constants=tuple(constants),
            children=children,
            code=code,
        )

    root = prototype((), None, 0)
    if reader.position != len(data):
        raise Lua50FormatError(
            f"{len(data) - reader.position} trailing bytes after root prototype"
        )
    return Lua50Chunk(data, header, root)


@dataclass(frozen=True)
class Lua50Binding:
    name: str
    prototype: Lua50Prototype
    owner_path: tuple[int, ...]
    closure_pc: int
    setglobal_pc: int
    register: int

    def json(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "prototypePath": list(self.prototype.path),
            "prototypePayloadSha256": self.prototype.payload_sha256,
            "prototypeSemanticSha256": self.prototype.semantic_sha256(),
            "ownerPrototypePath": list(self.owner_path),
            "closurePc": self.closure_pc,
            "setglobalPc": self.setglobal_pc,
            "register": self.register,
        }


def _constant_string(prototype: Lua50Prototype, index: int) -> str | None:
    if not 0 <= index < len(prototype.constants):
        return None
    constant = prototype.constants[index]
    if constant.kind != "string" or not isinstance(constant.value, bytes):
        return None
    try:
        return constant.value.decode("utf-8")
    except UnicodeDecodeError:
        return None


def global_function_bindings(root: Lua50Prototype) -> list[Lua50Binding]:
    """Recover exact direct CLOSURE/MOVE/SETGLOBAL bindings statically."""
    bindings: list[Lua50Binding] = []

    def visit(owner: Lua50Prototype) -> None:
        registers: dict[int, tuple[Lua50Prototype, int]] = {}
        capture_pcs: set[int] = set()
        for instruction in owner.code:
            if instruction.pc in capture_pcs:
                continue
            if instruction.name == "CLOSURE":
                if instruction.bx >= len(owner.children):
                    continue
                child = owner.children[instruction.bx]
                registers[instruction.a] = (child, instruction.pc)
                capture_pcs.update(
                    range(instruction.pc + 1, instruction.pc + 1 + child.nups)
                )
            elif instruction.name == "MOVE":
                value = registers.get(instruction.b)
                if value is None:
                    registers.pop(instruction.a, None)
                else:
                    registers[instruction.a] = value
            elif instruction.name == "SETGLOBAL":
                value = registers.get(instruction.a)
                name = _constant_string(owner, instruction.bx)
                if value is not None and name is not None:
                    child, closure_pc = value
                    bindings.append(Lua50Binding(
                        name=name,
                        prototype=child,
                        owner_path=owner.path,
                        closure_pc=closure_pc,
                        setglobal_pc=instruction.pc,
                        register=instruction.a,
                    ))
            elif instruction.name not in {
                "JMP", "EQ", "LT", "LE", "TEST", "RETURN", "SETUPVAL",
                "SETTABLE", "FORLOOP", "TFORLOOP", "TFORPREP", "SETLIST",
                "SETLISTO", "CLOSE",
            }:
                registers.pop(instruction.a, None)
        for child in owner.children:
            visit(child)

    visit(root)
    return bindings


def find_global_function(root: Lua50Prototype, name: str) -> Lua50Binding:
    matches = [
        binding for binding in global_function_bindings(root)
        if binding.name == name
    ]
    if not matches:
        raise KeyError(f"Lua 5.0 global function not found: {name}")
    semantic = {item.prototype.semantic_sha256() for item in matches}
    if len(matches) != 1 or len(semantic) != 1:
        raise ValueError(
            f"Lua 5.0 global function is ambiguous: {name} ({len(matches)} bindings)"
        )
    return matches[0]


Token = tuple[Any, ...]
State = dict[int, frozenset[Token]]
_UNKNOWN: Token = ("unknown",)
_ACTOR: Token = ("actor",)
_TABLE: Token = ("table",)
_CAMERA: Token = ("camera",)
_ORIENTATION: Token = ("orientation",)
_VECTOR: Token = ("vector",)
_RAY: Token = ("ray",)
_NUMBER: Token = ("number",)
_TOKEN_LIMIT = 64


def _merge_state(current: State | None, incoming: State) -> tuple[State, bool]:
    if current is None:
        return {key: frozenset(value) for key, value in incoming.items()}, True
    result: State = {}
    changed = False
    for register in current.keys() | incoming.keys():
        values = current.get(register, frozenset({_UNKNOWN})) | incoming.get(
            register, frozenset({_UNKNOWN}))
        if len(values) > _TOKEN_LIMIT:
            values = frozenset({_UNKNOWN})
        result[register] = values
        if result[register] != current.get(register):
            changed = True
    if current.keys() - result.keys():
        changed = True
    return result, changed


def _literal_token(value: Any) -> Token:
    if isinstance(value, float):
        return ("literal-number", value)
    if isinstance(value, str):
        return ("literal-string", value)
    if isinstance(value, bool):
        return ("literal-boolean", value)
    if value is None:
        return ("literal-nil",)
    return _UNKNOWN


def _token_literal(token: Token) -> Any:
    if token[0] == "literal-number":
        return token[1]
    if token[0] == "literal-string":
        return token[1]
    if token[0] == "literal-boolean":
        return token[1]
    if token[0] == "literal-nil":
        return None
    raise KeyError(token)


def _token_origin(token: Token) -> str:
    if token[0] in {
        "actor", "camera", "orientation", "vector", "ray", "number",
        "table", "unknown",
    }:
        return str(token[0])
    if token[0] in {"global", "method"}:
        return f"{token[0]}:{token[1]}"
    if token[0].startswith("literal-"):
        return token[0]
    return repr(token)


def _call_result_tokens(dispatch: str, api: str) -> frozenset[Token]:
    if dispatch == "global":
        return {
            "vector3": frozenset({_VECTOR}),
            "GetCamera": frozenset({_CAMERA}),
            "ray3": frozenset({_RAY}),
            "vec3_add": frozenset({_VECTOR}),
            "vec3_cross": frozenset({_VECTOR}),
            "vec3_normalize": frozenset({_VECTOR}),
        }.get(api, frozenset({_UNKNOWN}))
    return {
        "GetOrientation": frozenset({_ORIENTATION}),
        "Right": frozenset({_ORIENTATION}),
        "GetCameraOrientation": frozenset({_ORIENTATION}),
        "GetPos": frozenset({_VECTOR}),
        "GetRight": frozenset({_VECTOR}),
        "GetUp": frozenset({_VECTOR}),
        "GetDirection": frozenset({_VECTOR}),
        "GetNodePos": frozenset({_VECTOR}),
        "GetNodeDir": frozenset({_VECTOR}),
        "GetNodeUp": frozenset({_VECTOR}),
        "GetRangedAttackRay": frozenset({_RAY}),
        "GetCurCombo": frozenset({_NUMBER}),
    }.get(api, frozenset({_UNKNOWN}))


def _rk_tokens(
    prototype: Lua50Prototype,
    state: State,
    operand: int,
) -> frozenset[Token]:
    if operand < LUA50_RK_CONSTANT:
        return state.get(operand, frozenset({_UNKNOWN}))
    index = operand - LUA50_RK_CONSTANT
    if not 0 <= index < len(prototype.constants):
        return frozenset({_UNKNOWN})
    try:
        value = prototype.constants[index].carrier_value()
    except Lua50FormatError:
        return frozenset({_UNKNOWN})
    return frozenset({_literal_token(value)})


def _successors(
    prototype: Lua50Prototype,
    instruction: Lua50Instruction,
) -> tuple[int, ...]:
    pc = instruction.pc
    count = len(prototype.code)

    def admitted(candidate: int) -> bool:
        return 0 <= candidate < count

    if instruction.name in {"RETURN", "TAILCALL"}:
        return ()
    if instruction.name == "JMP":
        candidate = pc + 1 + instruction.sbx
        return (candidate,) if admitted(candidate) else ()
    if instruction.name in {"EQ", "LT", "LE", "TEST", "TFORLOOP"}:
        return tuple(
            candidate for candidate in (pc + 1, pc + 2)
            if admitted(candidate)
        )
    if instruction.name == "LOADBOOL" and instruction.c:
        candidate = pc + 2
        return (candidate,) if admitted(candidate) else ()
    if instruction.name == "FORLOOP":
        return tuple(dict.fromkeys(
            candidate
            for candidate in (pc + 1, pc + 1 + instruction.sbx)
            if admitted(candidate)
        ))
    if instruction.name == "TFORPREP":
        candidate = pc + 1 + instruction.sbx
        return (candidate,) if admitted(candidate) else ()
    if instruction.name == "CLOSURE" and instruction.bx < len(prototype.children):
        candidate = pc + 1 + prototype.children[instruction.bx].nups
        return (candidate,) if admitted(candidate) else ()
    return (pc + 1,) if admitted(pc + 1) else ()


def analyze_prototype(prototype: Lua50Prototype) -> dict[str, Any]:
    """Recover exact opcode use and conservative call requirements."""
    opcode_names = list(dict.fromkeys(item.name for item in prototype.code))
    global_reads = sorted({
        name
        for item in prototype.code
        if item.name == "GETGLOBAL"
        for name in [_constant_string(prototype, item.bx)]
        if name is not None
    })
    member_names = sorted({
        name
        for item in prototype.code
        if item.name == "SELF"
        for name in [
            _constant_string(prototype, item.c - LUA50_RK_CONSTANT)
            if item.c >= LUA50_RK_CONSTANT else None
        ]
        if name is not None
    })
    states: dict[int, State] = {0: {0: frozenset({_ACTOR})}}
    queue: deque[int] = deque([0]) if prototype.code else deque()
    calls: dict[tuple[int, str, str], dict[str, Any]] = {}
    while queue:
        pc = queue.popleft()
        instruction = prototype.code[pc]
        state = {key: frozenset(value) for key, value in states[pc].items()}
        a, b, c = instruction.a, instruction.b, instruction.c
        name = instruction.name
        if name == "MOVE":
            state[a] = state.get(b, frozenset({_UNKNOWN}))
        elif name == "LOADK":
            if 0 <= instruction.bx < len(prototype.constants):
                try:
                    value = prototype.constants[instruction.bx].carrier_value()
                    state[a] = frozenset({_literal_token(value)})
                except Lua50FormatError:
                    state[a] = frozenset({_UNKNOWN})
            else:
                state[a] = frozenset({_UNKNOWN})
        elif name == "LOADBOOL":
            state[a] = frozenset({("literal-boolean", b != 0)})
        elif name == "LOADNIL":
            for register in range(a, b + 1):
                state[register] = frozenset({("literal-nil",)})
        elif name == "GETGLOBAL":
            global_name = _constant_string(prototype, instruction.bx)
            state[a] = (
                frozenset({("global", global_name)})
                if global_name is not None else frozenset({_UNKNOWN})
            )
        elif name == "GETTABLE":
            values: set[Token] = set()
            for owner in state.get(b, frozenset({_UNKNOWN})):
                for key in _rk_tokens(prototype, state, c):
                    if owner[0] == "global" and key[0] == "literal-string":
                        values.add(("global", f"{owner[1]}.{key[1]}"))
                    else:
                        values.add(_UNKNOWN)
            state[a] = frozenset(values or {_UNKNOWN})
        elif name == "NEWTABLE":
            state[a] = frozenset({_TABLE})
        elif name == "SELF":
            receiver = state.get(b, frozenset({_UNKNOWN}))
            methods: set[Token] = set()
            for key in _rk_tokens(prototype, state, c):
                if key[0] == "literal-string":
                    methods.add(("method", key[1]))
                else:
                    methods.add(_UNKNOWN)
            state[a] = frozenset(methods)
            state[a + 1] = receiver
        elif name in {"ADD", "SUB", "MUL", "DIV", "POW"}:
            values: set[Token] = set()
            operation = {
                "ADD": lambda x, y: x + y,
                "SUB": lambda x, y: x - y,
                "MUL": lambda x, y: x * y,
                "DIV": lambda x, y: x / y,
                "POW": lambda x, y: x ** y,
            }[name]
            for left in _rk_tokens(prototype, state, b):
                for right in _rk_tokens(prototype, state, c):
                    if left[0] == right[0] == "literal-number":
                        try:
                            result = operation(left[1], right[1])
                        except (ArithmeticError, OverflowError):
                            continue
                        if isinstance(result, (int, float)) and math.isfinite(result):
                            values.add(("literal-number", float(result)))
            state[a] = frozenset(values or {_UNKNOWN})
        elif name == "UNM":
            values = {
                ("literal-number", -token[1])
                for token in state.get(b, frozenset({_UNKNOWN}))
                if token[0] == "literal-number"
            }
            state[a] = frozenset(values or {_UNKNOWN})
        elif name == "NOT":
            state[a] = frozenset({_UNKNOWN})
        elif name == "CALL":
            targets = state.get(a, frozenset({_UNKNOWN}))
            result_tokens: set[Token] = set()
            for target in targets:
                if target[0] not in {"global", "method"}:
                    continue
                dispatch = target[0]
                call_name = str(target[1])
                start = a + 2 if dispatch == "method" else a + 1
                end = a + b if b > 0 else start
                receiver_origins = (
                    sorted(
                        _token_origin(token)
                        for token in state.get(a + 1, frozenset({_UNKNOWN}))
                    )
                    if dispatch == "method" else []
                )
                arguments: list[Any] = []
                all_literal = b > 0
                for register in range(start, end):
                    tokens = state.get(register, frozenset({_UNKNOWN}))
                    literal_values: list[Any] = []
                    for token in tokens:
                        try:
                            literal_values.append(_token_literal(token))
                        except KeyError:
                            pass
                    unique = {
                        json.dumps(value, sort_keys=True, ensure_ascii=False)
                        for value in literal_values
                    }
                    if len(unique) == 1 and len(literal_values) == len(tokens):
                        arguments.append(literal_values[0])
                    else:
                        arguments.append(None)
                        all_literal = False
                call_key = (pc, dispatch, call_name)
                prior = calls.get(call_key)
                calls[call_key] = {
                    "pc": pc,
                    "dispatch": dispatch,
                    "api": call_name,
                    "arguments": arguments,
                    "allArgumentsLiteral": all_literal,
                    "receiverOrigins": sorted(set(receiver_origins) | set(
                        prior.get("receiverOrigins", []) if prior else []
                    )),
                }
                result_tokens.update(_call_result_tokens(dispatch, call_name))
            if c == 1:
                state.pop(a, None)
            else:
                returns = max(c - 1, 1)
                state[a] = frozenset(result_tokens or {_UNKNOWN})
                for register in range(a + 1, a + returns):
                    state[register] = frozenset({_UNKNOWN})
        elif name not in {
            "SETGLOBAL", "SETUPVAL", "SETTABLE", "JMP", "EQ", "LT", "LE",
            "TEST", "RETURN", "TAILCALL", "FORLOOP", "TFORLOOP",
            "TFORPREP", "SETLIST", "SETLISTO", "CLOSE",
        }:
            state[a] = frozenset({_UNKNOWN})

        for successor in _successors(prototype, instruction):
            merged, changed = _merge_state(states.get(successor), state)
            if changed:
                states[successor] = merged
                queue.append(successor)

    ordered_calls = [
        calls[key] for key in sorted(calls, key=lambda item: (item[0], item[1], item[2]))
    ]
    return {
        "pcBasis": "zero",
        "opcodes": opcode_names,
        "globalReads": global_reads,
        "memberNames": member_names,
        "calls": ordered_calls,
        "reachableInstructionCount": len(states),
        "instructionCount": len(prototype.code),
    }


def minecraft_ranged_compatibility(
    prototype: Lua50Prototype,
    analysis: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    analysis = dict(analysis or analyze_prototype(prototype))
    used_opcodes = set(analysis["opcodes"])
    unsupported_opcodes = sorted(
        used_opcodes - MINECRAFT_VM_SUPPORTED_OPCODES,
        key=LUA50_OPCODES.index,
    )
    global_calls = sorted({
        call["api"] for call in analysis["calls"]
        if call["dispatch"] == "global"
    })
    method_calls = sorted({
        call["api"] for call in analysis["calls"]
        if call["dispatch"] == "method"
    })
    admitted_globals = MINECRAFT_VM_BUILTIN_GLOBALS | MINECRAFT_RANGED_GLOBALS
    unsupported_globals = sorted(set(global_calls) - admitted_globals)
    unsupported_methods = sorted(set(method_calls) - MINECRAFT_RANGED_METHODS)
    admitted_receivers = {
        "GetOrientation": {"camera"},
        "Right": {"orientation"},
        "GetPos": {"orientation"},
        "GetRight": {"orientation"},
        "GetUp": {"orientation"},
        "GetDirection": {"orientation"},
        "GetCameraOrientation": {"actor"},
        "GetRangedAttackRay": {"actor"},
        "GetCurCombo": {"actor"},
        "AddCurCombo": {"actor"},
        "GetNodePos": {"actor"},
        "GetNodeDir": {"actor"},
        "GetNodeUp": {"actor"},
        "RangedAttackProc": {"actor"},
        "MissileAttackProc": {"actor"},
        "RangedMultiAttackProc": {"actor"},
        "CreateFang": {"actor"},
    }
    receiver_type_gaps: list[dict[str, Any]] = []
    for call in analysis["calls"]:
        if call["dispatch"] != "method" or call["api"] not in admitted_receivers:
            continue
        observed = set(call.get("receiverOrigins", []))
        unexpected = sorted(observed - admitted_receivers[call["api"]])
        if not observed or unexpected:
            receiver_type_gaps.append({
                "pc": call["pc"],
                "api": call["api"],
                "observed": sorted(observed) or ["unresolved"],
                "expected": sorted(admitted_receivers[call["api"]]),
            })
    structural_gaps: list[str] = []
    if prototype.numparams != 1:
        structural_gaps.append(
            f"executeImmediate requires one parameter, found {prototype.numparams}")
    if prototype.maxstacksize < 1:
        structural_gaps.append("executeImmediate requires maxStackSize >= 1")
    if prototype.nups:
        structural_gaps.append(
            f"BytecodeProgram/runtime carry no upvalue values, found {prototype.nups}")
    if prototype.children:
        structural_gaps.append(
            f"BytecodeProgram carries no child prototypes, found {len(prototype.children)}")
    if "math.random" in global_calls:
        structural_gaps.append(
            "executeImmediate installs a choice resolver that rejects math.random")
    unknown_call_pcs = sorted({
        item.pc for item in prototype.code
        if item.name == "CALL"
    } - {
        int(call["pc"]) for call in analysis["calls"]
    })
    if unknown_call_pcs:
        structural_gaps.append(
            "static call target unresolved at pc " + ",".join(map(str, unknown_call_pcs))
        )
    compatible = not (
        unsupported_opcodes or unsupported_globals or unsupported_methods
        or receiver_type_gaps or structural_gaps
    )
    return {
        "consumer": (
            "SourceSpecialCameraScriptVm.executeImmediate + "
            "SourceRangedScriptSession"
        ),
        "compatibleWithCurrentRangedImmediate": compatible,
        "supportedOpcodesUsed": sorted(
            used_opcodes & MINECRAFT_VM_SUPPORTED_OPCODES,
            key=LUA50_OPCODES.index,
        ),
        "unsupportedOpcodes": unsupported_opcodes,
        "globalCalls": global_calls,
        "unsupportedGlobalCalls": unsupported_globals,
        "methodCalls": method_calls,
        "unsupportedMethodCalls": unsupported_methods,
        "receiverTypeGaps": receiver_type_gaps,
        "structuralGaps": structural_gaps,
        "unknownCallPcs": unknown_call_pcs,
        "scope": (
            "static compatibility against current Java switch/runtime APIs; "
            "no Minecraft execution or gameplay acceptance"
        ),
    }


@dataclass(frozen=True)
class Lua50Toolchain:
    repository_root: Path
    compiler_binary: Path
    compiler_binary_sha256: str
    harness: Path
    harness_sha256: str
    archive: Path
    archive_sha256: str
    extracted_root: Path
    receipt: Path
    toolchain_compiler: Path
    toolchain_compiler_sha256: str
    official_source_hashes: Mapping[str, str]

    def json(self) -> dict[str, Any]:
        return {
            "schemaVersion": 1,
            "luaVersion": "5.0.3",
            "repositoryRoot": str(self.repository_root),
            "compilerBinary": str(self.compiler_binary),
            "compilerBinarySha256": self.compiler_binary_sha256,
            "compilerHarness": str(self.harness),
            "compilerHarnessSha256": self.harness_sha256,
            "officialLuaArchive": str(self.archive),
            "officialLuaArchiveSha256": self.archive_sha256,
            "extractedOfficialSources": str(self.extracted_root),
            "compilerReceipt": str(self.receipt),
            "compilerToolchain": str(self.toolchain_compiler),
            "compilerToolchainSha256": self.toolchain_compiler_sha256,
            "officialSourceHashes": dict(self.official_source_hashes),
            "compileOnlyOperations": ["luaL_loadfile", "lua_dump"],
            "forbiddenOperationsAbsent": [
                "lua_call", "lua_pcall", "luaopen_base", "game process launch",
            ],
            "sourceExecuted": False,
        }


def _primary_checkout(root: Path) -> Path | None:
    marker = root / ".git"
    if not marker.is_file():
        return root if (root / "sdgo_craft").is_dir() else None
    try:
        text = marker.read_text(encoding="utf-8").strip()
    except OSError:
        return None
    if not text.lower().startswith("gitdir:"):
        return None
    git_directory = Path(text.split(":", 1)[1].strip())
    if not git_directory.is_absolute():
        git_directory = (root / git_directory).resolve()
    if git_directory.parent.name == "worktrees" and git_directory.parent.parent.name == ".git":
        return git_directory.parent.parent.parent
    return None


def _official_files(extracted_root: Path) -> list[Path]:
    files = sorted(
        path for path in (extracted_root / "src").glob("*.c")
        if path.name != "ltests.c"
    )
    files.append(extracted_root / "src" / "lib" / "lauxlib.c")
    files.extend(sorted((extracted_root / "include").glob("*.h")))
    files.extend(sorted((extracted_root / "src").glob("*.h")))
    return files


def discover_toolchain(repository_root: str | Path | None = None) -> Lua50Toolchain:
    root = (
        Path(repository_root).resolve()
        if repository_root is not None
        else Path(__file__).resolve().parents[2]
    )
    primary = _primary_checkout(root)
    candidates = [
        item for item in (
            root,
            primary,
            primary.parent if primary is not None else None,
        )
        if item is not None
    ]
    repository = next(
        (candidate for candidate in candidates
         if (candidate / "sdgo_craft").is_dir()),
        None,
    )
    if repository is None:
        raise Lua50CompileError("cannot locate the primary sd-re checkout")
    craft = repository / "sdgo_craft"
    build = craft / "build" / "native"
    archive = build / "lua-5.0.3.tar.gz"
    extracted = build / "lua-5.0.3"
    binary = build / "sdgo_plain_lua_compile.exe"
    harness = craft / "scripts" / "native" / "sdgo_plain_lua_compile.c"
    receipt_path = craft / "sdgo-map" / "sdgo-plaintext-lua-compiler.json"
    compiler = repository / "toolchain" / "mingw32" / "bin" / (
        "i686-w64-mingw32-gcc.exe")
    for required in (
        archive, extracted, binary, harness, receipt_path, compiler,
    ):
        if not required.exists():
            raise Lua50CompileError(f"missing official Lua compiler input: {required}")
    archive_hash = _sha256_file(archive)
    harness_hash = _sha256_file(harness)
    binary_hash = _sha256_file(binary)
    if archive_hash != OFFICIAL_LUA_ARCHIVE_SHA256:
        raise Lua50CompileError("official Lua 5.0.3 archive hash drift")
    if harness_hash != OFFICIAL_COMPILER_HARNESS_SHA256:
        raise Lua50CompileError("compile-only Lua harness hash drift")
    if binary_hash != OFFICIAL_COMPILER_BINARY_SHA256:
        raise Lua50CompileError("official Lua compiler binary hash drift")
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    if (
        receipt.get("officialLuaArchiveSha256") != archive_hash
        or receipt.get("compilerHarnessSha256") != harness_hash
        or receipt.get("compilerBinarySha256") != binary_hash
        or receipt.get("sourceExecuted") is not False
    ):
        raise Lua50CompileError("existing Lua compiler receipt drift")
    official_hashes: dict[str, str] = {}
    files = _official_files(extracted)
    with tarfile.open(archive, "r:gz") as bundle:
        for path in files:
            relative = path.relative_to(craft).as_posix()
            member_name = path.relative_to(build).as_posix()
            member = bundle.extractfile(member_name)
            if member is None or member.read() != path.read_bytes():
                raise Lua50CompileError(
                    f"extracted official Lua source differs from archive: {path}")
            official_hashes[relative] = _sha256_file(path)
    receipt_bound_sources = {
        key: value for key, value in official_hashes.items()
        if key.endswith(".c")
    }
    if receipt.get("officialSourceHashes") != receipt_bound_sources:
        # The historical receipt binds compiler .c files.  Headers are newly
        # checked byte-for-byte above and remain in this report.
        raise Lua50CompileError("official Lua receipt source set/hash drift")
    compiler_hash = _sha256_file(compiler)
    if receipt.get("compilerToolchainSha256") != compiler_hash:
        raise Lua50CompileError("Lua compiler toolchain hash drift")
    return Lua50Toolchain(
        repository_root=repository,
        compiler_binary=binary,
        compiler_binary_sha256=binary_hash,
        harness=harness,
        harness_sha256=harness_hash,
        archive=archive,
        archive_sha256=archive_hash,
        extracted_root=extracted,
        receipt=receipt_path,
        toolchain_compiler=compiler,
        toolchain_compiler_sha256=compiler_hash,
        official_source_hashes=official_hashes,
    )


@dataclass(frozen=True)
class CompileReceipt:
    source_path: Path
    source_sha256: str
    output_path: Path
    output_sha256: str
    output_size: int
    repeat_sha256: str
    compiler_binary_sha256: str

    def json(self) -> dict[str, Any]:
        return {
            "sourcePath": str(self.source_path),
            "sourceSha256": self.source_sha256,
            "derivedBytecodePath": str(self.output_path),
            "derivedBytecodeSha256": self.output_sha256,
            "derivedBytecodeSize": self.output_size,
            "repeatSha256": self.repeat_sha256,
            "deterministic": self.output_sha256 == self.repeat_sha256,
            "compilerBinarySha256": self.compiler_binary_sha256,
            "sourceExecuted": False,
            "scope": (
                "official Lua 5.0.3 loadfile/dump of unchanged plaintext; "
                "compile-only; derived PCs are not shipped compiled PCs"
            ),
        }


def compile_source(
    source_path: str | Path,
    output_path: str | Path,
    *,
    toolchain: Lua50Toolchain | None = None,
    timeout: float = 15.0,
) -> tuple[Lua50Chunk, CompileReceipt]:
    """Compile one source twice with the verified harness, without calling it."""
    if timeout <= 0:
        raise ValueError("compile timeout must be positive")
    source = Path(source_path).resolve(strict=True)
    output = Path(output_path).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    selected = toolchain or discover_toolchain()
    repeat = output.with_name(output.name + f".{os.getpid()}.repeat")
    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)

    def invoke(destination: Path) -> None:
        result = subprocess.run(
            [str(selected.compiler_binary), str(source), str(destination)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=timeout,
            creationflags=creationflags,
        )
        if result.returncode != 0:
            message = result.stderr.decode("utf-8", "replace").strip()
            raise Lua50CompileError(
                f"official Lua 5.0.3 plaintext parse failed "
                f"(exit {result.returncode}): {message}"
            )
        if not destination.is_file():
            raise Lua50CompileError("Lua compiler returned success without output")

    try:
        invoke(output)
        invoke(repeat)
        first = output.read_bytes()
        second = repeat.read_bytes()
        if first != second:
            raise Lua50CompileError("official Lua 5.0.3 output is not deterministic")
        chunk = parse_chunk(first)
        receipt = CompileReceipt(
            source_path=source,
            source_sha256=_sha256_file(source),
            output_path=output,
            output_sha256=_sha256_bytes(first),
            output_size=len(first),
            repeat_sha256=_sha256_bytes(second),
            compiler_binary_sha256=selected.compiler_binary_sha256,
        )
        return chunk, receipt
    finally:
        if repeat.exists():
            repeat.unlink()


def _atomic_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f".{os.getpid()}.tmp")
    temporary.write_text(
        json.dumps(value, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    os.replace(temporary, path)


def _atomic_text(path: Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f".{os.getpid()}.tmp")
    temporary.write_text(value, encoding="utf-8", newline="\n")
    os.replace(temporary, path)


def _verify_priority_body(source: bytes, specification: Mapping[str, Any]) -> None:
    if _sha256_bytes(source) != specification["sourceSha256"]:
        raise Lua50CompileError(
            f"source hash drift for {specification['sourcePath']}")
    text = source.decode("utf-8-sig")
    start, end = specification["bodyCharacterRange"]
    body = text[start:end].encode("utf-8")
    if _sha256_bytes(body) != specification["bodySha256"]:
        raise Lua50CompileError(
            f"function body range/hash drift for {specification['function']}")


def _walk_prototypes(root: Lua50Prototype) -> Iterable[Lua50Prototype]:
    yield root
    for child in root.children:
        yield from _walk_prototypes(child)


def _write_rebuild_receipt(output: Path) -> dict[str, Any]:
    receipt = {
        "schemaVersion": 1,
        "outputs": {
            str(path.relative_to(output)).replace("\\", "/"): _sha256_file(path)
            for path in sorted(output.rglob("*"))
            if path.is_file() and path.name != "rebuild-receipt.json"
        },
    }
    _atomic_json(output / "rebuild-receipt.json", receipt)
    return receipt


def recover_text_corpus(
    archive_path: str | Path,
    cache_root: str | Path,
    inventory_path: str | Path,
    output_directory: str | Path,
    *,
    toolchain: Lua50Toolchain | None = None,
) -> dict[str, Any]:
    """Compile every native-v2 text SCP and retain exact success/refusal data."""
    from .index import read_index
    from .native_codec import NativeCodec
    from .resources import NamedResolver

    archive = Path(archive_path).resolve(strict=True)
    cache = Path(cache_root).resolve(strict=True)
    inventory_file = Path(inventory_path).resolve(strict=True)
    output = Path(output_directory).resolve()
    selected = toolchain or discover_toolchain()
    inventory = json.loads(inventory_file.read_text(encoding="utf-8"))
    archive_sha256 = _sha256_file(archive)
    if (
        inventory.get("codec_profile") != "union-native-load-cb32c0-zeroed-v2"
        or inventory.get("source", {}).get("sha256") != archive_sha256
    ):
        raise Lua50CompileError(
            "text inventory does not bind native-v2 selected source archive")
    resources = sorted(
        (
            item for item in inventory.get("resources", [])
            if item.get("kind") == "text"
        ),
        key=lambda item: str(item.get("normalized_path", "")),
    )
    if len(resources) != 185:
        raise Lua50CompileError(
            f"expected 185 native-v2 text SCP resources, found {len(resources)}")

    entries: list[dict[str, Any]] = []
    index = read_index(archive)
    with NativeCodec() as codec:
        resolver = NamedResolver(
            archive,
            index=index,
            bulk_cache=cache,
            cache=cache / "on-demand-scripts",
            decoder=codec,
            tail_policy="native-output-boundary",
        )
        for item in resources:
            source_path = str(item["path"])
            resource = resolver.read(source_path)
            source = bytes(resource["data"])
            if (
                resource["sha256"] != item["expanded_sha256"]
                or len(source) != item["expanded_size"]
                or index.resolve(source_path, normalized=True).offset
                != item["source_offset"]
            ):
                raise Lua50CompileError(
                    f"native-v2 inventory/resource drift for {source_path}")
            relative = Path(source_path.replace("\\", "/"))
            source_output = output / "corpus" / "sources" / relative
            source_output.parent.mkdir(parents=True, exist_ok=True)
            source_output.write_bytes(source)
            compiled_output = (
                output / "corpus" / "compiled" / relative.with_suffix(".lua50")
            )
            entry: dict[str, Any] = {
                "sourcePath": source_path,
                "sourceOffset": item["source_offset"],
                "sourceSize": len(source),
                "sourceSha256": resource["sha256"],
                "encoding": item.get("encoding"),
                "sourceExecuted": False,
            }
            try:
                chunk, receipt = compile_source(
                    source_output, compiled_output, toolchain=selected)
                prototypes = list(_walk_prototypes(chunk.root))
                bindings = global_function_bindings(chunk.root)
                opcodes = sorted(
                    {
                        instruction.name
                        for prototype in prototypes
                        for instruction in prototype.code
                    },
                    key=LUA50_OPCODES.index,
                )
                entry.update({
                    "status": "compiled",
                    "derivedBytecodePath": str(
                        compiled_output.relative_to(output)).replace("\\", "/"),
                    "derivedBytecodeSize": receipt.output_size,
                    "derivedBytecodeSha256": receipt.output_sha256,
                    "repeatSha256": receipt.repeat_sha256,
                    "prototypeCount": len(prototypes),
                    "instructionCount": sum(
                        len(prototype.code) for prototype in prototypes),
                    "globalFunctionBindingCount": len(bindings),
                    "globalFunctionNames": sorted({
                        binding.name for binding in bindings
                    }),
                    "opcodes": opcodes,
                })
            except Lua50CompileError as error:
                entry.update({
                    "status": "refused",
                    "error": str(error),
                })
            entries.append(entry)

    compiled = [item for item in entries if item["status"] == "compiled"]
    refused = [item for item in entries if item["status"] == "refused"]
    result = {
        "schemaVersion": 1,
        "sourceArchive": str(archive),
        "sourceArchiveSha256": archive_sha256,
        "codecProfile": inventory["codec_profile"],
        "inventoryPath": str(inventory_file),
        "inventorySha256": _sha256_file(inventory_file),
        "resourceCount": len(entries),
        "compiledCount": len(compiled),
        "refusedCount": len(refused),
        "allTextResourcesCompiled": not refused,
        "prototypeCount": sum(
            item.get("prototypeCount", 0) for item in compiled),
        "instructionCount": sum(
            item.get("instructionCount", 0) for item in compiled),
        "globalFunctionBindingCount": sum(
            item.get("globalFunctionBindingCount", 0) for item in compiled),
        "opcodes": sorted(
            {
                opcode
                for item in compiled
                for opcode in item.get("opcodes", [])
            },
            key=LUA50_OPCODES.index,
        ),
        "resources": entries,
        "sourceExecuted": False,
        "gameExecuted": False,
        "scope": (
            "unchanged native-v2 text SCP resources compiled by the official "
            "Lua5.0.3 loadfile/dump harness only; no chunk execution"
        ),
    }
    _atomic_json(output / "corpus" / "text-lua50-compile.json", result)
    lines = [
        "# G Union native-v2 text Lua corpus",
        "",
        f"- Text resources: `{len(entries)}`",
        f"- Compiled deterministically: `{len(compiled)}`",
        f"- Syntax/tool refusals: `{len(refused)}`",
        f"- Prototypes: `{result['prototypeCount']}`",
        f"- Instructions: `{result['instructionCount']}`",
        f"- Global function bindings: `{result['globalFunctionBindingCount']}`",
        "",
        "Every source was kept byte-identical and passed only through the "
        "verified Lua 5.0.3 `luaL_loadfile`/`lua_dump` harness. The source "
        "chunks and game were not executed.",
        "",
    ]
    if refused:
        lines.extend(["## Refusals", ""])
        for item in refused:
            lines.append(
                f"- `{item['sourcePath']}`: {item['error'].replace(chr(10), ' ')}")
        lines.append("")
    _atomic_text(output / "corpus" / "TEXT-CORPUS.md", "\n".join(lines))
    _write_rebuild_receipt(output)
    return result


def recover_priority_programs(
    archive_path: str | Path,
    cache_root: str | Path,
    output_directory: str | Path,
    *,
    toolchain: Lua50Toolchain | None = None,
) -> dict[str, Any]:
    """Recover the three authorized Overon functions into exact/carrier JSON."""
    from .index import read_index
    from .native_codec import NativeCodec
    from .resources import NamedResolver

    archive = Path(archive_path).resolve(strict=True)
    cache = Path(cache_root).resolve(strict=True)
    output = Path(output_directory).resolve()
    output.mkdir(parents=True, exist_ok=True)
    selected = toolchain or discover_toolchain()
    archive_sha256 = _sha256_file(archive)
    by_source: dict[str, list[Mapping[str, Any]]] = defaultdict(list)
    for item in PRIORITY_FUNCTIONS:
        by_source[str(item["sourcePath"])].append(item)

    chunks: dict[str, Lua50Chunk] = {}
    compile_receipts: dict[str, dict[str, Any]] = {}
    resource_receipts: dict[str, dict[str, Any]] = {}
    index = read_index(archive)
    with NativeCodec() as codec:
        resolver = NamedResolver(
            archive,
            index=index,
            bulk_cache=cache,
            cache=cache / "on-demand-scripts",
            decoder=codec,
            tail_policy="native-output-boundary",
        )
        for source_path, specifications in sorted(by_source.items()):
            entry = index.resolve(source_path, normalized=True)
            expected_offsets = {
                int(specification["sourceOffset"])
                for specification in specifications
            }
            if expected_offsets != {entry.offset}:
                raise Lua50CompileError(
                    f"source ATAD offset drift for {source_path}: "
                    f"0x{entry.offset:x}")
            resource = resolver.read(source_path)
            source = bytes(resource["data"])
            for specification in specifications:
                _verify_priority_body(source, specification)
            leaf = Path(source_path.replace("\\", "/")).name
            extracted = output / "sources" / "scp" / leaf
            extracted.parent.mkdir(parents=True, exist_ok=True)
            if extracted.exists() and extracted.read_bytes() != source:
                raise Lua50CompileError(f"owned extracted source drift: {extracted}")
            extracted.write_bytes(source)
            compiled = output / "compiled" / "scp" / f"{Path(leaf).stem}.lua50"
            chunk, receipt = compile_source(
                extracted, compiled, toolchain=selected)
            chunks[source_path] = chunk
            compile_receipts[source_path] = receipt.json()
            resource_receipts[source_path] = {
                "sourcePath": resource["source_path"],
                "normalizedPath": resource["path"],
                "sourceSha256": resource["sha256"],
                "sourceSize": len(source),
                "sourceArchive": str(archive),
                "sourceArchiveSha256": archive_sha256,
                "provenance": resource["provenance"],
            }
            _atomic_json(
                output / "exact" / f"{Path(leaf).stem}.lua50.json",
                chunk.exact_json(),
            )

    programs: dict[str, dict[str, Any]] = {}
    for specification in PRIORITY_FUNCTIONS:
        function = str(specification["function"])
        source_path = str(specification["sourcePath"])
        chunk = chunks[source_path]
        binding = find_global_function(chunk.root, function)
        analysis = analyze_prototype(binding.prototype)
        compatibility = minecraft_ranged_compatibility(
            binding.prototype, analysis)
        program = {
            "schemaVersion": 1,
            "weaponId": specification["weaponId"],
            "function": function,
            "sourcePath": source_path,
            "sourceOffset": specification["sourceOffset"],
            "sourceSha256": specification["sourceSha256"],
            "bodyCharacterRange": specification["bodyCharacterRange"],
            "bodyLineRange": specification["bodyLineRange"],
            "bodySha256": specification["bodySha256"],
            "binding": binding.json(),
            "prototype": binding.prototype.exact_json(),
            "bytecodeProgram": binding.prototype.bytecode_program(),
            "sourceRangedProgramCarrier": {
                "scriptFile": source_path.replace("\\", "/"),
                "prototypeSha256": binding.prototype.semantic_sha256(),
                "constants": binding.prototype.carrier_constants(),
                "codeWords": [
                    instruction.raw for instruction in binding.prototype.code
                ],
                "parameterCount": binding.prototype.numparams,
                "maxStackSize": binding.prototype.maxstacksize,
                "calls": [
                    {
                        **call,
                        "dispatch": (
                            "callable"
                            if call["dispatch"] == "global"
                            else call["dispatch"]
                        ),
                    }
                    for call in analysis["calls"]
                ],
                "opcodes": analysis["opcodes"],
                "scriptFiles": [source_path.replace("\\", "/")],
            },
            "analysis": analysis,
            "minecraftCompatibility": compatibility,
            "currentLibraryRegistration": {
                "registered": False,
                "reason": (
                    "SourceRangedScriptPrograms currently hardcodes the baseline "
                    "213-program/272-slot library and source EXE hash; this "
                    "carrier requires an explicit protocol/library extension"
                ),
            },
            "derivedProgramCounterNotice": (
                "PCs come from deterministic compilation of the unchanged "
                "plaintext resource; they are not claimed as shipped compiled PCs"
            ),
            "sourceExecuted": False,
        }
        programs[function] = program
        _atomic_json(output / "programs" / f"{function}.json", program)

    result = {
        "schemaVersion": 1,
        "goal": "compile-only Lua50 carriers for three G Union Overon functions",
        "sourceArchive": str(archive),
        "sourceArchiveSha256": archive_sha256,
        "codecProfile": "union-native-load-cb32c0-zeroed-v2",
        "toolchain": selected.json(),
        "resources": resource_receipts,
        "compilations": compile_receipts,
        "programs": programs,
        "allPriorityFunctionsRecovered": len(programs) == len(PRIORITY_FUNCTIONS),
        "allCurrentMinecraftCompatible": all(
            program["minecraftCompatibility"][
                "compatibleWithCurrentRangedImmediate"]
            for program in programs.values()
        ),
        "sourceExecuted": False,
        "gameExecuted": False,
        "scope": (
            "source-bound static recovery and official Lua5.0.3 compile-only "
            "export; no native ABI, Minecraft runtime, visual or gameplay claim"
        ),
    }
    _atomic_json(output / "toolchain.json", selected.json())
    _atomic_json(output / "priority-programs.json", result)
    summary_lines = [
        "# G Union Lua 5.0 compatibility recovery",
        "",
        "The unchanged `scp/2619.scp` and `scp/2640.scp` resources were "
        "compiled twice through the byte-verified official Lua 5.0.3 "
        "`luaL_loadfile`/`lua_dump` harness. No source chunk, game process, "
        "database or Minecraft runtime was executed.",
        "",
        f"- Source archive SHA-256: `{archive_sha256}`",
        f"- Priority functions recovered: `{len(programs)}/{len(PRIORITY_FUNCTIONS)}`",
        "- Exact binary chunks, recursive prototypes, constants, debug data, "
        "upvalues and code words are under `compiled/` and `exact/`.",
        "- Each file under `programs/` contains a concrete `bytecodeProgram` "
        "record and a `sourceRangedProgramCarrier` matching the current Java "
        "record fields.",
        "",
        "## Current Minecraft compatibility",
        "",
        "| Function | Weapon | Core opcodes | Unresolved globals | "
        "Unresolved methods | Current immediate VM |",
        "|---|---:|---|---|---|---|",
    ]
    for name, program in programs.items():
        compatibility = program["minecraftCompatibility"]
        opcode_gap = ", ".join(compatibility["unsupportedOpcodes"]) or "complete"
        global_gap = ", ".join(
            compatibility["unsupportedGlobalCalls"]) or "none"
        method_gap = ", ".join(
            compatibility["unsupportedMethodCalls"]) or "none"
        admitted = (
            "compatible"
            if compatibility["compatibleWithCurrentRangedImmediate"]
            else "protocol/runtime expansion required"
        )
        summary_lines.append(
            f"| `{name}` | {program['weaponId']} | {opcode_gap} | "
            f"{global_gap} | {method_gap} | {admitted} |"
        )
    summary_lines.extend([
        "",
        "`baitudouxiandanqiang` is statically compatible with the current "
        "ranged immediate VM and its existing runtime APIs. "
        "`oubeilongmgluanwu` uses only VM-supported opcodes but requires the "
        "private-server effect, game-interface, attachment, animation, timer, "
        "camera and target-effect APIs listed in its JSON. `oblmslkuosan` "
        "requires `ipairs` plus `SETLIST`, `TFORPREP` and `TFORLOOP`.",
        "",
        "The current `SourceRangedScriptPrograms` loader still pins the "
        "baseline 213 programs, 272 slots and source EXE hash. These carriers "
        "are therefore recovered and import-shaped, but are not registered in "
        "the shared Minecraft runtime by this bounded task.",
        "",
        "All program counters are from deterministic recompilation of the "
        "unchanged plaintext resources. They are not claims about any shipped "
        "compiled program counters or runtime/gameplay parity.",
        "",
    ])
    _atomic_text(output / "SUMMARY.md", "\n".join(summary_lines))
    _write_rebuild_receipt(output)
    return result


def _default_paths() -> tuple[Path, Path, Path]:
    worktree = Path(__file__).resolve().parents[2]
    checkout = _primary_checkout(worktree)
    primary = (
        checkout.parent
        if checkout is not None and (checkout.parent / "sdgo_craft").is_dir()
        else checkout or worktree
    )
    return (
        primary / "SDGO_UNION_5.4.6" / "DataPack.bin",
        primary / "gunion-recovered-native-v2",
        primary / "progress" / "gunion-recovery" / "lua50-compat",
    )


def main(argv: Sequence[str] | None = None) -> int:
    archive, cache, output = _default_paths()
    parser = argparse.ArgumentParser(
        description="Compile-only official Lua5.0.3 export for G Union scripts")
    parser.add_argument("--archive", type=Path, default=archive)
    parser.add_argument("--cache", type=Path, default=cache)
    parser.add_argument("--output", type=Path, default=output)
    parser.add_argument(
        "--check-toolchain", action="store_true",
        help="verify only the official compiler chain")
    parser.add_argument(
        "--compile-all-text", action="store_true",
        help="also compile all 185 native-v2 text SCP resources")
    parser.add_argument(
        "--inventory", type=Path,
        help="native-v2 luajit-script-inventory.json for --compile-all-text")
    arguments = parser.parse_args(argv)
    toolchain = discover_toolchain()
    if arguments.check_toolchain:
        print(json.dumps(toolchain.json(), ensure_ascii=False, indent=2))
        return 0
    result = recover_priority_programs(
        arguments.archive,
        arguments.cache,
        arguments.output,
        toolchain=toolchain,
    )
    corpus = None
    if arguments.compile_all_text:
        inventory = arguments.inventory or (
            toolchain.repository_root
            / "progress" / "gunion-recovery" / "scripts" / "native-v2"
            / "luajit-script-inventory.json"
        )
        corpus = recover_text_corpus(
            arguments.archive,
            arguments.cache,
            inventory,
            arguments.output,
            toolchain=toolchain,
        )
    print(json.dumps({
        "output": str(arguments.output.resolve()),
        "functions": sorted(result["programs"]),
        "allPriorityFunctionsRecovered": result[
            "allPriorityFunctionsRecovered"],
        "allCurrentMinecraftCompatible": result[
            "allCurrentMinecraftCompatible"],
        "textCorpus": (
            {
                "resourceCount": corpus["resourceCount"],
                "compiledCount": corpus["compiledCount"],
                "refusedCount": corpus["refusedCount"],
            }
            if corpus is not None else None
        ),
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
