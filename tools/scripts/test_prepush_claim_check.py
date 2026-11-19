#!/usr/bin/env python3
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent
TOOL = ROOT / "prepush_claim_check.py"


class ClaimCheckTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.stub = self.dir / "gh"
        self.stub.write_text("#!/bin/sh\ncat \"$PULP_CLAIM_JSON\"\n")
        self.stub.chmod(0o755)

    def tearDown(self):
        self.tmp.cleanup()

    def run_check(self, comments, *, own=None, pr=True):
        payload = {"data": {"repository": {"pullRequests": {"nodes": [] if not pr else [{"number": 42, "comments": {"nodes": [{"body": body} for body in comments]}}]}}}}
        data = self.dir / "response.json"
        data.write_text(json.dumps(payload))
        env = os.environ.copy()
        env.update(PULP_CLAIM_CHECK_GH=str(self.stub), PULP_CLAIM_JSON=str(data))
        command = ["python3", str(TOOL), "--repo", "OWNER/REPO", "--branch", "feature/test"]
        if own:
            command += ["--own-claim", own]
        return subprocess.run(command, env=env, text=True, capture_output=True)

    def test_live_foreign_claim_refuses(self):
        result = self.run_check(["repair-claim: lane-a until 2099-01-02T03:04Z"])
        self.assertEqual(result.returncode, 1)
        self.assertIn("live repair-claim lane-a", result.stderr)

    def test_own_claim_passes(self):
        self.assertEqual(self.run_check(["repair-claim: lane-a until 2099-01-02T03:04Z"], own="lane-a").returncode, 0)

    def test_expired_claim_passes(self):
        self.assertEqual(self.run_check(["repair-claim: lane-a until 2000-01-02T03:04Z"]).returncode, 0)

    def test_id_release_passes(self):
        self.assertEqual(self.run_check(["repair-claim: OWNER/REPO until 2099-01-02T03:04Z", "repair-claim-released: OWNER/REPO"]).returncode, 0)

    def test_bare_release_passes(self):
        self.assertEqual(self.run_check(["repair-claim: lane-a until 2099-01-02T03:04Z", "repair-claim-released:"]).returncode, 0)

    def test_read_failure_passes_with_warning(self):
        bad = self.dir / "bad"
        bad.write_text("#!/bin/sh\necho unavailable >&2\nexit 7\n")
        bad.chmod(0o755)
        env = os.environ.copy()
        env["PULP_CLAIM_CHECK_GH"] = str(bad)
        result = subprocess.run(["python3", str(TOOL), "--repo", "OWNER/REPO", "--branch", "feature/test"], env=env, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0)
        self.assertIn("claim check could not read", result.stderr)

    def test_unreadable_claim_time_counts_as_live(self):
        result = self.run_check(["repair-claim: lane-a until 2026-13-45T03:04Z"])
        self.assertEqual(result.returncode, 1)

    def test_hook_checks_claims_on_every_allowed_push_path(self):
        hook = (ROOT.parent.parent / ".githooks" / "pre-push").read_text()
        skip = hook.index('prepush_receipt_note skipped all-gates "PULP_SKIP_PREPUSH=1"')
        self.assertIn("prepush_claim_check_final", hook[skip:hook.index("exit 0", skip)])
        clean = hook.index('echo "[pre-push] gates clean." >&2')
        self.assertIn("prepush_claim_check_final", hook[hook.rindex("if [ \"$fail\" = \"0\" ]", 0, clean):clean])
        demoted = hook.index("gate failures present but demoted by env override")
        self.assertIn("prepush_claim_check_final", hook[demoted:hook.index("exit 0", demoted)])

    def test_no_pr_passes(self):
        self.assertEqual(self.run_check([], pr=False).returncode, 0)


if __name__ == "__main__":
    unittest.main()
