"""Load-independent proxies for the build-speed before/after verdict.

Wall-clock waits mostly track load (how many PRs, how many agents building on
the same hosts), so a before/after on them cannot say whether a change worked.
Each proxy here names one mechanism a change is supposed to affect and counts
that mechanism directly from logs, labels or annotations:

  release_placement   release jobs on a runner minted for their class
                      (GitHub job labels + runner_name, joined to tartci
                      `mint_jit` labels)
  unserved_labels     jobs that ended without a runner whose exact label set
                      no job in the same window was served on
  idle_with_demand    gate jobs that waited while an eligible, live tartci
                      lane on some host sat idle (tartci supervisor events)
  vm_discard          VMs booted and torn down without serving a job, per job
                      served (tartci `boot_ok` / `job_assigned`)
  unminted_boots      boot attempts that never minted a runner, per job served
                      (tartci `clone_start` / `mint_jit`)
  pr_head_no_runner   PR-head `macos` jobs cancelled before any runner took them
  wait_per_job_ahead  queue wait divided by (jobs ahead of it at creation + 1)
  refresh_cancels     PR-head gate runs cancelled by a push that merged main in,
                      per merged PR (run cancellation + head-SHA change + the
                      superseding commit's parents and subject)
  mq_attempts         merge-queue entries per merged PR, and ejections by cause
  tree_identical_groups  merged PRs whose merge-commit tree equals the PR-head
                      tree (the ceiling on exact-tree receipt reuse, G5)
  gate_runs           native gate runs per merged PR: PR head, merge group,
                      wasted (cancelled after a runner took it, or a failed
                      merge group)
  base_red            merge-group `macos` failures inside a cross-host streak
                      of consecutive failures sharing a failing test (main
                      itself carried the failure), per merge-group `macos`
                      failure (failed-job logs)

Every proxy carries a CONTROL: a count on the same instrument and target that
must be non-zero. A zero control means the instrument saw nothing, and the row
says INSTRUMENT BLIND instead of printing a value. A side with fewer units than
MIN_UNITS says "insufficient sample" instead of a percentage.

The raw inputs are collected once (`collect`) into a JSON cache so a report can
be re-rendered, and re-checked by hand, without another API sweep. Everything
after `collect` is pure.
"""

from __future__ import annotations

import bisect
import concurrent.futures
import datetime as dt
import json
import random
import re
import subprocess
import sys
from pathlib import Path
from typing import Any, Callable, Iterable

sys.path.insert(0, str(Path(__file__).resolve().parent))
# The ctest failure-block parser the queue attributor already uses, so a
# "failing test" means the same thing in both.
from queue_batch_attribute import FAILED_TEST_LINE_RE, parse_failing_tests  # noqa: E402

MIN_UNITS = 20
IDLE_MIN_SECONDS = 120
LIVE_GAP_SECONDS = 600
HOST_VM_LIMIT = 2
MAX_VM_LIFETIME = dt.timedelta(hours=4)
GATE_JOB = "macos"
BASE_LABELS = {"self-hosted", "macOS", "ARM64", "pulp-build", "pulp-build-vm"}
RELEASE_WORKFLOWS = ("release-cli.yml", "sign-and-release.yml", "release-path-pr-gate.yml")
REFRESH_SUBJECT = re.compile(r"^Merge (remote-tracking )?branch '(origin/)?main'|"
                             r"^Merge (origin/)?main into|^Merge commit '[0-9a-f]+' into")
DENIAL_EVENTS = {"assignment_v2_pre_mint_denied", "lease_unfit_now", "lease_denied",
                 "warm_handoff_denied"}
# A lane that logs one of these inside a window was trying to serve (or knew
# another lane covered the job); only a live lane that logs none of them was
# idle by choice. admission_precheck exists on every host only from
# 2026-09-23 and job_claim only from 2026-09-26, so a window before that reads
# an attempt as idle: compare event rows only where both sides carry them.
ATTEMPT_EVENTS = DENIAL_EVENTS | {"admission_precheck", "admission_precheck_deferred",
                                  "admission_deferred", "job_claim", "job_claim_contended",
                                  "clone_start", "admission_check"}

# Ejection log classification, first match wins. A link error inside a known
# host-cache incident window is infrastructure, not the PR's code.
LOG_CAUSES = (
    ("link_error", re.compile(r"Undefined symbols for architecture")),
    ("infra_hang", re.compile(r"lost communication with the server|exceeded the maximum "
                              r"execution time|The job has exceeded|timed out after|"
                              r"The operation was canceled")),
    ("infra_network", re.compile(r"Could not resolve host|Failed to connect to|"
                                 r"curl: \(\d+\)|TLS handshake timeout")),
    ("test_failure", re.compile(r"The following tests FAILED|tests failed out of|"
                                r"\s\d+ - \S+ \((Failed|Timeout|SEGFAULT|Subprocess aborted"
                                r"|Exception|Child aborted)\)")),
    ("compile_error", re.compile(r"error: |FAILED: \S+\.o\b")),
)
# ctest's summary line, "  12 - name (Failed)"; GitHub log lines carry a
# job/step/timestamp prefix, so the pattern is not anchored.
FAILED_TEST = re.compile(r"\s\d+ - (\S+) \((Failed|Timeout|SEGFAULT|Subprocess aborted"
                         r"|Exception|Child aborted)\)")


def parse_ts(s: str | None) -> dt.datetime | None:
    if not s:
        return None
    try:
        t = dt.datetime.fromisoformat(str(s).replace("Z", "+00:00"))
    except ValueError:
        return None
    return t if t.tzinfo else t.replace(tzinfo=dt.timezone.utc)


def _secs(a: str | None, b: str | None) -> float | None:
    ta, tb = parse_ts(a), parse_ts(b)
    return (tb - ta).total_seconds() if ta and tb else None


def job_class(labels: Iterable[str]) -> str:
    """The class token of a runs-on label set: what is left after the base set."""
    extra = sorted(set(labels or ()) - BASE_LABELS)
    return ",".join(extra) if extra else "none"


def physical_host(runner_name: str | None) -> str | None:
    if not runner_name or runner_name.startswith("GitHub Actions"):
        return None
    head = runner_name.split("-", 1)[0].lower()
    return {"studio": "m3"}.get(head, head)


def self_hosted(job: dict) -> bool:
    return "self-hosted" in (job.get("labels") or [])


# --------------------------------------------------------------------------
# windows
# --------------------------------------------------------------------------

class Windows:
    """before = [since, split); after = [after_from or split, until); minus exclusions."""

    def __init__(self, since: dt.datetime, split: dt.datetime,
                 after_from: dt.datetime | None = None, until: dt.datetime | None = None,
                 exclude: Iterable[tuple[dt.datetime, dt.datetime]] = ()):
        self.since, self.split = since, split
        self.after_from = after_from or split
        self.until = until
        self.exclude = list(exclude)

    def side(self, ts: str | dt.datetime | None) -> str | None:
        t = parse_ts(ts) if not isinstance(ts, dt.datetime) else ts
        if t is None:
            return None
        if any(lo <= t < hi for lo, hi in self.exclude):
            return None
        if self.since <= t < self.split:
            return "before"
        if t >= self.after_from and (self.until is None or t < self.until):
            return "after"
        return None

    def excluded(self, ts: str | None) -> bool:
        t = parse_ts(ts)
        return bool(t) and any(lo <= t < hi for lo, hi in self.exclude)


# --------------------------------------------------------------------------
# statistics
# --------------------------------------------------------------------------

def _boot(values: list[float], fn: Callable[[list[float]], float], seed: int = 7,
          n: int = 1000) -> tuple[float, float] | None:
    if len(values) < 2:
        return None
    rng = random.Random(seed)
    stats = sorted(fn([rng.choice(values) for _ in values]) for _ in range(n))
    return stats[int(0.025 * n)], stats[int(0.975 * n) - 1]


def _mean(v: list[float]) -> float:
    return sum(v) / len(v) if v else float("nan")


def _p50(v: list[float]) -> float:
    s = sorted(v)
    if not s:
        return float("nan")
    m = len(s) // 2
    return s[m] if len(s) % 2 else (s[m - 1] + s[m]) / 2


def side_value(units: list[float], stat: str = "mean") -> dict:
    """One side of a row: n units, the statistic and its bootstrap 95% CI."""
    fn = _p50 if stat == "p50" else _mean
    out: dict[str, Any] = {"n": len(units)}
    if not units:
        out["value"] = None
        return out
    out["value"] = fn(units)
    out["ci"] = _boot(units, fn)
    return out


def verdict(before: dict, after: dict, control: int, lower_is_better: bool = True) -> str:
    if control <= 0:
        return "INSTRUMENT BLIND (control is zero)"
    if before["n"] < MIN_UNITS or after["n"] < MIN_UNITS:
        return f"insufficient sample (n {before['n']} / {after['n']}, need {MIN_UNITS})"
    bci, aci = before.get("ci"), after.get("ci")
    if not bci or not aci:
        return "insufficient sample"
    if aci[1] < bci[0]:
        return "lower (CIs disjoint)" + (", better" if lower_is_better else ", worse")
    if aci[0] > bci[1]:
        return "higher (CIs disjoint)" + (", worse" if lower_is_better else ", better")
    return "no separable change (CIs overlap)"


def row(key: str, mechanism: str, metric: str, source: str, units: dict[str, list[float]],
        control_name: str, control: int, stat: str = "mean",
        lower_is_better: bool = True, extra: dict | None = None) -> dict:
    b = side_value(units.get("before", []), stat)
    a = side_value(units.get("after", []), stat)
    return {"key": key, "mechanism": mechanism, "metric": metric, "source": source,
            "stat": stat, "before": b, "after": a,
            "control": {"name": control_name, "value": control},
            "verdict": verdict(b, a, control, lower_is_better), **(extra or {})}


# --------------------------------------------------------------------------
# proxies over GitHub jobs (pure)
# --------------------------------------------------------------------------

def gate_jobs(runs: list[dict]) -> list[dict]:
    """Every attempt's `macos` job from Build runs, with its run's event/PR attached."""
    out = []
    for r in runs:
        for j in r.get("jobs") or []:
            if j.get("name") == GATE_JOB:
                out.append({**j, "event": r.get("event"), "pr": r.get("pr"),
                            "run_id": r.get("id"), "head_sha": r.get("head_sha")})
    return out


def release_placement(release_runs: list[dict], mints: dict[str, str], win: Windows) -> dict:
    units: dict[str, list[float]] = {"before": [], "after": []}
    joined = unjoined = unclassified = 0
    for r in release_runs:
        for j in r.get("jobs") or []:
            if not self_hosted(j) or not j.get("runner_name"):
                continue
            side = win.side(j.get("created_at"))
            if not side:
                continue
            want = job_class(j.get("labels"))
            got = mints.get(j["runner_name"])
            if got is None:
                unjoined += 1
                continue
            joined += 1
            if want == "none":
                unclassified += 1
            ok = want != "none" and set(want.split(",")) <= set(got.split(","))
            units[side].append(1.0 if ok else 0.0)
    return row("release_placement", "release jobs run on a runner minted for their class "
               "(tartci release classes)", "share of self-hosted release jobs whose class "
               "labels are all on the runner's mint labels (no class label = mismatch: it "
               "took whatever gate runner came)",
               "GitHub release-workflow jobs (labels, runner_name) joined to tartci mint_jit",
               units, "release jobs joined to a mint event", joined, lower_is_better=False,
               extra={"unjoined": unjoined, "unclassified": unclassified})


def unserved_labels(all_jobs: list[dict], win: Windows, days: dict[str, float]) -> dict:
    served: dict[str, set[str]] = {"before": set(), "after": set()}
    never: dict[str, list[str]] = {"before": [], "after": []}
    for j in all_jobs:
        if not self_hosted(j):
            continue
        side = win.side(j.get("created_at"))
        if not side:
            continue
        key = ",".join(sorted(j.get("labels") or []))
        if j.get("runner_name"):
            served[side].add(key)
        elif j.get("status") == "completed":
            never[side].append(key)
    counts = {s: sum(1 for k in never[s] if k not in served[s]) for s in never}
    units = {s: [counts[s] / days[s]] if days.get(s) else [] for s in counts}
    r = row("unserved_labels", "every requested runs-on label set is advertised by some runner",
            "jobs per day that ended without a runner and whose exact label set no job in the "
            "window was served on", "GitHub jobs (labels, runner_name) of Build + release "
            "workflows", units, "distinct label sets served",
            len(served["before"] | served["after"]))
    r["before"].update(count=counts["before"], n=counts["before"])
    r["after"].update(count=counts["after"], n=counts["after"])
    r["verdict"] = ("INSTRUMENT BLIND (control is zero)" if not r["control"]["value"]
                    else f"count {counts['before']} → {counts['after']} (a count, not a rate)")
    return r


def pr_head_no_runner(gjobs: list[dict], win: Windows) -> dict:
    units: dict[str, list[float]] = {"before": [], "after": []}
    for j in gjobs:
        if j.get("event") != "pull_request" or j.get("status") != "completed":
            continue
        if not self_hosted(j) or j.get("conclusion") == "skipped":
            continue
        side = win.side(j.get("created_at"))
        if side:
            units[side].append(1.0 if (j.get("conclusion") == "cancelled"
                                       and not j.get("runner_name")) else 0.0)
    return row("pr_head_no_runner", "PR-head gate demand is served before a newer push "
               "supersedes it", "share of native PR-head `macos` jobs cancelled before any "
               "runner took them", "GitHub Build jobs (conclusion, runner_name)", units,
               "native PR-head `macos` jobs", sum(len(v) for v in units.values()))


def queue_depths(jobs: list[dict]) -> list[tuple[dict, int]]:
    """(job, jobs ahead) for every runner-assigned self-hosted job.

    Jobs ahead = other self-hosted jobs created before it and still unstarted
    (or still queued when cancelled) at its creation.
    """
    iv = []
    for j in jobs:
        if not self_hosted(j):
            continue
        c = parse_ts(j.get("created_at"))
        s = parse_ts(j.get("started_at")) if j.get("runner_name") else parse_ts(j.get("completed_at"))
        if c and s:
            iv.append((c, s, j))
    iv.sort(key=lambda x: x[0])
    out = []
    for i, (c, s, j) in enumerate(iv):
        if not j.get("runner_name"):
            continue
        ahead = sum(1 for c2, s2, _ in iv[:i] if s2 > c)
        out.append((j, ahead))
    return out


def wait_per_job_ahead(jobs: list[dict], win: Windows) -> dict:
    units: dict[str, list[float]] = {"before": [], "after": []}
    deep = 0
    zero: dict[str, list[float]] = {"before": [], "after": []}
    for j, ahead in queue_depths(jobs):
        if j.get("name") != GATE_JOB:
            continue
        side = win.side(j.get("created_at"))
        wait = _secs(j.get("created_at"), j.get("started_at"))
        if not side or wait is None or wait < 0:
            continue
        units[side].append(wait / 60.0 / (ahead + 1))
        deep += ahead > 0
        if ahead == 0:
            zero[side].append(wait / 60.0)
    return row("wait_per_job_ahead", "a slot turns over as soon as a job is ahead of it",
               "p50 minutes of queue wait per queue position (wait / (jobs ahead + 1)) for "
               "runner-assigned `macos` jobs", "GitHub jobs (created_at, started_at) of Build + "
               "release workflows on self-hosted runners", units,
               "gate jobs that had ≥1 job ahead", deep, stat="p50",
               extra={"wait_at_depth_0": {s: side_value(v, "p50") for s, v in zero.items()}})


def refresh_cancels(runs: list[dict], commits: dict[str, dict], merged: list[dict],
                    win: Windows) -> dict:
    """Gate runs cancelled by a push that merged main in, per merged PR."""
    by_branch: dict[str, list[dict]] = {}
    for r in runs:
        if r.get("event") == "pull_request" and r.get("head_branch"):
            by_branch.setdefault(r["head_branch"], []).append(r)
    superseded = 0
    refresh = {"before": 0, "after": 0}
    per_pr: dict[int, int] = {}
    for rs in by_branch.values():
        rs.sort(key=lambda r: r.get("created_at") or "")
        for i, r in enumerate(rs):
            gate = [j for j in r.get("jobs") or [] if j.get("name") == GATE_JOB and self_hosted(j)]
            if not gate or not any(j.get("conclusion") == "cancelled" for j in gate):
                continue
            nxt = next((x for x in rs[i + 1:] if x.get("head_sha") != r.get("head_sha")), None)
            if nxt is None:
                continue
            superseded += 1
            c = commits.get(nxt["head_sha"]) or {}
            if len(c.get("parents") or []) >= 2 and REFRESH_SUBJECT.search(c.get("subject") or ""):
                side = win.side(r.get("created_at"))
                if side:
                    refresh[side] += 1
                if r.get("pr"):
                    per_pr[r["pr"]] = per_pr.get(r["pr"], 0) + 1
    units: dict[str, list[float]] = {"before": [], "after": []}
    for p in merged:
        side = win.side(p.get("mergedAt"))
        if side:
            units[side].append(float(per_pr.get(p["number"], 0)))
    return row("refresh_cancels", "a PR's gate is not restarted by merging main into a PR that "
               "is only behind (refresh_branch = only-if-conflicting)", "native PR-head gate runs "
               "cancelled by a push whose head merges main in, per merged PR",
               "GitHub Build runs (cancelled macos job, head_sha change) + the superseding "
               "commit's parents and subject", units,
               "cancelled gate runs superseded by any new head", superseded,
               extra={"refresh_cancelled_runs": refresh})


def mq_attempts(merged: list[dict], win: Windows) -> dict:
    units: dict[str, list[float]] = {"before": [], "after": []}
    with_enqueue = 0
    for p in merged:
        side = win.side(p.get("mergedAt"))
        if not side:
            continue
        n = sum(1 for e in p.get("events") or []
                if e.get("type") == "AddedToMergeQueueEvent" and e["at"] <= p["mergedAt"])
        if n:
            with_enqueue += 1
            units[side].append(float(n))
    return row("mq_attempts", "a PR enters the queue once and lands", "merge-queue entries "
               "per merged PR", "GitHub PR timeline (AddedToMergeQueueEvent)", units,
               "merged PRs with ≥1 queue entry", with_enqueue)


def classify_log(text: str, at: str | None, win: Windows, read: bool = True) -> str:
    if not read:
        return "log_unavailable"
    for cause, rx in LOG_CAUSES:
        if rx.search(text or ""):
            if cause == "link_error" and win.excluded(at):
                return "infra_host_cache"
            if cause == "test_failure":
                names = sorted(set(m.group(1) for m in FAILED_TEST.finditer(text)))
                return "test_failure:" + ",".join(names[:3]) if names else cause
            return cause
    return "unclassified"


def ejection_causes(merged: list[dict], runs: list[dict], causes: dict[str, str],
                    win: Windows) -> dict[str, dict[str, int]]:
    """Removals before merge, by reason; failed_checks refined by the failing job's log.

    A failed_checks removal is attributed to the failed merge-group `macos` job
    whose queue branch names this PR and was created during that queue stay;
    with none, the cause is `neighbour_or_non_macos` (another entry's group
    failed, or a different required check did). Removals inside an exclusion
    window are counted apart, under `excluded`, never dropped silently.
    """
    mg: dict[int, list[dict]] = {}
    for r in runs:
        if r.get("event") == "merge_group" and r.get("pr"):
            mg.setdefault(r["pr"], []).append(r)
    out: dict[str, dict[str, int]] = {"before": {}, "after": {}, "excluded": {}}
    for p in merged:
        enq = None
        for e in sorted(p.get("events") or [], key=lambda e: e["at"]):
            if e["type"] == "AddedToMergeQueueEvent":
                enq = e["at"]
                continue
            if e["type"] != "RemovedFromMergeQueueEvent" or e["at"] > p["mergedAt"]:
                continue
            reason = (e.get("reason") or "unknown").lower()
            if reason == "merged":
                continue
            side = win.side(e["at"]) or ("excluded" if win.excluded(e["at"]) else None)
            if side is None:
                continue
            cause = _failed_cause(p, mg, enq, e, causes) if reason == "failed_checks" else reason
            out[side][cause] = out[side].get(cause, 0) + 1
    return out


def _failed_cause(p, mg, enq, e, causes) -> str:
    for r in mg.get(p["number"], []):
        if enq and not (enq <= (r.get("created_at") or "") <= e["at"]):
            continue
        for j in r.get("jobs") or []:
            if j.get("name") == GATE_JOB and j.get("conclusion") in ("failure", "timed_out"):
                return causes.get(str(j["id"]), "log_unavailable")
    return "neighbour_or_non_macos"


def gate_runs(runs: list[dict], merged: list[dict], win: Windows) -> dict:
    by_pr: dict[int, dict[str, int]] = {}
    for j in gate_jobs(runs):
        if not self_hosted(j) or not j.get("pr") or not j.get("runner_name"):
            continue
        b = by_pr.setdefault(j["pr"], {"pr_head": 0, "merge_group": 0, "wasted": 0})
        ev = "pr_head" if j.get("event") == "pull_request" else "merge_group"
        b[ev] += 1
        if j.get("conclusion") == "cancelled" or (
                ev == "merge_group" and j.get("conclusion") in ("failure", "timed_out")):
            b["wasted"] += 1
    rows = {}
    control = 0
    for part in ("pr_head", "merge_group", "wasted"):
        units: dict[str, list[float]] = {"before": [], "after": []}
        for p in merged:
            side = win.side(p.get("mergedAt"))
            if not side or parse_ts(p.get("createdAt")) is None or (
                    parse_ts(p["createdAt"]) < win.since):
                continue
            c = by_pr.get(p["number"], {})
            units[side].append(float(c.get(part, 0)))
            if part == "pr_head" and sum(c.values()):
                control += 1
        rows[part] = units
    out = []
    for part, units in rows.items():
        out.append(row(f"gate_runs_{part}", "a merged PR costs as few native gate runs as "
                       "possible", f"native `macos` runs per merged PR: {part.replace('_', '-')} "
                       "(runner-assigned attempts; wasted = cancelled after a runner took it, "
                       "or a failed merge group)", "GitHub Build jobs of PRs opened and merged "
                       "in the window", units, "merged PRs with ≥1 native gate run", control))
    return {"rows": out}


def base_red(runs: list[dict], digests: dict[str, dict], win: Windows) -> dict:
    """Merge-group `macos` failures that main itself carried, per `macos` failure.

    A merge group is main plus its entries, so when main fails a test every
    group inherits it. That shows up as a streak: consecutive merge-group
    `macos` jobs (by creation) that fail sharing a test, with no executed pass
    between them, on two or more hosts. Every member of such a streak counts.
    A streak on one host is that host's problem (a poisoned cache, a
    host-sensitive test), not main's, so it does not count.

    Only self-hosted jobs that ran are in the sequence: a hosted placeholder
    or a cancelled job observed nothing. A failure whose log names no test (a
    compile, link or infrastructure failure) neither extends nor breaks a
    streak, but it is in the denominator. The control is the test failures
    outside any cross-host streak (unique causes): a zero there means the log
    reader saw no single failure, so the ratio cannot be trusted.
    """
    seq = sorted((j for j in gate_jobs(runs)
                  if j.get("event") == "merge_group" and physical_host(j.get("runner_name"))
                  and j.get("conclusion") in ("success", "failure", "timed_out")),
                 key=lambda j: (j.get("created_at") or "", j.get("id") or 0))
    tests: dict[Any, frozenset[str]] = {}
    unread = 0
    for j in seq:
        if j["conclusion"] == "success":
            continue
        d = digests.get(str(j["id"])) or {}
        if not d.get("read", False):
            unread += 1
        tests[j["id"]] = frozenset(parse_failing_tests(d.get("digest", "")))

    streaks: list[dict] = []
    cur: dict | None = None
    for j in seq:
        if j["conclusion"] == "success":
            cur = None
            continue
        names = tests[j["id"]]
        if not names:
            continue
        if cur and cur["shared"] & names:
            cur["shared"] = cur["shared"] & names
            cur["members"].append(j)
        else:
            cur = {"shared": names, "members": [j]}
            streaks.append(cur)
    red: set[Any] = set()
    summary: dict[str, list[str]] = {"before": [], "after": []}
    for st in streaks:
        hosts = {physical_host(m.get("runner_name")) for m in st["members"]}
        if len(st["members"]) < 2 or len(hosts) < 2:
            continue
        red |= {m["id"] for m in st["members"]}
        side = win.side(st["members"][0].get("created_at"))
        if side:
            summary[side].append(f"{','.join(sorted(st['shared'])[:2])} x{len(st['members'])} "
                                 f"on {'/'.join(sorted(hosts))}")

    units: dict[str, list[float]] = {"before": [], "after": []}
    unique = 0
    for j in seq:
        if j["conclusion"] == "success":
            continue
        side = win.side(j.get("created_at"))
        if not side:
            continue
        units[side].append(1.0 if j["id"] in red else 0.0)
        if tests[j["id"]] and j["id"] not in red:
            unique += 1
    return row("base_red", "main is kept green, so no merge group inherits a failure from "
               "its base", "merge-group `macos` failures inside a cross-host streak sharing "
               "a failing test, per merge-group `macos` failure", "failed merge-group "
               "`macos` job logs (ctest FAILED block) + job runner/creation order", units,
               "failures with a unique cause (a named test, no cross-host streak)", unique,
               extra={"streaks": summary, "logs_unread": unread})


# --------------------------------------------------------------------------
# proxies over tartci supervisor events (pure)
# --------------------------------------------------------------------------

def vm_lifecycles(events: list[dict]) -> dict[str, dict]:
    """vm → {host, lane, clone, boot, mint, assigned, end, labels}.

    tartci logs `clone_start` before the VM is named (its `vm` is empty), so a
    clone is attached to the next named VM of the same host/lane/runner. A
    clone followed by another clone with no named VM between is an attempt
    that never produced a VM; it is kept under a synthetic `clone-only:` key.
    """
    vms: dict[str, dict] = {}
    pending: dict[tuple, str] = {}
    for e in sorted(events, key=lambda e: str(e.get("ts") or "")):
        key = (e.get("host"), e.get("lane"), e.get("runner"))
        kind, ts, vm = e.get("event"), e.get("ts"), e.get("vm")
        if kind == "clone_start" and not vm:
            if key in pending:
                vms[f"clone-only:{key}:{pending[key]}"] = {
                    "host": key[0], "lane": key[1], "clone": pending[key]}
            pending[key] = ts
            continue
        if not vm:
            continue
        v = vms.setdefault(vm, {"host": e.get("host"), "lane": e.get("lane")})
        # Any named event means the VM came up (runner_version, aqua_preflight
        # and admission_check precede mint_jit and boot_ok).
        v.setdefault("up", ts)
        if key in pending and "clone" not in v:
            v["clone"] = pending.pop(key)
        if kind == "clone_start":
            v.setdefault("clone", ts)
        elif kind == "boot_ok":
            v.setdefault("boot", ts)
        elif kind == "mint_jit":
            v.setdefault("mint", ts)
            m = re.search(r"labels=(\S+)", e.get("detail") or "")
            if m:
                v["labels"] = m.group(1).split(",")
        elif kind == "job_assigned":
            v.setdefault("assigned", ts)
        elif kind in ("teardown", "teardown_incomplete", "idle_timeout"):
            v["end"] = max(v.get("end") or ts, ts)
    for key, ts in pending.items():
        vms[f"clone-only:{key}:{ts}"] = {"host": key[0], "lane": key[1], "clone": ts}
    return vms


def mint_classes(events: list[dict]) -> dict[str, str]:
    return {vm: ",".join(sorted(set(v["labels"]) - BASE_LABELS)) or "none"
            for vm, v in vm_lifecycles(events).items() if v.get("labels")}


def event_host_rows(events: list[dict], win: Windows) -> list[dict]:
    """Per host: VMs discarded per job served, and unminted boots per job served."""
    out = []
    hosts = sorted({e.get("host") for e in events if e.get("host")})
    vms = vm_lifecycles(events)
    for host in hosts:
        disc: dict[str, list[float]] = {"before": [], "after": []}
        unminted: dict[str, list[float]] = {"before": [], "after": []}
        per_job: dict[str, list[int]] = {"before": [0, 0], "after": [0, 0]}
        served = 0
        for vm, v in vms.items():
            if v.get("host") != host:
                continue
            side = win.side(v.get("clone") or v.get("up"))
            if not side:
                continue
            if v.get("assigned"):
                served += 1
                per_job[side][1] += 1
            if v.get("up"):
                disc[side].append(0.0 if v.get("assigned") else 1.0)
                per_job[side][0] += 0 if v.get("assigned") else 1
            if v.get("clone"):
                unminted[side].append(0.0 if v.get("mint") else 1.0)
        ratio = {s: (round(d / j, 2) if j else None) for s, (d, j) in per_job.items()}
        out.append(row(f"vm_discard[{host}]", "a VM is booted only for a job it will serve "
                       "(job claim, admission before boot)", "share of VMs that came up "
                       "(any named event) and were torn down without serving a job",
                       f"tartci events.jsonl on {host} (runner_version/admission_check/boot_ok, "
                       "job_assigned)", disc, "VMs that served a job", served,
                       extra={"discarded_per_job_served": ratio}))
        out.append(row(f"unminted_boots[{host}]", "a clone becomes a runner (admission and "
                       "lease checks before the clone, not after)", "share of VM clones that "
                       "never minted a runner", f"tartci events.jsonl on {host} (clone_start, "
                       "mint_jit)", unminted, "VMs that served a job", served))
    return out


def lane_intervals(events: list[dict]) -> dict[tuple[str, str], dict]:
    """(host, lane) → busy intervals, event times (liveness), denial times, classes."""
    lanes: dict[tuple[str, str], dict] = {}
    for e in events:
        key = (e.get("host"), e.get("lane"))
        ln = lanes.setdefault(key, {"busy": [], "seen": [], "denied": [], "classes": set()})
        t = parse_ts(e.get("ts"))
        if t:
            ln["seen"].append(t)
            if e.get("event") in ATTEMPT_EVENTS:
                ln["denied"].append(t)
    for vm, v in vm_lifecycles(events).items():
        ln = lanes.setdefault((v.get("host"), v.get("lane")),
                              {"busy": [], "seen": [], "denied": [], "classes": set()})
        a, b = parse_ts(v.get("clone") or v.get("boot")), parse_ts(v.get("end"))
        if a and b and b >= a:
            ln["busy"].append((a, b))
        if v.get("labels"):
            ln["classes"].add(job_class(v["labels"]))
    for ln in lanes.values():
        ln["busy"].sort()
        ln["seen"].sort()
        ln["denied"].sort()
    return lanes


def _idle_windows(ln: dict, lo: dt.datetime, hi: dt.datetime) -> list[tuple[dt.datetime, dt.datetime]]:
    """Sub-intervals of [lo, hi) where the lane had no VM and its supervisor was live."""
    free, cur = [], lo
    starts = ln.setdefault("_starts", [a for a, _ in ln["busy"]])
    first = bisect.bisect_left(starts, lo - MAX_VM_LIFETIME)
    for a, b in ln["busy"][first:bisect.bisect_left(starts, hi)]:
        if b <= cur:
            continue
        if a > cur:
            free.append((cur, a))
        cur = max(cur, b)
    if cur < hi:
        free.append((cur, hi))
    live = []
    for a, b in free:
        seen = ln["seen"][bisect.bisect_left(ln["seen"], a - dt.timedelta(seconds=LIVE_GAP_SECONDS)):
                          bisect.bisect_right(ln["seen"], b)]
        if not seen:
            continue
        pts = [a] + [t for t in seen if a <= t <= b] + [b]
        if max((y - x).total_seconds() for x, y in zip(pts, pts[1:])) <= LIVE_GAP_SECONDS:
            live.append((a, b))
    return live


def _host_busy(lanes: dict, host: str, t: dt.datetime) -> int:
    return sum(1 for (h, _), ln in lanes.items() if h == host
               for a, b in ln["busy"] if a <= t < b)


def idle_with_demand(events: list[dict], gjobs: list[dict], win: Windows) -> list[dict]:
    """Gate jobs that waited ≥ IDLE_MIN_SECONDS while an eligible live lane was idle.

    Eligible: the lane has minted this job's class before and its host ran
    fewer than HOST_VM_LIMIT VMs at the idle window's start. The window is
    idle-with-demand when the lane logged no attempt (ATTEMPT_EVENTS) inside
    it: it neither tried to boot nor knew the job was covered. A window with
    attempts but no VM is `blocked` (a lease, disk or admission refusal),
    counted in its own row, because capacity and assignment are different
    mechanisms. One event per (job, host) for each.
    """
    lanes = lane_intervals(events)
    hosts = sorted({h for h, _ in lanes if h})
    per_host = {h: {"before": [], "after": []} for h in hosts}
    blocked = {h: {"before": [], "after": []} for h in hosts}
    idle_found = 0
    for j in gjobs:
        if not self_hosted(j) or j.get("status") != "completed":
            continue
        side = win.side(j.get("created_at"))
        c = parse_ts(j.get("created_at"))
        s = parse_ts(j.get("started_at")) if j.get("runner_name") else parse_ts(j.get("completed_at"))
        if not side or not c or not s or s <= c:
            continue
        want = set(job_class(j.get("labels")).split(","))
        hit_hosts, blocked_hosts = set(), set()
        for (host, _lane), ln in lanes.items():
            if not host or not any(want <= set(c.split(",")) for c in ln["classes"]):
                continue
            for a, b in _idle_windows(ln, c, s):
                if (b - a).total_seconds() < IDLE_MIN_SECONDS:
                    continue
                idle_found += 1
                if _host_busy(lanes, host, a) >= HOST_VM_LIMIT:
                    continue
                lo_i = bisect.bisect_left(ln["denied"], a)
                if lo_i < len(ln["denied"]) and ln["denied"][lo_i] < b:
                    blocked_hosts.add(host)
                    continue
                hit_hosts.add(host)
                break
        for h in hosts:
            per_host[h][side].append(1.0 if h in hit_hosts else 0.0)
            blocked[h][side].append(1.0 if h in blocked_hosts and h not in hit_hosts else 0.0)
    first_precheck = {}
    for e in events:
        if e.get("event") == "admission_precheck" and e.get("host"):
            h = e["host"]
            first_precheck[h] = min(first_precheck.get(h) or e["ts"], e["ts"])
    rows = []
    for h in hosts:
        cover = {"attempt_events_from": first_precheck.get(h),
                 "before_side_blind_to_attempts": bool(
                     first_precheck.get(h) is None
                     or parse_ts(first_precheck[h]) > win.since)}
        rows.append(row(f"idle_with_demand[{h}]", "an idle, eligible slot takes waiting work "
                        "(work-conserving assignment)", f"share of queued native gate jobs that "
                        f"waited ≥{IDLE_MIN_SECONDS}s while an eligible live lane on {h} had no VM "
                        "and made no boot attempt", f"tartci events.jsonl on {h} (VM lifetimes, "
                        "liveness, attempt events) + GitHub job queue intervals", per_host[h],
                        "lane idle windows overlapping a queued job", idle_found, extra=cover))
        rows.append(row(f"blocked_with_demand[{h}]", "a lane that tries to serve waiting work "
                        "gets a VM (lease, disk, admission capacity)", f"share of queued native "
                        f"gate jobs that waited ≥{IDLE_MIN_SECONDS}s while an eligible lane on {h} "
                        "kept attempting without a VM", f"tartci events.jsonl on {h}", blocked[h],
                        "lane idle windows overlapping a queued job", idle_found, extra=cover))
    return rows


# --------------------------------------------------------------------------
# context (wall time; reported beside the proxies, never the verdict)
# --------------------------------------------------------------------------

def tree_identical_groups(merged: list[dict], commits: dict[str, dict], win: Windows) -> dict:
    """Share of merged PRs whose merge commit has the same tree as the PR head.

    A merge group is built on the merge commit; an exact-tree receipt from the
    PR head can only be reused when that tree equals the head's, i.e. main did
    not move under the PR. This is the ceiling on whole-job receipt reuse (G5),
    read from commit trees, not from reuse verdicts."""
    units: dict[str, list[float]] = {"before": [], "after": []}
    resolved = 0
    for p in merged:
        side = win.side(p.get("mergedAt"))
        mc = commits.get(p.get("mergeCommit") or "") or {}
        parents = mc.get("parents") or []
        if not side or len(parents) != 2 or not mc.get("tree"):
            continue
        head = commits.get(parents[1]) or {}
        if not head.get("tree"):
            continue
        resolved += 1
        units[side].append(1.0 if head["tree"] == mc["tree"] else 0.0)
    return row("tree_identical_groups", "a merge group's tree equals its PR head's, so an "
               "exact-tree PR-head receipt could serve it (the G5 reuse ceiling)",
               "merged PRs whose merge-commit tree == PR-head tree, share",
               "GitHub commits API (merge commit + second parent trees)", units,
               "merged PRs with both trees resolved", resolved, lower_is_better=False)


def context_rows(runs: list[dict], merged: list[dict], win: Windows) -> list[dict]:
    lat: dict[str, list[float]] = {"before": [], "after": []}
    for p in merged:
        side = win.side(p.get("mergedAt"))
        enq = [e["at"] for e in p.get("events") or []
               if e["type"] == "AddedToMergeQueueEvent" and e["at"] <= p["mergedAt"]]
        if side and enq:
            lat[side].append(_secs(max(enq), p["mergedAt"]) / 60.0)
    gate: dict[str, list[float]] = {"before": [], "after": []}
    for j in gate_jobs(runs):
        if j.get("event") == "merge_group" and j.get("conclusion") == "success" and self_hosted(j):
            side = win.side(j.get("created_at"))
            d = _secs(j.get("started_at"), j.get("completed_at"))
            if side and d:
                gate[side].append(d / 60.0)
    return [{"key": "ctx_last_enqueue_to_merged", "metric": "last enqueue → merged, p50 min",
             "before": side_value(lat["before"], "p50"), "after": side_value(lat["after"], "p50")},
            {"key": "ctx_merge_group_gate", "metric": "merge-group `macos` job, p50 min",
             "before": side_value(gate["before"], "p50"), "after": side_value(gate["after"], "p50")}]


def log_causes(data: dict, win: Windows) -> dict[str, str]:
    """Failed merge-group job id → cause. Classified per report, not cached,
    because a link error's cause depends on the report's exclusion windows."""
    return {jid: classify_log(d.get("digest", ""), d.get("at"), win, d.get("read", True))
            for jid, d in (data.get("log_digests") or {}).items()}


def attach_prs(runs: list[dict], merged: list[dict]) -> int:
    """Give each pull_request run its PR number from its head branch.

    GitHub leaves `pull_requests` empty on runs of App-opened PRs (most of
    this repo's), so the head branch is the join key: the merged PR whose
    headRefName it is, the latest one when a branch was reused, and only a
    run created before that PR merged. Returns how many runs gained a PR.
    """
    by_branch: dict[str, list[dict]] = {}
    for p in merged:
        if p.get("headRefName"):
            by_branch.setdefault(p["headRefName"], []).append(p)
    gained = 0
    for r in runs:
        if r.get("event") != "pull_request" or r.get("pr"):
            continue
        cands = [p for p in by_branch.get(r.get("head_branch") or "", [])
                 if (r.get("created_at") or "") <= p["mergedAt"]]
        if cands:
            r["pr"] = min(cands, key=lambda p: p["mergedAt"])["number"]
            gained += 1
    return gained


def compute(data: dict, win: Windows) -> dict:
    runs, release = data.get("runs", []), data.get("release_runs", [])
    attach_prs(runs, data.get("merged", []))
    merged, events = data.get("merged", []), data.get("events", [])
    gj = gate_jobs(runs)
    all_jobs = gj + [j for r in release for j in r.get("jobs") or []]
    hours = {"before": (win.split - win.since).total_seconds() / 86400}
    end = win.until or parse_ts(data.get("collected_at")) or dt.datetime.now(dt.timezone.utc)
    hours["after"] = max((end - win.after_from).total_seconds() / 86400, 0)
    for lo, hi in win.exclude:
        for s, (a, b) in (("before", (win.since, win.split)), ("after", (win.after_from, end))):
            ov = (min(hi, b) - max(lo, a)).total_seconds()
            if ov > 0:
                hours[s] -= ov / 86400
    rows = [release_placement(release, mint_classes(events), win),
            unserved_labels(all_jobs, win, hours),
            *idle_with_demand(events, gj, win),
            *event_host_rows(events, win),
            pr_head_no_runner(gj, win),
            wait_per_job_ahead(all_jobs, win),
            refresh_cancels(runs, data.get("commits", {}), merged, win),
            mq_attempts(merged, win),
            tree_identical_groups(merged, data.get("commits", {}), win),
            *gate_runs(runs, merged, win)["rows"],
            base_red(runs, data.get("log_digests") or {}, win)]
    return {"windows": {"since": win.since.isoformat(), "split": win.split.isoformat(),
                        "after_from": win.after_from.isoformat(),
                        "until": win.until.isoformat() if win.until else None,
                        "exclude": [[a.isoformat(), b.isoformat()] for a, b in win.exclude],
                        "days": {k: round(v, 2) for k, v in hours.items()}},
            "hosts_read": data.get("hosts_read", {}),
            "rows": rows,
            "ejections": ejection_causes(merged, runs, log_causes(data, win), win),
            "context": context_rows(runs, merged, win)}


# --------------------------------------------------------------------------
# rendering
# --------------------------------------------------------------------------

def _fmt_side(s: dict, stat: str) -> str:
    if s.get("value") is None:
        return f"n={s['n']} —"
    v = s["value"]
    txt = f"{v:.3f}" if abs(v) < 10 else f"{v:.1f}"
    if "count" in s:
        return f"{s['count']} ({v:.2f}/day)"
    ci = s.get("ci")
    ci_txt = f" [{ci[0]:.3f}, {ci[1]:.3f}]" if ci and abs(v) < 10 else (
        f" [{ci[0]:.1f}, {ci[1]:.1f}]" if ci else "")
    return f"n={s['n']} {stat} {txt}{ci_txt}"


def render_markdown(rep: dict) -> str:
    w = rep["windows"]
    lines = [f"### Load-independent proxies (before {w['since']} → {w['split']}; after "
             f"{w['after_from']} → {w['until'] or 'now'}; excluded {w['exclude'] or 'none'}; "
             f"days before/after {w['days']['before']}/{w['days']['after']})", "",
             "| Proxy | Mechanism | Metric | Source | Before | After | Control (must be > 0) "
             "| Verdict | Notes |", "|---|---|---|---|---|---|---|---|---|"]
    core = {"key", "mechanism", "metric", "source", "stat", "before", "after", "control",
            "verdict"}
    for r in rep["rows"]:
        notes = "; ".join(f"{k} {json.dumps(v, default=str)}" for k, v in r.items()
                          if k not in core)
        lines.append(f"| {r['key']} | {r['mechanism']} | {r['metric']} | {r['source']} | "
                     f"{_fmt_side(r['before'], r['stat'])} | {_fmt_side(r['after'], r['stat'])} | "
                     f"{r['control']['name']}: {r['control']['value']} | {r['verdict']} | "
                     f"{notes} |")
    lines += ["", "Ejections by cause (removals before merge):", ""]
    for side in ("before", "after", "excluded"):
        causes = rep["ejections"].get(side) or {}
        lines.append(f"- {side}: " + (", ".join(f"{k} {v}" for k, v in
                                               sorted(causes.items(), key=lambda x: -x[1]))
                                      or "none"))
    lines += ["", "Context only (wall time tracks load; not the verdict):", ""]
    for c in rep["context"]:
        lines.append(f"- {c['metric']}: before {_fmt_side(c['before'], 'p50')}; "
                     f"after {_fmt_side(c['after'], 'p50')}")
    reads = rep.get("hosts_read") or {}
    if reads:
        lines += ["", "tartci events read: " + ", ".join(f"{h} {v}" for h, v in sorted(reads.items()))]
    return "\n".join(lines)


# --------------------------------------------------------------------------
# collection (I/O)
# --------------------------------------------------------------------------

JOB_FIELDS = ("id", "name", "status", "conclusion", "created_at", "started_at", "completed_at",
              "runner_name", "labels", "run_attempt")


def _slim_job(j: dict) -> dict:
    return {k: j.get(k) for k in JOB_FIELDS}


def _run_pr(run: dict) -> int | None:
    prs = run.get("pull_requests") or []
    if prs:
        return prs[0].get("number")
    m = re.search(r"gh-readonly-queue/.+/pr-(\d+)-", run.get("head_branch") or "")
    return int(m.group(1)) if m else None


def _slim_run(r: dict, jobs: list[dict]) -> dict:
    return {"id": r["id"], "event": r.get("event"), "head_sha": r.get("head_sha"),
            "head_branch": r.get("head_branch"), "created_at": r.get("created_at"),
            "conclusion": r.get("conclusion"), "pr": _run_pr(r),
            "jobs": [_slim_job(j) for j in jobs]}


def read_host_events(alias: str, target: str, since: dt.datetime,
                     ssh: Callable[..., subprocess.CompletedProcess]) -> tuple[list[dict], str]:
    """All lanes' tartci supervisor events on one host since `since` (read-only)."""
    root = ".tartci/state/macos-fleet"
    cutoff = since.strftime("%Y-%m-%dT%H:%M:%S")
    try:
        if target == "local":
            base = Path.home() / root
            lanes = sorted(p.name for p in base.iterdir() if (p / "events.jsonl").exists())
            texts = {ln: (base / ln / "events.jsonl").read_text(errors="replace") for ln in lanes}
        else:
            proc = ssh(target, "ls", root)
            if proc.returncode != 0:
                return [], f"UNREACHABLE ({proc.stderr.strip()[:80]})"
            texts = {}
            for ln in proc.stdout.split():
                p = ssh(target, "cat", f"{root}/{ln}/events.jsonl", timeout=120)
                if p.returncode == 0:
                    texts[ln] = p.stdout
    except (OSError, subprocess.TimeoutExpired) as exc:
        return [], f"UNREACHABLE ({str(exc)[:80]})"
    out = []
    for lane, text in texts.items():
        for line in text.splitlines():
            try:
                e = json.loads(line)
            except ValueError:
                continue
            if str(e.get("ts") or "") >= cutoff:
                e["host"], e["lane"] = alias, lane
                out.append(e)
    return out, f"{len(out)} events / {len(texts)} lanes"


def _retry(fn: Callable, *a, attempts: int = 3, wait: float = 5.0):
    """A transient API/network failure is retried; the last error propagates."""
    import time
    for i in range(attempts):
        try:
            return fn(*a)
        except RuntimeError:
            if i == attempts - 1:
                raise
            time.sleep(wait * (i + 1))


def collect(gh, since: dt.datetime, hosts: Iterable[str], ssh: Callable,
            cache: dict | None = None, workers: int = 8,
            log: Callable[[str], None] = print) -> dict:
    """Sweep GitHub + tartci into one dict. `cache` (a prior collect) is reused for
    completed runs, commits and log classifications, which never change."""
    cache = cache or {}
    now = dt.datetime.now(dt.timezone.utc)
    days = [(since + dt.timedelta(days=i)).date() for i in range((now.date() - since.date()).days + 1)]
    old_runs = {r["id"]: r for r in cache.get("runs", []) + cache.get("release_runs", [])}

    def runs_of(workflow: str, events: tuple[str | None, ...]) -> list[dict]:
        out = []
        for day in days:
            for ev in events:
                out += [r for r in _retry(gh.runs_for_day, day, ev, workflow)
                        if r.get("status") == "completed"]
        return out

    def with_jobs(r: dict) -> dict:
        if r["id"] in old_runs:
            return old_runs[r["id"]]
        jobs = _retry(gh.api, f"repos/{gh.repo}/actions/runs/{r['id']}/jobs?per_page=100&filter=all")
        return _slim_run(r, jobs.get("jobs", []))

    build = runs_of("build.yml", ("pull_request", "merge_group"))
    release = [r for wf in RELEASE_WORKFLOWS for r in runs_of(wf, (None,))]
    log(f"proxies: {len(build)} Build runs, {len(release)} release runs since {since.date()}")
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        runs = list(pool.map(with_jobs, build))
        release_runs = list(pool.map(with_jobs, release))

    query = ("query($q:String!,$after:String){search(query:$q,type:ISSUE,first:100,"
             "after:$after){pageInfo{hasNextPage endCursor} nodes{... on PullRequest{"
             "number createdAt mergedAt headRefName mergeCommit{oid} timelineItems(first:100,itemTypes:"
             "[ADDED_TO_MERGE_QUEUE_EVENT,REMOVED_FROM_MERGE_QUEUE_EVENT]){nodes{__typename"
             " ... on AddedToMergeQueueEvent{createdAt}"
             " ... on RemovedFromMergeQueueEvent{createdAt reason}}}}}}}")
    merged = []
    for day in days:
        after = None
        while True:
            args = ["-f", f"q=repo:{gh.repo} is:pr is:merged merged:{day.isoformat()}",
                    "-f", f"query={query}"] + (["-f", f"after={after}"] if after else [])
            data = _retry(gh.api, "graphql", *args)["data"]["search"]
            for n in data["nodes"]:
                if n:
                    merged.append({"number": n["number"], "createdAt": n["createdAt"],
                                   "mergedAt": n["mergedAt"], "headRefName": n.get("headRefName"),
                                   "mergeCommit": (n.get("mergeCommit") or {}).get("oid"),
                                   "events": [{"type": e["__typename"], "at": e["createdAt"],
                                               "reason": e.get("reason")}
                                              for e in n["timelineItems"]["nodes"]]})
            if not data["pageInfo"]["hasNextPage"]:
                break
            after = data["pageInfo"]["endCursor"]

    commits = dict(cache.get("commits", {}))
    need = set()
    by_branch: dict[str, list[dict]] = {}
    for r in runs:
        if r["event"] == "pull_request" and r.get("head_branch"):
            by_branch.setdefault(r["head_branch"], []).append(r)
    for rs in by_branch.values():
        rs.sort(key=lambda r: r.get("created_at") or "")
        for i, r in enumerate(rs):
            if any(j.get("name") == GATE_JOB and j.get("conclusion") == "cancelled"
                   for j in r["jobs"]):
                nxt = next((x for x in rs[i + 1:] if x["head_sha"] != r["head_sha"]), None)
                if nxt and nxt["head_sha"] not in commits:
                    need.add(nxt["head_sha"])

    def commit(sha: str) -> tuple[str, dict]:
        c = _retry(gh.api, f"repos/{gh.repo}/commits/{sha}")
        return sha, {"parents": [p["sha"] for p in c.get("parents", [])],
                     "subject": (c.get("commit", {}).get("message") or "").splitlines()[0],
                     "tree": ((c.get("commit") or {}).get("tree") or {}).get("sha")}

    digests = dict(cache.get("log_digests", {}))
    failed = [j for r in runs if r["event"] == "merge_group" for j in r["jobs"]
              if j.get("name") == GATE_JOB and j.get("conclusion") in ("failure", "timed_out")
              and str(j["id"]) not in digests]

    def log_of(j: dict) -> tuple[str, dict]:
        proc = subprocess.run([gh.gh, "run", "view", "--job", str(j["id"]), "--log"],
                              cwd=gh.cwd, capture_output=True, text=True, timeout=180)
        text = proc.stdout if proc.returncode == 0 else ""
        return str(j["id"]), {"digest": _log_digest(text), "read": proc.returncode == 0,
                              "runner": j.get("runner_name"), "at": j.get("created_at")}

    # Merge commits (and, once known, their PR-head parents) carry the trees the
    # tree_identical_groups proxy compares. Cached rows without a tree are
    # re-read once.
    need |= {p["mergeCommit"] for p in merged
             if p.get("mergeCommit") and not (commits.get(p["mergeCommit"]) or {}).get("tree")}
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        for sha, c in pool.map(commit, sorted(need)):
            commits[sha] = c
        heads = {c["parents"][1] for p in merged
                 for c in [commits.get(p.get("mergeCommit") or "") or {}]
                 if len(c.get("parents") or []) == 2
                 and not (commits.get(c["parents"][1]) or {}).get("tree")}
        for sha, c in pool.map(commit, sorted(heads)):
            commits[sha] = c
        digests.update(pool.map(log_of, failed))
    log(f"proxies: {len(merged)} merged PRs, {len(need)} commits, {len(failed)} failed-job logs")

    events, reads = [], {}
    for h in hosts:
        alias, _, target = h.partition("=")
        ev, status = read_host_events(alias, target or alias, since, ssh)
        events += ev
        reads[alias] = status
    return {"collected_at": now.isoformat(), "since": since.isoformat(), "runs": runs,
            "release_runs": release_runs, "merged": merged, "commits": commits,
            "log_digests": digests, "events": events, "hosts_read": reads}


def _log_digest(text: str) -> str:
    """The lines a cause is read from: failures, errors and ctest's failed list.

    ctest's FAILED block comes last and the error lines of a failing test's
    output can fill the cap before it, so the block is always kept in full.
    """
    lines = text.splitlines()
    keep = [ln for ln in lines
            if any(rx.search(ln) for _, rx in LOG_CAUSES) or FAILED_TEST.search(ln)][:200]
    kept = set(keep)
    block = [ln for ln in lines if ln not in kept and (
        "The following tests FAILED" in ln or FAILED_TEST_LINE_RE.search(ln))]
    return "\n".join(keep + block)
