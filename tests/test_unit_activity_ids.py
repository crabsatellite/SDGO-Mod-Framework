import unittest
from owner.units.activity_ids import allocate_activity_id,is_supported_machine_id,machine_id_from_number


class ActivityIds(unittest.TestCase):
    def test_appends_below_pinned_and_legacy_rows(self):
        ids={101,9201,9301,990001,1990001}
        self.assertEqual(9401,allocate_activity_id(ids,listed=ids))
        self.assertEqual(9501,allocate_activity_id(ids|{9401},listed=ids|{9401}))

    def test_skips_occupied_unlisted_ids_and_rejects_pinned_number(self):
        self.assertEqual(9501,allocate_activity_id({9301,9401},listed={9301}))
        for value in (9900,19900,True,0):
            with self.subTest(value=value),self.assertRaises(ValueError):machine_id_from_number(value)
        with self.assertRaises(ValueError):allocate_activity_id({989901},listed={989901})

    def test_accepts_standard_and_preserves_legacy_read_compatibility(self):
        for value in (101,9401,989901,1990001,1990999):self.assertTrue(is_supported_machine_id(value))
        for value in (True,0,9402,990001,990101,1991000):self.assertFalse(is_supported_machine_id(value))


if __name__=='__main__':unittest.main()
