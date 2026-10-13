#!/usr/bin/env python3
"""Tests for release_failure_diagnosis.py and its use by release_reconcile.py.

The fixture is the real v0.952.0-v0.955.1 outage, recorded from the GitHub API:
a skia-builder release asset was re-uploaded with different bytes, every
Release CLI run then failed `CLI windows-arm64 › Fetch Skia binaries` on a
sha256 mismatch, and the reconciler's re-dispatches (from `main`) failed only
the dispatch-ref guard — which is what the incident then reported, so the real
cause was never named. v0.951.0, published normally, is the negative control.
"""

from __future__ import annotations

import contextlib
import io
import json
import sys
import unittest
from datetime import timedelta
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import release_failure_diagnosis as rfd  # noqa: E402
import release_reconcile as rr  # noqa: E402
from test_release_reconcile import NOW, sweep  # noqa: E402

FIXTURE = json.loads(
    (HERE / "fixtures" / "release_failure" / "v0955_1_outage.json").read_text(encoding="utf-8")
)
LOG = FIXTURE["push_failed_job_log"]
PINNED = "8e6fb2012ad026d8261eadfe3a9f638a165e82cefa2b896f8596dbaece9c6bde"
OBSERVED = "18e57481e84992fecccbc4ac0f8f283c4a68d90d7978a61ce2e68f21dd34a9c7"
URL = ("https://github.com/danielraffel/skia-builder/releases/download/"
       "chrome/m153/skia-build-win-arm64-gpu-release.zip")
UPSTREAM = FIXTURE["skia_m153_win_arm64_asset"]["digest"].removeprefix("sha256:")

# The pin as tools/deps/manifest.json carried it during the outage. Ten filler
# lines put the sha256 on line 14, so the reported line number is checked.
MANIFEST = "\n" * 10 + f'''        "release_assets": {{
          "win-arm64": {{
            "url": "{URL}",
            "sha256": "{PINNED}"
          }},
'''


def failed_job(jobs: list[dict]) -> dict:
    return next(j for j in jobs if j["conclusion"] == "failure")


class FirstErrorLine(unittest.TestCase):
    def test_real_log_yields_the_mismatch_not_the_exit_code(self) -> None:
        self.assertEqual(rfd.first_error_line(LOG), "ERROR: sha256 mismatch")

    def test_generic_exit_code_is_only_a_fallback(self) -> None:
        log = "2026-10-12T06:56:17.7Z ##[error]Process completed with exit code 1.\n"
        self.assertEqual(
            rfd.first_error_line(log), "##[error]Process completed with exit code 1."
        )

    def test_echoed_error_annotations_are_commands_not_errors(self) -> None:
        log = 'echo "::error::would fail"\nfatal: real problem error: here\n'
        self.assertEqual(rfd.first_error_line(log), "fatal: real problem error: here")


class DigestMismatch(unittest.TestCase):
    def diag(self, **overrides) -> rfd.Diagnosis:
        job = failed_job(FIXTURE["push_jobs"])
        kwargs = dict(
            workflow="Release CLI", tag="v0.955.1", run_url=job["html_url"],
            job=job["name"], step="Fetch Skia binaries", log_text=LOG,
            manifest_text=MANIFEST, upstream_digest=UPSTREAM,
        )
        kwargs.update(overrides)
        return rfd.diagnose(**kwargs)

    def test_real_failure_is_classified_with_pin_line_and_both_digests(self) -> None:
        d = self.diag()
        self.assertEqual(d.cause, rfd.DIGEST_MISMATCH)
        self.assertEqual(d.details["pinned"], PINNED)
        self.assertEqual(d.details["observed"], OBSERVED)
        self.assertEqual(d.details["pin_line"], 14)
        self.assertEqual(d.details["pin_key"], "win-arm64")
        text = "\n".join(d.render())
        for needle in (
            "`CLI windows-arm64` › `Fetch Skia binaries`",
            "`ERROR: sha256 mismatch`",
            "pinned-asset digest mismatch",
            f"`tools/deps/manifest.json` line 14 (release_assets `win-arm64`): `{PINNED}` → `{OBSERVED}`",
            "gh api repos/danielraffel/skia-builder/releases/tags/chrome/m153",
            "msvc-toolchain.json",
            "next SDK tag's `Release CLI` run",
            FIXTURE["push_run"]["html_url"],
        ):
            with self.subTest(needle=needle):
                self.assertIn(needle, text)

    def test_upstream_confirmation_makes_it_tag_immutable(self) -> None:
        self.assertEqual(UPSTREAM, OBSERVED, "fixture: upstream now carries the new bytes")
        self.assertTrue(self.diag().tag_immutable)

    def test_unconfirmed_mismatch_stays_retryable(self) -> None:
        d = self.diag(upstream_digest="0" * 64)
        self.assertFalse(d.tag_immutable)
        self.assertIn("NOT confirmed", "\n".join(d.render()))
        self.assertFalse(self.diag(upstream_digest=None).tag_immutable)

    def test_main_that_already_repinned_says_cut_a_new_tag(self) -> None:
        d = self.diag(manifest_text=MANIFEST.replace(PINNED, OBSERVED))
        self.assertIn("already pins the new digest", "\n".join(d.render()))


class RootCauseIsNotTheBrokenRedispatch(unittest.TestCase):
    """The incident named the dispatch guard five times and the mismatch never."""

    def cache(self) -> dict[int, list[dict]]:
        return {
            FIXTURE["push_run"]["id"]: FIXTURE["push_jobs"],
            FIXTURE["guard_run"]["id"]: FIXTURE["guard_jobs"],
        }

    def test_guard_failure_on_a_newer_redispatch_does_not_mask_the_push_failure(self) -> None:
        runs = [FIXTURE["push_run"], FIXTURE["guard_run"]]
        self.assertGreater(runs[1]["created_at"], runs[0]["created_at"])
        run, _ = rr.root_cause_run(runs, self.cache())
        self.assertEqual(run["id"], FIXTURE["push_run"]["id"])
        self.assertEqual(
            rr.newest_failed_legs(runs, self.cache()),
            ("CLI windows-arm64 › Fetch Skia binaries",),
        )

    def test_guard_only_history_is_diagnosed_as_a_wrong_ref_dispatch(self) -> None:
        runs = [FIXTURE["guard_run"]]
        run, jobs = rr.root_cause_run(runs, self.cache())
        diags = rr.diagnose_run("Release CLI", "v0.955.1", run, jobs, lambda _id: "")
        self.assertEqual([d.cause for d in diags], [rfd.DISPATCH_REF])
        self.assertIn("--ref v0.955.1", "\n".join(diags[0].render()))

    def test_redispatch_targets_the_tag_not_main(self) -> None:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rr.redispatch("example/repo", "v0.955.1", dry_run=True)
        self.assertIn("--ref v0.955.1", out.getvalue())
        self.assertNotIn("--ref main", out.getvalue())


def outage_states() -> list[rr.TagState]:
    runs = [FIXTURE["push_run"], FIXTURE["guard_run"]]
    cache = {
        FIXTURE["push_run"]["id"]: FIXTURE["push_jobs"],
        FIXTURE["guard_run"]["id"]: FIXTURE["guard_jobs"],
    }
    push_job_id = failed_job(FIXTURE["push_jobs"])["id"]
    run, jobs = rr.root_cause_run(runs, cache)
    diags = rr.diagnose_run(
        "Release CLI", "v0.955.1", run, jobs,
        lambda job_id: LOG if job_id == push_job_id else "",
        manifest_text=MANIFEST, upstream_digest=lambda url: UPSTREAM if url == URL else None,
    )
    broken = rr.TagState(
        tag="v0.955.1", created_at=NOW - timedelta(hours=7), published=False,
        has_release_object=False, assets=frozenset(), run_states=("completed", "completed"),
        validation_failures=tuple(f"{d.job}: {d.cause}" for d in diags if d.tag_immutable),
        dispatch_attempts=1, failed_legs=rr.newest_failed_legs(runs, cache), diagnoses=diags,
    )
    rel = FIXTURE["release_v0_951_0"]
    healthy = rr.TagState(
        tag=rel["tag_name"], created_at=NOW - timedelta(hours=16), published=not rel["draft"],
        has_release_object=True, assets=frozenset(a["name"] for a in rel["assets"]),
        run_states=("completed",), validation_failures=(), dispatch_attempts=0,
    )
    return [broken, healthy]


class SweepOverTheRealOutage(unittest.TestCase):
    def test_v0_955_1_is_reported_actionably_and_v0_951_0_is_not(self) -> None:
        report, rebuilt = sweep(outage_states())
        self.assertEqual(sorted(report), ["v0.955.1"], "v0.951.0 is the negative control")
        self.assertEqual(rebuilt, [], "an upstream-confirmed mismatch must not be rebuilt")
        why = report["v0.955.1"]
        for needle in ("CLI windows-arm64 › Fetch Skia binaries", "ERROR: sha256 mismatch",
                       "line 14", PINNED, OBSERVED, "Verify"):
            with self.subTest(needle=needle):
                self.assertIn(needle, why)

    def test_sign_and_release_failure_on_a_published_tag_is_still_reported(self) -> None:
        _, healthy = outage_states()
        sign = rfd.diagnose(workflow="Sign and Release", tag=healthy.tag, run_url="u",
                            job="sign", step="Notarize", log_text="ERROR: notary rejected\n")
        from dataclasses import replace
        report, _ = sweep([replace(healthy, sign_failures=(sign,))])
        self.assertIn("Sign and Release", report[healthy.tag])
        self.assertIn("ERROR: notary rejected", report[healthy.tag])


class SignAndReleaseRuns(unittest.TestCase):
    def test_successful_or_live_runs_are_not_failures(self) -> None:
        ok = {"id": 1, "head_branch": "v0.951.0", "status": "completed",
              "conclusion": "success", "created_at": "2026-10-11T21:57:29Z"}
        live = dict(ok, id=2, status="in_progress", conclusion=None,
                    created_at="2026-10-11T22:00:00Z")
        self.assertEqual(rr.sign_and_release_failures("r", "v0.951.0", [ok], lambda _: ""), ())
        self.assertEqual(rr.sign_and_release_failures("r", "v0.951.0", [ok, live], lambda _: ""), ())
        self.assertEqual(rr.sign_and_release_failures("r", "v0.955.1", [ok], lambda _: ""), ())


class IncidentNotifies(unittest.TestCase):
    def test_only_tags_new_to_the_open_incident_trigger_a_comment(self) -> None:
        previous = "intro\n\n- `v0.952.0` — old\n  - **Workflow:** x\n"
        report = [("v0.952.0", "still"), ("v0.955.1", "new")]
        self.assertEqual(rr.newly_reported(previous, report), ["v0.955.1"])
        self.assertEqual(rr.newly_reported("", report), ["v0.952.0", "v0.955.1"])


if __name__ == "__main__":
    unittest.main()
