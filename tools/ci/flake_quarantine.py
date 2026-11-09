#!/usr/bin/env python3
"""Quarantine for tests the merge-group gate keeps excusing as flakes.

`flake_exoneration.py` excuses a merge-group failure only when every failing
test is a corroborated flake, and uploads a `pulp-flake-excusal/v1` record as
the `flake-excusals-macos` artifact each time. A test excused 3 or more times
in 7 days is not a passing accident any more; it is a known flake that costs
every batch it lands in a retry budget. This tool moves it out of the required
lane for a bounded time:

- `sweep` (the scheduled `flake-quarantine.yml` workflow) counts excusal
  records from trusted runs (this repository's `build.yml`, `merge_group`
  event) over the last 7 days. A test at the threshold gets an owner issue,
  labelled `ci:flake` and `flake-quarantine`, whose body carries a
  `pulp-flake-quarantine/v1` marker with its start and a 14-day expiry. An
  issue whose expiry has passed loses the `flake-quarantine` label and gets a
  comment: the test is back in the required lane and the issue stays open for
  its owner. Only excusals after a test's latest quarantine started count, so
  a test whose issue was closed as fixed does not re-enter on stale evidence.
- `active` (the gate job, before ctest) reads the open quarantine issues and
  writes the test names and an anchored ctest `--exclude-regex` fragment. The
  gate excludes them and an advisory step runs them, reporting only.

A quarantine is active only while its issue is OPEN, carries the
`flake-quarantine` label, was opened by the workflow's bot account, and its
expiry has not passed. Closing the issue (fixed) or removing the label ends it
at once. A test the current change touches is never quarantined for that run:
a pull request that fixes the flake runs it in the required lane. Any read
failure yields an empty list, so the test stays in the required lane.

Mode: unless `PULP_FLAKE_EXONERATION_MODE=enforce`, `active` writes an empty
list and `sweep` only reports what it would do (shadow is the default).

    flake_quarantine.py active --repo O/R --sha SHA [--pr N] --source-root S --out-dir D
    flake_quarantine.py sweep  --repo O/R [--dry-run]
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
import zipfile
from urllib.parse import quote
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable

sys.path.insert(0, str(Path(__file__).resolve().parent))
import base_poison_detector as bpd  # noqa: E402
import flake_exoneration as fx  # noqa: E402

MARKER_SCHEMA = "pulp-flake-quarantine/v1"
MARKER_RE = re.compile(r"<!--\s*" + re.escape(MARKER_SCHEMA) + r"\s+(\{.*?\})\s*-->", re.S)
FLAKE_LABEL = "ci:flake"
QUARANTINE_LABEL = "flake-quarantine"
BOT_LOGIN = "github-actions[bot]"
ARTIFACT_NAME = "flake-excusals-macos"
RECORD_FILE = "flake-excusal.json"
WORKFLOW_PATH = ".github/workflows/build.yml"
TRUSTED_EVENT = "merge_group"
THRESHOLD = 3
WINDOW_DAYS = 7
QUARANTINE_DAYS = 14
MAX_RECORD_BYTES = 1 << 20
# CMake regular-expression metacharacters.
_CMAKE_RE_META = set("^$.[]*+?()|\\")


def _parse_time(value) -> dt.datetime | None:
    try:
        return dt.datetime.fromisoformat(str(value).replace("Z", "+00:00"))
    except (TypeError, ValueError):
        return None


def _iso(t: dt.datetime) -> str:
    return t.astimezone(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


@dataclass(frozen=True)
class Quarantine:
    number: int
    test: str
    state: str
    labels: frozenset[str]
    author: str
    quarantined_at: dt.datetime
    expires_at: dt.datetime

    def active(self, now: dt.datetime, bot: str = BOT_LOGIN) -> bool:
        return (self.state == "open" and QUARANTINE_LABEL in self.labels and self.author == bot
                and self.quarantined_at <= now < self.expires_at)


def marker(test: str, start: dt.datetime, days: int = QUARANTINE_DAYS, runs: Iterable[str] = ()) -> str:
    payload = {"test": test, "quarantined_at": _iso(start),
               "expires_at": _iso(start + dt.timedelta(days=days)), "excusal_runs": sorted(runs)}
    return f"<!-- {MARKER_SCHEMA} {json.dumps(payload, sort_keys=True)} -->"


def parse_issue(issue: dict) -> Quarantine | None:
    """The quarantine an issue records, or None when it carries no valid marker."""
    m = MARKER_RE.search(issue.get("body") or "")
    if not m:
        return None
    try:
        data = json.loads(m.group(1))
    except json.JSONDecodeError:
        return None
    start, end = _parse_time(data.get("quarantined_at")), _parse_time(data.get("expires_at"))
    test = data.get("test")
    if not isinstance(test, str) or not test or start is None or end is None or end <= start:
        return None
    return Quarantine(number=int(issue.get("number") or 0), test=test, state=issue.get("state") or "",
                      labels=frozenset(l.get("name", "") for l in issue.get("labels") or []),
                      author=((issue.get("user") or {}).get("login") or ""),
                      quarantined_at=start, expires_at=end)


def quarantines(issues: Iterable[dict]) -> list[Quarantine]:
    return [q for q in (parse_issue(i) for i in issues if "pull_request" not in i) if q]


def active_tests(issues: Iterable[dict], now: dt.datetime, bot: str = BOT_LOGIN) -> set[str]:
    return {q.test for q in quarantines(issues) if q.active(now, bot)}


def count_excusals(records: Iterable[dict], now: dt.datetime, since: dict[str, dt.datetime],
                   window_days: int = WINDOW_DAYS) -> dict[str, set[str]]:
    """test -> distinct runs that excused it inside the window, after its latest quarantine began."""
    floor = now - dt.timedelta(days=window_days)
    out: dict[str, set[str]] = {}
    for rec in records:
        if rec.get("schema") != fx.SCHEMA:
            continue
        at = _parse_time(rec.get("excused_at"))
        if at is None or at < floor or at > now:
            continue
        for t in rec.get("tests") or []:
            name = t.get("name") if isinstance(t, dict) else None
            if not name:
                continue
            if name in since and at <= since[name]:
                continue
            out.setdefault(name, set()).add(str(rec.get("run_id")))
    return out


@dataclass(frozen=True)
class Action:
    kind: str           # "quarantine" | "expire"
    test: str
    issue: int | None   # existing issue to reuse or expire
    runs: tuple[str, ...] = ()


def plan_sweep(records: list[dict], issues: list[dict], now: dt.datetime, threshold: int = THRESHOLD,
               bot: str = BOT_LOGIN) -> list[Action]:
    qs = [q for q in quarantines(issues) if q.author == bot]
    latest: dict[str, Quarantine] = {}
    for q in qs:
        if q.test not in latest or q.quarantined_at > latest[q.test].quarantined_at:
            latest[q.test] = q
    actions: list[Action] = []
    for q in qs:
        if q.state == "open" and QUARANTINE_LABEL in q.labels and now >= q.expires_at:
            actions.append(Action("expire", q.test, q.number))
    since = {t: q.quarantined_at for t, q in latest.items()}
    for test, runs in sorted(count_excusals(records, now, since).items()):
        if len(runs) < threshold:
            continue
        cur = latest.get(test)
        if cur is not None and cur.active(now, bot):
            continue
        reuse = cur.number if cur is not None and cur.state == "open" else None
        actions.append(Action("quarantine", test, reuse, tuple(sorted(runs))))
    return actions


def cmake_regex_literal(name: str) -> str:
    return "".join("\\" + c if c in _CMAKE_RE_META else c for c in name)


def exclude_fragment(names: Iterable[str]) -> str:
    return "|".join(f"^{cmake_regex_literal(n)}$" for n in sorted(names))


# --------------------------------------------------------------------------
# Network adapters.
# --------------------------------------------------------------------------


def list_issues(repo: str, bot: str = BOT_LOGIN, state: str = "all") -> list[dict] | None:
    out: list[dict] = []
    for page in range(1, 6):
        raw = bpd.gh(f"repos/{repo}/issues?labels={quote(FLAKE_LABEL)}&state={state}&creator={quote(bot)}"
                     f"&per_page=100&page={page}")
        if raw is None:
            return None
        try:
            batch = json.loads(raw)
        except json.JSONDecodeError:
            return None
        out.extend(batch)
        if len(batch) < 100:
            break
    return out


def trust_refusal(repo: str, artifact: dict, run: dict | None) -> str | None:
    if not run:
        return "run unreadable"
    if run.get("id") != (artifact.get("workflow_run") or {}).get("id"):
        return "run id differs from the artifact's run"
    if run.get("path") != WORKFLOW_PATH:
        return f"not written by {WORKFLOW_PATH}"
    if run.get("event") != TRUSTED_EVENT:
        return f"event {run.get('event')!r} is not {TRUSTED_EVENT}"
    for key in ("repository", "head_repository"):
        if (run.get(key) or {}).get("full_name") != repo:
            return f"{key} is not {repo}"
    return None


def load_records(repo: str, now: dt.datetime, window_days: int = WINDOW_DAYS) -> tuple[list[dict], list[str]]:
    floor = now - dt.timedelta(days=window_days)
    raw = bpd.gh(f"repos/{repo}/actions/artifacts?name={ARTIFACT_NAME}&per_page=100")
    if raw is None:
        raise RuntimeError("excusal artifacts could not be listed")
    records, refusals = [], []
    for art in json.loads(raw).get("artifacts", []):
        created = _parse_time(art.get("created_at"))
        if art.get("expired") or created is None or created < floor:
            continue
        run_id = (art.get("workflow_run") or {}).get("id")
        run_raw = bpd.gh(f"repos/{repo}/actions/runs/{run_id}")
        run = json.loads(run_raw) if run_raw else None
        why = trust_refusal(repo, art, run)
        if why:
            refusals.append(f"artifact {art.get('id')}: {why}")
            continue
        blob = bpd.gh_bytes(f"repos/{repo}/actions/artifacts/{art['id']}/zip")
        try:
            with zipfile.ZipFile(io.BytesIO(blob or b"")) as z:
                rec = json.loads(z.read(RECORD_FILE)[:MAX_RECORD_BYTES])
        except (zipfile.BadZipFile, KeyError, json.JSONDecodeError):
            refusals.append(f"artifact {art.get('id')}: record unreadable")
            continue
        if str(rec.get("run_id")) != str(run_id) or rec.get("repository") != repo:
            refusals.append(f"artifact {art.get('id')}: record names another run")
            continue
        records.append(rec)
    return records, refusals


def issue_body(test: str, start: dt.datetime, runs: Iterable[str], repo: str) -> str:
    runs = sorted(runs)
    links = "\n".join(f"- https://github.com/{repo}/actions/runs/{r}" for r in runs)
    end = start + dt.timedelta(days=QUARANTINE_DAYS)
    return (f"The merge-group gate excused `{test}` as a known flake {len(runs)} times in "
            f"{WINDOW_DAYS} days, so it is quarantined: the required `macos` gate excludes it and an "
            f"advisory step runs it and reports only.\n\n"
            f"Excusals:\n{links}\n\n"
            f"The quarantine ends on {_iso(end)}, when the test returns to the required lane. "
            f"Close this issue as completed once the flake is fixed to end it now; removing the "
            f"`{QUARANTINE_LABEL}` label also ends it. A pull request that changes the test runs it "
            f"in the required lane regardless.\n\n"
            f"Written by `tools/ci/flake_quarantine.py`; see the `ci` skill.\n\n"
            f"{marker(test, start, runs=runs)}\n")


def gh_write(method: str, path: str, payload: dict) -> bool:
    proc = subprocess.run([bpd.gh_cli(), "api", "-X", method, path, "--input", "-"],
                          input=json.dumps(payload), capture_output=True, text=True)
    if proc.returncode != 0:
        print(f"flake-quarantine: {method} {path} failed: {proc.stderr.strip()[:300]}", file=sys.stderr)
    return proc.returncode == 0


def ensure_labels(repo: str, write=gh_write) -> None:
    for name, color, desc in ((FLAKE_LABEL, "d93f0b", "A test that fails intermittently on CI"),
                              (QUARANTINE_LABEL, "fbca04", "Test excluded from the required gate for a bounded time")):
        if bpd.gh(f"repos/{repo}/labels/{name.replace(':', '%3A')}") is None:
            write("POST", f"repos/{repo}/labels", {"name": name, "color": color, "description": desc})


def apply(repo: str, actions: list[Action], now: dt.datetime, write=gh_write) -> int:
    failures = 0
    for act in actions:
        if act.kind == "expire":
            ok = write("POST", f"repos/{repo}/issues/{act.issue}/comments", {
                "body": f"Quarantine expired on {_iso(now)}: `{act.test}` is back in the required "
                        f"lane. Close this issue once the flake is fixed."})
            ok = write("DELETE", f"repos/{repo}/issues/{act.issue}/labels/{QUARANTINE_LABEL}", {}) and ok
        elif act.issue is not None:
            ok = write("PATCH", f"repos/{repo}/issues/{act.issue}",
                       {"body": issue_body(act.test, now, act.runs, repo),
                        "labels": [FLAKE_LABEL, QUARANTINE_LABEL]})
            ok = write("POST", f"repos/{repo}/issues/{act.issue}/comments", {
                "body": f"Quarantined again: excused {len(act.runs)} times in {WINDOW_DAYS} days."}) and ok
        else:
            ok = write("POST", f"repos/{repo}/issues", {
                "title": f"Flaky test quarantined: {act.test}"[:250],
                "body": issue_body(act.test, now, act.runs, repo),
                "labels": [FLAKE_LABEL, QUARANTINE_LABEL]})
        failures += 0 if ok else 1
    return failures


# --------------------------------------------------------------------------
# Commands.
# --------------------------------------------------------------------------


def cmd_active(a: argparse.Namespace, *, issues_fn=None, scope_fn=None, now: dt.datetime | None = None) -> int:
    out = Path(a.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    names_file, regex_file = out / "names.txt", out / "exclude.regex"
    names_file.write_text("", encoding="utf-8")
    regex_file.write_text("", encoding="utf-8")
    mode = fx.resolve_mode(a.mode if a.mode is not None else os.environ.get(fx.MODE_ENV))
    if mode != fx.MODE_ENFORCE:
        print("flake-quarantine: shadow mode; no test is quarantined")
        return 0
    now = now or dt.datetime.now(dt.timezone.utc)
    issues = (issues_fn or (lambda: list_issues(a.repo, a.bot_login, state="open")))()
    if issues is None:
        print("::warning title=flake-quarantine::quarantine issues unreadable; every test stays in the required lane")
        return 0
    tests = active_tests(issues, now, a.bot_login)
    if not tests:
        print("flake-quarantine: no active quarantine")
        return 0
    scope = (scope_fn or (lambda: fx.read_scope(a.repo, a.sha, a.pr)))()
    touched = fx.touched_tests(tests, scope, Path(a.source_root), fx.load_script_inputs(Path(a.source_root)), {})
    kept = sorted(tests - set(touched))
    for name, why in sorted(touched.items()):
        print(f"flake-quarantine: `{name}` stays in the required lane ({why})")
    names_file.write_text("".join(n + "\n" for n in kept), encoding="utf-8")
    regex_file.write_text(exclude_fragment(kept), encoding="utf-8")
    if kept:
        print(f"::notice title=flake-quarantine::{len(kept)} quarantined test(s) run in the advisory "
              f"step, not the gate: {json.dumps(kept)}")
    return 0


def cmd_sweep(a: argparse.Namespace, now: dt.datetime | None = None) -> int:
    now = now or dt.datetime.now(dt.timezone.utc)
    mode = fx.resolve_mode(a.mode if a.mode is not None else os.environ.get(fx.MODE_ENV))
    dry = a.dry_run or mode != fx.MODE_ENFORCE
    issues = list_issues(a.repo, a.bot_login)
    if issues is None:
        print("flake-quarantine: issues unreadable; nothing changed", file=sys.stderr)
        return 1
    records, refusals = load_records(a.repo, now)
    for r in refusals:
        print(f"flake-quarantine: refused {r}")
    actions = plan_sweep(records, issues, now, a.threshold, a.bot_login)
    print(f"flake-quarantine: {len(records)} excusal record(s) in {WINDOW_DAYS} d; "
          f"{len(actions)} action(s){' (dry run)' if dry else ''}")
    for act in actions:
        print(f"  {act.kind}: {act.test} issue={act.issue} runs={list(act.runs)}")
    if dry or not actions:
        return 0
    ensure_labels(a.repo)
    return 1 if apply(a.repo, actions, now) else 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    act = sub.add_parser("active")
    act.add_argument("--repo", required=True)
    act.add_argument("--sha", required=True)
    act.add_argument("--pr", type=int, default=None)
    act.add_argument("--source-root", required=True)
    act.add_argument("--out-dir", required=True)
    sw = sub.add_parser("sweep")
    sw.add_argument("--repo", required=True)
    sw.add_argument("--dry-run", action="store_true")
    sw.add_argument("--threshold", type=int, default=THRESHOLD)
    for p in (act, sw):
        p.add_argument("--mode", default=None, help=f"override ${fx.MODE_ENV}")
        p.add_argument("--bot-login", default=BOT_LOGIN)
    a = ap.parse_args(argv[1:])
    if a.cmd == "active":
        try:
            return cmd_active(a)
        except Exception as exc:  # noqa: BLE001 - a failed read keeps every test required
            print(f"::warning title=flake-quarantine::no quarantine applied ({exc!r})")
            return 0
    return cmd_sweep(a)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
