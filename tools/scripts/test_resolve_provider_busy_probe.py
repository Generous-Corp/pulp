#!/usr/bin/env python3
"""Tests for build.yml's resolve-provider macOS busy probe.

The probe (`_count_busy_local_mac_runners`) decides whether a PR's macOS
Build-and-Test leg runs on the local self-hosted M1 runners or overflows
to github-hosted `macos-15`. It must count ONLY macOS jobs that are
actually occupying a local M1 right now — a job whose
`status == "in_progress"` AND whose `labels` include the local
self-hosted label.

Regression covered (pulp #2467 class): an earlier cut enumerated
`in_progress` + `queued` runs and PESSIMISTICALLY counted a
not-yet-registered macOS job as local-busy. During a deep Actions queue
(~250 runs) that over-counts catastrophically — hundreds of undispatched
queued runs read as local-busy, BUSY blows past the threshold, every new
macOS leg overflows, and the local M1s sit 100% idle while macOS work
starves on the github-hosted pool.

The probe is inline Python inside build.yml's resolve-provider step. This
test extracts that heredoc, dedents it, execs ONLY the helper definitions
in an isolated namespace with a mocked `subprocess`, and exercises
`_count_busy_local_mac_runners` against deterministic `gh api` payloads.

Run with:
    python3 tools/scripts/test_resolve_provider_busy_probe.py
"""

from __future__ import annotations

import json
import re
import sys
import types
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build.yml"

# The local self-hosted label build.yml's probe defaults to when the repo
# variable is unset (PULP_LOCAL_MAC_RUNNER_LABEL || 'sanitizer').
LOCAL_LABEL = "sanitizer"


def _assert(cond: bool, msg: str) -> None:
    if not cond:
        raise AssertionError(msg)


def _extract_probe_source() -> str:
    """Pull the inline Python heredoc out of build.yml and dedent it."""
    text = BUILD_WORKFLOW.read_text(encoding="utf-8")
    match = re.search(r"python3 - <<'PY'\n(?P<body>.*?)\n[ ]*PY\b", text, re.DOTALL)
    _assert(match is not None, "build.yml missing the resolve-provider PY heredoc")
    body = match.group("body")
    # The run-block body is indented 10 spaces under the YAML `run: |`.
    lines = body.split("\n")
    dedented = [
        line[10:] if line.startswith(" " * 10) else line
        for line in lines
    ]
    return "\n".join(dedented)


def _slice_probe_function(source: str) -> str:
    """Return just the `_count_busy_local_mac_runners` def (+ nested helpers)."""
    lines = source.split("\n")
    start = None
    for i, line in enumerate(lines):
        if line.startswith("def _count_busy_local_mac_runners"):
            start = i
            break
    _assert(start is not None, "probe function not found in build.yml heredoc")
    end = len(lines)
    for i in range(start + 1, len(lines)):
        line = lines[i]
        # First top-level (column-0, non-blank, non-comment) line after the
        # def ends the function body.
        if line and not line[0].isspace() and not line.startswith("#"):
            end = i
            break
    return "\n".join(lines[start:end])


class _FakeCompletedProcess:
    def __init__(self, stdout: str) -> None:
        self.stdout = stdout
        self.returncode = 0


def _build_fake_subprocess(in_progress_run_ids, jobs_by_run, *, raise_for=None):
    """Construct a stand-in `subprocess` module for the probe.

    `in_progress_run_ids` — list[str] of run IDs the runs list-call returns.
    `jobs_by_run` — dict[run_id] -> list[job dict] for the per-run jobs call.
    `raise_for` — optional set of run_ids whose jobs call raises (API blip).
    """
    raise_for = raise_for or set()

    class _SubprocessError(Exception):
        pass

    def _run(args, **kwargs):  # noqa: ANN001
        # args[0:2] == ["gh", "api"]; args[2] is the endpoint, args[4] the jq.
        endpoint = args[2]
        jq = args[4]
        if "/actions/runs?status=in_progress" in endpoint:
            return _FakeCompletedProcess(
                "\n".join(str(r) for r in in_progress_run_ids) + "\n"
            )
        m = re.search(r"/actions/runs/([^/]+)/jobs", endpoint)
        _assert(m is not None, f"unexpected endpoint: {endpoint}")
        run_id = m.group(1)
        if run_id in raise_for:
            raise _SubprocessError("simulated gh api failure")
        jobs = jobs_by_run.get(run_id, [])
        # Apply the probe's jq: count macOS jobs in_progress carrying the
        # local label. The jq embeds json.dumps(local_label); recover it.
        label_match = re.search(r"index\((\"[^\"]+\")\)", jq)
        _assert(label_match is not None, f"jq missing index(label): {jq}")
        wanted = json.loads(label_match.group(1))
        count = sum(
            1
            for j in jobs
            if j.get("name", "").startswith("macOS")
            and j.get("status") == "in_progress"
            and wanted in (j.get("labels") or [])
        )
        return _FakeCompletedProcess(f"{count}\n")

    mod = types.SimpleNamespace()
    mod.run = _run
    mod.SubprocessError = _SubprocessError
    mod.CalledProcessError = _SubprocessError
    return mod


def _make_probe(in_progress_run_ids, jobs_by_run, *, raise_for=None,
                repository="danielraffel/pulp", current_run_id="999"):
    """Exec the extracted probe in an isolated namespace and return the fn."""
    source = _slice_probe_function(_extract_probe_source())
    ns: dict = {
        "json": json,
        "sys": sys,
        "subprocess": _build_fake_subprocess(
            in_progress_run_ids, jobs_by_run, raise_for=raise_for
        ),
        "os": types.SimpleNamespace(
            environ={
                "GITHUB_RUN_ID": current_run_id,
                "LOCAL_MAC_RUNNER_LABEL": LOCAL_LABEL,
            }
        ),
        "REPOSITORY": repository,
    }
    exec(compile(source, "<build.yml probe>", "exec"), ns)  # noqa: S102
    return ns["_count_busy_local_mac_runners"]


def _macos_job(status: str, *, local: bool = True, name: str = "macOS (ARM64) [local]"):
    labels = [LOCAL_LABEL] if local else ["macos-15"]
    return {"name": name, "status": status, "labels": labels}


# ── tests ────────────────────────────────────────────────────────────────


def test_counts_macos_leg_in_progress_on_local() -> None:
    probe = _make_probe(["1"], {"1": [_macos_job("in_progress", local=True)]})
    _assert(probe() == 1, "an in_progress macOS leg on a local M1 must count 1")


def test_two_in_progress_local_legs_count_two() -> None:
    probe = _make_probe(
        ["1", "2"],
        {
            "1": [_macos_job("in_progress", local=True)],
            "2": [_macos_job("in_progress", local=True)],
        },
    )
    _assert(probe() == 2, "two busy local M1 legs must count 2 (threshold hit)")


def test_overflow_leg_does_not_count_as_local() -> None:
    # A macOS leg in_progress on github-hosted macos-15 is NOT on an M1.
    probe = _make_probe(["1"], {"1": [_macos_job("in_progress", local=False)]})
    _assert(probe() == 0, "an overflow (macos-15) leg must count 0")


def test_queued_macos_leg_counts_zero() -> None:
    # The core regression: a queued macOS job has not started — it is not
    # occupying an M1, so it must count 0 (old probe wrongly counted it).
    probe = _make_probe(["1"], {"1": [_macos_job("queued", local=True)]})
    _assert(probe() == 0, "a queued macOS leg must count 0, not 1")


def test_not_yet_registered_macos_leg_counts_zero() -> None:
    # An in_progress run whose macOS matrix job is not yet registered:
    # /jobs returns no macOS job. Old probe pessimistically counted 1;
    # the fix counts 0. This is the deep-queue starvation regression.
    probe = _make_probe(
        ["1", "2", "3"],
        {
            "1": [{"name": "Linux (x64)", "status": "in_progress",
                   "labels": ["ubuntu-latest"]}],
            "2": [],
            "3": [{"name": "resolve-provider", "status": "in_progress",
                   "labels": ["ubuntu-latest"]}],
        },
    )
    _assert(probe() == 0,
            "in_progress runs with no registered macOS leg must count 0")


def test_deep_queue_does_not_overcount() -> None:
    # 50 in_progress runs, but only 2 have a macOS leg actually running on
    # a local M1; the rest are mid-build on other legs / not-yet-expanded.
    # The probe must report 2, not 50 — this is the exact failure mode.
    ids = [str(i) for i in range(50)]
    jobs = {i: [] for i in ids}
    jobs["0"] = [_macos_job("in_progress", local=True)]
    jobs["1"] = [_macos_job("in_progress", local=True)]
    jobs["2"] = [_macos_job("queued", local=True)]          # queued → 0
    jobs["3"] = [_macos_job("in_progress", local=False)]    # overflow → 0
    probe = _make_probe(ids, jobs)
    _assert(probe() == 2,
            "deep queue with 2 real local legs must report BUSY=2, not 50")


def test_completed_macos_leg_counts_zero() -> None:
    # A completed macOS job has freed its M1.
    probe = _make_probe(["1"], {"1": [_macos_job("completed", local=True)]})
    _assert(probe() == 0, "a completed macOS leg must count 0")


def test_api_blip_undercounts_to_zero() -> None:
    # An exception from the per-run jobs call must under-count (return
    # False for that run) — erring toward "use local", never overflow.
    probe = _make_probe(
        ["1", "2"],
        {"1": [_macos_job("in_progress", local=True)]},
        raise_for={"2"},
    )
    _assert(probe() == 1,
            "an API blip on one run must not inflate the busy count")


def test_empty_repository_returns_zero() -> None:
    probe = _make_probe(["1"], {"1": [_macos_job("in_progress")]}, repository="")
    _assert(probe() == 0, "no REPOSITORY → probe returns 0")


def test_self_run_excluded_from_listing() -> None:
    # The runs list jq excludes the current run id; confirm the jq filter
    # text in build.yml still carries that select().
    source = _extract_probe_source()
    _assert('select((.id | tostring) != "{current_run_id}")' in source
            or 'tostring) != "' in source,
            "probe must still exclude the current run id from the runs list")


def test_probe_does_not_enumerate_queued_runs() -> None:
    # Guard: the fix must NOT list status=queued runs. If a future edit
    # re-adds `_list_runs("queued")` the deep-queue regression returns.
    source = _extract_probe_source()
    probe_fn = _slice_probe_function(source)
    _assert("status=queued" not in probe_fn,
            "probe must not enumerate queued runs (deep-queue regression)")
    _assert("status=in_progress" in probe_fn,
            "probe must enumerate in_progress runs")


def _all_tests() -> list:
    return [
        obj for name, obj in globals().items()
        if name.startswith("test_") and callable(obj)
    ]


def main() -> int:
    failures = 0
    for fn in _all_tests():
        try:
            fn()
            print(f"ok  {fn.__name__}")
        except Exception as exc:  # noqa: BLE001
            failures += 1
            print(f"FAIL {fn.__name__}: {exc}")
    if failures:
        print(f"\n{failures} failing test(s)")
        return 1
    print(f"\nall {len(_all_tests())} tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
