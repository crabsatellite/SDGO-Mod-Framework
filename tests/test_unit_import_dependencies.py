import unittest

from owner.units import dependencies


HASH_A = "a" * 64
HASH_B = "b" * 64


def manifest():
    return {
        "schema": dependencies.SCHEMA,
        "owner": "example-owner",
        "records": [
            {
                "skill_id": 71001,
                "source_row": 17,
                "profile_id": "native-type13-1132-v1",
                "source_sha256": HASH_A,
                "target_sha256": HASH_B,
                "dispatch": 2,
                "dependency_ids": ["effect:71001", "server:71001"],
                "blockers": ["server-only fields remain unknown"],
            }
        ],
        "edges": [
            {
                "id": "effect:71001",
                "owner": "example-owner",
                "kind": "effect-zfx-texture",
                "reference": "zfxs/private-effect.zfx",
                "state": "resolved",
                "required": True,
                "sha256": HASH_A,
                "proof_sha256": HASH_B,
            },
            {
                "id": "server:71001",
                "owner": "example-owner",
                "kind": "server-skill-fields",
                "reference": "skill:71001",
                "state": "unknown",
                "required": True,
            },
        ],
    }


class UnitImportDependenciesTests(unittest.TestCase):
    def test_resolved_skill_parameters_need_the_exact_record_hash(self):
        value=manifest();edge={'id':'parameters:71001','owner':'example-owner','kind':'skill-parameter-record',
            'reference':'kind79:8000789','state':'resolved','required':True,'proof_sha256':HASH_B}
        value['edges'].append(edge);value['records'][0]['dependency_ids'].append(edge['id'])
        with self.assertRaisesRegex(ValueError,'record SHA-256'):dependencies.validate_manifest(value)
        edge['sha256']=HASH_A
        result=dependencies.validate_manifest(value)
        self.assertTrue(result['admission']['native_registration_blocked'])

    def test_unknown_required_dependency_is_preserved_as_blocker(self):
        result = dependencies.validate_manifest(manifest())

        self.assertEqual(result["admission"]["record_count"], 1)
        self.assertEqual(result["admission"]["file_count"], 1)
        self.assertFalse(
            result["admission"]["required_dependencies_resolved"]
        )
        self.assertTrue(result["admission"]["native_registration_blocked"])
        self.assertIn(
            "dependency:server:71001:unknown",
            result["admission"]["blockers"],
        )
        self.assertIn(
            "skill:71001:server-only fields remain unknown",
            result["admission"]["blockers"],
        )

    def test_cross_owner_and_unknown_edge_references_are_rejected(self):
        value = manifest()
        value["edges"][0]["owner"] = "another-owner"
        with self.assertRaisesRegex(ValueError, "crosses owners"):
            dependencies.validate_manifest(value)

        value = manifest()
        value["records"][0]["dependency_ids"].append("missing:edge")
        with self.assertRaisesRegex(ValueError, "unknown dependencies"):
            dependencies.validate_manifest(value)

    def test_resolved_file_dependencies_require_artifact_and_proof_hashes(self):
        value = manifest()
        del value["edges"][0]["sha256"]
        with self.assertRaisesRegex(ValueError, "needs SHA-256"):
            dependencies.validate_manifest(value)

        value = manifest()
        del value["edges"][0]["proof_sha256"]
        with self.assertRaisesRegex(ValueError, "needs proof"):
            dependencies.validate_manifest(value)

    def test_file_budget_fails_closed(self):
        value = manifest()
        value["records"][0]["dependency_ids"] = []
        value["edges"] = [
            {
                "id": f"effect:{index}",
                "owner": "example-owner",
                "kind": "effect-zfx-texture",
                "reference": f"zfxs/effect-{index}.zfx",
                "state": "missing",
                "required": False,
            }
            for index in range(dependencies.MAX_FILES + 1)
        ]
        with self.assertRaisesRegex(ValueError, "exceeds 512 files"):
            dependencies.validate_manifest(value)


if __name__ == "__main__":
    unittest.main()
