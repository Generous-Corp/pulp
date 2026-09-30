#!/usr/bin/env python3
"""Pulp's collector for the reuse-policy replay: GitHub history and ctest logs -> neutral corpus.

`reuse_policy_replay.py` holds the project-neutral part (record schema,
policies, scoring, scenarios). This module is the Pulp adapter behind its
`collect` subcommand: it knows build.yml's job names and log shape, ctest's
result lines and `--repeat until-pass` retry lines, the receipt notices and
decision annotations, the required-context ruleset, and how a merge-group
commit binds to its PR head. Everything it writes is the neutral JSONL the
scorer reads.
"""
from __future__ import annotations

import concurrent.futures
import datetime as dt
import gzip
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any, Callable, Iterable, Iterator

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
from reuse_policy_replay import (  # noqa: E402
    FAIL_OUTCOMES, GREEN_CONCLUSIONS, MIN_SELECTED_PERCENT, PAIR_SCHEMA, RUN_SCHEMA,
    SCRIPT_INPUTS_PATH, TEST_SCHEMA, _parse_time, validate_run, write_jsonl,
)

# --------------------------------------------------------------------------
# ctest job-log parsing
# --------------------------------------------------------------------------

_TS = r"^\S+Z\s"
TEST_LINE_RE = re.compile(
    _TS + r"\s*(?:(?P<index>\d+)/(?P<total>\d+) )?\s*Test\s+#(?P<num>\d+): (?P<name>.+?) \.*\s*"
    r"(?P<status>Passed|\*\*\*[^\d]*?)\s+(?P<sec>\d+(?:\.\d+)?) sec\s*$")
CTEST_START_RE = re.compile(_TS + r"Test project ")
CTEST_SUMMARY_RE = re.compile(_TS + r"\d+% tests passed, (?P<failed>\d+) tests? failed out of (?P<total>\d+)")
GIT_LOG_CMD_RE = re.compile(_TS + r"\[command\]\S*git log -1 --format=%H\s*$")
SHA_LINE_RE = re.compile(_TS + r"(?P<sha>[0-9a-f]{40})\s*$")
GATE_ARGS_RE = re.compile(_TS + r"ctest gate args: label_exclude=(?P<exclude>\S*)")
# Workflow commands render without their title in job logs; match the rendered
# line, never the step script that echoes it (which is also in the log).
RECEIPT_ISSUED_RE = re.compile(_TS + r"##\[notice\]exact-tree receipt for [0-9a-f]{40} on base [0-9a-f]{40}")
RECEIPT_NOT_ISSUED_RE = re.compile(_TS + r"##\[warning\].* the merge group will validate in full")
# Bump when parse_job_log's output changes, so cached parses are redone.
PARSER_VERSION = 2


def outcome_of(status: str) -> str:
    status = status.strip("* ").lower()
    if status == "passed":
        return "pass"
    if status.startswith("skipped") or status.startswith("not run (disabled)"):
        return "skipped"
    if status.startswith("timeout"):
        return "timeout"
    return "fail"


def parse_job_log(lines: Iterable[str]) -> dict:
    """The facts a job log carries: the commit checked out, the ctest session
    that ran the most tests (per-test final outcome, attempts, seconds), and
    whether a receipt was issued."""
    checkout_sha = None
    expect_sha = False
    label_exclude = None
    receipt = None
    sessions: list[dict] = []
    current: dict | None = None
    for line in lines:
        if expect_sha:
            match = SHA_LINE_RE.match(line)
            if match and checkout_sha is None:
                checkout_sha = match.group("sha")
            expect_sha = False
        if GIT_LOG_CMD_RE.match(line):
            expect_sha = True
            continue
        if CTEST_START_RE.match(line):
            current = {"tests": {}, "summary": None, "total": None}
            sessions.append(current)
            continue
        if current is not None:
            match = TEST_LINE_RE.match(line)
            if match:
                num = int(match.group("num"))
                rec = current["tests"].get(num)
                if rec is None:
                    rec = current["tests"][num] = {"test_id": match.group("name").rstrip(" ."), "num": num,
                                                   "attempts": 0, "duration_s": 0.0}
                rec["attempts"] += 1
                rec["duration_s"] = round(rec["duration_s"] + float(match.group("sec")), 3)
                rec["outcome"] = outcome_of(match.group("status"))
                if match.group("total"):
                    current["total"] = int(match.group("total"))
                continue
            match = CTEST_SUMMARY_RE.match(line)
            if match:
                current["summary"] = {"failed": int(match.group("failed")), "total": int(match.group("total"))}
                continue
        match = GATE_ARGS_RE.match(line)
        if match:
            label_exclude = match.group("exclude")
        if RECEIPT_NOT_ISSUED_RE.match(line):
            receipt = False
        elif receipt is None and RECEIPT_ISSUED_RE.match(line):
            receipt = True
    main = max(sessions, key=lambda s: len(s["tests"]), default=None)
    tests = sorted(main["tests"].values(), key=lambda r: r["num"]) if main else []
    failed = sum(1 for t in tests if t["outcome"] in FAIL_OUTCOMES)
    ctest = {
        "ran": bool(tests),
        "complete": bool(main and main["summary"] is not None),
        "selected": (main or {}).get("total"),
        "executed": len(tests),
        "failed": failed,
        "test_seconds": round(sum(t["duration_s"] for t in tests), 3),
        "label_exclude": label_exclude,
    }
    return {"checkout_sha": checkout_sha, "ctest": ctest, "tests": tests, "receipt_issued": receipt}


# --------------------------------------------------------------------------
# collect: GitHub history -> corpus
# --------------------------------------------------------------------------

API = "https://api.github.com"
REPOSITORY = "Generous-Corp/pulp"
WORKFLOW = "build.yml"
QUEUE_BRANCH_RE = re.compile(r"^gh-readonly-queue/[^/]+/pr-(?P<pr>\d+)-(?P<base>[0-9a-f]{40})$")
CTEST_JOB_NAMES = (re.compile(r"^macOS \(ARM64\)"), re.compile(r"^macos$"))


class _CredentialSafeRedirect(urllib.request.HTTPRedirectHandler):
    """Follow a log redirect to blob storage without forwarding the token."""

    def redirect_request(self, request, fp, code, msg, headers, new_url):
        redirected = super().redirect_request(request, fp, code, msg, headers, new_url)
        if redirected is not None and (urllib.parse.urlsplit(request.full_url).netloc
                                       != urllib.parse.urlsplit(new_url).netloc):
            redirected.remove_header("Authorization")
        return redirected


class GitHub:
    """Minimal REST client: token refresh on 401, backoff on rate limits."""

    def __init__(self, repository: str, token: str | None, reserve: int = 2500) -> None:
        self.repository = repository
        self._token = token
        self._explicit = token is not None
        self._lock = threading.Lock()
        self.reserve = reserve
        self.calls = 0

    def token(self, refresh: bool = False) -> str:
        with self._lock:
            if self._token and not refresh:
                return self._token
            if self._explicit and self._token:
                return self._token
            for var in ("GH_TOKEN", "GITHUB_TOKEN"):
                if os.environ.get(var) and not refresh:
                    self._token = os.environ[var]
                    return self._token
            ghapp = shutil.which("ghapp")
            if not ghapp:
                raise RuntimeError("no token: pass --token, set GH_TOKEN, or install ghapp")
            self._token = subprocess.run([ghapp, "auth", "token"], check=True, capture_output=True,
                                         text=True).stdout.strip()
            return self._token

    def _request(self, url: str, accept: str) -> Any:
        for attempt in range(6):
            req = urllib.request.Request(url, headers={
                "Authorization": f"Bearer {self.token()}", "Accept": accept,
                "X-GitHub-Api-Version": "2022-11-28", "User-Agent": "pulp-reuse-policy-replay"})
            opener = urllib.request.build_opener(_CredentialSafeRedirect)
            try:
                self.calls += 1
                resp = opener.open(req, timeout=120)
                remaining = resp.headers.get("x-ratelimit-remaining")
                if remaining is not None and int(remaining) < self.reserve:
                    reset = int(resp.headers.get("x-ratelimit-reset", "0"))
                    wait = max(0, reset - int(time.time())) + 5
                    print(f"collect: rate limit reserve reached ({remaining} left); sleeping {wait}s",
                          file=sys.stderr)
                    time.sleep(wait)
                return resp
            except urllib.error.HTTPError as err:
                if err.code == 401 and urllib.parse.urlsplit(err.url or url).netloc == "api.github.com":
                    self.token(refresh=True)
                    continue
                if err.code in (403, 429) and err.headers.get("x-ratelimit-remaining") == "0":
                    reset = int(err.headers.get("x-ratelimit-reset", "0"))
                    time.sleep(max(0, reset - int(time.time())) + 5)
                    continue
                if err.code >= 500:
                    time.sleep(2 ** attempt)
                    continue
                raise
            except (urllib.error.URLError, TimeoutError, ConnectionError):
                time.sleep(2 ** attempt)
        raise RuntimeError(f"GitHub request kept failing: {url}")

    def json(self, path: str) -> Any:
        url = path if path.startswith("http") else f"{API}/{path.lstrip('/')}"
        with self._request(url, "application/vnd.github+json") as resp:
            return json.loads(resp.read().decode("utf-8"))

    def job_log_lines(self, job_id: int) -> Iterator[str]:
        url = f"{API}/repos/{self.repository}/actions/jobs/{job_id}/logs"
        with self._request(url, "application/vnd.github+json") as resp:
            for raw in resp:
                yield raw.decode("utf-8", "replace").rstrip("\r\n")


def _days(since: dt.datetime, until: dt.datetime) -> Iterator[str]:
    day = since.date()
    while day <= until.date():
        yield day.isoformat()
        day += dt.timedelta(days=1)


class Collector:
    def __init__(self, gh: GitHub, out: Path, repo: Path, workers: int) -> None:
        self.gh = gh
        self.out = out
        self.repo = repo
        self.workers = workers
        self.cache = out / "cache"
        self.cache.mkdir(parents=True, exist_ok=True)
        self._git_lock = threading.Lock()
        self._commit_cache: dict[str, dict | None] = {}

    # -- cached reads ------------------------------------------------------
    def _cached(self, name: str, fetch: Callable[[], Any], keep: bool = True) -> Any:
        path = self.cache / name
        if keep and path.exists():
            with gzip.open(path, "rt", encoding="utf-8") as handle:
                return json.load(handle)
        value = fetch()
        if keep:
            path.parent.mkdir(parents=True, exist_ok=True)
            # Two workers can fetch the same key (a head run shared by several
            # groups); each writes its own temporary file, the last rename wins.
            tmp = path.with_name(f"{path.name}.{os.getpid()}.{threading.get_ident()}.tmp")
            with gzip.open(tmp, "wt", encoding="utf-8") as handle:
                json.dump(value, handle)
            tmp.replace(path)
        return value

    def list_runs(self, event: str, since: dt.datetime, until: dt.datetime) -> list[dict]:
        today = dt.datetime.now(dt.timezone.utc).date().isoformat()
        runs: list[dict] = []
        for day in _days(since, until):
            def fetch(day=day) -> list[dict]:
                rows, page = [], 1
                while True:
                    doc = self.gh.json(f"repos/{self.gh.repository}/actions/workflows/{WORKFLOW}/runs"
                                       f"?event={event}&created={day}&per_page=100&page={page}")
                    batch = doc.get("workflow_runs", [])
                    rows += [{k: r.get(k) for k in ("id", "head_sha", "head_branch", "created_at",
                                                    "updated_at", "status", "conclusion", "event",
                                                    "run_attempt", "path")} for r in batch]
                    if len(batch) < 100 or page >= 10:
                        return rows
                    page += 1
            rows = self._cached(f"runs/{event}-{day}.json.gz", fetch, keep=day < today)
            runs += [r for r in rows if since <= _parse_time(r["created_at"]) <= until]
        return runs

    def jobs(self, run_id: int) -> list[dict]:
        def fetch() -> list[dict]:
            doc = self.gh.json(f"repos/{self.gh.repository}/actions/runs/{run_id}/jobs?per_page=100&filter=latest")
            return [{k: j.get(k) for k in ("id", "name", "status", "conclusion", "runner_name",
                                           "started_at", "completed_at")}
                    | {"steps": [{"name": s.get("name"), "conclusion": s.get("conclusion")}
                                 for s in j.get("steps") or []]}
                    for j in doc.get("jobs", [])]
        return self._cached(f"jobs/{run_id}.json.gz", fetch)

    def parsed_log(self, job_id: int) -> dict:
        def fetch() -> dict:
            try:
                return parse_job_log(self.gh.job_log_lines(job_id))
            except urllib.error.HTTPError as err:
                if err.code not in (404, 410):
                    raise
                # Expired past the log retention window: nothing is known.
                return {"checkout_sha": None, "ctest": {"log_unavailable": True}, "tests": [],
                        "receipt_issued": None}
        return self._cached(f"logs-v{PARSER_VERSION}/{job_id}.json.gz", fetch)

    def decision(self, job_id: int) -> str | None:
        def fetch() -> list:
            return self.gh.json(f"repos/{self.gh.repository}/check-runs/{job_id}/annotations?per_page=100")
        verdict = None
        for ann in self._cached(f"annotations/{job_id}.json.gz", fetch):
            if ann.get("title") != "shipyard-receipt-decision":
                continue
            try:
                note = json.loads(ann.get("message") or "{}")
            except json.JSONDecodeError:
                continue
            if note.get("target") == "macos":
                verdict = note.get("verdict")
        return verdict

    def required_contexts_green(self, head_sha: str, required: tuple[str, ...],
                                as_of: str | None = None) -> bool | None:
        """Every required context other than macos green on the head, reading
        only check-runs that had completed by `as_of` (the group's creation)."""
        def fetch() -> list:
            rows, page = [], 1
            while True:
                doc = self.gh.json(f"repos/{self.gh.repository}/commits/{head_sha}/check-runs?per_page=100&page={page}")
                batch = doc.get("check_runs", [])
                rows += [{k: c.get(k) for k in ("name", "conclusion", "completed_at")} for c in batch]
                if len(batch) < 100 or page >= 5:
                    return rows
                page += 1
        try:
            runs = self._cached(f"checks/{head_sha}.json.gz", fetch)
        except urllib.error.HTTPError:
            return None
        latest: dict[str, dict] = {}
        for cr in runs:
            name = cr.get("name")
            if as_of and (not cr.get("completed_at") or cr["completed_at"] > as_of):
                continue
            if name and (name not in latest or str(cr.get("completed_at") or "") >= str(latest[name].get("completed_at") or "")):
                latest[name] = cr
        for context in required:
            if context == "macos":
                continue  # the full-suite ctest facts stand in for the head's own macos check
            cr = latest.get(context)
            if cr is None or cr.get("conclusion") not in GREEN_CONCLUSIONS:
                return False
        return True

    def merged_at(self, since: dt.datetime) -> dict[int, str | None]:
        """merged_at per closed pull request updated since `since`."""
        out: dict[int, str | None] = {}
        page = 1
        while page <= 100:
            batch = self.gh.json(f"repos/{self.gh.repository}/pulls?state=closed&sort=updated"
                                 f"&direction=desc&per_page=100&page={page}")
            for pr in batch:
                out[int(pr["number"])] = pr.get("merged_at")
            if len(batch) < 100 or _parse_time(batch[-1]["updated_at"]) < since:
                return out
            page += 1
        return out

    # -- git ---------------------------------------------------------------
    def _git(self, *args: str, check: bool = True, stdin: str | None = None) -> subprocess.CompletedProcess:
        return subprocess.run(["git", "-C", str(self.repo), *args], capture_output=True, text=True,
                              check=check, input=stdin, env=self._git_env)

    @property
    def _git_env(self) -> dict[str, str]:
        """Read commits through any shallow grafts in the clone.

        A shared clone can carry grafts that some other tool's `--depth`
        fetch wrote; they make a merge commit look parentless and cut main's
        history short. Parents and ancestry must come from the commit
        objects, so point git at an empty shallow file for these reads."""
        env = getattr(self, "_env", None)
        if env is None:
            empty = self.cache / "empty-shallow"
            empty.parent.mkdir(parents=True, exist_ok=True)
            empty.write_text("")
            # GIT_NO_LAZY_FETCH: a missing object in a partial clone reads as
            # missing (commit() then asks the API) instead of fetching into it.
            env = self._env = {**os.environ, "GIT_SHALLOW_FILE": str(empty), "GIT_NO_LAZY_FETCH": "1"}
        return env

    def _read_commits(self, shas: list[str]) -> dict[str, dict]:
        """Tree and parents of every local commit among `shas`, in two git calls."""
        if not shas:
            return {}
        check = self._git("cat-file", "--batch-check", check=False, stdin="\n".join(shas) + "\n")
        present = [line.split()[0] for line in check.stdout.splitlines()
                   if len(line.split()) == 3 and line.split()[1] == "commit"]
        if not present:
            return {}
        log = self._git("log", "--no-walk=unsorted", "--format=%H %T %P", "--stdin", check=False,
                        stdin="\n".join(present) + "\n")
        found: dict[str, dict] = {}
        for line in log.stdout.splitlines():
            sha, tree, *parents = line.split()
            found[sha] = {"tree": tree, "parents": parents, "local": True}
        return found

    def prime_commits(self, shas: Iterable[str]) -> None:
        """Read tree and parents of the local ones among `shas` in one pass.

        Never fetches: the collector reads the clone it is given and writes
        nothing to it: a by-sha fetch into a shallow or partial clone can add
        shallow grafts that truncate history for every worktree sharing it.
        A commit that is not local is described from the API by commit()."""
        wanted = [s for s in dict.fromkeys(shas) if s]
        with self._git_lock:
            self._commit_cache.update(self._read_commits(
                [s for s in wanted if not (self._commit_cache.get(s) or {}).get("local")]))

    def commit(self, sha: str | None) -> dict | None:
        if not sha:
            return None
        with self._git_lock:
            if sha not in self._commit_cache:
                res = self._git("show", "-s", "--format=%T %P", sha, check=False)
                if not res.returncode:
                    tree, *parents = res.stdout.split()
                    self._commit_cache[sha] = {"tree": tree, "parents": parents, "local": True}
                else:
                    # An ejected group's merge commit is often no longer
                    # fetchable, but the API still describes it.
                    try:
                        doc = self.gh.json(f"repos/{self.gh.repository}/git/commits/{sha}")
                        self._commit_cache[sha] = {"tree": doc["tree"]["sha"], "local": False,
                                                   "parents": [p["sha"] for p in doc.get("parents", [])]}
                    except (urllib.error.HTTPError, KeyError):
                        self._commit_cache[sha] = None
            return self._commit_cache[sha]

    def diff_files(self, a: str, b: str) -> list[str] | None:
        res = self._git("diff", "--no-renames", "--name-only", a, b, check=False)
        return None if res.returncode else sorted(p for p in res.stdout.splitlines() if p)

    def drift(self, head: dict, group: dict) -> tuple[list[str] | None, str | None]:
        """Files that differ between the head run's and the group's checkouts.

        When either merge commit is not available locally, both are merges of
        the same PR head, so every differing file changed between the two
        bases: that base diff is a SUPERSET of the drift, which can only make
        a drift look less inert, never more."""
        if head["merge_tree"] == group["merge_tree"]:
            return [], "trees"
        a, b = self.commit(head["checkout_sha"]), self.commit(group["checkout_sha"])
        if a and b and a.get("local") and b.get("local"):
            return self.diff_files(head["checkout_sha"], group["checkout_sha"]), "trees"
        if head["checkout_parents"][1] != group["checkout_parents"][1]:
            return None, None
        self.prime_commits([head["base_sha"], group["base_sha"]])
        files = self.diff_files(head["base_sha"], group["base_sha"])
        return (files, "bases") if files else (None, None)

    def input_list_at(self, rev: str) -> set[str] | None:
        res = self._git("show", f"{rev}:{SCRIPT_INPUTS_PATH}", check=False)
        if res.returncode:
            return None
        try:
            doc = json.loads(res.stdout)
        except json.JSONDecodeError:
            return None
        return {i.rstrip("/") for entry in doc.get("tests", {}).values() for i in entry.get("inputs", [])}

    def declared_input_hits(self, group: dict, files: list[str]) -> list[str] | None:
        """Drifted files a script test declares, by the list at the group's
        checkout, or (not available locally) the union of its parents' lists."""
        inputs = self.input_list_at(group["checkout_sha"])
        if inputs is None:
            lists = [self.input_list_at(p) for p in group["checkout_parents"]]
            if any(lst is None for lst in lists):
                return None
            inputs = set().union(*lists)
        return sorted(f for f in files if any(f == i or f.startswith(i + "/") for i in inputs))

    # -- runs --------------------------------------------------------------
    @staticmethod
    def ctest_job(jobs: list[dict]) -> dict | None:
        for pattern in CTEST_JOB_NAMES:
            for job in jobs:
                if (pattern.match(job.get("name") or "") and job.get("runner_name")
                        and not str(job.get("runner_name")).startswith("GitHub Actions")
                        and job.get("conclusion") in ("success", "failure")):
                    return job
        return None

    def run_record(self, run: dict, kind: str, pr: int | None, head_sha: str | None) -> tuple[dict, list[dict]]:
        jobs = self.jobs(run["id"])
        job = self.ctest_job(jobs)
        parsed = self.parsed_log(job["id"]) if job else {"checkout_sha": None, "ctest": {}, "tests": [],
                                                           "receipt_issued": None}
        checkout = parsed["checkout_sha"]
        if checkout is None and job is None and kind == "merge_group":
            # No suite ran (reused, or no native input): the group's commit is
            # still exactly the run's own head.
            checkout = run["head_sha"]
        commit = self.commit(checkout)
        record = {
            "schema": RUN_SCHEMA, "run_id": str(run["id"]), "run_kind": kind, "pr": pr,
            "head_sha": head_sha, "group_sha": run["head_sha"] if kind == "merge_group" else None,
            "workflow_head_sha": run["head_sha"], "created_at": run["created_at"],
            "completed_at": run.get("updated_at"), "run_conclusion": run.get("conclusion"),
            "job_id": job["id"] if job else None, "job_conclusion": job["conclusion"] if job else None,
            "runner_name": job.get("runner_name") if job else None, "runner_image": None,
            "checkout_sha": checkout,
            "checkout_parents": commit["parents"] if commit else None,
            "base_sha": commit["parents"][0] if commit and commit["parents"] else None,
            "merge_tree": commit["tree"] if commit else None,
            "ctest": parsed["ctest"], "receipt_issued": parsed["receipt_issued"],
            "build_failed": bool(job and job["conclusion"] == "failure" and not parsed["ctest"].get("ran")),
            "required_contexts_green": None, "observed_decision": None,
        }
        if kind == "merge_group":
            reuse_job = next((j for j in jobs if j.get("name") == "protected-receipt-reuse"
                              and j.get("conclusion") == "success"), None)
            if reuse_job:
                record["observed_decision"] = self.decision(reuse_job["id"])
        return record, parsed["tests"]

    def collect(self, since: dt.datetime, until: dt.datetime, lookback_days: int = 7) -> dict:
        groups = self.list_runs("merge_group", since, until)
        groups = [g for g in groups if QUEUE_BRANCH_RE.match(g.get("head_branch") or "")
                  and g.get("status") == "completed"]
        print(f"collect: {len(groups)} completed merge-group runs", file=sys.stderr)
        heads_listing = self.list_runs("pull_request", since - dt.timedelta(days=lookback_days), until)
        by_head: dict[str, list[dict]] = {}
        for run in heads_listing:
            if run.get("status") == "completed":
                by_head.setdefault(run["head_sha"], []).append(run)

        with concurrent.futures.ThreadPoolExecutor(self.workers) as pool:
            for _ in pool.map(lambda g: self.jobs(g["id"]) and None, groups):
                pass
        group_logs = [self.ctest_job(self.jobs(g["id"])) for g in groups]
        with concurrent.futures.ThreadPoolExecutor(self.workers) as pool:
            # Warm the log cache only; keeping the parses would hold every
            # run's per-test rows in memory at once.
            for _ in pool.map(lambda j: j and self.parsed_log(j["id"]) and None, group_logs):
                pass
        self.prime_commits([self.parsed_log(j["id"])["checkout_sha"] for j in group_logs if j]
                            + [g["head_sha"] for g in groups])

        runs: dict[str, dict] = {}
        pairs: list[dict] = []
        group_records: list[dict] = []
        for g in groups:
            pr = int(QUEUE_BRANCH_RE.match(g["head_branch"]).group("pr"))
            commit = self.commit(g["head_sha"])
            head_sha = commit["parents"][1] if commit and len(commit["parents"]) == 2 else None
            record, group_tests = self.run_record(g, "merge_group", pr, head_sha)
            runs[record["run_id"]] = record
            self.write_tests(record, group_tests)  # written now: 21k rows per run add up
            group_records.append(record)

        # Head candidates: completed PR-head runs of the same head created before
        # the group, latest first; logs are read until the first eligible one.
        def head_candidates(group: dict) -> list[dict]:
            created = _parse_time(group["created_at"])
            cands = [r for r in by_head.get(group["head_sha"] or "", []) if _parse_time(r["created_at"]) < created]
            return sorted(cands, key=lambda r: r["created_at"], reverse=True)

        required = _required_contexts(self.repo)

        def resolve_heads(group: dict) -> list[dict]:
            out = []
            for cand in head_candidates(group):
                if not self.ctest_job(self.jobs(cand["id"])):
                    continue
                out.append(cand)
                job = self.ctest_job(self.jobs(cand["id"]))
                parsed = self.parsed_log(job["id"])
                ctest = parsed["ctest"]
                if ctest.get("complete") and ctest.get("failed") == 0:
                    break
            return out

        with concurrent.futures.ThreadPoolExecutor(self.workers) as pool:
            resolved = list(pool.map(resolve_heads, group_records))
        self.prime_commits([self.parsed_log(self.ctest_job(self.jobs(c["id"]))["id"])["checkout_sha"]
                             for cands in resolved for c in cands])

        queue_commits = {g["group_sha"]: g for g in group_records}
        merged_at = self.merged_at(since - dt.timedelta(days=lookback_days))
        head_ids: list[list[str]] = []
        for group, cands in zip(group_records, resolved):
            ids = []
            for cand in cands:
                record, head_tests = self.run_record(cand, "pr_head", group["pr"], cand["head_sha"])
                group_selected = reference_selected(group, group_records)
                executed = record["ctest"].get("executed") or 0
                record["ctest"]["full_suite"] = (bool(group_selected) and
                                                 executed * 100 >= MIN_SELECTED_PERCENT * group_selected)
                if record["run_id"] not in runs:
                    runs[record["run_id"]] = record
                    self.write_tests(record, head_tests)
                ids.append(record["run_id"])
            head_ids.append(ids)

        def build_pair(item: tuple[dict, list[str]]) -> dict:
            group, ids = item
            head_rows = []
            for run_id in ids:
                record = runs[run_id]
                drift = hits = source = None
                if not validate_run(record) and not validate_run(group):
                    drift, source = self.drift(record, group)
                    if drift:
                        hits = self.declared_input_hits(group, drift)
                    elif drift == []:
                        hits = []
                green = None
                if record["ctest"].get("complete") and record["ctest"].get("failed") == 0:
                    green = self.required_contexts_green(record["head_sha"], required, group["created_at"])
                head_rows.append({"run_id": run_id, "drift_files": drift, "drift_source": source,
                                  "drift_declared_input_hits": hits, "required_contexts_green": green})
            return {"schema": PAIR_SCHEMA, "pr": group["pr"], "head_sha": group["head_sha"],
                    "group_run_id": group["run_id"], "heads": head_rows,
                    "stacked": _stacked(group, queue_commits, merged_at, self)}

        with concurrent.futures.ThreadPoolExecutor(self.workers) as pool:
            pairs = list(pool.map(build_pair, zip(group_records, head_ids)))

        for record in runs.values():
            record["rejected"] = validate_run(record)

        self.write(runs, pairs)
        manifest = {
            "schema": "pulp-reuse-replay-corpus/v1", "repository": self.gh.repository,
            "since": since.isoformat(), "until": until.isoformat(),
            "merge_groups": len(group_records),
            "pairs_with_head_run": sum(1 for p in pairs if p["heads"]),
            "head_runs": sum(1 for r in runs.values() if r["run_kind"] == "pr_head"),
            "rejected_runs": sum(1 for r in runs.values() if r["rejected"]),
            "api_calls": self.gh.calls,
        }
        (self.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        return manifest

    def write(self, runs: dict[str, dict], pairs: list[dict]) -> None:
        write_jsonl(self.out / "runs.jsonl", sorted(runs.values(), key=lambda r: r["created_at"]))
        write_jsonl(self.out / "pairs.jsonl", pairs)

    def write_tests(self, run: dict, rows: list[dict]) -> None:
        run_id = run["run_id"]
        write_jsonl(self.out / "tests" / f"{run_id}.jsonl.gz", (
                {"schema": TEST_SCHEMA, "run_id": run_id, "run_kind": run["run_kind"], "pr": run["pr"],
                 "head_sha": run["head_sha"], "base_sha": run["base_sha"], "merge_tree": run["merge_tree"],
                 "test_id": t["test_id"], "executable": None, "outcome": t["outcome"],
                 "attempts": t["attempts"], "duration_s": t["duration_s"], "source_key": None,
                 "output_key": None, "runner_image": run["runner_image"], "exonerated": None}
                for t in rows))


def reference_selected(group: dict, groups: list[dict]) -> int:
    """How many tests a full suite ran at the group's time: its own count, or,
    when it ran none (reused, build failed), the nearest group that did."""
    own = (group.get("ctest") or {}).get("executed") or 0
    if (group.get("ctest") or {}).get("complete") and own:
        return own
    when = _parse_time(group["created_at"])
    done = [g for g in groups if (g.get("ctest") or {}).get("complete") and (g["ctest"].get("executed") or 0)]
    if not done:
        return 0
    nearest = min(done, key=lambda g: abs((_parse_time(g["created_at"]) - when).total_seconds()))
    return nearest["ctest"]["executed"]


def _stacked(group: dict, queue_commits: dict[str, dict], merged_at: dict[int, str | None],
             collector: Collector) -> bool | None:
    """Was the group's base an entry that had not landed when the group was created?

    The queue lands with the MERGE method, so a landed entry's merge-group
    commit IS the main commit; it landed when its pull request merged."""
    base = group.get("base_sha")
    if not base:
        return None
    if collector._git("merge-base", "--is-ancestor", base, "origin/main", check=False).returncode:
        return True  # the base never reached main
    parent = queue_commits.get(base)
    if parent is None:
        return False  # a main commit whose own group predates the window
    landed = merged_at.get(parent["pr"])
    if landed is None:
        return None
    return _parse_time(landed) > _parse_time(group["created_at"])


def _required_contexts(repo: Path) -> tuple[str, ...]:
    import protected_merge_receipt as pmr
    return pmr.required_contexts_from_ruleset(repo / ".github" / "rulesets" / "main-protection.json")


