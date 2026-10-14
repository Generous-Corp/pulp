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



class ConfirmedMismatchStopsRedispatchEndToEnd(unittest.TestCase):
    """collect() -> main(): an upstream-confirmed mismatch must not be rebuilt.

    Drives the real collect() over the recorded runs, jobs, log and upstream
    asset with only the GitHub transport stubbed, then feeds its states to the
    sweep. The tag has one prior re-dispatch, so without the tag-immutable
    circuit it would sit on the retry path and be re-dispatched.
    """

    def collect_outage(self, upstream_digest: str) -> list[rr.TagState]:
        import subprocess

        push_job_id = failed_job(FIXTURE["push_jobs"])["id"]
        jobs = {FIXTURE["push_run"]["id"]: FIXTURE["push_jobs"],
                FIXTURE["guard_run"]["id"]: FIXTURE["guard_jobs"]}

        def fake_gh_json(args):
            path = args[-1] if args[0] == "api" else ""
            if "/releases?" in path:
                return [[]]
            if "release-cli.yml/runs" in path:
                return [{"workflow_runs": [FIXTURE["push_run"], FIXTURE["guard_run"]]}]
            if "sign-and-release.yml/runs" in path:
                return [{"workflow_runs": []}]
            if "/jobs?" in path:
                run_id = int(path.split("/runs/")[1].split("/")[0])
                return {"jobs": jobs.get(run_id, [])}
            if path == "repos/danielraffel/skia-builder/releases/tags/chrome/m153":
                return {"assets": [{"name": "skia-build-win-arm64-gpu-release.zip",
                                    "digest": f"sha256:{upstream_digest}"}]}
            raise AssertionError(f"unexpected gh call: {args}")

        def fake_run(cmd, *a, **kw):
            if cmd[:2] == ["gh", "workflow"]:
                raise AssertionError("collect must never dispatch")
            log = LOG if f"repos/example/repo/actions/jobs/{push_job_id}/logs" in cmd else ""
            return subprocess.CompletedProcess(cmd, 0, stdout=log, stderr="")

        saved = (rr.gh_json, rr.subprocess.run, rr.read_pin_file)
        try:
            rr.gh_json = fake_gh_json
            rr.subprocess.run = fake_run
            rr.read_pin_file = lambda *_: MANIFEST
            return rr.collect("example/repo", [("v0.955.1", NOW - timedelta(hours=7))],
                              NOW - timedelta(days=4))
        finally:
            rr.gh_json, rr.subprocess.run, rr.read_pin_file = saved

    def test_confirmed_mismatch_opens_the_circuit_and_is_not_redispatched(self) -> None:
        states = self.collect_outage(OBSERVED)
        self.assertEqual(states[0].dispatch_attempts, 1, "fixture: one prior re-dispatch")
        report, rebuilt = sweep(states)
        self.assertEqual(rebuilt, [], "a confirmed mismatch must never be re-dispatched")
        self.assertIn("tag-immutable", report["v0.955.1"])
        self.assertIn("circuit open", report["v0.955.1"])

    def test_unconfirmed_mismatch_stays_on_the_retry_path(self) -> None:
        """Control: the same history with a non-matching upstream digest."""
        report, rebuilt = sweep(self.collect_outage("0" * 64))
        self.assertEqual(rebuilt, ["v0.955.1"])
        self.assertNotIn("tag-immutable", report.get("v0.955.1", ""))


class IncidentLifecycle(unittest.TestCase):
    def run_sync(self, report, existing):
        import subprocess

        calls: list[list[str]] = []

        def fake_gh_json(args):
            if args[:2] == ["issue", "list"]:
                return existing
            if args[:2] == ["issue", "view"]:
                return {"body": ""}
            raise AssertionError(args)

        def fake_run(cmd, *a, **kw):
            calls.append(cmd)
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

        saved = (rr.gh_json, rr.subprocess.run)
        try:
            rr.gh_json, rr.subprocess.run = fake_gh_json, fake_run
            with contextlib.redirect_stdout(io.StringIO()):
                rr.sync_incident("example/repo", report, dry_run=False)
        finally:
            rr.gh_json, rr.subprocess.run = saved
        return calls

    def test_recovery_closes_the_open_incident(self) -> None:
        open_issue = [{"number": 6880, "title": rr.INCIDENT_TITLE, "state": "OPEN"}]
        calls = self.run_sync([], open_issue)
        self.assertEqual([c[:3] for c in calls], [["gh", "issue", "close"]])
        self.assertIn("6880", calls[0])

    def test_recovery_with_no_open_incident_does_nothing(self) -> None:
        closed = [{"number": 6880, "title": rr.INCIDENT_TITLE, "state": "CLOSED"}]
        self.assertEqual(self.run_sync([], closed), [])


class IncidentBodyIsBounded(unittest.TestCase):
    """GitHub rejects a body over 65,536 characters; a rejected edit would fail
    every sweep. Build a worst case: many tags, each with long diagnoses."""

    GITHUB_LIMIT = 65_536

    def long_report(self, tags: int) -> list[tuple[str, str]]:
        # Three full diagnoses per tag: the most one Release CLI entry carries.
        detail = "\n".join(DigestMismatch().diag().render())
        why = "failed immutable product validation" + ("\n" + detail) * 3
        return [(f"v1.{999 - i}.0", f"{why} [entry {i}]") for i in range(tags)]

    def test_many_long_entries_stay_under_the_cap_and_keep_the_newest_in_full(self) -> None:
        report = self.long_report(300)
        raw = sum(len(why) for _, why in report)
        body = rr.incident_body(report)
        print(f"\n  incident body at the limit: {len(report)} tags, raw {raw} chars "
              f"-> body {len(body)} chars (cap {rr.INCIDENT_BODY_LIMIT}, GitHub {self.GITHUB_LIMIT})")
        self.assertGreater(raw, self.GITHUB_LIMIT * 10, "fixture must be far over the limit")
        self.assertLessEqual(len(body), rr.INCIDENT_BODY_LIMIT)
        self.assertLess(len(body), self.GITHUB_LIMIT)
        for tag, why in report[: rr.INCIDENT_FULL_ENTRIES]:
            with self.subTest(tag=tag):
                self.assertIn(f"- `{tag}` — {why}", body, "newest tags keep the full diagnosis")
        older_tag, older_why = report[rr.INCIDENT_FULL_ENTRIES]
        self.assertNotIn(older_why, body, "older tags are summarised")
        self.assertIn(f"`{older_tag}`", body)
        # Every tag is still named, so none re-triggers a "newly unresolved" comment.
        self.assertEqual(rr.newly_reported(body, report), [])

    def test_a_small_report_is_rendered_in_full(self) -> None:
        report = self.long_report(2)
        body = rr.incident_body(report)
        for _, why in report:
            self.assertIn(why, body)

    def test_one_oversized_entry_is_truncated_not_dropped(self) -> None:
        body = rr.incident_body([("v1.0.0", "x\n" + "  - line\n" * 20_000)])
        self.assertLess(len(body), rr.INCIDENT_BODY_LIMIT)
        self.assertIn("diagnosis truncated", body)


class FoldedTagsAreNotReannounced(unittest.TestCase):
    """Once enough old tags exist, the oldest fold into one line. A folded tag
    must still count as already reported, or every sweep would comment on it
    again — the spam the comment-on-join rule must never produce."""

    def folding_report(self) -> list[tuple[str, str]]:
        # One-line summaries keep up to 240 chars of an entry's first line, so
        # long first lines make the one-liners alone overflow the cap.
        reason = "still unpublished after 3 re-dispatches; failing leg(s): " + "x" * 300
        return [(f"v2.{999 - i}.0", f"{reason}\n  - detail {i}") for i in range(400)]

    def test_the_oldest_tags_really_fold_and_every_tag_is_still_named(self) -> None:
        report = self.folding_report()
        body = rr.incident_body(report)
        folded = [l for l in body.splitlines() if l.startswith("- older tags folded")]
        self.assertEqual(len(folded), 1, "fixture must be large enough to fold")
        self.assertIn(f"`{report[-1][0]}`", folded[0], "the oldest tag is folded")
        self.assertNotIn(f"- `{report[-1][0]}` —", body)
        self.assertLessEqual(len(body), rr.INCIDENT_BODY_LIMIT)
        for tag, _ in report:
            with self.subTest(tag=tag):
                self.assertIn(f"`{tag}`", body)

    def test_a_second_sweep_over_a_folded_body_reports_nothing_new(self) -> None:
        report = self.folding_report()
        first = rr.incident_body(report)
        self.assertEqual(rr.newly_reported(first, report), [])
        # Next sweep: one genuinely new tag joins; only it is announced.
        second = [("v3.0.0", "new failure")] + report
        self.assertEqual(rr.newly_reported(first, second), ["v3.0.0"])
        self.assertEqual(rr.newly_reported(rr.incident_body(second), second), [])

    def test_two_sweeps_through_sync_incident_comment_once(self) -> None:
        """End to end through sync_incident: the second sweep posts no comment."""
        import subprocess

        report = self.folding_report()
        state = {"body": ""}
        comments: list[list[str]] = []

        def fake_gh_json(args):
            if args[:2] == ["issue", "list"]:
                return [{"number": 1, "title": rr.INCIDENT_TITLE, "state": "OPEN"}]
            if args[:2] == ["issue", "view"]:
                return {"body": state["body"]}
            raise AssertionError(args)

        def fake_run(cmd, *a, **kw):
            if cmd[:3] == ["gh", "issue", "edit"]:
                state["body"] = cmd[cmd.index("--body") + 1]
            elif cmd[:3] == ["gh", "issue", "comment"]:
                comments.append(cmd)
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

        saved = (rr.gh_json, rr.subprocess.run)
        try:
            rr.gh_json, rr.subprocess.run = fake_gh_json, fake_run
            with contextlib.redirect_stdout(io.StringIO()):
                rr.sync_incident("example/repo", report, dry_run=False)
                self.assertEqual(len(comments), 1, "first sweep announces the tags once")
                rr.sync_incident("example/repo", report, dry_run=False)
        finally:
            rr.gh_json, rr.subprocess.run = saved
        self.assertIn("- older tags folded", state["body"])
        self.assertEqual(len(comments), 1, "second sweep over the same tags must not comment")


class AcceptedArchiveIsNotUndone(unittest.TestCase):
    """The m153 win-arm64 archive repinned by PR 10170 has a broken provenance
    file but was accepted on archive comparison; the alert must not tell an
    agent to undo that repin."""

    def test_the_accepted_archive_names_the_exception(self) -> None:
        d = DigestMismatch().diag(manifest_text=MANIFEST.replace(PINNED, OBSERVED))
        text = "\n".join(d.render())
        for needle in ("PR 10170", OBSERVED, "known broken", "archive comparison",
                       "37c5ad6a", "8b8c3872", "do not revert",
                       "both arm64 uploads built 37c5ad6a", "37596862943", "38178800952",
                       "https://github.com/danielraffel/skia-builder/pull/32#issuecomment-6135800920"):
            with self.subTest(needle=needle):
                self.assertIn(needle, text)
        self.assertNotIn("Do not repin to an archive whose provenance is broken", text)
        self.assertNotIn(f"→ `{PINNED}`", text, "must never tell an agent to restore the old pin")
        self.assertIn("already pins the new digest", text)

    def test_any_other_windows_archive_keeps_the_provenance_rule(self) -> None:
        other = "a" * 64
        log = LOG.replace(OBSERVED, other)
        d = DigestMismatch().diag(log_text=log, upstream_digest=other)
        text = "\n".join(d.render())
        self.assertIn("Do not repin to an archive whose provenance is broken", text)
        self.assertNotIn("PR 10170", text)


# ── v0.963.0-v0.965.0 stall ─────────────────────────────────────────────────

PACKAGE_LOG = (
    HERE / "fixtures" / "release_failure" / "v0964_1_linux_x64_package_cli.log"
).read_text(encoding="utf-8")
WGPU_ERROR = "ERROR: wgpu native library not found"


class JobLogFetch(unittest.TestCase):
    """The reconciler read every job log as "" in CI: the runner's gh refuses
    to print a response with terminal escape sequences unless asked, so every
    classification, tag-immutable circuit included, saw no evidence."""

    @staticmethod
    def gh(*, knows_flag: bool, refuses_escapes: bool, fail: str = ""):
        import subprocess

        calls: list[list[str]] = []

        def run(cmd, *a, **kw):
            calls.append(cmd)
            flag = "--allow-escape-sequences" in cmd
            if fail:
                return subprocess.CompletedProcess(cmd, 1, stdout="", stderr=fail)
            if flag and not knows_flag:
                return subprocess.CompletedProcess(
                    cmd, 1, stdout="", stderr="unknown flag: --allow-escape-sequences")
            if refuses_escapes and not flag:
                return subprocess.CompletedProcess(
                    cmd, 1, stdout="",
                    stderr="the response contains terminal escape sequences; pass "
                           "--allow-escape-sequences to output it anyway")
            return subprocess.CompletedProcess(cmd, 0, stdout="\x1b[31mlog\x1b[0m", stderr="")

        return run, calls

    def test_a_gh_that_refuses_escapes_still_yields_the_log(self) -> None:
        run, calls = self.gh(knows_flag=True, refuses_escapes=True)
        self.assertEqual(rr.fetch_job_log("o/r", 7, run=run), ("\x1b[31mlog\x1b[0m", ""))
        self.assertIn("--allow-escape-sequences", calls[0])

    def test_the_bare_command_is_exactly_what_failed_in_ci(self) -> None:
        """Control: the command the reconciler used to run gets nothing."""
        import subprocess

        run, _ = self.gh(knows_flag=True, refuses_escapes=True)
        bare = run(["gh", "api", "repos/o/r/actions/jobs/7/logs"],
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.assertEqual(bare.returncode, 1)
        self.assertIn("terminal escape sequences", bare.stderr)

    def test_a_gh_too_old_for_the_flag_falls_back(self) -> None:
        run, calls = self.gh(knows_flag=False, refuses_escapes=False)
        self.assertEqual(rr.fetch_job_log("o/r", 7, run=run)[0], "\x1b[31mlog\x1b[0m")
        self.assertEqual(len(calls), 2)
        self.assertNotIn("--allow-escape-sequences", calls[1])

    def test_a_real_failure_is_reported_not_swallowed(self) -> None:
        run, calls = self.gh(knows_flag=True, refuses_escapes=False, fail="HTTP 404: Not Found")
        text, error = rr.fetch_job_log("o/r", 7, run=run)
        self.assertEqual(text, "")
        self.assertIn("HTTP 404", error)
        self.assertEqual(len(calls), 1, "an unrelated failure is not retried")


class FirstErrorIsInTheFailingStep(unittest.TestCase):
    """Recorded v0.964.1 `CLI linux-x64` job: an apt step prints a dbus error
    thousands of lines before `Package CLI (Unix)` refuses to package."""

    def test_the_packager_refusal_is_the_first_error(self) -> None:
        self.assertTrue(rfd.first_error_line(PACKAGE_LOG).startswith(WGPU_ERROR))

    def test_control_the_fixture_carries_the_earlier_decoy(self) -> None:
        lines = [rfd.strip_timestamp(l).strip() for l in PACKAGE_LOG.splitlines()]
        decoy, _ = rfd._scan_errors(lines)
        self.assertIn("system_bus_socket", decoy, "a whole-log scan picks the decoy")

    def test_a_log_without_step_markers_keeps_the_whole_log_scan(self) -> None:
        self.assertEqual(rfd.first_error_line("ok\nERROR: boom\n"), "ERROR: boom")


def package_run(run_id: int, job_id: int, step: str = "Package CLI (Unix)") -> tuple[dict, list[dict]]:
    run = {"id": run_id, "status": "completed", "conclusion": "failure",
           "created_at": f"2026-10-14T0{run_id}:00:00Z", "event": "workflow_dispatch"}
    jobs = [{"id": job_id, "name": "CLI linux-x64", "conclusion": "failure", "run_attempt": 1,
             "steps": [{"name": "Build", "conclusion": "success"},
                       {"name": step, "conclusion": "failure"}]}]
    return run, jobs


class RepeatedPackageFailureIsTagImmutable(unittest.TestCase):
    def failures(self, logs: dict[int, str], step: str = "Package CLI (Unix)"):
        runs, cache = [], {}
        for run_id, job_id in ((1, 11), (2, 12)):
            run, jobs = package_run(run_id, job_id, step)
            runs.append(run)
            cache[run_id] = jobs
        return rr.repeated_package_failures(runs, cache, lambda job_id: logs.get(job_id, ""))

    def test_an_identical_repeat_opens_the_circuit(self) -> None:
        found = self.failures({11: PACKAGE_LOG, 12: PACKAGE_LOG})
        self.assertEqual(len(found), 1)
        self.assertIn(WGPU_ERROR, found[0])
        state = rr.TagState(
            tag="v0.964.1", created_at=NOW - timedelta(hours=5), published=False,
            has_release_object=False, assets=frozenset(), run_states=("completed",) * 2,
            validation_failures=found, dispatch_attempts=1,
        )
        report, rebuilt = sweep([state])
        self.assertEqual(rebuilt, [], "a repeated package failure must not be re-dispatched")
        self.assertIn("circuit open", report["v0.964.1"])

    def test_a_single_failure_stays_on_the_retry_path(self) -> None:
        runs, cache = [], {}
        run, jobs = package_run(1, 11)
        self.assertEqual(rr.repeated_package_failures([run], {1: jobs}, lambda _: PACKAGE_LOG), ())

    def test_different_errors_are_not_a_repeat(self) -> None:
        other = PACKAGE_LOG.replace(WGPU_ERROR, "ERROR: disk full")
        self.assertEqual(self.failures({11: PACKAGE_LOG, 12: other}), ())

    def test_a_cancelled_package_step_is_no_evidence(self) -> None:
        """Two transient failures (runner lost or step cancelled) leave no
        packager error; the whole-log fallback would name the apt dbus decoy
        for both and open the circuit on a flake."""
        head, _, _ = PACKAGE_LOG.partition("ERROR: wgpu native library not found")
        head = head.rsplit("\n", 1)[0]
        cancelled = head + "\n2026-10-14T03:03:16.9Z ##[error]The operation was canceled.\n"
        self.assertIn("system_bus_socket", rfd.first_error_line(cancelled),
                      "control: the human-facing diagnosis still falls back to the decoy")
        self.assertEqual(self.failures({11: cancelled, 12: cancelled}), ())

    def test_missing_logs_are_no_evidence(self) -> None:
        self.assertEqual(self.failures({}), ())

    def test_another_step_is_not_a_package_failure(self) -> None:
        self.assertEqual(self.failures({11: PACKAGE_LOG, 12: PACKAGE_LOG}, step="Build"), ())


class OneIssuePerStallEpisode(unittest.TestCase):
    """#6880 had been reopened since July; its reopen for v0.963.0 read as
    routine. A new stall gets a new issue; an open one is updated in place."""

    def run_sync(self, report, existing):
        import subprocess

        calls: list[list[str]] = []

        def fake_gh_json(args):
            if args[:2] == ["issue", "list"]:
                self.assertIn("open", args, "only an OPEN issue may continue an episode")
                return existing
            if args[:2] == ["issue", "view"]:
                return {"body": ""}
            raise AssertionError(args)

        def fake_run(cmd, *a, **kw):
            calls.append(cmd)
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

        saved = (rr.gh_json, rr.subprocess.run)
        try:
            rr.gh_json, rr.subprocess.run = fake_gh_json, fake_run
            with contextlib.redirect_stdout(io.StringIO()):
                rr.sync_incident("example/repo", report, dry_run=False, now=NOW)
        finally:
            rr.gh_json, rr.subprocess.run = saved
        return calls

    REPORT = [("v0.964.1", "circuit open"), ("v0.963.0", "circuit open")]

    def test_a_new_stall_creates_a_new_issue_named_for_its_oldest_tag(self) -> None:
        calls = self.run_sync(self.REPORT, [])
        self.assertEqual([c[:3] for c in calls], [["gh", "issue", "create"]])
        title = calls[0][calls[0].index("--title") + 1]
        self.assertTrue(title.startswith("Release stalled: v0.963.0 unpublished ("), title)
        self.assertNotIn("reopen", [c[2] for c in calls])

    def test_an_open_episode_is_edited_not_duplicated(self) -> None:
        open_episode = [{"number": 9, "title": "Release stalled: v0.963.0 unpublished (2026-10-13)",
                         "state": "OPEN"}]
        calls = self.run_sync(self.REPORT, open_episode)
        self.assertNotIn(["gh", "issue", "create"], [c[:3] for c in calls])
        self.assertIn(["gh", "issue", "edit", "9"], [c[:4] for c in calls])

    def test_the_open_legacy_incident_is_continued(self) -> None:
        legacy = [{"number": 6880, "title": rr.INCIDENT_TITLE, "state": "OPEN"}]
        calls = self.run_sync(self.REPORT, legacy)
        self.assertIn(["gh", "issue", "edit", "6880"], [c[:4] for c in calls])

    def test_recovery_closes_the_open_episode(self) -> None:
        open_episode = [{"number": 9, "title": "Release stalled: v0.963.0 unpublished (2026-10-13)",
                         "state": "OPEN"}]
        calls = self.run_sync([], open_episode)
        self.assertEqual([c[:4] for c in calls], [["gh", "issue", "close", "9"]])

    def test_an_unrelated_open_issue_is_not_an_episode(self) -> None:
        other = [{"number": 3, "title": "Release notes typo", "state": "OPEN"}]
        calls = self.run_sync(self.REPORT, other)
        self.assertEqual([c[:3] for c in calls], [["gh", "issue", "create"]])


if __name__ == "__main__":
    unittest.main()
