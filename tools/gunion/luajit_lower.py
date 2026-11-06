"""Source-attributed LuaJIT FR1 subset -> the existing Lua 5.0 execution spine.

This is a derived instruction carrier, never recovered Lua 5.0 bytecode. One
or more derived instructions correspond to each original instruction; all
branch targets and host-call occurrences retain an explicit source-PC mapping.
Unsupported opcodes, frame layouts, constants and ambiguous method lookups are
rejected. The original dump and prototype hashes remain authoritative.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import math
import struct
from typing import Any

from .luajit import FLAG_BIG_ENDIAN, FLAG_FFI, FLAG_FR2, LuaJITDump, Prototype


class LoweringError(ValueError):
    pass


@dataclass(frozen=True)
class LoweredProgram:
    source_sha256: str
    source_prototype_sha256: str
    source_prototype_id: int
    constant_pool: tuple[Any, ...]
    code_words: tuple[int, ...]
    parameter_count: int
    max_stack_size: int
    source_opcode_counts: dict[str, int]
    source_pc_to_derived_pc: tuple[int, ...]

    def document(self) -> dict[str, Any]:
        return {
            "schema": "capsulearena:luajit-fr1-derived-lua50-spine/1",
            "sourceSha256": self.source_sha256,
            "sourcePrototypeSha256": self.source_prototype_sha256,
            "sourcePrototypeId": self.source_prototype_id,
            "sourceEncoding": "luajit-2.1-legacy-fr1",
            "derivedEncoding": "lua50-register-instruction-carrier",
            "pcMapping": "sourcePcToDerivedPc[source-one-based-pc - 1] is the derived zero-based entry",
            "sourcePcToDerivedPc": list(self.source_pc_to_derived_pc),
            "sourceOpcodeCounts": self.source_opcode_counts,
            "constantPool": list(self.constant_pool),
            "codeWords": list(self.code_words),
            "parameterCount": self.parameter_count,
            "maxStackSize": self.max_stack_size,
            "originalDumpChanged": False,
            "originalLua50BytecodeClaimed": False,
            "requiresIndependentVmOracleBeforeRuntimeAdmission": True,
        }


def _abc(op: int, a: int, b: int = 0, c: int = 0) -> int:
    if not (0 <= op < 64 and 0 <= a < 256 and 0 <= b < 512 and 0 <= c < 512):
        raise LoweringError("derived ABC operand out of range")
    return op | a << 24 | b << 15 | c << 6


def _abx(op: int, a: int, bx: int) -> int:
    if not 0 <= bx < 262144:
        raise LoweringError("derived Bx operand out of range")
    return _abc(op, a) | bx << 6


def lower_prototype(dump: LuaJITDump, prototype: Prototype) -> LoweredProgram:
    if dump.opcode_profile != "luajit-2.1-legacy" or dump.flags & (FLAG_BIG_ENDIAN | FLAG_FFI | FLAG_FR2):
        raise LoweringError("requires little-endian legacy FR1 dump without FFI")
    if prototype.upvalues or prototype.child_prototype_ids or prototype.flags or not 0 < prototype.frame_size <= 250:
        raise LoweringError("unsupported closure/vararg/prototype frame")
    pool: list[Any] = []
    pool_index: dict[str, int] = {}

    def constant(value: Any) -> int:
        if isinstance(value,bool) or not (value is None or isinstance(value, (str, int, float))):
            raise LoweringError("unsupported derived constant type")
        if isinstance(value, (int, float)) and not isinstance(value, bool) and not math.isfinite(value):
            raise LoweringError("nonfinite source numeric constant")
        key = json.dumps([type(value).__name__, value], ensure_ascii=True, allow_nan=False)
        if key not in pool_index:
            pool_index[key] = len(pool)
            pool.append(value)
        return pool_index[key]

    def string(operand: int) -> str:
        item = prototype.gc_constant_for_operand(operand)
        if item is None or item.kind != "string":
            raise LoweringError("source string constant required")
        try:
            return item.value.decode("utf-8", "strict")
        except UnicodeDecodeError as error:
            raise LoweringError("non-UTF8 source string is not admitted by the host carrier") from error

    def number(operand: int) -> Any:
        if not 0 <= operand < len(prototype.numeric_constants):
            raise LoweringError("source numeric constant index")
        return prototype.numeric_constants[operand].value

    def primitive(operand: int) -> Any:
        if operand not in (0, 1, 2):
            raise LoweringError("source primitive operand")
        return (None, False, True)[operand]

    def rk(value: Any) -> int:
        index = constant(value)
        if index >= 262:
            raise LoweringError("derived RK constant exceeds the existing spine")
        return 250 + index

    instructions = prototype.instructions
    jump_targets = {i.pc + 1 + i.jump for i in instructions if i.opcode_name == "JMP"}
    # Lua 5.0 has no boolean constants. Exact primitive comparisons therefore
    # load an unused scratch register before EQ, with all branches relocated.
    scratch=prototype.frame_size
    if scratch>=250 and any(i.opcode_name in ("ISEQP","ISNEP") for i in instructions):
        raise LoweringError("no scratch register for exact primitive comparisons")
    pc_map=[];derived_count=0
    for ins in instructions:
        pc_map.append(derived_count)
        derived_count+=2 if ins.opcode_name in ("ISEQP","ISNEP") else 1
    words: list[int] = []
    counts: dict[str, int] = {}
    for position, ins in enumerate(instructions):
        op, a = ins.opcode_name, ins.a
        counts[op] = counts.get(op, 0) + 1
        if ins.pc != position + 1:
            raise LoweringError("source instruction ordering")
        if op == "MOV":
            word = _abc(0, a, ins.d)
        elif op in ("KSTR", "KNUM", "KSHORT"):
            value = string(ins.d) if op == "KSTR" else number(ins.d) if op == "KNUM" else ins.d_signed
            word = _abx(1, a, constant(value))
        elif op=="KPRI":
            value=primitive(ins.d)
            word=_abc(3,a,a) if value is None else _abc(2,a,int(value),0)
        elif op=="KNIL":
            if not a<=ins.d<prototype.frame_size:raise LoweringError("source nil register range")
            word=_abc(3,a,ins.d)
        elif op in ("GGET", "GSET"):
            word = _abx(5 if op == "GGET" else 7, a, constant(string(ins.d)))
        elif op == "TGETS":
            previous = instructions[position - 1] if position else None
            # LuaJIT emits MOV self; TGETS function for a colon call. SELF is
            # equivalent only when that MOV dominates this occurrence. Ordinary
            # table/member reads need a separately proven lowering.
            key=string(ins.c)
            is_self=(previous is not None and previous.opcode_name=="MOV" and previous.a==a+1 and previous.d==ins.b and ins.pc not in jump_targets)
            if is_self:word=_abc(11,a,ins.b,rk(key))
            # The existing ranged scene represents ray3 with explicit pos/dir
            # fields. These are value reads, just like vector components; SELF
            # would additionally overwrite A+1 with a receiver.
            elif key in ("x","y","z","pos","dir"):word=_abc(6,a,ins.b,rk(key))
            else:raise LoweringError(f"unproven colon-call/field TGETS at source pc {ins.pc}")
        elif op=="TSETS":
            key=string(ins.c)
            if key not in ("x","y","z"):raise LoweringError("unproven host field write")
            word=_abc(9,ins.b,rk(key),a)
        elif op in ("CALL", "CALLM"):
            if a >= prototype.frame_size or (ins.b and a + ins.b - 1 > prototype.frame_size):
                raise LoweringError("source call result register range")
            if op == "CALL" and (ins.c == 0 or a + ins.c > prototype.frame_size):
                raise LoweringError("unsupported source fixed call argument range")
            # FR1 CALL has B=nresults+1/C=nargs+1. CALLM consumes the previous
            # open result at top; the existing spine's B=0 has the same end.
            word = _abc(25, a, ins.c if op == "CALL" else 0, ins.b)
        elif op in ("ISEQN", "ISNEN", "ISEQP", "ISNEP"):
            if position + 1 >= len(instructions) or instructions[position + 1].opcode_name != "JMP":
                raise LoweringError("source comparison is not followed by its branch")
            if op.endswith("N"):
                word=_abc(21,int(op.startswith("ISEQ")),a,rk(number(ins.d)))
            else:
                value=primitive(ins.d)
                words.append(_abc(3,scratch,scratch) if value is None else _abc(2,scratch,int(value),0))
                word=_abc(21,int(op.startswith("ISEQ")),a,scratch)
        elif op == "JMP":
            target = ins.pc + 1 + ins.jump
            if not 1 <= target <= len(instructions):
                raise LoweringError("source branch target outside prototype")
            relative=pc_map[target-1]-(pc_map[position]+1)
            word = _abx(20, 0, relative + 0x1ffff)
        elif op in ("RET0", "RET1"):
            word = _abc(27, a, 1 if op == "RET0" else 2)
        else:
            raise LoweringError(f"unsupported source opcode {op} at pc {ins.pc}")
        words.append(word)
    if len(words) != derived_count:
        raise LoweringError("source instruction attribution failed")
    return LoweredProgram(dump.sha256, prototype.sha256, prototype.prototype_id,
                          tuple(pool), tuple(words), prototype.num_parameters,
                          prototype.frame_size+(derived_count>len(instructions)),counts,tuple(pc_map))


def native_lua50_chunk(dump: LuaJITDump) -> tuple[bytes, dict[str, Any]]:
    """Encode strict root function bindings and independently lowered children.

    The result is a target derivative. It must be executed in the official
    Lua 5.0 reference with matching external fixtures before native admission.
    """
    from .lua50_compile import LUA50_TEST_NUMBER, parse_chunk
    root=dump.prototypes[dump.root_prototype_id]
    if root.upvalues or root.num_parameters or root.flags!=3:
        raise LoweringError("unsupported script root frame")
    children=[];bindings=[];code=[];constants=[]
    instructions=root.instructions
    for position in range(0,len(instructions)-1,2):
        first,second=instructions[position:position+2]
        if first.opcode_name!="FNEW" or second.opcode_name!="GSET" or first.a!=second.a:
            raise LoweringError("root is not explicit FNEW/GSET bindings")
        child=root.gc_constant_for_operand(first.d);name=root.gc_constant_for_operand(second.d)
        if child is None or child.kind!="child" or name is None or name.kind!="string":
            raise LoweringError("root binding constants")
        prototype=dump.prototypes[child.child_prototype_id];lowered=lower_prototype(dump,prototype)
        index=len(children);children.append(lowered);constants.append(name.value.decode('utf-8','strict'))
        code.extend([_abx(34,first.a,index),_abx(7,first.a,index)])
        bindings.append(dict(function=constants[-1],sourcePrototypeId=prototype.prototype_id,
                             sourcePrototypeSha256=prototype.sha256,targetChildIndex=index))
    if len(instructions)!=len(children)*2+1 or instructions[-1].opcode_name!="RET0":
        raise LoweringError("root terminal return")
    code.append(_abc(27,instructions[-1].a,1))
    def i32(n):return struct.pack('<i',n)
    def string(s):
        if s is None:return i32(0)
        b=s.encode('utf-8');return i32(len(b)+1)+b+b'\0'
    def proto(pool,words,parameters,stack,nested=(),vararg=False,source=None):
        result=string(source)+i32(0)+bytes([0,parameters,int(vararg),stack])+i32(0)*3+i32(len(pool))
        for value in pool:
            if value is None:result+=b'\0'
            elif isinstance(value,str):result+=b'\4'+string(value)
            elif isinstance(value,(int,float)) and not isinstance(value,bool):result+=b'\3'+struct.pack('<d',float(value))
            else:raise LoweringError('constant cannot be encoded by official Lua 5.0')
        result+=i32(len(nested))+b''.join(nested)+i32(len(words))+b''.join(struct.pack('<I',w) for w in words)
        return result
    nested=[proto(p.constant_pool,p.code_words,p.parameter_count,p.max_stack_size) for p in children]
    raw=b'\x1bLua'+bytes([0x50,1,4,4,4,6,8,9,9,8])+struct.pack('<d',LUA50_TEST_NUMBER)
    raw+=proto(constants,code,0,max(root.frame_size,2),nested,True,'@union-derived/'+dump.sha256[:16]+'.scp')
    decoded=parse_chunk(raw)
    if len(decoded.root.children)!=len(children):raise LoweringError('target prototype count differs')
    for original,target in zip(children,decoded.root.children):
        if tuple(i.raw for i in target.code)!=original.code_words:raise LoweringError('target instruction serialization differs')
    return raw,dict(sourceSha256=dump.sha256,targetSha256=hashlib.sha256(raw).hexdigest(),
                    targetEncoding='official-32bit-little-endian-lua-5.0',bindings=bindings,
                    sourceBytesChanged=False,nativeRuntimeAdmitted=False)
