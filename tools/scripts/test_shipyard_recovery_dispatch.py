import unittest
from datetime import datetime, timezone

import shipyard_recovery_dispatch as dispatch


HEAD_A = "a" * 40
HEAD_B = "b" * 40
BLOCKER_FINGERPRINT = dispatch._fingerprint(
    "Generous-Corp/pulp",
    7,
    HEAD_A,
    {"action": "required_failed", "contexts": ["Linux"]},
)
NOW = datetime(2026, 8, 14, 14, 0, tzinfo=timezone.utc)


def report(head=HEAD_A, action="required_failed", error=None):
    decision = {"action": action}
    if action == "required_failed":
        decision["contexts"] = ["Linux"]
    elif action == "needs_update":
        decision["merge_state"] = "DIRTY"
    return {
        "repos": [
            {
                "repo": "Generous-Corp/pulp",
                "prs": [
                    {
                        "number": 7,
                        "head_sha": head,
                        "decision": decision,
                        "error": error,
                    }
                ],
            }
        ]
    }


def census(head, *statuses):
    return [{"head_sha": head, "statuses": list(statuses)}]


def status(state, description="", created="2026-08-14T13:30:00Z", ident=1):
    if not description:
        description = f"attempt=1 epoch=1 fingerprint={BLOCKER_FINGERPRINT}"
    return {
        "id": ident,
        "context": dispatch.CONTEXT,
        "state": state,
        "description": description,
        "created_at": created,
    }


class RecoveryDispatchTests(unittest.TestCase):
    def test_new_blocker_gets_one_exact_head_assignment(self):
        plan = dispatch.build_plan(report(), [], 31801399309)
        self.assertEqual(plan["candidate_count"], 1)
        candidate = plan["candidates"][0]
        self.assertEqual(candidate["expected_head"], HEAD_A)
        self.assertEqual(candidate["assignment_epoch"], 31801399309)
        self.assertEqual(candidate["dispatch_attempt"], 1)
        self.assertRegex(candidate["fingerprint"], r"^[0-9a-f]{64}$")

    def test_any_non_error_dispatch_status_deduplicates_repeated_ticks(self):
        for state in ("pending", "success", "failure"):
            with self.subTest(state=state):
                plan = dispatch.build_plan(
                    report(), census(HEAD_A, status(state)), 2, now=NOW
                )
                self.assertEqual(plan["candidate_count"], 0)

    def test_dispatch_transport_error_gets_one_bounded_retry(self):
        first_error = status(
            "error",
            f"dispatch_failed attempt=1 epoch=1 fingerprint={BLOCKER_FINGERPRINT}",
        )
        plan = dispatch.build_plan(report(), census(HEAD_A, first_error), 3, now=NOW)
        self.assertEqual(plan["candidates"][0]["dispatch_attempt"], 2)
        second_error = status(
            "error",
            f"dispatch_failed attempt=2 epoch=2 fingerprint={BLOCKER_FINGERPRINT}",
            "2026-08-14T12:01:00Z",
            2,
        )
        plan = dispatch.build_plan(
            report(), census(HEAD_A, first_error, second_error), 4, now=NOW
        )
        self.assertEqual(plan["candidate_count"], 0)

    def test_stale_pending_assignment_gets_one_refenced_retry(self):
        stale = status("pending", created="2026-08-14T12:00:00Z")
        plan = dispatch.build_plan(report(), census(HEAD_A, stale), 9, now=NOW)
        self.assertEqual(plan["candidate_count"], 1)
        self.assertEqual(plan["candidates"][0]["dispatch_attempt"], 2)

    def test_fresh_or_unparseable_pending_assignment_fails_closed(self):
        for created in ("2026-08-14T13:30:00Z", "not-a-time", ""):
            with self.subTest(created=created):
                plan = dispatch.build_plan(
                    report(), census(HEAD_A, status("pending", created=created)), 9, now=NOW
                )
                self.assertEqual(plan["candidate_count"], 0)

    def test_newer_epoch_wins_over_late_terminal_status_from_old_worker(self):
        newer = status(
            "pending",
            f"pending attempt=2 epoch=20 fingerprint={BLOCKER_FINGERPRINT}",
            "2026-08-14T13:00:00Z",
            2,
        )
        late_old = status(
            "error",
            f"completed attempt=1 epoch=19 fingerprint={BLOCKER_FINGERPRINT}",
            "2026-08-14T13:30:00Z",
            3,
        )
        plan = dispatch.build_plan(
            report(), census(HEAD_A, newer, late_old), 21, now=NOW
        )
        self.assertEqual(plan["candidate_count"], 0)

    def test_changed_blocker_on_same_head_gets_a_new_assignment(self):
        old = status("success")
        changed = report(action="needs_update")
        plan = dispatch.build_plan(changed, census(HEAD_A, old), 10, now=NOW)
        self.assertEqual(plan["candidate_count"], 1)
        self.assertNotEqual(plan["candidates"][0]["fingerprint"], BLOCKER_FINGERPRINT)

    def test_new_head_is_a_new_assignment(self):
        old = census(HEAD_A, status("failure"))
        plan = dispatch.build_plan(report(head=HEAD_B), old, 5)
        self.assertEqual(plan["candidate_count"], 1)
        self.assertEqual(plan["candidates"][0]["expected_head"], HEAD_B)

    def test_normal_or_steward_mutation_error_does_not_launch_model(self):
        for payload in (
            report(action="waiting_required"),
            report(error="could not write recovery status"),
        ):
            with self.subTest(payload=payload):
                self.assertEqual(dispatch.build_plan(payload, [], 6)["candidate_count"], 0)

    def test_invalid_head_fails_closed(self):
        plan = dispatch.build_plan(report(head="short"), [], 7)
        self.assertEqual(plan["candidate_count"], 0)
        self.assertEqual(plan["errors"], ["Generous-Corp/pulp has invalid recovery target"])

    def test_enveloped_shipyard_report_is_supported(self):
        plan = dispatch.build_plan({"data": report()}, [], 8)
        self.assertEqual(plan["candidate_count"], 1)


if __name__ == "__main__":
    unittest.main()
