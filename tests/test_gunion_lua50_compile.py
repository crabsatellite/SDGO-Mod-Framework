"""Tests for compile-only official Lua 5.0.3 recovery.

No test calls a compiled chunk.  The integration cases invoke only the
byte-verified luaL_loadfile/lua_dump harness.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest

from tools.gunion.lua50_compile import (
    LUA50_OPCODES,
    LUA50_SIGNATURE,
    LUA50_TEST_NUMBER,
    Lua50FormatError,
    analyze_prototype,
    compile_source,
    decode_instruction,
    discover_toolchain,
    find_global_function,
    global_function_bindings,
    minecraft_ranged_compatibility,
    parse_chunk,
)


def lua_string(value: bytes | None) -> bytes:
    if value is None:
        return struct.pack("<I", 0)
    return struct.pack("<I", len(value) + 1) + value + b"\0"


def instruction(name: str, *, a=0, b=0, c=0, bx=None, sbx=None) -> int:
    opcode = LUA50_OPCODES.index(name)
    if sbx is not None:
        bx = sbx + 0x1FFFF
    if bx is not None:
        return opcode | (a << 24) | (bx << 6)
    return opcode | (a << 24) | (b << 15) | (c << 6)


def prototype(
    *,
    source: bytes | None,
    line=0,
    nups=0,
    params=0,
    vararg=0,
    stack=2,
    lines=(),
    locals_=(),
    upvalues=(),
    constants=(),
    children=(),
    code=(),
) -> bytes:
    result = bytearray()
    result += lua_string(source)
    result += struct.pack("<iBBBB", line, nups, params, vararg, stack)
    result += struct.pack("<i", len(lines))
    result += b"".join(struct.pack("<i", value) for value in lines)
    result += struct.pack("<i", len(locals_))
    for name, start, end in locals_:
        result += lua_string(name) + struct.pack("<ii", start, end)
    result += struct.pack("<i", len(upvalues))
    result += b"".join(lua_string(value) for value in upvalues)
    result += struct.pack("<i", len(constants))
    for kind, value in constants:
        if kind == "nil":
            result += b"\0"
        elif kind == "number":
            result += b"\3" + struct.pack("<d", value)
        elif kind == "string":
            result += b"\4" + lua_string(value)
        else:
            raise AssertionError(kind)
    result += struct.pack("<i", len(children))
    result += b"".join(children)
    result += struct.pack("<i", len(code))
    result += b"".join(struct.pack("<I", value) for value in code)
    return bytes(result)


def chunk(root: bytes) -> bytes:
    return (
        LUA50_SIGNATURE
        + bytes((0x50, 1, 4, 4, 4, 6, 8, 9, 9, 8))
        + struct.pack("<d", LUA50_TEST_NUMBER)
        + root
    )


class Lua50ParserTest(unittest.TestCase):
    def test_exact_parser_preserves_children_upvalues_debug_and_raw_layout(self):
        child = prototype(
            source=None,
            line=12,
            nups=1,
            params=1,
            stack=3,
            lines=(12, 13),
            locals_=((b"actor", 0, 2),),
            upvalues=(b"captured",),
            constants=(("number", 1.5), ("string", b"Node")),
            code=(
                instruction("GETUPVAL", a=1, b=0),
                instruction("RETURN", a=1, b=2),
            ),
        )
        root = prototype(
            source=b"@scp/fixture.scp",
            constants=(("string", b"Bound"),),
            children=(child,),
            code=(
                instruction("CLOSURE", a=0, bx=0),
                instruction("MOVE", a=0, b=0),
                instruction("SETGLOBAL", a=0, bx=0),
                instruction("RETURN", a=0, b=1),
            ),
        )
        parsed = parse_chunk(chunk(root))

        self.assertEqual(parsed.header.version, 0x50)
        self.assertEqual(parsed.root.effective_source, b"@scp/fixture.scp")
        self.assertEqual(parsed.root.children[0].effective_source,
                         b"@scp/fixture.scp")
        self.assertEqual(parsed.root.children[0].upvalue_names, (b"captured",))
        self.assertEqual(parsed.root.children[0].locals[0].name, b"actor")
        self.assertEqual(parsed.root.children[0].constants[0].raw_hex,
                         struct.pack("<d", 1.5).hex())
        self.assertEqual(parsed.root.code[0].a, 0)
        self.assertEqual(parsed.root.code[0].bx, 0)
        self.assertEqual(parsed.root.code[1].b, 0)
        self.assertEqual(parsed.root.payload_sha256,
                         hashlib.sha256(root).hexdigest())
        binding = find_global_function(parsed.root, "Bound")
        self.assertEqual(binding.prototype.path, (0,))
        self.assertEqual(binding.closure_pc, 0)
        self.assertEqual(binding.setglobal_pc, 2)
        exported = parsed.exact_json()
        self.assertEqual(exported["root"]["children"][0]["upvalueCount"], 1)
        self.assertEqual(exported["root"]["code"][2]["name"], "SETGLOBAL")

    def test_instruction_decoder_uses_lua50_a_b_c_positions(self):
        raw = instruction("SELF", a=0xA5, b=0x101, c=0x155)
        decoded = decode_instruction(raw, 7)
        self.assertEqual(decoded.pc, 7)
        self.assertEqual(decoded.a, 0xA5)
        self.assertEqual(decoded.b, 0x101)
        self.assertEqual(decoded.c, 0x155)
        self.assertEqual(decoded.bx, (0x101 << 9) | 0x155)

    def test_official_loader_rejects_boolean_constant_tag(self):
        malformed = bytearray(chunk(prototype(
            source=b"@bad", constants=(("nil", None),), code=())))
        # Locate the single nil tag at the tail: child count and code count
        # follow it, so changing it to boolean exercises the official rejection.
        malformed[-9] = 1
        with self.assertRaisesRegex(Lua50FormatError, "constant tag 1"):
            parse_chunk(bytes(malformed))


class Lua50CompileOnlyIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.toolchain = discover_toolchain()

    def compile_text(self, text: str):
        directory = tempfile.TemporaryDirectory()
        root = Path(directory.name)
        source = root / "fixture.scp"
        output = root / "fixture.lua50"
        source.write_text(text, encoding="utf-8", newline="")
        parsed, receipt = compile_source(
            source, output, toolchain=self.toolchain)
        return directory, parsed, receipt

    def test_verified_harness_compiles_twice_without_executing_top_level(self):
        directory, parsed, receipt = self.compile_text(
            'error("this would fail if the source chunk were called")\n'
            'function SafeCompileOnly(actor)\n'
            '  actor:AddCurCombo()\n'
            '  return 1\n'
            'end\n'
        )
        self.addCleanup(directory.cleanup)

        self.assertEqual(receipt.output_sha256, receipt.repeat_sha256)
        self.assertEqual(receipt.compiler_binary_sha256,
                         self.toolchain.compiler_binary_sha256)
        binding = find_global_function(parsed.root, "SafeCompileOnly")
        self.assertEqual(binding.prototype.numparams, 1)
        analysis = analyze_prototype(binding.prototype)
        self.assertEqual(
            [(call["dispatch"], call["api"]) for call in analysis["calls"]],
            [("method", "AddCurCombo")],
        )
        compatibility = minecraft_ranged_compatibility(
            binding.prototype, analysis)
        self.assertTrue(
            compatibility["compatibleWithCurrentRangedImmediate"],
            json.dumps(compatibility, indent=2),
        )

    def test_upvalue_and_generic_for_remain_visible_as_protocol_gaps(self):
        directory, parsed, _ = self.compile_text(
            'local captured = 7\n'
            'function NeedsProtocolExpansion(actor)\n'
            '  for _, value in ipairs({1, 2}) do\n'
            '    actor:PrivateServerOnly(value + captured)\n'
            '  end\n'
            'end\n'
        )
        self.addCleanup(directory.cleanup)

        bindings = global_function_bindings(parsed.root)
        self.assertEqual([item.name for item in bindings],
                         ["NeedsProtocolExpansion"])
        prototype_ = bindings[0].prototype
        self.assertEqual(prototype_.nups, 1)
        self.assertEqual(prototype_.upvalue_names, (b"captured",))
        analysis = analyze_prototype(prototype_)
        compatibility = minecraft_ranged_compatibility(prototype_, analysis)
        self.assertIn("GETUPVAL", compatibility["unsupportedOpcodes"])
        self.assertTrue(
            {"TFORLOOP", "TFORPREP"} & set(
                compatibility["unsupportedOpcodes"]))
        self.assertIn("ipairs", compatibility["unsupportedGlobalCalls"])
        self.assertIn(
            "PrivateServerOnly", compatibility["unsupportedMethodCalls"])
        self.assertFalse(
            compatibility["compatibleWithCurrentRangedImmediate"])

    def test_method_name_support_does_not_hide_receiver_type_gap(self):
        directory, parsed, _ = self.compile_text(
            'function ReceiverGap(actor)\n'
            '  local camera = GetCamera()\n'
            '  camera:SetCameraNormal(0.25)\n'
            'end\n'
        )
        self.addCleanup(directory.cleanup)

        prototype_ = find_global_function(
            parsed.root, "ReceiverGap").prototype
        analysis = analyze_prototype(prototype_)
        compatibility = minecraft_ranged_compatibility(prototype_, analysis)
        self.assertIn("SetCameraNormal",
                      compatibility["unsupportedMethodCalls"])
        self.assertFalse(
            compatibility["compatibleWithCurrentRangedImmediate"])


if __name__ == "__main__":
    unittest.main()
