import unittest

import shipyard_recovery_repair as repair


class RecoveryRepairTests(unittest.TestCase):
    def test_prompt_requires_authorized_triage_and_is_bounded(self):
        assignment = {
            "number": 7,
            "title": "Fix exact failure",
            "body": "Plan: private link",
            "head": "a" * 40,
            "assignment_epoch": 19,
            "fingerprint": "b" * 64,
        }
        triage = {
            "classification": "needs_sol_fix",
            "summary": "A source change is required",
            "evidence": ["focused failure"],
            "next_action": "repair it",
        }
        prompt = repair.render_prompt(assignment, triage, "x" * 130_000)
        self.assertIn("single bounded implementation stage", prompt)
        self.assertIn("Do not commit, push, merge", prompt)
        self.assertLess(len(prompt), 144_000)
        triage["classification"] = "needs_human"
        with self.assertRaises(ValueError):
            repair.render_prompt(assignment, triage, "evidence")

    def test_changed_paths_are_bounded_and_protect_control_plane(self):
        self.assertEqual(
            repair.validate_changed_paths(["src/fix.cpp", "./test/fix.cpp"]),
            ["src/fix.cpp", "test/fix.cpp"],
        )
        for paths in (
            [],
            ["../escape"],
            [".github/workflows/shipyard-recovery-worker.yml"],
            ["tools/scripts/shipyard_recovery_worker.py"],
            [f"file-{index}" for index in range(repair.MAX_CHANGED_FILES + 1)],
        ):
            with self.subTest(paths=paths), self.assertRaises(ValueError):
                repair.validate_changed_paths(paths)


if __name__ == "__main__":
    unittest.main()
