import unittest

import shipyard_recovery_worker as worker


HEAD = "a" * 40
FINGERPRINT = "b" * 64


def pull(*, head=HEAD, state="open", labels=None):
    return {
        "number": 7,
        "title": "Fix the exact failure",
        "body": "Plan: private link",
        "state": state,
        "head": {"sha": head},
        "labels": [
            {"name": label}
            for label in (labels or ["shipyard:managed", "shipyard:needs-agent"])
        ],
    }


def statuses(*, epoch=19, fingerprint=FINGERPRINT, dispatch_state="pending"):
    return [
        {
            "id": 1,
            "context": worker.HANDOFF_CONTEXT,
            "state": "success",
            "created_at": "2026-08-14T12:00:00Z",
        },
        {
            "id": 2,
            "context": worker.DISPATCH_CONTEXT,
            "state": dispatch_state,
            "description": f"epoch={epoch} fingerprint={fingerprint}",
            "created_at": "2026-08-14T12:01:00Z",
        },
    ]


class RecoveryWorkerTests(unittest.TestCase):
    def test_accepts_one_exact_head_pending_assignment(self):
        got = worker.validate_assignment(
            pull(), statuses(), expected_head=HEAD, assignment_epoch=19,
            fingerprint=FINGERPRINT,
        )
        self.assertEqual(got["head"], HEAD)
        self.assertEqual(got["assignment_epoch"], 19)

    def test_refuses_changed_closed_or_unmanaged_pull(self):
        cases = (
            pull(head="c" * 40),
            pull(state="closed"),
            pull(labels=["shipyard:needs-agent"]),
            pull(labels=["shipyard:managed"]),
        )
        for value in cases:
            with self.subTest(value=value), self.assertRaises(ValueError):
                worker.validate_assignment(
                    value, statuses(), expected_head=HEAD, assignment_epoch=19,
                    fingerprint=FINGERPRINT,
                )

    def test_refuses_missing_completed_or_wrong_dispatch(self):
        for value in (
            statuses()[:1],
            statuses(dispatch_state="success"),
            statuses(epoch=20),
            statuses(fingerprint="c" * 64),
        ):
            with self.subTest(value=value), self.assertRaises(ValueError):
                worker.validate_assignment(
                    pull(), value, expected_head=HEAD, assignment_epoch=19,
                    fingerprint=FINGERPRINT,
                )

    def test_prompt_is_read_only_and_bounds_untrusted_evidence(self):
        assignment = worker.validate_assignment(
            pull(), statuses(), expected_head=HEAD, assignment_epoch=19,
            fingerprint=FINGERPRINT,
        )
        prompt = worker.render_prompt(assignment, "x" * 130_000)
        self.assertIn("Do not edit files", prompt)
        self.assertIn("untrusted context", prompt)
        self.assertLess(len(prompt), 142_000)


if __name__ == "__main__":
    unittest.main()
