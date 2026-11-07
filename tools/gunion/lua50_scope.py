"""Typed string-constant lowering for already recovered Lua 5.0 chunks.

This is not a string scanner or a script compiler. Every rewrite names one
prototype/constant carrier and an externally reviewed role. Instruction words,
numbers, debug carriers and every undeclared string remain byte-identical.
Native lookup/collision/publication authority and gameplay are separate.
"""
from dataclasses import dataclass
import hashlib
import struct

from .lua50_compile import parse_chunk


def _i(value):return struct.pack('<I',value)
def _string(value):return _i(0) if value is None else _i(len(value)+1)+value+b'\0'


def encode_chunk(chunk,replacements=None):
    replacements=replacements or {}
    def prototype(p):
        data=bytearray(_string(p.source)+_i(p.line_defined)+bytes((p.nups,p.numparams,p.is_vararg,p.maxstacksize)))
        data+=_i(len(p.line_info))+b''.join(_i(v)for v in p.line_info)
        data+=_i(len(p.locals))
        for local in p.locals:data+=_string(local.name)+_i(local.start_pc)+_i(local.end_pc)
        data+=_i(len(p.upvalue_names))+b''.join(_string(v)for v in p.upvalue_names)
        data+=_i(len(p.constants))
        for index,constant in enumerate(p.constants):
            data.append(constant.tag)
            key=(p.path,index)
            if constant.kind=='string':data+=_string(replacements.get(key,constant.value))
            else:
                if key in replacements:raise ValueError('only typed string constants may change')
                data+=bytes.fromhex(constant.raw_hex)
        data+=_i(len(p.children))+b''.join(prototype(c)for c in p.children)
        data+=_i(len(p.code))+b''.join(_i(i.raw)for i in p.code)
        return bytes(data)
    return bytes.fromhex(chunk.header.raw_hex)+prototype(chunk.root)


def prototypes(root):
    yield root
    for child in root.children:yield from prototypes(child)


def constant_uses(prototype):
    result={index:[] for index in range(len(prototype.constants))}
    for instruction in prototype.code:
        operands=[]
        if instruction.name in ('GETGLOBAL','SETGLOBAL','LOADK'):operands.append(('Bx',instruction.bx))
        if instruction.name in ('GETTABLE','SELF') and instruction.c>=250:operands.append(('C',instruction.c-250))
        if instruction.name in ('SETTABLE','ADD','SUB','MUL','DIV','POW','EQ','LT','LE'):
            for field,value in [('B',instruction.b),('C',instruction.c)]:
                if value>=250:operands.append((field,value-250))
        for operand,index in operands:
            if index not in result:raise ValueError('constant operand outside declared pool')
            result[index].append(dict(pc=instruction.pc,opcode=instruction.name,operand=operand))
    return result


@dataclass(frozen=True)
class Rewrite:
    prototype:tuple[int,...]
    constant:int
    source:bytes
    target:bytes
    role:str
    authority:str
    consumer_api:bytes|None=None


def lower(raw,rewrites):
    chunk=parse_chunk(raw)
    if encode_chunk(chunk)!=raw:raise ValueError('serializer does not reproduce complete original chunk')
    by_path={p.path:p for p in prototypes(chunk.root)}
    replacements={};proof=[]
    for declaration in rewrites:
        key=(declaration.prototype,declaration.constant)
        if key in replacements:raise ValueError('duplicate scoped constant declaration')
        p=by_path.get(declaration.prototype)
        if p is None or not 0<=declaration.constant<len(p.constants):raise ValueError('unknown scoped constant carrier')
        constant=p.constants[declaration.constant]
        if constant.kind!='string' or constant.value!=declaration.source:raise ValueError('scoped constant source differs')
        if not declaration.authority or not declaration.target or b'\0' in declaration.target:raise ValueError('scoped constant authority/target missing')
        uses=constant_uses(p)[declaration.constant]
        if not uses:raise ValueError('unused constant cannot establish a writable semantic carrier')
        operations={u['opcode']for u in uses}
        if declaration.role in ('global-export','mutable-global'):
            if not operations<= {'GETGLOBAL','SETGLOBAL'}:raise ValueError('global constant also has non-global uses')
            if declaration.role=='global-export' and declaration.prototype!=():raise ValueError('export must be a root declaration')
            if declaration.role=='mutable-global' and 'SETGLOBAL' not in operations:raise ValueError('read-only host globals cannot be scoped as mutable state')
            if declaration.role=='global-export':
                for use in uses:
                    pc=use['pc']
                    if use['opcode']=='SETGLOBAL' and (pc==0 or p.code[pc-1].name!='CLOSURE' or p.code[pc-1].a!=p.code[pc].a):
                        raise ValueError('root export is not a direct closure publication')
        elif declaration.role=='callback-name':
            if operations!={'LOADK'}or declaration.consumer_api!=b'SpecialScriptFuncStr':
                raise ValueError('callback name requires exact reviewed script-name setter')
            for use in uses:
                pc=use['pc']
                if pc<1 or pc+1>=len(p.code):raise ValueError('callback setter producer incomplete')
                producer,method,call=p.code[pc],p.code[pc-1],p.code[pc+1]
                if method.name!='SELF' or call.name!='CALL' or method.a!=call.a or producer.a!=call.a+2 or call.b!=3 or method.c<250:
                    raise ValueError('callback constant does not feed the exact single-argument method call')
                constant_index=method.c-250
                if constant_index>=len(p.constants)or p.constants[constant_index].value!=declaration.consumer_api:
                    raise ValueError('callback setter identity differs')
        elif declaration.role=='global-import':
            # A read-only reference to another owner-scoped export or scoped mutable global.
            if operations!={'GETGLOBAL'}:raise ValueError('import constant must only be read as a global')
        elif declaration.role=='resource-path':
            if operations!={'LOADK'}:raise ValueError('resource carrier requires reviewed LOADK argument producers')
        else:raise ValueError('unreviewed scoped constant role')
        replacements[key]=declaration.target
        proof.append(dict(prototype=list(declaration.prototype),constant=declaration.constant,role=declaration.role,
                          sourceHex=declaration.source.hex(),targetHex=declaration.target.hex(),uses=uses,authority=declaration.authority,
                          consumerApiHex=declaration.consumer_api.hex()if declaration.consumer_api else None))
    target=encode_chunk(chunk,replacements);parsed=parse_chunk(target)
    if encode_chunk(parsed)!=target:raise ValueError('lowered chunk cannot roundtrip')
    for a,b in zip(prototypes(chunk.root),prototypes(parsed.root),strict=True):
        for field in ('source','line_defined','nups','numparams','is_vararg','maxstacksize','line_info','locals','upvalue_names'):
            if getattr(a,field)!=getattr(b,field):raise ValueError('lowering changed prototype metadata')
        if [i.raw for i in a.code]!=[i.raw for i in b.code]:raise ValueError('lowering changed instructions')
        for index,(left,right)in enumerate(zip(a.constants,b.constants,strict=True)):
            expected=replacements.get((a.path,index),left.value)
            if left.kind!=right.kind or right.value!=expected or left.kind!='string' and left.raw_hex!=right.raw_hex:
                raise ValueError('lowering changed undeclared constant or numeric bits')
    return target,dict(schema='sdgo.typed-lua50-string-scope/1',sourceSha256=hashlib.sha256(raw).hexdigest(),
                       targetSha256=hashlib.sha256(target).hexdigest(),carriers=proof,
                       everyInstructionAndNumericConstantUnchanged=True,debugMetadataUnchanged=True,
                       nativeRootOrGameplayAccepted=False)
