#!/usr/bin/env python3
import copy
import unittest

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

    def test_failed_review_check_and_unknown_fields_fail_closed(self):
        item = self.valid(); item["review"]["verdict"] = "pending"
        with self.assertRaises(evidence.EvidenceError): evidence.validate(item)
        item = self.valid(); item["required_checks"][0]["conclusion"] = "failure"
        with self.assertRaises(evidence.EvidenceError): evidence.validate(item)
        item = self.valid(); item["unexpected"] = True
        with self.assertRaises(evidence.EvidenceError): evidence.validate(item)

    def test_merged_at_alone_cannot_validate(self):
        item = self.valid(); item["required_checks"] = []
        with self.assertRaises(evidence.EvidenceError): evidence.validate(item)


if __name__ == "__main__":
    unittest.main()
