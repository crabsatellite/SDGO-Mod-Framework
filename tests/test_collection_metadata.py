import unittest
import hashlib
import json
import tempfile
from pathlib import Path
from owner.collection_capsule.metadata import imported_roots, registered_imports, validate_source
from owner.collection_capsule.pool import build_pool


def unit(uid, target=0):
    return dict(UnitNumber=uid, UnitNumber_Transform=target, SallyReady=1,
                UnitGradeNumber=4, UnitGradeNumber2=4, UnitGraph=1000000+uid,
                UnitModelName="Imported unit")


def family(root="base"):
    return {"protocol": "sdgo.unit-family", "protocol_version": 1,
            "installable": True, "package_id": "fixture.import",
            "family": {"root_form": root, "forms": [
                {"id": 90002, "key": "base", "transform_target": 90001},
                {"id": 90001, "key": "alternate", "transform_target": 90002}]}}


class MetadataTests(unittest.TestCase):
    def setUp(self):
        self.rows = [unit(90002, 90001), unit(90001, 90002)]

    def test_import_metadata_registers_larger_id_as_root_without_code_changes(self):
        roots = imported_roots(self.rows, [family()])
        result = build_pool(self.rows, verified_bases=roots)
        self.assertEqual([row["unit_id"] for row in result["units"]], [90002])

    def test_incomplete_authoring_draft_cannot_be_used_as_installed_metadata(self):
        value = family()
        value["installable"] = False
        with self.assertRaises(ValueError):
            imported_roots(self.rows, [value])

    def test_missing_root_fails_instead_of_choosing_minimum_id(self):
        with self.assertRaises(ValueError):
            imported_roots(self.rows, [family(None)])

    def test_live_transform_mismatch_rejects_stale_family_metadata(self):
        self.rows[0]["UnitNumber_Transform"] = 0
        with self.assertRaises(ValueError):
            imported_roots(self.rows, [family()])

    def test_missing_family_member_is_not_silently_ignored(self):
        value = family()
        value["family"]["forms"].pop()
        with self.assertRaises(ValueError):
            imported_roots(self.rows, [value])

    def test_entry_variant_may_transform_into_a_form_registered_by_another_package(self):
        entry = {"protocol": "sdgo.unit-family", "protocol_version": 1, "installable": True, "package_id": "fixture.entry",
                 "family": {"root_form": "entry", "forms": [{"id": 90003, "key": "entry", "transform_target": 90001}]}}
        rows = self.rows + [unit(90003, 90001)]
        roots = imported_roots(rows, [entry], external_forms=frozenset({90001}))
        self.assertEqual([90003], list(roots))
        # The same reference is refused when the pair form is not registered elsewhere.
        with self.assertRaises(ValueError):
            imported_roots(rows, [entry])

    def test_published_entry_variant_can_reference_another_verified_import_family(self):
        rows = self.rows + [unit(90003, 90001)]
        registry = [dict(UnitID=row["UnitNumber"], RootUnitID=90003 if row["UnitNumber"] == 90003 else 90002,
                         PackageID="fixture.entry" if row["UnitNumber"] == 90003 else "fixture.base", RevisionHash="a"*64,
                         TransformTo=row["UnitNumber_Transform"], Ready=1, UnitRank=4, UnitSubgrade=4, UnitGraph=row["UnitGraph"])
                    for row in rows]
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);path=root/"GOnlineGame/collection.imported-forms.json";path.parent.mkdir()
            def save(entries):
                raw=json.dumps(entries).encode();path.write_bytes(raw)
                (root/"_meta.json").write_text(json.dumps({"files":{"GOnlineGame/collection.imported-forms.json":{"sha256":hashlib.sha256(raw).hexdigest()}}}))
            save(registry)
            roots,claimed=registered_imports(rows,root)
            self.assertEqual(set(roots),{90002,90003})
            self.assertEqual(claimed,{90001,90002,90003})
            # A reference to an unregistered form remains incomplete.
            save([registry[0],registry[2]])
            with self.assertRaisesRegex(ValueError,"incomplete"):
                registered_imports(rows,root)

    def test_sql_source_fields_cannot_inject_statements(self):
        self.rows[0]["UnitGraph"] = "0);DROP TABLE dbo.tbl_Users;--"
        with self.assertRaises(ValueError):
            validate_source(self.rows)

    def test_boolean_and_negative_native_ids_are_rejected(self):
        for value in (True, -1):
            rows = [dict(self.rows[0], UnitNumber=value)]
            with self.assertRaises(ValueError):
                validate_source(rows)


if __name__ == "__main__":
    unittest.main()
