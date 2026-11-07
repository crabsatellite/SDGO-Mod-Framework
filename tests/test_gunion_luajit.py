"""Independent structural tests for the G Union LuaJIT bytecode reader."""
import hashlib
import os
from pathlib import Path
import unittest

from tools.gunion.container import Archive
from tools.gunion.luajit import (
    FLAG_BIG_ENDIAN,
    FLAG_BITOP,
    FLAG_FFI,
    FLAG_STRIPPED,
    KGC_CHILD,
    KGC_STRING,
    LEGACY_OPCODE_SPECS,
    BITOP_OPCODE_SPECS,
    LuaJITFormatError,
    ParseLimits,
    UnsupportedLuaJITFeature,
    opcode_number,
    parse_dump,
)


def uleb(value):
    if value < 0:
        value &= 0xFFFFFFFF
    encoded = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            encoded.append(byte | 0x80)
        else:
            encoded.append(byte)
            return bytes(encoded)


def instruction(name, *, a=0, b=0, c=0, d=None, byte_order="little", bitop=False):
    opcode = opcode_number(name, bitop=bitop)
    if d is None:
        word = opcode | (a << 8) | (c << 16) | (b << 24)
    else:
        word = opcode | (a << 8) | (d << 16)
    return word.to_bytes(4, byte_order)


def prototype(
    *,
    flags=0,
    parameters=0,
    frame_size=2,
    upvalues=(),
    instructions=(),
    gc_constants=(),
    numeric_constants=(),
    debug=None,
    first_line=0,
    line_span=0,
    stripped=True,
    byte_order="little",
):
    payload = bytearray((flags, parameters, frame_size, len(upvalues)))
    payload += uleb(len(gc_constants))
    payload += uleb(len(numeric_constants))
    payload += uleb(len(instructions))
    if not stripped:
        payload += uleb(len(debug or b""))
        if debug:
            payload += uleb(first_line)
            payload += uleb(line_span)
    payload += b"".join(instructions)
    payload += b"".join(int(value).to_bytes(2, byte_order) for value in upvalues)
    payload += b"".join(gc_constants)
    payload += b"".join(numeric_constants)
    payload += debug or b""
    return bytes(payload)


def dump(*prototypes, flags=FLAG_STRIPPED, name=b""):
    result = bytearray(b"\x1bLJ\x02")
    result += uleb(flags)
    if not flags & FLAG_STRIPPED:
        result += uleb(len(name)) + name
    for payload in prototypes:
        result += uleb(len(payload)) + payload
    result += b"\0"
    return bytes(result)


class LuaJITReaderTest(unittest.TestCase):
    def test_official_version_two_opcode_profiles_are_explicit(self):
        self.assertEqual(len(LEGACY_OPCODE_SPECS), 97)
        self.assertEqual(len(BITOP_OPCODE_SPECS), 104)
        self.assertEqual(opcode_number("GGET"), 54)
        self.assertEqual(opcode_number("FNEW"), 51)
        self.assertEqual(opcode_number("FUNCF"), 89)
        self.assertEqual(opcode_number("BNOT", bitop=True), 89)

    def test_preserves_bytes_and_resolves_direct_host_global(self):
        source = dump(
            prototype(
                instructions=(
                    instruction("GGET", a=0, d=0),
                    instruction("RET0", a=0, d=1),
                ),
                gc_constants=(uleb(KGC_STRING + 5) + b"print",),
            )
        )
        parsed = parse_dump(source)
        self.assertEqual(parsed.raw, source)
        self.assertEqual(parsed.sha256, hashlib.sha256(source).hexdigest())
        self.assertEqual(parsed.version, 2)
        self.assertEqual(parsed.opcode_profile, "luajit-2.1-legacy")
        self.assertEqual(parsed.root_prototype_id, 0)
        root = parsed.prototypes[0]
        self.assertEqual(root.raw_payload, source[root.payload_offset:root.payload_offset + root.payload_size])
        self.assertEqual([item.opcode_name for item in root.instructions], ["GGET", "RET0"])
        self.assertEqual(root.instructions[0].raw_word, opcode_number("GGET"))
        self.assertEqual(root.gc_constant_for_operand(0).value, b"print")
        self.assertEqual(
            [(item.name, item.access, item.pc) for item in parsed.host_api_references],
            [(b"print", "read", 1)],
        )
        inventory = parsed.inventory(include_raw=True)
        self.assertEqual(inventory["prototype_count"], 1)
        self.assertIn("raw_base64", inventory)
        self.assertIn("execution and native host ABI", " ".join(parsed.unsupported_features))

    def test_nested_prototypes_consume_postorder_child_stack(self):
        child = prototype(
            instructions=(instruction("RET0", d=1),),
        )
        parent = prototype(
            flags=1,
            instructions=(
                instruction("FNEW", d=0),
                instruction("RET0", d=1),
            ),
            gc_constants=(uleb(KGC_CHILD),),
        )
        parsed = parse_dump(dump(child, parent))
        self.assertEqual(parsed.root_prototype_id, 1)
        self.assertEqual(parsed.prototypes[0].depth, 1)
        self.assertEqual(parsed.prototypes[1].depth, 2)
        self.assertEqual(parsed.prototypes[1].child_prototype_ids, (0,))
        child_constant = parsed.prototypes[1].gc_constant_for_operand(0)
        self.assertEqual(child_constant.kind, "child")
        self.assertEqual(child_constant.child_prototype_id, 0)

    def test_numeric_and_template_table_constants_preserve_words_and_values(self):
        table = (
            uleb(1)
            + uleb(2)
            + uleb(1)
            + uleb(3)
            + uleb(0xFFFFFFFF)
            + uleb(8)
            + b"abc"
            + uleb(8)
            + b"key"
            + uleb(4)
            + uleb(0)
            + uleb(0x40040000)
        )
        source = dump(
            prototype(
                instructions=(instruction("RET0", d=1),),
                gc_constants=(table,),
                numeric_constants=(
                    uleb(14),
                    uleb(1) + uleb(0x3FF80000),
                ),
            )
        )
        root = parse_dump(source).prototypes[0]
        self.assertEqual(
            [(constant.kind, constant.value) for constant in root.numeric_constants],
            [("integer", 7), ("number", 1.5)],
        )
        template = root.gc_constants[0].value
        self.assertEqual(template.array[0].value, -1)
        self.assertEqual(template.array[1].value, b"abc")
        self.assertEqual(template.hash_entries[0][0].value, b"key")
        self.assertEqual(template.hash_entries[0][1].value, 2.5)

    def test_debug_lines_upvalues_and_locals_are_structured(self):
        debug = b"\x02" + b"env\0" + b"x\0" + uleb(0) + uleb(1) + b"\0"
        source = dump(
            prototype(
                upvalues=(0xC003,),
                instructions=(instruction("RET0", d=1),),
                debug=debug,
                first_line=10,
                line_span=20,
                stripped=False,
            ),
            flags=0,
            name=b"@unit.lua",
        )
        parsed = parse_dump(source)
        self.assertEqual(parsed.chunk_name, b"@unit.lua")
        root = parsed.prototypes[0]
        self.assertEqual(root.upvalues[0].source, "local")
        self.assertEqual(root.upvalues[0].source_index, 3)
        self.assertTrue(root.upvalues[0].immutable)
        self.assertEqual(root.debug.lines, (12,))
        self.assertEqual(root.debug.upvalue_names, (b"env",))
        self.assertEqual(root.debug.variables[0].name, b"x")
        self.assertEqual((root.debug.variables[0].start_pc, root.debug.variables[0].end_pc), (0, 1))

    def test_big_endian_bitop_and_ffi_words_are_not_guessed(self):
        flags = FLAG_STRIPPED | FLAG_BIG_ENDIAN | FLAG_BITOP | FLAG_FFI
        source = dump(
            prototype(
                flags=FLAG_FFI,
                instructions=(
                    instruction("BNOT", a=2, d=3, byte_order="big", bitop=True),
                    instruction("RET0", d=1, byte_order="big", bitop=True),
                ),
                gc_constants=(uleb(2) + uleb(0xFFFFFFFF) + uleb(0xFFFFFFFF),),
                byte_order="big",
            ),
            flags=flags,
        )
        parsed = parse_dump(source)
        self.assertEqual(parsed.byte_order, "big")
        self.assertEqual(parsed.opcode_profile, "luajit-2.1-bitop-2026")
        self.assertEqual(parsed.prototypes[0].instructions[0].opcode_name, "BNOT")
        self.assertEqual(parsed.prototypes[0].instructions[0].d, 3)
        self.assertEqual(parsed.prototypes[0].gc_constants[0].value, -1)
        self.assertIn("LuaJIT FFI/cdata support", parsed.host_requirements)

    def test_unknown_opcode_is_preserved_and_marked_unsupported(self):
        raw_instruction = (0xFE | (7 << 8) | (0x1234 << 16)).to_bytes(4, "little")
        parsed = parse_dump(dump(prototype(instructions=(raw_instruction,))))
        decoded = parsed.prototypes[0].instructions[0]
        self.assertEqual(decoded.raw_bytes, raw_instruction)
        self.assertEqual(decoded.raw_word, 0x123407FE)
        self.assertEqual(decoded.opcode_name, "UNKNOWN_0xFE")
        self.assertIn("unknown/private opcode semantics", parsed.unsupported_features)

    def test_malformed_lengths_stacks_roots_and_flags_are_refused(self):
        good = dump(prototype(instructions=(instruction("RET0", d=1),)))
        with self.assertRaisesRegex(LuaJITFormatError, "truncated"):
            parse_dump(good[:-2])
        with self.assertRaisesRegex(LuaJITFormatError, "stack underflow"):
            parse_dump(
                dump(
                    prototype(
                        flags=1,
                        instructions=(instruction("FNEW", d=0),),
                        gc_constants=(uleb(KGC_CHILD),),
                    )
                )
            )
        one = prototype(instructions=(instruction("RET0", d=1),))
        with self.assertRaisesRegex(LuaJITFormatError, "2 roots"):
            parse_dump(dump(one, one))
        with self.assertRaises(UnsupportedLuaJITFeature):
            parse_dump(dump(one, flags=FLAG_STRIPPED | 0x20))

    def test_instruction_prototype_and_depth_budgets_fail_closed(self):
        one = prototype(instructions=(instruction("RET0", d=1),))
        with self.assertRaisesRegex(LuaJITFormatError, "instruction budget"):
            parse_dump(dump(one), limits=ParseLimits(maximum_instructions=0))
        with self.assertRaisesRegex(LuaJITFormatError, "prototype budget"):
            parse_dump(dump(one), limits=ParseLimits(maximum_prototypes=0))
        parent = prototype(
            flags=1,
            instructions=(instruction("FNEW", d=0),),
            gc_constants=(uleb(KGC_CHILD),),
        )
        with self.assertRaisesRegex(LuaJITFormatError, "nesting budget"):
            parse_dump(dump(one, parent), limits=ParseLimits(maximum_depth=1))

    @unittest.skipUnless(
        os.environ.get("GUNION_DATAPACK"),
        "owner-supplied private input not selected",
    )
    def test_authenticated_private_sample_59667(self):
        path = Path(os.environ["GUNION_DATAPACK"])
        archive = Archive(path)
        offset = 0x00C720C9DC
        with path.open("rb") as stream:
            stream.seek(offset)
            prefix = stream.read(18)
            identity_size = int.from_bytes(prefix[4:6], "little")
            stream.seek(offset)
            header = stream.read(18 + identity_size)
        block = Archive._block(59667, offset, header)
        recovered = archive.read(block)
        self.assertEqual(
            recovered.expanded_sha256,
            "3a5919625f7b1ca7cd9b6827a29e76c765fc2269d9eb2410a12fcc0920367608",
        )
        parsed = parse_dump(recovered.data)
        self.assertEqual(parsed.flags, FLAG_STRIPPED)
        self.assertGreater(len(parsed.prototypes), 1)
        self.assertEqual(parsed.root_prototype_id, len(parsed.prototypes) - 1)
        self.assertTrue(parsed.host_api_references)


if __name__ == "__main__":
    unittest.main()
