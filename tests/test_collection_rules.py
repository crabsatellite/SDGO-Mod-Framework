from pathlib import Path
import tempfile,unittest
from PIL import Image
from owner.collection_capsule import rules,art
from owner.collection_capsule.client import _make_activity_row

ROOT=Path(__file__).resolve().parents[1]

class CollectionRules(unittest.TestCase):
    def test_default_is_weekly_fifteen_and_invalid_prices_or_periods_are_rejected(self):
        v=rules.defaults();self.assertEqual((v['price_gp'],v['limit_period'],v['draw_limit']),(100000,'week',15))
        for change in ({'price_gp':0},{'price_gp':True},{'draw_limit':-1},{'limit_period':'month'},{'limit_period':'none'},{'utc_offset_minutes':900}):
            with self.subTest(change=change),self.assertRaises(ValueError):rules.validate({**v,**change})
        self.assertEqual(rules.validate({**v,'limit_period':'none','draw_limit':0})['draw_limit'],0)

    def test_client_price_and_styled_poster_follow_the_same_rules(self):
        import struct
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);source=ROOT/'mods/collection_capsule/assets/cover-base.png'
            a=art.render_cover(source,root/'default.png',rules.defaults())
            custom={**rules.defaults(),'price_gp':76543,'draw_limit':4,'limit_period':'day'}
            b=art.render_cover(source,root/'custom.png',custom)
            self.assertNotEqual(a.read_bytes(),b.read_bytes())
            with Image.open(a) as first,Image.open(b) as second:
                self.assertEqual(first.size,(361,322));self.assertEqual(first.crop((0,0,361,250)).tobytes(),second.crop((0,0,361,250)).tobytes())
            row=_make_activity_row(bytes(4096),1,990001,custom['price_gp'])
            self.assertEqual(struct.unpack_from('<I',row,15)[0],76543)

if __name__=='__main__':unittest.main()
