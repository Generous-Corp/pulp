#!/usr/bin/env python3
"""Cover the three ways this guard has given a WRONG answer in practice.

Each case below is a real false reading observed on 2026-09-23, not a
hypothetical. A guard that reassures you is worse than no guard, so every one
of these must be asserted rather than assumed.
"""
import importlib.util, pathlib, sys, unittest

MOD = pathlib.Path(__file__).resolve().parent / "queue_admission_guard.py"
spec = importlib.util.spec_from_file_location("guard", MOD)
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)


class _Fake:
    """Stands in for the API. Records what was asked so absence is provable."""

    def __init__(self, runs, jobs, steps, last_merge):
        self.runs, self.jobs, self.steps, self.last_merge = runs, jobs, steps, last_merge
        self.asked = []

    def __call__(self, path, jq=None):
        self.asked.append(path)
        if "commits?sha=main" in path:
            return self.last_merge
        if "/runs?event=merge_group" in path or "build.yml/runs" in path:
            return "\n".join(self.runs)
        if "/jobs?" in path:
            rid = path.split("/runs/")[1].split("/")[0]
            return self.jobs.get(rid, "")
        if "/actions/jobs/" in path:
            jid = path.rsplit("/", 1)[-1]
            return self.steps.get(jid, "0")
        return None


def _install(tc, runs, jobs, steps, last_merge="2026-09-23T10:00:00Z"):
    fake = _Fake(runs, jobs, steps, last_merge)
    orig = guard.gh
    guard.gh = fake
    tc.addCleanup(lambda: setattr(guard, "gh", orig))
    return fake


class AdmissionGuardTest(unittest.TestCase):
    def test_receipt_reuse_is_not_a_pass(self):
        """A 3-step macos job ran NOTHING. Counting it as green reported a
        healthy base while main was broken, for hours."""
        _install(self, ["111 success 2026-09-23T12:00:00Z"], {"111": "900"}, {"900": "3"})
        self.assertEqual(guard.main(), 1)

    def test_a_real_run_that_passed_is_a_pass(self):
        """Control: the same shape with a genuine step count must return 0,
        otherwise the test above proves nothing."""
        _install(self, ["111 success 2026-09-23T12:00:00Z"], {"111": "900"}, {"900": "41"})
        self.assertEqual(guard.main(), 0)

    def test_stale_pass_predating_the_last_merge_is_refused(self):
        """Live failure: after cancelling the day's batches the guard reached
        back twenty days, found an old success and declared the base healthy."""
        _install(self, ["111 success 2026-09-03T23:41:00Z"], {"111": "900"},
                 {"900": "41"}, last_merge="2026-09-23T10:00:00Z")
        self.assertEqual(guard.main(), 1)

    def test_a_real_failure_is_red(self):
        _install(self, ["111 failure 2026-09-23T12:00:00Z"], {"111": "900"}, {"900": "41"})
        self.assertEqual(guard.main(), 1)

    def test_no_executed_batch_at_all_is_red_not_green(self):
        """Absence of evidence must never read as evidence of health."""
        _install(self, [], {}, {})
        self.assertEqual(guard.main(), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
