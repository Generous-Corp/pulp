import unittest

import shipyard_recovery_worker_select as selector


def runner(worker, *, ident, status="online", busy=False, extra_labels=()):
    labels = [*selector.BASE_LABELS, f"shipyard-recovery-{worker}", *extra_labels]
    return {
        "id": ident,
        "name": f"recovery-{worker}-{ident}",
        "status": status,
        "busy": busy,
        "labels": [{"name": label} for label in labels],
    }


class RecoveryWorkerSelectTests(unittest.TestCase):
    def test_preference_is_m3_then_m5_then_m1(self):
        result = selector.select_worker(
            {"runners": [runner("m1", ident=1), runner("m5", ident=5), runner("m3", ident=3)]}
        )
        self.assertEqual(result["selected"]["worker"], "m3")
        self.assertEqual(result["eligible_workers"], ["m3", "m5", "m1"])

    def test_m3_offline_selects_m5_and_restoration_changes_only_next_selection(self):
        unavailable = {"runners": [runner("m3", ident=3, status="offline"), runner("m5", ident=5)]}
        first = selector.select_worker(unavailable)
        self.assertEqual(first["selected"]["worker"], "m5")
        restored = {"runners": [runner("m3", ident=3), runner("m5", ident=5)]}
        second = selector.select_worker(restored)
        self.assertEqual(second["selected"]["worker"], "m3")
        self.assertEqual(first["selected"]["runner_id"], 5)

    def test_busy_preferred_worker_falls_back(self):
        result = selector.select_worker(
            {"runners": [runner("m3", ident=3, busy=True), runner("m5", ident=5)]}
        )
        self.assertEqual(result["selected"]["worker"], "m5")

    def test_ordinary_ci_runner_is_never_implicitly_enrolled(self):
        ordinary = {
            "id": 9,
            "name": "pulp-vm-ordinary",
            "status": "online",
            "busy": False,
            "labels": [{"name": label} for label in selector.BASE_LABELS],
        }
        result = selector.select_worker({"runners": [ordinary]})
        self.assertIsNone(result["selected"])
        self.assertEqual(result["eligible_workers"], [])

    def test_missing_base_label_is_not_eligible(self):
        value = runner("m3", ident=3)
        value["labels"] = [label for label in value["labels"] if label["name"] != "pulp-build-vm"]
        self.assertIsNone(selector.select_worker({"runners": [value]})["selected"])

    def test_duplicate_machine_enrollment_fails_closed(self):
        result = selector.select_worker(
            {"runners": [runner("m3", ident=3), runner("m3", ident=4)]}
        )
        self.assertIsNone(result["selected"])
        self.assertEqual(
            result["errors"],
            ["multiple eligible runners advertise shipyard-recovery-m3"],
        )

    def test_one_runner_cannot_advertise_multiple_worker_identities(self):
        value = runner(
            "m3",
            ident=3,
            extra_labels=("shipyard-recovery-m5",),
        )
        result = selector.select_worker({"runners": [value]})
        self.assertIsNone(result["selected"])
        self.assertEqual(
            result["errors"],
            ["runner recovery-m3-3 advertises multiple recovery workers"],
        )

    def test_string_labels_are_supported_for_fixtures(self):
        value = runner("m1", ident=1)
        value["labels"] = [label["name"] for label in value["labels"]]
        self.assertEqual(selector.select_worker({"runners": [value]})["selected"]["worker"], "m1")

    def test_malformed_census_is_rejected(self):
        for value in (None, [], {}, {"runners": None}):
            with self.subTest(value=value), self.assertRaises(ValueError):
                selector.select_worker(value)


if __name__ == "__main__":
    unittest.main()
