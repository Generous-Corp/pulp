#!/usr/bin/env python3
"""Merge-group flake exoneration: the verdict that keeps a known flake from ejecting a batch.

The merge-group `macos` job runs ctest with `--repeat until-pass:2
--stop-on-failure`. A test that fails both attempts stops the suite and fails
the job, and GitHub ejects the entry and rebuilds every group behind it. When
the failure is a flake that other, unrelated pull requests hit too, that costs
a 20-minute group plus the rebuilds and proves nothing about the batch.

This tool is the exoneration verdict (Chromium's OCCURS_ON_OTHER_CLS). It runs
in the ctest step after a failed merge-group run and excuses the failure only
when ALL of these hold for EVERY failing test:

- it failed on 2 or more OTHER pull requests in the last 24 h, counted by pull
  request number from their own head runs (`pull_request` runs whose base is
  main), never counting a pull request that is part of this group;
- it PASSED in main's latest validated gate run (its JUnit report names it as
  run and passed), so a test new in this group is never excused;
- it is not deterministic: never a `pr-fast` test or a drift check named in
  `tools/ci/drift_fast.json`;
- it is not touched by the group: its name does not appear in a file the group
  changes, and no file its command or declared script inputs name changed;
- it is not a build failure: no test in the run is "not run" for a reason other
  than a SKIP_RETURN_CODE or a disabled test (a missing executable is a build
  failure), and the failing test's output carries no compiler, linker or CMake
  error.

and the run itself is clean otherwise: ctest exited 8 (tests failed), every
test ctest listed as failed is in the JUnit report, and every input above was
actually read. Anything short of the full condition fails exactly as before.

Merge-group runs are not counted as corroboration: a group carries every pull
request ahead of it in the queue, so a broken entry fails each group it joins
under a different tip, which is the false corroboration
`base_poison_detector.py` measured.

An excused run first runs the tests `--stop-on-failure` skipped
(`--tests-from-file`, same selection, same retries) and judges the union, so an
excusal never leaves part of the suite unexecuted. Each excusal is logged three
ways: a `::warning title=flake-exoneration::` annotation, a job-summary table,
and a `pulp-flake-excusal/v1` record the workflow uploads as the
`flake-excusals-macos` artifact, which `flake_quarantine.py` counts.

Kill switch: the repository variable `PULP_FLAKE_EXONERATION_MODE=shadow`
(any value other than `enforce` or empty) returns to annotating only: the
verdict is computed and printed and the job fails as before.

    flake_exoneration.py gate --repo O/R --merge-sha SHA --ctest-exit N \\
        --build-dir B --source-root S --junit J --selected-json I \\
        --label-exclude L --exclude-regex E --jobs N --record-out F
"""
from __future__ import annotations

import argparse
import datetime as dt
import io
import json
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable

sys.path.insert(0, str(Path(__file__).resolve().parent))
import base_poison_detector as bpd  # noqa: E402
import flake_exoneration_shadow as fes  # noqa: E402

SCHEMA = "pulp-flake-excusal/v1"
MODE_ENV = "PULP_FLAKE_EXONERATION_MODE"
MODE_ENFORCE = "enforce"
MODE_SHADOW = "shadow"
MIN_OTHER_PRS = 2
WINDOW_HOURS = 24
CTEST_TESTS_FAILED = 8
QUEUE_BRANCH = "main"
MAIN_JUNIT_MEMBER = "ctest.junit.xml"
SCRIPT_INPUTS_DIR = "test/ctest_script_inputs/tests"
# GitHub's compare endpoint lists at most this many files; a diff that reaches
# the cap may have changed a file it did not list.
COMPARE_FILE_CAP = 300
MERGE_SUBJECT_RE = re.compile(r"^Merge pull request #(\d+)\b")
# A failing test whose output carries one of these failed to BUILD something
# (a nested configure or compile smoke), which is never a flake.
BUILD_ERROR_RE = re.compile(
    r"(fatal error:|: error: |CMake Error|ninja: build stopped|Undefined symbols for architecture"
    r"|linker command failed|error: linking with|could not compile)")
# A "not run" test is a build failure unless ctest skipped it on purpose.
DELIBERATE_SKIP_RE = re.compile(r"^(SKIP_RETURN_CODE=|SKIP_REGULAR_EXPRESSION_MATCHED|Disabled)")


def resolve_mode(value: str | None) -> str:
    """`enforce` when unset or `enforce`; every other value is the kill switch."""
    v = (value or "").strip().lower()
    return MODE_ENFORCE if v in ("", MODE_ENFORCE) else MODE_SHADOW


# --------------------------------------------------------------------------
# The run's own evidence: JUnit and LastTestsFailed.log.
# --------------------------------------------------------------------------


@dataclass
class RunReport:
    """What one or more JUnit reports say about this run."""

    failing: dict[str, set[str]] = field(default_factory=dict)  # name -> labels
    ran: set[str] = field(default_factory=set)                  # every testcase named
    passed: set[str] = field(default_factory=set)
    build_failures: dict[str, str] = field(default_factory=dict)  # name -> why


def read_junit(paths: Iterable[Path]) -> RunReport:
    """Merge JUnit reports; a test that passed in a later report is no longer failing."""
    report = RunReport()
    for path in paths:
        for case in ET.parse(path).getroot().iter("testcase"):
            name = case.get("name") or ""
            if not name:
                continue
            report.ran.add(name)
            labels: set[str] = set()
            for prop in case.iterfind("properties/property"):
                if prop.get("name") == "cmake_labels":
                    labels.update(x for x in (prop.get("value") or "").split(";") if x)
            tags = {c.tag for c in case}
            if tags & {"failure", "error"}:
                report.failing[name] = labels
                report.passed.discard(name)
                out = "".join((c.text or "") for c in case if c.tag in ("system-out", "failure", "error"))
                out += "".join((c.get("message") or "") for c in case if c.tag in ("failure", "error"))
                hit = BUILD_ERROR_RE.search(out)
                if hit:
                    report.build_failures[name] = f"output carries a build error ({hit.group(1).strip()})"
                continue
            if case.get("status") == "notrun" or "skipped" in tags:
                why = " ".join((c.get("message") or "") for c in case if c.tag == "skipped").strip()
                if not DELIBERATE_SKIP_RE.match(why):
                    report.build_failures[name] = f"not run: {why or 'no reason given'}"
                continue
            report.passed.add(name)
            report.failing.pop(name, None)
            report.build_failures.pop(name, None)
    return report


def passed_names_from_junit_bytes(blob: bytes) -> set[str]:
    out: set[str] = set()
    for case in ET.fromstring(blob).iter("testcase"):
        name = case.get("name") or ""
        tags = {c.tag for c in case}
        if name and case.get("status") == "run" and not tags & {"failure", "error", "skipped"}:
            out.add(name)
    return out


# --------------------------------------------------------------------------
# Scope of the change: the pull requests in this group and what they touch.
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class Scope:
    prs: frozenset[int]
    files: frozenset[str]
    reason: str = ""

    @property
    def read(self) -> bool:
        return not self.reason


def scope_from_compare(payload: dict | None, pr_number: int | None = None) -> Scope:
    """The pull requests and changed files `compare/main...<sha>` reports, or an unread scope."""
    if not isinstance(payload, dict):
        return Scope(frozenset(), frozenset(), "compare unreadable")
    commits = payload.get("commits") or []
    files = payload.get("files")
    if files is None:
        return Scope(frozenset(), frozenset(), "compare listed no files")
    if int(payload.get("total_commits") or 0) > len(commits):
        return Scope(frozenset(), frozenset(), "compare truncated its commit list")
    if len(files) >= COMPARE_FILE_CAP:
        return Scope(frozenset(), frozenset(), f"compare listed {len(files)} files (the cap)")
    prs = set() if pr_number is None else {pr_number}
    for c in commits:
        m = MERGE_SUBJECT_RE.match(((c.get("commit") or {}).get("message") or ""))
        if m:
            prs.add(int(m.group(1)))
    paths = set()
    for f in files:
        for key in ("filename", "previous_filename"):
            if f.get(key):
                paths.add(f[key])
    return Scope(frozenset(prs), frozenset(paths))


def read_scope(repo: str, sha: str, pr_number: int | None = None) -> Scope:
    raw = bpd.gh(f"repos/{repo}/compare/{QUEUE_BRANCH}...{sha}")
    try:
        payload = json.loads(raw) if raw else None
    except json.JSONDecodeError:
        payload = None
    return scope_from_compare(payload, pr_number)


def name_variants(name: str) -> set[str]:
    """The spellings a test name can take in source (ctest escapes some characters)."""
    out = {name, name.replace("\\", "")}
    out.add(name.replace('"', '\\"'))
    return {v for v in out if v}


def load_script_inputs(source_root: Path) -> dict[str, list[str]]:
    """Declared inputs of every script-driven test, by test name."""
    out: dict[str, list[str]] = {}
    for path in sorted((source_root / SCRIPT_INPUTS_DIR).glob("*.json")):
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        rec = data.get("record") or {}
        names = list(rec.get("inputs") or [])
        if rec.get("entry"):
            names.append(rec["entry"])
        if data.get("name"):
            out[data["name"]] = names
    return out


def command_paths(selected: dict | None, source_root: Path) -> dict[str, list[str]]:
    """Repository-relative paths each test's command line names (ctest --show-only=json-v1)."""
    out: dict[str, list[str]] = {}
    root = str(source_root.resolve()).rstrip("/") + "/"
    for t in (selected or {}).get("tests", []) or []:
        rels = []
        for arg in t.get("command") or []:
            if isinstance(arg, str) and arg.startswith(root):
                rels.append(arg[len(root):])
        out[t.get("name", "")] = rels
    return out


def _covers(declared: str, changed: str) -> bool:
    declared = declared.rstrip("/")
    return changed == declared or changed.startswith(declared + "/")


def touched_tests(names: Iterable[str], scope: Scope, source_root: Path,
                  script_inputs: dict[str, list[str]], commands: dict[str, list[str]],
                  read_text: Callable[[Path], str] | None = None) -> dict[str, str]:
    """Failing tests the change touches, with the reason. Unread scope touches everything."""
    names = list(names)
    if not scope.read:
        return {n: f"change scope unknown: {scope.reason}" for n in names}
    read_text = read_text or (lambda p: p.read_text(encoding="utf-8", errors="replace"))
    texts: dict[str, str] = {}
    for rel in sorted(scope.files):
        p = source_root / rel
        if p.is_file():
            try:
                texts[rel] = read_text(p)
            except OSError:
                continue
    out: dict[str, str] = {}
    for name in names:
        for rel, text in texts.items():
            if any(v in text for v in name_variants(name)):
                out[name] = f"named in changed file {rel}"
                break
        if name in out:
            continue
        for declared in script_inputs.get(name, []) + commands.get(name, []):
            hit = next((c for c in sorted(scope.files) if _covers(declared, c)), None)
            if hit:
                out[name] = f"input {declared} changed ({hit})"
                break
    return out


# --------------------------------------------------------------------------
# Corroboration: other pull requests' head runs.
# --------------------------------------------------------------------------


def _pr_targeting_main(prs: list[dict]) -> int | None:
    """The single pull request in `prs` whose base is main; None when ambiguous or absent."""
    if len(prs) != 1:
        return None
    pr = prs[0]
    if ((pr.get("base") or {}).get("ref") or "") != QUEUE_BRANCH:
        return None
    try:
        return int(pr.get("number"))
    except (TypeError, ValueError):
        return None


def run_pr(run: dict, lookup: Callable[[str], list[dict] | None] | None = None) -> int | None:
    """The pull request a `pull_request` run belongs to, if it targets main.

    A run's own `pull_requests` list is empty once its branch is gone, so the
    head commit's associated pull requests are read instead. A commit that
    belongs to more than one pull request (a stack) is ambiguous and counts as
    none.
    """
    own = run.get("pull_requests") or []
    if own:
        return _pr_targeting_main(own)
    if lookup is None or not run.get("head_sha"):
        return None
    return _pr_targeting_main(lookup(str(run["head_sha"])) or [])


def commit_pulls(repo: str) -> Callable[[str], list[dict] | None]:
    def lookup(sha: str) -> list[dict] | None:
        raw = bpd.gh(f"repos/{repo}/commits/{sha}/pulls?per_page=10")
        try:
            return json.loads(raw) if raw else None
        except json.JSONDecodeError:
            return None
    return lookup


def recent_pr_runs(repo: str, hours: int, now: dt.datetime | None = None,
                   pages: int = 3, page_fn: Callable[[int], list[dict]] | None = None) -> list[dict]:
    """Completed `pull_request` Build runs inside the window, newest first."""
    now = now or dt.datetime.now(dt.timezone.utc)
    since = now - dt.timedelta(hours=hours)

    def default_page(page: int) -> list[dict]:
        raw = bpd.gh(f"repos/{repo}/actions/workflows/build.yml/runs?event=pull_request"
                     f"&status=completed&per_page=100&page={page}")
        try:
            return json.loads(raw).get("workflow_runs", []) if raw else []
        except json.JSONDecodeError:
            return []

    page_fn = page_fn or default_page
    out: list[dict] = []
    for page in range(1, pages + 1):
        batch = page_fn(page)
        if not batch:
            break
        older = False
        for r in batch:
            created = fes._created(r)
            if created is None or created < since:
                older = True
                continue
            out.append(r)
        if older:
            break
    return out


def failures_by_pr(repo: str, runs: list[dict], exclude: Iterable[int], interesting: Iterable[str],
                   failing_fn: Callable[[str, str], Iterable[str]] | None = None,
                   lookup: Callable[[str], list[dict] | None] | None = None) -> dict[str, set[int]]:
    """test name -> other pull requests whose own head run failed it (for the tests asked about)."""
    failing_fn = failing_fn or bpd.artifact_failing_tests
    lookup = lookup if lookup is not None else commit_pulls(repo)
    excluded = set(exclude)
    wanted = set(interesting)
    table: dict[str, set[int]] = {}
    for r in runs:
        if r.get("conclusion") not in ("failure", "timed_out"):
            continue
        hits = wanted.intersection(failing_fn(repo, str(r["id"])))
        if not hits:
            continue
        pr = run_pr(r, lookup)
        if pr is None or pr in excluded:
            continue
        for name in hits:
            table.setdefault(name, set()).add(pr)
    return table


# --------------------------------------------------------------------------
# main: the latest validated gate run, read per test.
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class MainPasses:
    run_id: str | None
    passed: frozenset[str]
    reason: str = ""

    @property
    def read(self) -> bool:
        return not self.reason


def read_artifact_member(repo: str, run_id: str, artifact: str, member: str) -> bytes | None:
    raw = bpd.gh(f"repos/{repo}/actions/runs/{run_id}/artifacts?per_page=100",
                 f'[.artifacts[]|select(.name=="{artifact}" and (.expired|not))]|.[0].id')
    if not raw or not raw.strip().isdigit():
        return None
    blob = bpd.gh_bytes(f"repos/{repo}/actions/artifacts/{raw.strip()}/zip")
    if not blob:
        return None
    try:
        with zipfile.ZipFile(io.BytesIO(blob)) as archive:
            return archive.read(member)
    except (zipfile.BadZipFile, KeyError):
        return None


def main_passes(repo: str, observe_fn=None, junit_fn=None) -> MainPasses:
    """The tests main's tip gate run executed and passed; unread unless the report was read."""
    observe_fn = observe_fn or (lambda r: bpd.observe_main(r)[0])
    junit_fn = junit_fn or (lambda r, rid: read_artifact_member(
        r, rid, f"{bpd.CTEST_ARTIFACT_PREFIX}macos", MAIN_JUNIT_MEMBER))
    obs = observe_fn(repo)
    if obs is None:
        return MainPasses(None, frozenset(), "no merge-group run observed main's tip")
    if not obs.is_evidence:
        return MainPasses(obs.run_id, frozenset(), "main's gate did not execute the suite")
    blob = junit_fn(repo, obs.run_id)
    if not blob:
        return MainPasses(obs.run_id, frozenset(), "main's JUnit report could not be read")
    try:
        passed = passed_names_from_junit_bytes(blob)
    except ET.ParseError:
        return MainPasses(obs.run_id, frozenset(), "main's JUnit report is not XML")
    if not passed:
        return MainPasses(obs.run_id, frozenset(), "main's JUnit report names no passing test")
    return MainPasses(obs.run_id, frozenset(passed))


# --------------------------------------------------------------------------
# The verdict. Pure.
# --------------------------------------------------------------------------


@dataclass
class Evidence:
    report: RunReport
    ctest_exit: int
    last_failed: tuple[str, ...] | None
    by_pr: dict[str, set[int]]
    main: MainPasses
    scope: Scope
    touched: dict[str, str]
    deterministic: set[str]


def verdict(ev: Evidence, min_other_prs: int = MIN_OTHER_PRS) -> dict:
    """Excused only when the run is clean otherwise and every failing test qualifies."""
    run_reasons: list[str] = []
    if ev.ctest_exit != CTEST_TESTS_FAILED:
        run_reasons.append(f"ctest exited {ev.ctest_exit}, not {CTEST_TESTS_FAILED} (tests failed)")
    if not ev.report.failing:
        run_reasons.append("the JUnit report names no failing test")
    if ev.report.build_failures:
        first = sorted(ev.report.build_failures.items())[0]
        run_reasons.append(f"build failure: {first[0]}: {first[1]}"
                           + (f" (+{len(ev.report.build_failures) - 1} more)"
                              if len(ev.report.build_failures) > 1 else ""))
    if ev.last_failed is None:
        run_reasons.append("LastTestsFailed.log could not be read")
    else:
        unexplained = sorted(set(ev.last_failed) - set(ev.report.failing))
        if unexplained:
            run_reasons.append(f"ctest failed tests the JUnit report does not: {', '.join(unexplained[:5])}")
    if not ev.main.read:
        run_reasons.append(f"main evidence unread: {ev.main.reason}")
    if not ev.scope.read:
        run_reasons.append(f"change scope unread: {ev.scope.reason}")

    tests = {}
    for name in sorted(ev.report.failing):
        prs = sorted(ev.by_pr.get(name, set()))
        why = (
            "deterministic test (pr-fast or drift check)" if name in ev.deterministic else
            f"touched by this change: {ev.touched[name]}" if name in ev.touched else
            f"build failure: {ev.report.build_failures[name]}" if name in ev.report.build_failures else
            "did not pass on main's tip (failing there, or new)" if ev.main.read and name not in ev.main.passed else
            f"main evidence unread: {ev.main.reason}" if not ev.main.read else
            f"failed on {len(prs)} other pull request(s), needs {min_other_prs}" if len(prs) < min_other_prs else
            ""
        )
        tests[name] = {"excused": not why, "other_prs": prs, "reason": why or "OCCURS_ON_OTHER_CLS"}
    excused = not run_reasons and bool(tests) and all(t["excused"] for t in tests.values())
    return {"excused": excused, "run_reasons": run_reasons, "tests": tests,
            "main_run_id": ev.main.run_id, "group_prs": sorted(ev.scope.prs)}


# --------------------------------------------------------------------------
# The gate step.
# --------------------------------------------------------------------------


def read_last_failed(build_dir: Path) -> tuple[str, ...] | None:
    path = build_dir / bpd.LAST_TESTS_FAILED_MEMBER
    try:
        return bpd.parse_last_tests_failed(path.read_text(encoding="utf-8", errors="replace"))
    except FileNotFoundError:
        return ()
    except OSError:
        return None


def remainder(selected: dict | None, ran: set[str]) -> list[str]:
    """Selected tests the stopped run never started."""
    return [t["name"] for t in (selected or {}).get("tests", []) or []
            if t.get("name") and t["name"] not in ran]


def merge_junit_into(primary: Path, extra: Path) -> None:
    """Append the extra report's testcases to the primary report, so later steps see the whole run."""
    tree = ET.parse(primary)
    root = tree.getroot()
    for case in ET.parse(extra).getroot().iter("testcase"):
        root.append(case)
    cases = list(root.iter("testcase"))
    root.set("tests", str(len(cases)))
    root.set("failures", str(sum(1 for c in cases if any(x.tag in ("failure", "error") for x in c))))
    tree.write(primary, encoding="UTF-8", xml_declaration=True)


def excusal_record(result: dict, *, repo: str, run_id: str, run_attempt: str, merge_sha: str,
                   now: dt.datetime | None = None) -> dict:
    now = now or dt.datetime.now(dt.timezone.utc)
    return {"schema": SCHEMA, "repository": repo, "run_id": str(run_id), "run_attempt": str(run_attempt),
            "merge_sha": merge_sha, "excused_at": now.isoformat().replace("+00:00", "Z"),
            "main_run_id": result.get("main_run_id"), "group_prs": result.get("group_prs", []),
            "tests": [{"name": n, "other_prs": t["other_prs"]} for n, t in sorted(result["tests"].items())]}


def summary_markdown(result: dict, mode: str) -> str:
    head = ("Excused as known flakes" if result["excused"] and mode == MODE_ENFORCE else
            "Would excuse (shadow mode, job fails as before)" if result["excused"] else
            "Not excused")
    lines = [f"### Merge-group flake exoneration: {head}", "",
             "| test | verdict | other PRs | reason |", "|---|---|---|---|"]
    for name, t in sorted(result["tests"].items()):
        prs = " ".join(f"#{p}" for p in t["other_prs"]) or "none"
        lines.append(f"| `{name}` | {'excused' if t['excused'] else 'counts'} | {prs} | "
                     f"{t['reason'].replace('|', '/')} |")
    for r in result["run_reasons"]:
        lines.append(f"\n- run: {r}")
    return "\n".join(lines) + "\n"


def run_ctest_remainder(names: list[str], a: argparse.Namespace, workdir: Path) -> int:
    listing = workdir / "remainder.txt"
    listing.write_text("".join(n + "\n" for n in names), encoding="utf-8")
    cmd = ["ctest", "--test-dir", a.build_dir, "--output-on-failure", "--repeat", "until-pass:2",
           "-C", "Release", "-LE", a.label_exclude, f"-j{a.jobs}", "--timeout", "120",
           "--tests-from-file", str(listing), "--output-junit", str(workdir / "remainder.junit.xml")]
    if a.exclude_regex:
        cmd += ["--exclude-regex", a.exclude_regex]
    print(f"flake-exoneration: running the {len(names)} test(s) --stop-on-failure skipped", flush=True)
    return subprocess.run(cmd).returncode


def cmd_gate(a: argparse.Namespace, *, collect=None, run_remainder=None, write_summary=None) -> int:
    """Return the exit status the ctest step should end with."""
    if a.ctest_exit == 0:
        return 0
    mode = resolve_mode(a.mode if a.mode is not None else os.environ.get(MODE_ENV))
    build_dir = Path(a.build_dir)
    source_root = Path(a.source_root)
    junit = Path(a.junit)
    workdir = Path(a.work_dir)
    workdir.mkdir(parents=True, exist_ok=True)
    try:
        report = read_junit([junit])
    except (OSError, ET.ParseError) as exc:
        print(f"flake-exoneration: JUnit unreadable, failing as before: {exc}", file=sys.stderr)
        return a.ctest_exit
    last_failed = read_last_failed(build_dir)
    try:
        selected = json.loads(Path(a.selected_json).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        selected = None
    collect = collect or collect_evidence
    shared = collect(a, report, selected)  # by_pr, main, scope, deterministic, script_inputs, commands

    def judge(rep: RunReport, lf: tuple[str, ...] | None) -> dict:
        touched = touched_tests(rep.failing, shared["scope"], source_root,
                                shared["script_inputs"], shared["commands"])
        drift = fes.drift_fast_tests()
        det = {n for n, labels in rep.failing.items() if fes.DETERMINISTIC_LABEL in labels or n in drift}
        ev = Evidence(rep, a.ctest_exit, lf, shared["by_pr"], shared["main"], shared["scope"],
                      touched, det)
        return verdict(ev, a.min_other_prs)

    result = judge(report, last_failed)
    write_summary = write_summary or _append_summary
    if result["excused"] and mode == MODE_ENFORCE:
        rest = remainder(selected, report.ran)
        if selected is None:
            result = dict(result, excused=False,
                          run_reasons=result["run_reasons"] + ["selected-test inventory unreadable"])
        elif rest:
            saved = list(last_failed or ())
            rc = (run_remainder or run_ctest_remainder)(rest, a, workdir)
            extra = workdir / "remainder.junit.xml"
            if rc not in (0, CTEST_TESTS_FAILED) or not extra.is_file():
                result = dict(result, excused=False,
                              run_reasons=result["run_reasons"] + [f"remainder ctest exited {rc}"])
            else:
                merge_junit_into(junit, extra)
                more = read_last_failed(build_dir) if rc else ()
                union = tuple(dict.fromkeys(saved + list(more or ())))
                _write_last_failed(build_dir, union)
                report = read_junit([junit])
                missing = sorted(set(rest) - report.ran)
                result = judge(report, None if more is None else union)
                if missing:
                    result = dict(result, excused=False, run_reasons=result["run_reasons"] + [
                        f"{len(missing)} skipped test(s) still did not run, e.g. {missing[0]}"])
    print(f"::{'warning' if result['excused'] else 'notice'} title=flake-exoneration::"
          f"{json.dumps(dict(result, mode=mode), sort_keys=True)}")
    write_summary(summary_markdown(result, mode))
    if result["excused"] and mode == MODE_ENFORCE:
        record = excusal_record(result, repo=a.repo, run_id=a.run_id, run_attempt=a.run_attempt,
                                merge_sha=a.merge_sha)
        Path(a.record_out).parent.mkdir(parents=True, exist_ok=True)
        Path(a.record_out).write_text(json.dumps(record, indent=1, sort_keys=True) + "\n", encoding="utf-8")
        print(f"flake-exoneration: EXCUSED {len(result['tests'])} failing test(s) as known flakes; "
              f"record {a.record_out}")
        return 0
    return a.ctest_exit


def _write_last_failed(build_dir: Path, names: tuple[str, ...]) -> None:
    path = build_dir / bpd.LAST_TESTS_FAILED_MEMBER
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(f"{i}:{n}\n" for i, n in enumerate(names, 1)), encoding="utf-8")


def _append_summary(text: str) -> None:
    target = os.environ.get("GITHUB_STEP_SUMMARY")
    if target:
        with open(target, "a", encoding="utf-8") as fh:
            fh.write(text)
    else:
        print(text)


def collect_evidence(a: argparse.Namespace, report: RunReport, selected: dict | None) -> dict:
    scope = read_scope(a.repo, a.merge_sha)
    runs = recent_pr_runs(a.repo, a.hours) if report.failing else []
    by_pr = failures_by_pr(a.repo, runs, scope.prs, report.failing) if scope.read else {}
    return {"scope": scope, "by_pr": by_pr, "main": main_passes(a.repo),
            "script_inputs": load_script_inputs(Path(a.source_root)),
            "commands": command_paths(selected, Path(a.source_root))}


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gate", help="judge a failed merge-group ctest run")
    g.add_argument("--repo", required=True)
    g.add_argument("--merge-sha", required=True)
    g.add_argument("--ctest-exit", type=int, required=True)
    g.add_argument("--build-dir", required=True)
    g.add_argument("--source-root", required=True)
    g.add_argument("--junit", required=True)
    g.add_argument("--selected-json", required=True)
    g.add_argument("--label-exclude", required=True)
    g.add_argument("--exclude-regex", default="")
    g.add_argument("--jobs", type=int, default=8)
    g.add_argument("--hours", type=int, default=WINDOW_HOURS)
    g.add_argument("--min-other-prs", type=int, default=MIN_OTHER_PRS)
    g.add_argument("--mode", default=None, help=f"override ${MODE_ENV}")
    g.add_argument("--work-dir", default=os.path.join(os.environ.get("RUNNER_TEMP", "/tmp"), "flake-exoneration"))
    g.add_argument("--record-out", required=True)
    g.add_argument("--run-id", default=os.environ.get("GITHUB_RUN_ID", ""))
    g.add_argument("--run-attempt", default=os.environ.get("GITHUB_RUN_ATTEMPT", "1"))
    return ap


def main(argv: list[str]) -> int:
    a = build_parser().parse_args(argv[1:])
    try:
        return cmd_gate(a)
    except Exception as exc:  # noqa: BLE001 - any error fails exactly as before
        print(f"flake-exoneration: no verdict ({exc!r}); failing as before", file=sys.stderr)
        return a.ctest_exit or 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
