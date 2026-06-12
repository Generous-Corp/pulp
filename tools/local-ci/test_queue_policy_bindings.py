#!/usr/bin/env python3
"""Tests for queue policy facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_policy_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueuePolicyBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_queue_policy_bindings_delegate_to_orchestrator(self):
        captured = {}

        def make_job(*args, **kwargs):
            captured["make_job"] = (args, kwargs)
            return {"id": "job"}

        def supersedence_result(job, superseded_by, reason, *, now_iso_fn):
            captured["supersedence_result"] = (job, superseded_by, reason, now_iso_fn)
            return {"overall": "superseded"}

        def cancellation_result(job, reason, *, now_iso_fn):
            captured["cancellation_result"] = (job, reason, now_iso_fn)
            return {"overall": "canceled"}

        def trim_removed(queue, *, keep_completed_jobs):
            captured["trim_removed"] = (queue, keep_completed_jobs)
            return queue, {"old"}

        def trim(queue, *, keep_completed_jobs):
            captured["trim"] = (queue, keep_completed_jobs)
            return queue

        orchestrator = types.SimpleNamespace(
            default_priority_for=lambda command, config: f"{command}:{config['priority']}",
            make_fingerprint=lambda branch, sha, targets, validation: "|".join([branch, sha, ",".join(targets), validation]),
            make_job=make_job,
            supersedence_result=supersedence_result,
            cancellation_result=cancellation_result,
            supersedence_key=lambda job: ("branch", ("mac",), "full"),
            supersedence_identity_key=lambda job: ("branch", "sha", "full"),
            jobs_share_supersedence_scope=lambda newer, older: newer["branch"] == older["branch"],
            job_has_narrower_same_identity_scope=lambda newer, older: newer["targets"] != older["targets"],
            supersedence_reason=lambda newer, older: "newer_sha",
            trim_completed_jobs_with_removed_ids=trim_removed,
            trim_completed_jobs=trim,
            job_sort_key=lambda job: (0, job["created_at"], job["id"]),
            queue_status_groups=lambda queue: (queue[:1], queue[1:2], queue[2:]),
            recent_completed_jobs_for_status=lambda jobs, *, limit: jobs[:limit],
            find_job_unlocked=lambda queue, job_ref, statuses=None: queue[0],
            validate_ci_branch_name=lambda branch: branch.strip(),
        )
        bindings = {
            "_queue_orchestrator": orchestrator,
            "KEEP_COMPLETED_JOBS": 7,
            "ROOT": Path("/repo"),
            "now_iso": object(),
            "uuid": types.SimpleNamespace(uuid4=lambda: types.SimpleNamespace(hex="uuidhex")),
            "validate_ci_branch_name": object(),
        }

        self.assertEqual(self.mod.default_priority_for(bindings, "ship", {"priority": "high"}), "ship:high")
        self.assertEqual(self.mod.make_fingerprint(bindings, "b", "s", ["mac"], "full"), "b|s|mac|full")
        self.assertEqual(self.mod.make_job(bindings, "b", "s", "normal", ["mac"], "run", "full"), {"id": "job"})
        self.assertIs(captured["make_job"][1]["now_iso_fn"], bindings["now_iso"])
        self.assertEqual(captured["make_job"][1]["uuid_hex_fn"](), "uuidhex")
        self.assertIs(captured["make_job"][1]["root"], bindings["ROOT"])
        self.assertIs(captured["make_job"][1]["validate_branch_fn"], bindings["validate_ci_branch_name"])
        self.assertEqual(self.mod.supersedence_result(bindings, {"id": "old"}, "new", "newer_sha"), {"overall": "superseded"})
        self.assertIs(captured["supersedence_result"][3], bindings["now_iso"])
        self.assertEqual(self.mod.cancellation_result(bindings, {"id": "old"}, "operator"), {"overall": "canceled"})
        self.assertIs(captured["cancellation_result"][2], bindings["now_iso"])
        self.assertEqual(self.mod.supersedence_key(bindings, {"id": "job"}), ("branch", ("mac",), "full"))
        self.assertEqual(self.mod.supersedence_identity_key(bindings, {"id": "job"}), ("branch", "sha", "full"))
        self.assertTrue(self.mod.jobs_share_supersedence_scope(bindings, {"branch": "b"}, {"branch": "b"}))
        self.assertTrue(self.mod.job_has_narrower_same_identity_scope(bindings, {"targets": ["mac"]}, {"targets": ["mac", "linux"]}))
        self.assertEqual(self.mod.supersedence_reason(bindings, {"id": "new"}, {"id": "old"}), "newer_sha")
        self.assertEqual(self.mod.trim_completed_jobs_with_removed_ids(bindings, [{"id": "old"}]), ([{"id": "old"}], {"old"}))
        self.assertEqual(captured["trim_removed"][1], 7)
        self.assertEqual(self.mod.trim_completed_jobs(bindings, [{"id": "old"}]), [{"id": "old"}])
        self.assertEqual(captured["trim"][1], 7)
        self.assertEqual(self.mod.job_sort_key(bindings, {"id": "job", "created_at": "now"}), (0, "now", "job"))
        self.assertEqual(self.mod.queue_status_groups(bindings, [{"id": "a"}, {"id": "b"}, {"id": "c"}]), ([{"id": "a"}], [{"id": "b"}], [{"id": "c"}]))
        self.assertEqual(self.mod.recent_completed_jobs_for_status(bindings, [{"id": "a"}, {"id": "b"}], limit=1), [{"id": "a"}])
        self.assertEqual(self.mod.find_job_unlocked(bindings, [{"id": "a"}], "a", {"pending"}), {"id": "a"})
        self.assertEqual(self.mod.validate_ci_branch_name(bindings, " branch "), "branch")


if __name__ == "__main__":
    unittest.main()
