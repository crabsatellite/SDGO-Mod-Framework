import struct
import unittest
from tools.gunion.native_alias_carriers import obs_carriers,rewrite_prefixes


def wide(value,size):
    data=(value+'\0').encode('utf-16-le')
    return data+b'\0'*(size-len(data))


def fixture():
    raw=bytearray(struct.pack('<I',0xC6E9)+b'prefix bytes retained________')
    offset=len(raw)
    raw+=struct.pack('<i',1)+wide('Txrs\\old.txr',64)+bytes(3*64+68)+struct.pack('<i',2)
    start=len(raw);raw+=bytes(4)+wide('Global_Script',128)+b'\1'
    raw+=struct.pack('<iI',1,0x20003)+wide('Fxrs\\old.zfx',64)
    raw+=struct.pack('<iii',1,-1,1)+b'\xa5'*172
    struct.pack_into('<i',raw,start,len(raw))
    start=len(raw);raw+=bytes(4)+wide('range_fire',128)+wide('anrs\\old.ani',128)+b'\0'
    struct.pack_into('<i',raw,start,len(raw))
    return bytes(raw),offset


class NativeObsCarrierTest(unittest.TestCase):
    def test_fixed_fields_round_trip_and_all_absolute_skips_and_events_stay_identical(self):
        raw,start=fixture();carriers,proof=obs_carriers(raw,start)
        self.assertEqual(3,len(carriers));self.assertEqual(len(raw),proof['parsedEnd'])
        result,spans=rewrite_prefixes(raw,carriers,{'txrs/old.txr':'txrs/scoped.txr','fxrs/old.zfx':'fxrs/scoped.zfx','anrs/old.ani':'anrs/scoped.ani'})
        self.assertEqual(len(raw),len(result));self.assertEqual(3,len(spans))
        updated,new_proof=obs_carriers(result,start)
        self.assertEqual(proof,new_proof)
        self.assertEqual({'txrs/scoped.txr','fxrs/scoped.zfx','anrs/scoped.ani'},{c.path for c in updated})
        self.assertEqual(raw[:start],result[:start])

    def test_truncation_and_corrupt_absolute_skip_refuse(self):
        raw,start=fixture()
        with self.assertRaises(ValueError):obs_carriers(raw[:-1],start)
        carriers,proof=obs_carriers(raw,start);bad=bytearray(raw)
        struct.pack_into('<i',bad,proof['absoluteSkipFields'][0]['offset'],len(raw)+1)
        with self.assertRaisesRegex(ValueError,'skip outside'):obs_carriers(bytes(bad),start)
