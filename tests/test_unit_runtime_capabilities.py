import struct
import unittest
from owner.units.runtime_capabilities import directed_forms
from unit_import_support import native_dll


class RuntimeCapabilitiesTests(unittest.TestCase):
    def test_explicit_static_export_and_old_runtime_are_distinguished(self):
        self.assertEqual(64,directed_forms(native_dll(directed=True)))
        self.assertEqual(0,directed_forms(native_dll()))
        self.assertEqual(0,directed_forms(b'not a PE'))

    def test_text_marker_is_not_an_export_and_forwarders_are_not_data(self):
        raw=bytearray(native_dll(directed=True))
        struct.pack_into('<I',raw,0x98+96,0)
        self.assertEqual(0,directed_forms(bytes(raw)))
        raw=bytearray(native_dll(directed=True));struct.pack_into('<I',raw,0x240,0x1060)
        self.assertEqual(0,directed_forms(bytes(raw)))

    def test_incompatible_mutable_and_truncated_capabilities_refuse(self):
        for offset,value in ((0x178+36,0xc0000040),(0x40c,2),(0x410,65),(0x418,0),
                             (0x200+20,4097),(0x244,0xffffffff)):
            with self.subTest(offset=offset,value=value):
                raw=bytearray(native_dll(directed=True));struct.pack_into('<I',raw,offset,value)
                self.assertEqual(0,directed_forms(bytes(raw)))
        self.assertEqual(0,directed_forms(native_dll(directed=True)[:1024]))


if __name__=='__main__':unittest.main()
