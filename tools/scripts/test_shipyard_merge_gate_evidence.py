#!/usr/bin/env python3
import copy
import tempfile
import unittest
from pathlib import Path

import shipyard_merge_gate_evidence as evidence


class EvidenceTest(unittest.TestCase):
    def valid(self):
        item = {
            "schema": evidence.SCHEMA, "repository": "Generous-Corp/pulp",
            "base_sha": "a" * 40, "head_sha": "b" * 40,
            "merge_group_sha": "c" * 40, "merge_commit_sha": "d" * 40,
            "merged_at": "2026-10-16T00:00:00Z",
            "review": {"verdict": "approved", "digest": "1" * 64},
            "required_checks": [{"name": "macos", "conclusion": "success", "source_digest": "2" * 64}],
            "source_digests": ["1" * 64, "2" * 64],
        }
        item["evidence_digest"] = evidence.digest(item)
        return item

    @staticmethod
    def signed(item):
        item["evidence_digest"] = evidence.digest(
            {key: value for key, value in item.items() if key != "evidence_digest"}
        )
        return item

    def test_valid_and_digest_mutation_rejected(self):
        item = self.valid()
        self.assertEqual(evidence.validate(item), item)
        changed = copy.deepcopy(item)
        changed["head_sha"] = "e" * 40
        with self.assertRaises(evidence.EvidenceError):
            evidence.validate(changed)

    def test_missing_review_check_identity_or_source_rejected(self):
        for field in ("review", "required_checks", "source_digests"):
            item = self.valid(); del item[field]
            with self.assertRaises(evidence.EvidenceError): evidence.validate(item)
        for field in ("base_sha", "head_sha", "merge_group_sha", "merge_commit_sha"):
            item = self.valid(); item[field] = "0" * 40; item["evidence_digest"] = evidence.digest({k: v for k, v in item.items() if k != "evidence_digest"})
            with self.assertRaises(evidence.EvidenceError): evidence.validate(item)

    def test_identity_and_timestamp_validation(self):
        for repository in ("/", "a/b/c", " /x", "owner/"):
            item = self.valid(); item["repository"] = repository; self.signed(item)
            with self.assertRaisesRegex(evidence.EvidenceError, "repository"):
                evidence.validate(item)
        for timestamp in (" not a date", "2026-10-16T00:00:00+01:00", "2026-10-16"):
            item = self.valid(); item["merged_at"] = timestamp; self.signed(item)
            with self.assertRaisesRegex(evidence.EvidenceError, "merged_at"):
                evidence.validate(item)
        item = self.valid(); item["merge_commit_sha"] = item["head_sha"]; self.signed(item)
        with self.assertRaisesRegex(evidence.EvidenceError, "identities"):
            evidence.validate(item)

    def test_failed_review_check_and_unknown_fields_fail_closed(self):
        item = self.valid(); item["review"]["verdict"] = "pending"; self.signed(item)
        with self.assertRaisesRegex(evidence.EvidenceError, "verdict"):
            evidence.validate(item)
        item = self.valid(); item["required_checks"][0]["conclusion"] = "failure"; self.signed(item)
        with self.assertRaisesRegex(evidence.EvidenceError, "not successful"):
            evidence.validate(item)
        item = self.valid(); item["unexpected"] = True
        with self.assertRaisesRegex(evidence.EvidenceError, "fields"):
            evidence.validate(item)

    def test_source_digests_are_unique_and_referenced(self):
        item = self.valid(); item["source_digests"].append(item["source_digests"][0]); self.signed(item)
        with self.assertRaisesRegex(evidence.EvidenceError, "unique"):
            evidence.validate(item)
        item = self.valid(); item["source_digests"] = ["3" * 64]; self.signed(item)
        with self.assertRaisesRegex(evidence.EvidenceError, "referenced"):
            evidence.validate(item)

    def test_duplicate_json_keys_are_rejected(self):
        with tempfile.NamedTemporaryFile("w+", encoding="utf-8") as stream:
            stream.write('{"schema":"bad","schema":"still-bad"}')
            stream.flush()
            with self.assertRaisesRegex(evidence.EvidenceError, "duplicate JSON key"):
                evidence.load(Path(stream.name))

    def test_merged_at_alone_cannot_validate(self):
        item = self.valid(); item["required_checks"] = []
        with self.assertRaises(evidence.EvidenceError): evidence.validate(item)


if __name__ == "__main__":
    unittest.main()
