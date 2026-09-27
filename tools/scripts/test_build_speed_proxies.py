#!/usr/bin/env python3
"""Tests for tools/scripts/build_speed_proxies.py (the load-independent verdict).

Inputs are small hand-built GitHub job/run/timeline shapes and tartci
supervisor events, so each proxy is checked against the exact mechanism it
claims to count, and each control is checked to go to zero (INSTRUMENT BLIND)
when the instrument sees nothing.

Run:  python3 -m unittest test_build_speed_proxies   (from tools/scripts)
"""
from __future__ import annotations

import datetime as dt
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, HERE / f"{name}.py")
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


px = _load("build_speed_proxies")
UTC = dt.timezone.utc
BASE = ["self-hosted", "macOS", "ARM64", "pulp-build", "pulp-build-vm"]
PR_HEAD = BASE + ["pulp-build-pr-head"]


def T(s: str) -> dt.datetime:
    return dt.datetime.fromisoformat(s).replace(tzinfo=UTC)


def win(**kw) -> "px.Windows":
    return px.Windows(T("2026-09-01T00:00:00"), T("2026-09-10T00:00:00"), **kw)


def job(created, started=None, done=None, runner=None, conclusion="success",
        labels=PR_HEAD, name="macos", jid=1):
    return {"id": jid, "name": name, "status": "completed", "conclusion": conclusion,
            "created_at": created, "started_at": started or created,
            "completed_at": done or started or created, "runner_name": runner, "labels": labels}


class ClassAndWindowTests(unittest.TestCase):
    def test_job_class_is_what_remains_after_the_base_labels(self):
        self.assertEqual(px.job_class(PR_HEAD), "pulp-build-pr-head")
        self.assertEqual(px.job_class(BASE), "none")
        self.assertEqual(px.job_class(["macos-15"]), "macos-15")

    def test_windows_assign_sides_and_drop_exclusions(self):
        w = win(after_from=T("2026-09-12T00:00:00"),
                exclude=[(T("2026-09-13T00:00:00"), T("2026-09-14T00:00:00"))])
        self.assertEqual(w.side("2026-09-05T00:00:00Z"), "before")
        self.assertIsNone(w.side("2026-09-11T00:00:00Z"))  # between split and after_from
        self.assertEqual(w.side("2026-09-12T01:00:00Z"), "after")
        self.assertIsNone(w.side("2026-09-13T05:00:00Z"))
        self.assertTrue(w.excluded("2026-09-13T05:00:00Z"))


class VerdictTests(unittest.TestCase):
    def test_zero_control_is_blind_not_a_value(self):
        b = px.side_value([0.0] * 30)
        self.assertTrue(px.verdict(b, b, 0).startswith("INSTRUMENT BLIND"))

    def test_small_n_says_insufficient_sample(self):
        b, a = px.side_value([1.0] * 5), px.side_value([0.0] * 50)
        self.assertTrue(px.verdict(b, a, 10).startswith("insufficient sample"))

    def test_disjoint_intervals_read_as_a_change(self):
        b, a = px.side_value([1.0] * 30), px.side_value([0.0] * 30)
        self.assertEqual(px.verdict(b, a, 10), "lower (CIs disjoint), better")


class ReleasePlacementTests(unittest.TestCase):
    def test_match_mismatch_unclassified_and_unjoined(self):
        rel = BASE + ["pulp-release-tagged"]
        runs = [{"jobs": [
            job("2026-09-05T00:00:00Z", runner="m5-a-1", labels=rel),        # matches
            job("2026-09-05T00:00:00Z", runner="m5-a-2", labels=rel),        # minted as gate
            job("2026-09-05T00:00:00Z", runner="m1-a-3", labels=BASE),       # no class
            job("2026-09-05T00:00:00Z", runner="m1-a-4", labels=rel),        # no mint seen
            job("2026-09-05T00:00:00Z", runner="GitHub Actions 1", labels=["ubuntu-latest"]),
            # a runner minted with more class labels than the job asked for still matches
            job("2026-09-05T00:00:00Z", runner="m5-a-5", labels=BASE + ["pulp-build-vm-release"]),
        ]}]
        mints = {"m5-a-1": "pulp-release-tagged", "m5-a-2": "pulp-build-merge-group",
                 "m1-a-3": "pulp-build-merge-group",
                 "m5-a-5": "pulp-build-vm-release,pulp-release-tagged"}
        r = px.release_placement(runs, mints, win())
        self.assertEqual(r["before"]["n"], 4)
        self.assertAlmostEqual(r["before"]["value"], 2 / 4)
        self.assertEqual((r["unjoined"], r["unclassified"]), (1, 1))
        self.assertEqual(r["control"]["value"], 4)

    def test_no_mint_events_is_blind(self):
        runs = [{"jobs": [job("2026-09-05T00:00:00Z", runner="m5-a-1")]}]
        r = px.release_placement(runs, {}, win())
        self.assertTrue(r["verdict"].startswith("INSTRUMENT BLIND"))


class GitHubJobProxyTests(unittest.TestCase):
    def test_unserved_counts_only_label_sets_nobody_served(self):
        odd = BASE + ["pulp-build-vm-release"]
        jobs = [job("2026-09-05T00:00:00Z", runner="m3-x"),
                job("2026-09-05T01:00:00Z", conclusion="cancelled"),           # served set
                job("2026-09-05T02:00:00Z", conclusion="cancelled", labels=odd),
                job("2026-09-05T03:00:00Z", conclusion="cancelled", labels=odd)]
        r = px.unserved_labels(jobs, win(), {"before": 9.0, "after": 1.0})
        self.assertEqual(r["before"]["count"], 2)
        self.assertEqual(r["control"]["value"], 1)

    def test_pr_head_cancelled_without_runner_share(self):
        gj = [dict(job("2026-09-05T00:00:00Z", runner="m3-x"), event="pull_request"),
              dict(job("2026-09-05T00:00:00Z", conclusion="cancelled"), event="pull_request"),
              dict(job("2026-09-05T00:00:00Z", runner="m3-y", conclusion="cancelled"),
                   event="pull_request"),
              dict(job("2026-09-05T00:00:00Z", conclusion="cancelled"), event="merge_group")]
        r = px.pr_head_no_runner(gj, win())
        self.assertEqual(r["before"]["n"], 3)
        self.assertAlmostEqual(r["before"]["value"], 1 / 3)

    def test_queue_depth_counts_jobs_still_waiting_at_creation(self):
        jobs = [job("2026-09-05T00:00:00Z", "2026-09-05T00:30:00Z", runner="a", jid=1),
                job("2026-09-05T00:10:00Z", "2026-09-05T00:40:00Z", runner="b", jid=2),
                job("2026-09-05T00:20:00Z", "2026-09-05T00:25:00Z", runner="c", jid=3),
                job("2026-09-05T00:50:00Z", "2026-09-05T00:51:00Z", runner="d", jid=4)]
        ahead = {j["id"]: n for j, n in px.queue_depths(jobs)}
        self.assertEqual(ahead, {1: 0, 2: 1, 3: 2, 4: 0})
        r = px.wait_per_job_ahead(jobs, win())
        # waits 30, 30, 5, 1 min over positions 1, 2, 3, 1 → 30, 15, 1.67, 1
        self.assertAlmostEqual(r["before"]["value"], (15 + 5 / 3) / 2)
        self.assertEqual(r["control"]["value"], 2)


class RefreshAndQueueTests(unittest.TestCase):
    def _run(self, rid, sha, created, conclusion="cancelled"):
        return {"id": rid, "event": "pull_request", "pr": 7, "head_sha": sha,
                "head_branch": "feat/x",
                "created_at": created,
                "jobs": [job(created, runner=None, conclusion=conclusion)]}

    def test_only_a_main_merge_head_counts_as_a_refresh(self):
        runs = [self._run(1, "a", "2026-09-05T00:00:00Z"),
                self._run(2, "b", "2026-09-05T01:00:00Z"),
                self._run(3, "c", "2026-09-05T02:00:00Z"),
                self._run(4, "d", "2026-09-05T03:00:00Z"),
                self._run(5, "e", "2026-09-05T04:00:00Z", "success")]
        commits = {"b": {"parents": ["p", "q"],
                         "subject": "Merge remote-tracking branch 'origin/main' into feat/x"},
                   "c": {"parents": ["b", "z"], "subject": "Merge branch 'feature/y' into feat/x"},
                   # a cherry-picked merge subject is not a merge
                   "d": {"parents": ["c"], "subject": "Merge branch 'main' into feat/x"},
                   "e": {"parents": ["d"], "subject": "fix the thing"}}
        merged = [{"number": 7, "createdAt": "2026-09-04T00:00:00Z",
                   "mergedAt": "2026-09-06T00:00:00Z", "events": []}]
        r = px.refresh_cancels(runs, commits, merged, win())
        self.assertEqual(r["refresh_cancelled_runs"], {"before": 1, "after": 0})
        self.assertEqual(r["control"]["value"], 4)
        self.assertEqual(r["before"]["value"], 1.0)

    def test_runs_without_pull_requests_join_by_head_branch(self):
        runs = [{"event": "pull_request", "pr": None, "head_branch": "feat/x",
                 "created_at": "2026-09-05T00:00:00Z"},
                {"event": "pull_request", "pr": None, "head_branch": "feat/x",
                 "created_at": "2026-09-08T00:00:00Z"},
                {"event": "pull_request", "pr": None, "head_branch": "feat/y",
                 "created_at": "2026-09-05T00:00:00Z"}]
        merged = [{"number": 7, "headRefName": "feat/x", "mergedAt": "2026-09-06T00:00:00Z"},
                  {"number": 9, "headRefName": "feat/x", "mergedAt": "2026-09-09T00:00:00Z"}]
        self.assertEqual(px.attach_prs(runs, merged), 2)
        self.assertEqual([r["pr"] for r in runs], [7, 9, None])

    def test_queue_attempts_per_merged_pr(self):
        merged = [{"number": 1, "mergedAt": "2026-09-05T05:00:00Z", "events": [
            {"type": "AddedToMergeQueueEvent", "at": "2026-09-05T01:00:00Z"},
            {"type": "RemovedFromMergeQueueEvent", "at": "2026-09-05T02:00:00Z",
             "reason": "FAILED_CHECKS"},
            {"type": "AddedToMergeQueueEvent", "at": "2026-09-05T03:00:00Z"}]},
            {"number": 2, "mergedAt": "2026-09-05T05:00:00Z", "events": [
                {"type": "AddedToMergeQueueEvent", "at": "2026-09-05T01:00:00Z"}]}]
        r = px.mq_attempts(merged, win())
        self.assertEqual(r["before"]["value"], 1.5)

    def test_ejections_are_attributed_by_log_and_window(self):
        w = win(exclude=[(T("2026-09-06T00:00:00"), T("2026-09-07T00:00:00"))])
        merged = []
        runs = []
        for n, at, digest in ((1, "2026-09-05", "macos\tBuild\tts Undefined symbols for architecture arm64:"),
                              (2, "2026-09-06", "macos\tBuild\tts Undefined symbols for architecture arm64:"),
                              (3, "2026-09-05", "macos\tTest\tts   42 - flaky-thing (Failed)\n"
                                                "The following tests FAILED:")):
            merged.append({"number": n, "mergedAt": f"{at}T09:00:00Z", "events": [
                {"type": "AddedToMergeQueueEvent", "at": f"{at}T01:00:00Z"},
                {"type": "RemovedFromMergeQueueEvent", "at": f"{at}T02:00:00Z",
                 "reason": "FAILED_CHECKS"}]})
            runs.append({"event": "merge_group", "pr": n, "created_at": f"{at}T01:01:00Z",
                         "jobs": [job(f"{at}T01:01:00Z", runner="studio-x", conclusion="failure",
                                      jid=100 + n)]})
        merged.append({"number": 4, "mergedAt": "2026-09-05T09:00:00Z", "events": [
            {"type": "AddedToMergeQueueEvent", "at": "2026-09-05T01:00:00Z"},
            {"type": "RemovedFromMergeQueueEvent", "at": "2026-09-05T02:00:00Z",
             "reason": "FAILED_CHECKS"}]})
        data = {"log_digests": {
            "101": {"digest": "Undefined symbols for architecture", "at": "2026-09-05T01:01:00Z"},
            "102": {"digest": "Undefined symbols for architecture", "at": "2026-09-06T01:01:00Z"},
            "103": {"digest": "   42 - flaky-thing (Failed)", "at": "2026-09-05T01:01:00Z"}}}
        out = px.ejection_causes(merged, runs, px.log_causes(data, w), w)
        self.assertEqual(out["before"], {"link_error": 1, "test_failure:flaky-thing": 1,
                                         "neighbour_or_non_macos": 1})
        self.assertEqual(out["excluded"], {"infra_host_cache": 1})

    def test_failed_test_name_is_read_through_the_log_prefix(self):
        text = "macos\tTest\t2026-09-26T09:00:00Z      12 - pulp-thing-selftest (Timeout)"
        self.assertEqual(px.classify_log(text, None, win()), "test_failure:pulp-thing-selftest")
        self.assertEqual(px.classify_log("", None, win(), read=False), "log_unavailable")


def _failed_block(*names: str) -> str:
    """A `macos` job log's ctest FAILED block, with GitHub's per-line prefix."""
    return "\n".join(["macos\tTest\t2026-09-05T00:00:00Z The following tests FAILED:"] + [
        f"macos\tTest\t2026-09-05T00:00:00Z \t{i + 10} - {n} (Failed)"
        for i, n in enumerate(names)])


class BaseRedTests(unittest.TestCase):
    """A failure main carried: a cross-host streak of merge groups sharing a failing test."""

    def _mg(self, jobs):
        """jobs: (id, created, runner, conclusion, failing tests or None for a non-test log)."""
        runs, digests = [], {}
        for jid, at, runner, concl, names in jobs:
            runs.append({"event": "merge_group", "id": jid, "created_at": at,
                         "jobs": [job(at, runner=runner, conclusion=concl, jid=jid)]})
            if concl != "success":
                digests[str(jid)] = {"digest": _failed_block(*names) if names else
                                     "macos\tBuild\tts error: no member named 'x'",
                                     "read": True, "at": at}
        return runs, digests

    def test_cross_host_streak_counts_every_member_and_a_lone_failure_is_the_control(self):
        runs, digests = self._mg([
            (1, "2026-09-05T01:00:00Z", "m1-a", "failure", ["base-red-test"]),
            (2, "2026-09-05T01:10:00Z", "m5-a", "failure", ["base-red-test", "other"]),
            (3, "2026-09-05T01:20:00Z", "studio-a", "failure", ["base-red-test"]),
            (4, "2026-09-05T02:00:00Z", "studio-a", "success", None),
            (5, "2026-09-05T03:00:00Z", "m5-a", "failure", ["flaky test with spaces"]),
            # a compile error neither breaks nor extends a streak, but is a failure
            (6, "2026-09-05T04:00:00Z", "m1-a", "failure", None),
        ])
        r = px.base_red(runs, digests, win())
        self.assertEqual(r["before"]["n"], 5)
        self.assertAlmostEqual(r["before"]["value"], 3 / 5)
        self.assertEqual(r["control"]["value"], 1)  # the lone flake
        self.assertEqual(r["streaks"]["before"], ["base-red-test x3 on m1/m3/m5"])

    def test_one_host_streak_is_a_host_problem_not_main(self):
        runs, digests = self._mg([
            (1, "2026-09-05T01:00:00Z", "m1-a", "failure", ["browser-capture"]),
            (2, "2026-09-05T01:10:00Z", "m1-b", "failure", ["browser-capture"]),
        ])
        r = px.base_red(runs, digests, win())
        self.assertEqual((r["before"]["value"], r["control"]["value"]), (0.0, 2))

    def test_an_executed_pass_breaks_the_streak_and_placeholders_do_not(self):
        runs, digests = self._mg([
            (1, "2026-09-05T01:00:00Z", "m1-a", "failure", ["t"]),
            (2, "2026-09-05T01:05:00Z", "GitHub Actions 7", "success", None),
            (3, "2026-09-05T01:06:00Z", "m3-a", "cancelled", None),
            (4, "2026-09-05T01:10:00Z", "m5-a", "failure", ["t"]),
            (5, "2026-09-05T01:20:00Z", "m3-a", "success", None),
            (6, "2026-09-05T01:30:00Z", "m1-a", "failure", ["t"]),
        ])
        r = px.base_red(runs, digests, win())
        self.assertEqual(r["before"]["n"], 3)
        self.assertAlmostEqual(r["before"]["value"], 2 / 3)  # 1+4 streak; 6 after a real pass
        self.assertEqual(r["control"]["value"], 1)

    def test_a_failure_that_names_no_test_does_not_break_a_streak(self):
        runs, digests = self._mg([
            (1, "2026-09-05T01:00:00Z", "m1-a", "failure", ["t"]),
            (2, "2026-09-05T01:05:00Z", "m3-a", "failure", None),
            (3, "2026-09-05T01:10:00Z", "m5-a", "failure", ["t"]),
        ])
        r = px.base_red(runs, digests, win())
        self.assertAlmostEqual(r["before"]["value"], 2 / 3)
        self.assertEqual(r["streaks"]["before"], ["t x2 on m1/m5"])

    def test_no_named_failure_is_instrument_blind(self):
        runs, digests = self._mg([(1, "2026-09-05T01:00:00Z", "m1-a", "failure", None)])
        self.assertTrue(px.base_red(runs, digests, win())["verdict"].startswith(
            "INSTRUMENT BLIND"))

    def test_digest_keeps_the_failed_block_past_the_cap(self):
        noise = "\n".join(f"macos\tTest\tts error: line {i}" for i in range(400))
        digest = px._log_digest(noise + "\n" + _failed_block("a name with spaces", "b"))
        self.assertEqual(px.parse_failing_tests(digest), ["a name with spaces", "b"])


class TartciEventTests(unittest.TestCase):
    def _vm(self, vm, t0, served=True, minted=True, host="m3", lane="pulp-gate",
            labels="self-hosted,macOS,ARM64,pulp-build,pulp-build-vm,pulp-build-pr-head",
            end=None):
        ev = [{"ts": t0, "event": "clone_start", "vm": vm},
              {"ts": t0, "event": "boot_ok", "vm": vm}]
        if minted:
            ev.append({"ts": t0, "event": "mint_jit", "vm": vm, "detail": f"labels={labels} tier=0"})
        if served:
            ev.append({"ts": t0, "event": "job_assigned", "vm": vm})
        ev.append({"ts": end or t0, "event": "teardown", "vm": vm})
        return [dict(e, host=host, lane=lane) for e in ev]

    def test_an_unnamed_clone_attaches_to_the_next_vm_of_its_runner(self):
        ev = [{"ts": "2026-09-05T00:00:00Z", "event": "clone_start", "vm": "", "runner": "r",
               "host": "m3", "lane": "L"},
              {"ts": "2026-09-05T00:01:00Z", "event": "boot_ok", "vm": "v1", "runner": "r",
               "host": "m3", "lane": "L"},
              {"ts": "2026-09-05T01:00:00Z", "event": "clone_start", "vm": "", "runner": "r",
               "host": "m3", "lane": "L"},
              {"ts": "2026-09-05T02:00:00Z", "event": "clone_start", "vm": "", "runner": "r",
               "host": "m3", "lane": "L"}]
        vms = px.vm_lifecycles(ev)
        self.assertEqual(vms["v1"]["clone"], "2026-09-05T00:00:00Z")
        self.assertEqual(sorted(v["clone"] for k, v in vms.items() if k.startswith("clone-only:")),
                         ["2026-09-05T01:00:00Z", "2026-09-05T02:00:00Z"])

    def test_discarded_and_unminted_vms_per_host(self):
        ev = (self._vm("v1", "2026-09-05T00:00:00Z") + self._vm("v2", "2026-09-05T01:00:00Z", served=False)
              + self._vm("v3", "2026-09-05T02:00:00Z", served=False, minted=False))
        # Came up (named) and discarded before mint: no boot_ok is logged for it.
        ev += [{"ts": "2026-09-05T03:00:00Z", "event": "runner_version", "vm": "v4",
                "host": "m3", "lane": "pulp-gate"}]
        rows = {r["key"]: r for r in px.event_host_rows(ev, win())}
        self.assertAlmostEqual(rows["vm_discard[m3]"]["before"]["value"], 3 / 4)
        self.assertEqual(rows["vm_discard[m3]"]["discarded_per_job_served"]["before"], 3.0)
        self.assertAlmostEqual(rows["unminted_boots[m3]"]["before"]["value"], 1 / 3)
        self.assertEqual(rows["vm_discard[m3]"]["control"]["value"], 1)
        self.assertEqual(px.mint_classes(ev)["v1"], "pulp-build-pr-head")

    def _live(self, start, minutes, host="m3", lane="pulp-gate"):
        t = T(start)
        return [{"ts": (t + dt.timedelta(minutes=m)).strftime("%Y-%m-%dT%H:%M:%SZ"),
                 "event": "assignment_stale_demand", "vm": "", "host": host, "lane": lane}
                for m in range(0, minutes + 1, 2)]

    def _queued(self):
        return [dict(job("2026-09-05T10:00:00Z", "2026-09-05T10:20:00Z", runner="m5-z"),
                     event="pull_request")]

    def test_a_job_waiting_beside_an_idle_live_lane_is_counted(self):
        ev = self._vm("v1", "2026-09-05T08:00:00Z", end="2026-09-05T09:00:00Z") + \
            self._live("2026-09-05T09:00:00", 90)
        r = px.idle_with_demand(ev, self._queued(), win())[0]
        self.assertEqual(r["key"], "idle_with_demand[m3]")
        self.assertEqual(r["before"]["value"], 1.0)
        self.assertGreater(r["control"]["value"], 0)

    def test_a_full_host_a_denial_or_a_dead_supervisor_is_not_idle_with_demand(self):
        busy_other = self._vm("o1", "2026-09-05T09:30:00Z", lane="forge-a", end="2026-09-05T11:00:00Z") + \
            self._vm("o2", "2026-09-05T09:30:00Z", lane="forge-b", end="2026-09-05T11:00:00Z")
        base = self._vm("v1", "2026-09-05T08:00:00Z", end="2026-09-05T09:00:00Z")
        live = self._live("2026-09-05T09:00:00", 90)
        full = px.idle_with_demand(base + live + busy_other, self._queued(), win())[0]
        self.assertEqual(full["before"]["value"], 0.0)
        denial = [{"ts": "2026-09-05T10:05:00Z", "event": "lease_unfit_now", "vm": "",
                   "host": "m3", "lane": "pulp-gate"}]
        denied = px.idle_with_demand(base + live + denial, self._queued(), win())
        self.assertEqual(denied[0]["before"]["value"], 0.0)
        # A lane that kept attempting without a VM is blocked, not idle by choice.
        self.assertEqual(denied[1]["key"], "blocked_with_demand[m3]")
        self.assertEqual(denied[1]["before"]["value"], 1.0)
        dead = px.idle_with_demand(base, self._queued(), win())[0]
        self.assertEqual(dead["before"]["value"], 0.0)
        self.assertTrue(dead["verdict"].startswith("INSTRUMENT BLIND"))
        # A supervisor last heard from just before the wait, then silent for
        # longer than LIVE_GAP_SECONDS, is not a live idle slot.
        went_quiet = base + self._live("2026-09-05T09:50:00", 6)
        quiet = px.idle_with_demand(went_quiet, self._queued(), win())[0]
        self.assertEqual(quiet["before"]["value"], 0.0)
        # A short wait with no supervisor event at all proves nothing live.
        short = [dict(job("2026-09-05T10:00:00Z", "2026-09-05T10:05:00Z", runner="m5-z"),
                      event="pull_request")]
        silent = px.idle_with_demand(base, short, win())[0]
        self.assertEqual(silent["before"]["value"], 0.0)


class CliTests(unittest.TestCase):
    def test_no_collect_renders_from_cache_and_exits_3_when_blind(self):
        data = {"collected_at": "2026-09-12T00:00:00+00:00", "runs": [], "release_runs": [],
                "merged": [], "commits": {}, "log_digests": {}, "events": [], "hosts_read": {}}
        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(tmp) / "c.json"
            cache.write_text(json.dumps(data))
            proc = subprocess.run(
                [sys.executable, str(HERE / "build_speed_scorecard.py"), "proxies",
                 "--since", "2026-09-01", "--split", "2026-09-10T00:00Z", "--no-collect",
                 "--cache", str(cache)], capture_output=True, text=True, timeout=60)
        self.assertEqual(proc.returncode, 3, proc.stderr)
        self.assertIn("Load-independent proxies", proc.stdout)
        self.assertIn("INSTRUMENT BLIND", proc.stdout)
        self.assertIn("instrument blind", proc.stderr)


if __name__ == "__main__":
    unittest.main()
