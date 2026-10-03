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
import http.client
import io
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
import zipfile
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
PARSER_VERSION = 4


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
            current = {"tests": {}, "summary": None, "total": None, "seen": set()}
            sessions.append(current)
            continue
        if current is not None:
            match = TEST_LINE_RE.match(line)
            if match:
                if line in current["seen"]:
                    continue  # the job log repeats a block verbatim; a retry has its own timestamp
                current["seen"].add(line)
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
REUSE_RECORD_ARTIFACT = "reuse-record-macos"
# The first merge-group and PR-head jobs that write a reuse record (the
# per-target codemodel recording landed then); earlier jobs never had one.
RECORDING_SINCE = "2026-10-01T07:46:19Z"
RECORD_STEP_PREFIX = "Record per-test results for reuse replay"
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

    def job_log_lines(self, job_id: int) -> list[str]:
        """The whole job log, or TruncatedLog when the body is shorter than
        its declared length. Iterating a response that drops mid-stream just
        ends early, and a log cut off mid-ctest parses as a plausible short
        run (no receipt, an incomplete suite) rather than as an error."""
        url = f"{API}/repos/{self.repository}/actions/jobs/{job_id}/logs"
        with self._request(url, "application/vnd.github+json") as resp:
            try:
                body = resp.read()
            except http.client.IncompleteRead as err:
                raise TruncatedLog(f"job {job_id}: {len(err.partial)} bytes before the stream ended") from err
            declared = resp.headers.get("Content-Length")
            if declared is not None and int(declared) != len(body):
                raise TruncatedLog(f"job {job_id}: {len(body)} of {declared} bytes")
        return body.decode("utf-8", "replace").splitlines()


class TruncatedLog(RuntimeError):
    """A job log arrived shorter than the length the server declared."""


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
            for attempt in range(4):
                try:
                    return parse_job_log(self.gh.job_log_lines(job_id))
                except urllib.error.HTTPError as err:
                    if err.code not in (404, 410):
                        raise
                    # Expired past the log retention window: nothing is known.
                    return {"checkout_sha": None, "ctest": {"log_unavailable": True}, "tests": [],
                            "receipt_issued": None}
                except (TruncatedLog, ConnectionError, TimeoutError, urllib.error.URLError):
                    if attempt == 3:
                        raise  # never cache a partial log as if it were the run
                    time.sleep(2 ** attempt)
            raise AssertionError("unreachable")
        try:
            return self._cached(f"logs-v{PARSER_VERSION}/{job_id}.json.gz", fetch)
        except TruncatedLog:
            # Unknown for this collect, and not cached, so the next one retries.
            return {"checkout_sha": None, "ctest": {"log_unavailable": True}, "tests": [],
                    "receipt_issued": None}

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

    def required_contexts(self, head_sha: str, required: tuple[str, ...],
                          as_of: str | None = None) -> dict[str, list[str]] | None:
        """The required contexts other than macos that were red, and those with
        no completed check-run, on the head as of `as_of` (the group's
        creation). None when the head's check-runs cannot be read."""
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
        out: dict[str, list[str]] = {"red": [], "absent": []}
        for context in required:
            if context == "macos":
                continue  # the full-suite ctest facts stand in for the head's own macos check
            cr = latest.get(context)
            if cr is None:
                out["absent"].append(context)
            elif cr.get("conclusion") not in GREEN_CONCLUSIONS:
                out["red"].append(context)
        return out

    def reuse_record(self, run_id: str) -> dict:
        """What a run's `reuse-record-macos` artifact says about its build:
        `targets` (per CMake target: digest, type, artifacts, dependencies),
        `link` (per executable: direct objects and the archive members its
        link pulled, names expanded), `executables` (test id -> the
        executable it ran), `binaries` (executable -> sha256 of the bytes
        the job ran), the codemodel's `digest_schema` and `generated_headers`
        coverage, and `declared_commit_bound` (executables whose tests the
        record marks `commit_bound`, plus codemodel targets flagged so; None
        when the job did not declare them). A part the run did not record is None."""
        def member(zf: zipfile.ZipFile, prefix: str) -> dict | None:
            name = next((n for n in zf.namelist() if Path(n).name.startswith(prefix)), None)
            return json.loads(zf.read(name)) if name else None

        def fetch() -> dict:
            listing = self.gh.json(f"repos/{self.gh.repository}/actions/runs/{run_id}/artifacts?per_page=100")
            art = next((a for a in listing.get("artifacts", [])
                        if a.get("name") == REUSE_RECORD_ARTIFACT and not a.get("expired")), None)
            if art is None:
                return {"targets": None, "link": None, "executables": None, "binaries": None,
                        "digest_schema": None, "generated_headers": None, "declared_commit_bound": None}
            with self.gh._request(art["archive_download_url"], "application/vnd.github+json") as resp:
                blob = resp.read()
            with zipfile.ZipFile(io.BytesIO(blob)) as zf:
                model, links, identity = member(zf, "codemodel-"), member(zf, "link-members-"), member(zf, "identity.json")
                tests = None
                # Executables of tests the record marks commit_bound. A row
                # with no verdict (null: the job had no codemodel) makes the
                # whole declaration unknown.
                bound_exes: set[str] = set()
                bound_known = False
                name = next((n for n in zf.namelist() if Path(n).name == "tests.jsonl"), None)
                if name:
                    tests = {}
                    bound_known = True
                    for line in zf.read(name).decode("utf-8", "replace").splitlines():
                        if not line.strip():
                            continue
                        row = json.loads(line)
                        exe = (row.get("executable") or "").removeprefix("<build>/")
                        if exe:
                            tests[row["test_id"]] = exe
                        if row.get("commit_bound") is None:
                            bound_known = False
                        elif row["commit_bound"] and exe:
                            bound_exes.add(exe)
            link = None
            if links is not None:
                names = links.get("members") or {}
                link = {exe.removeprefix("<build>/"): {
                            "objects": [o.removeprefix("<build>/") for o in rec.get("objects") or []],
                            "members": {a.removeprefix("<build>/"): (list(names.get(a) or []) if arc.get("whole") else
                                                                    [names[a][i] for i in arc.get("members") or []
                                                                     if a in names and i < len(names[a])])
                                        for a, arc in (rec.get("archives") or {}).items()}}
                        for exe, rec in (links.get("executables") or {}).items()}
            targets = None if model is None else {
                n: {k: t.get(k) for k in ("digest", "type", "artifacts", "dependencies")}
                for n, t in (model.get("targets") or {}).items()}
            binaries = None if identity is None else {
                exe.removeprefix("<build>/"): rec.get("sha256")
                for exe, rec in (identity.get("executables") or {}).items() if exe.startswith("<build>/")}
            # Declared only when the build said which targets are commit-bound
            # and every test row carries a verdict; the codemodel's own flags
            # add bound targets no test of this job ran.
            declared = None
            if bound_known and model is not None and isinstance(model.get("commit_bound_declared"), list):
                declared = sorted(bound_exes | {a.removeprefix("<build>/") for t in (model.get("targets") or {}).values()
                                                if t.get("commit_bound") and t.get("type") == "EXECUTABLE"
                                                for a in t.get("artifacts") or []})
            return {"targets": targets, "link": link, "executables": tests, "binaries": binaries,
                    "digest_schema": None if model is None else model.get("schema"),
                    "generated_headers": None if model is None else model.get("generated_headers"),
                    "declared_commit_bound": declared}
        return self._cached(f"record-v4/{run_id}.json.gz", fetch)

    def codemodel_targets(self, run_id: str) -> dict[str, dict] | None:
        """The per-target codemodel digests a run recorded, or None."""
        return self.reuse_record(run_id)["targets"]

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

    def script_inputs_at(self, rev: str) -> dict | None:
        """The checked-in script-input list at `rev`, or None when it cannot
        be read. A run's checkout is a merge commit GitHub made, which the
        local clone has usually never fetched (and the collector never
        fetches), so a commit missing locally is read through the API."""
        res = self._git("show", f"{rev}:{SCRIPT_INPUTS_PATH}", check=False)
        text = None if res.returncode else res.stdout
        if text is None and self.gh is not None and re.fullmatch(r"[0-9a-f]{40}", rev):
            def fetch() -> str | None:
                url = f"{API}/repos/{self.gh.repository}/contents/{SCRIPT_INPUTS_PATH}?ref={rev}"
                try:
                    with self.gh._request(url, "application/vnd.github.raw+json") as resp:
                        return resp.read().decode("utf-8")
                except urllib.error.HTTPError as err:
                    if err.code == 404:
                        return None  # absent at that commit: cached
                    raise
            try:
                text = self._cached(f"script-inputs/{rev}.json.gz", fetch)
            except (urllib.error.URLError, RuntimeError, OSError, UnicodeDecodeError):
                text = None  # a failed fetch is unread (its script tests run), and is not cached
        if text is None:
            return None
        try:
            return json.loads(text)
        except json.JSONDecodeError:
            return None

    def input_list_at(self, rev: str) -> set[str] | None:
        doc = self.script_inputs_at(rev)
        if doc is None:
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

    def record_coverage(self, runs: Iterable[dict], since: str = RECORDING_SINCE) -> dict:
        """Completed runs whose macOS suite job executed a test step (it
        concluded success or failure) but that published no reuse record.
        A cancelled or skipped job ran nothing, and a job whose runner was
        lost (its later steps have no conclusion) never reached its
        record step (its later steps have no conclusion); neither is
        counted, the second is reported as `interrupted`. 0 is the expected reading; anything else is a
        recording that went missing although the suite ran."""
        executed, missing, interrupted = 0, [], []
        for run in runs:
            if run.get("status") != "completed" or run["created_at"] < since:
                continue
            job = self.ctest_job(self.jobs(run["id"]))
            if job is None:
                continue
            steps = job.get("steps") or []
            record_step = next((st for st in steps if str(st.get("name", "")).startswith(RECORD_STEP_PREFIX)), None)
            lost = (record_step is not None and record_step.get("conclusion") is None
                    and any(st.get("conclusion") is None for st in steps))
            ran_tests = any(str(st.get("name", "")).startswith("Test") and st.get("conclusion") in ("success", "failure")
                            for st in steps)
            if lost or not ran_tests:
                # The runner went away before the record step could run (its
                # later steps carry no conclusion): no step was left to warn.
                if lost:
                    interrupted.append({"run_id": str(run["id"]), "job_id": job["id"], "runner": job.get("runner_name"),
                                        "tests_ran": ran_tests})
                continue
            executed += 1
            listing = self._cached(f"artifacts/{run['id']}.json.gz", lambda r=run: [
                a.get("name") for a in self.gh.json(
                    f"repos/{self.gh.repository}/actions/runs/{r['id']}/artifacts?per_page=100").get("artifacts", [])])
            if not any(n == REUSE_RECORD_ARTIFACT or str(n).startswith(REUSE_RECORD_ARTIFACT + "-attempt-")
                       for n in listing):
                missing.append({"run_id": str(run["id"]), "event": run.get("event"), "job_id": job["id"],
                                "job_conclusion": job.get("conclusion")})
        return {"since": since, "executed_jobs": executed, "without_record": len(missing),
                "runs_without_record": missing[:50], "interrupted_jobs": len(interrupted),
                "interrupted": interrupted[:50]}

    def run_record(self, run: dict, kind: str, pr: int | None, head_sha: str | None) -> tuple[dict, list[dict]]:
        jobs = self.jobs(run["id"])
        job = self.ctest_job(jobs)
        parsed = self.parsed_log(job["id"]) if job else {"checkout_sha": None, "ctest": {}, "tests": [],
                                                           "receipt_issued": None}
        checkout = parsed["checkout_sha"]
        if checkout is None and kind == "merge_group":
            # The job never checked out (reused, no native input, cancelled
            # before checkout, log expired): the group's commit is still
            # exactly the run's own head, and no suite ran on it.
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
            # A build failure got as far as checking out and never reached ctest;
            # a job that failed before checkout (a cancelled leg) ran nothing.
            "build_failed": bool(job and job["conclusion"] == "failure" and parsed["checkout_sha"]
                                 and not parsed["ctest"].get("ran")),
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
                contexts = None
                if record["ctest"].get("complete") and record["ctest"].get("failed") == 0:
                    contexts = self.required_contexts(record["head_sha"], required, group["created_at"])
                head_rows.append({
                    "run_id": run_id, "drift_files": drift, "drift_source": source,
                    "drift_declared_input_hits": hits,
                    "required_contexts_green": None if contexts is None else not (contexts["red"] or contexts["absent"]),
                    "required_contexts_red": None if contexts is None else contexts["red"],
                    "required_contexts_absent": None if contexts is None else contexts["absent"]})
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
            "record_coverage": self.record_coverage(groups + [r for r in heads_listing
                                                              if since <= _parse_time(r["created_at"]) <= until]),
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




# --------------------------------------------------------------------------
# Tier-1a source keys reconstructed from git and a build graph
# --------------------------------------------------------------------------
#
# A per-executable source key matches the PR head's when nothing the
# executable is built from changed between the head run's tree and the
# group's tree. Recorded keys do not exist for history, so the replay
# reconstructs the verdict: the drift (files that differ between the two
# checkouts) is propagated through a Ninja graph (sources, headers via the
# recorded deps, archives, links) to the executables it rebuilds. A script
# test is keyed on its own entry in test/ctest_script_inputs.json. Three
# variants are written per pair:
#
#   per-entry    a compiled test runs when one of its executables is
#                rebuilt or one of its sources drifted; a script test runs
#                when its own entry changed or a declared input drifted.
#   strict-data  per-entry, plus fail-closed rules for what keys cannot
#                see yet: any drifted runtime data file (no compiled test
#                declares the data it reads) or CMake file runs every
#                compiled test; whole-tree tests (drift, lint, registry,
#                census, probe, ...) and tests whose registration names a
#                shared host resource (a RESOURCE_LOCK, or a gpu or
#                browser-capture label) always run; and a script test is
#                skippable only when it is labelled `hermetic`. This is the
#                variant to ship.
#   list-level   per-entry, except a script test also runs whenever the
#                list file itself changed. A negative control: it must read
#                lower than per-entry.
#   cmake-codemodel
#                strict-data, except a CMake change no longer re-keys every
#                target: only executables whose recorded file-API codemodel
#                digest (sources, compile groups, link line, test
#                registrations) differs between the head job and the group
#                job, or that depend on such a target, are re-keyed. Written
#                only when both jobs recorded a codemodel; an executable the
#                group's codemodel does not describe (a name the graph knows
#                but the recorded build does not) keeps the strict rule.
#
# A test the map does not know (an unmapped compiled test, an executable
# the graph does not build, a script test with no entry) always runs.

SOURCE_KEY_VARIANTS = ("per-entry", "strict-data", "list-level")
# Files that pin third-party dependencies. A bump can change a dependency's
# content without changing any path the codemodel digests or any archive
# the recorded graph follows (FetchContent archives are treated as pinned),
# so drift in one of them re-keys every executable.
DEPENDENCY_PIN_PATHS = frozenset({"tools/deps/manifest.json", "tools/cmake/PulpDependencies.cmake",
                                  "tools/cmake/PulpFetchContent.cmake"})
# The root CMakeLists.txt carries project(VERSION), which configure_file
# stamps into generated headers; the codemodel digests the generated file's
# path, not its content, so a version bump moves bytes no digest covers.
CONFIGURE_STAMP_PATHS = frozenset({"CMakeLists.txt"})
# A codemodel digest of this schema, with generated headers read from the
# Ninja dependency log, covers the CONTENT of every build-tree file a target
# compiles or includes, so a version bump re-keys only its consumers.
CONTENT_KEYED_SCHEMA = "pulp-codemodel-digest/v2"


def content_keyed(record: dict | None) -> bool:
    return bool(record) and record.get("digest_schema") == CONTENT_KEYED_SCHEMA \
        and record.get("generated_headers") == "ninja-deps"
# Registration labels that mean the test drives a shared host resource.
ENVIRONMENT_LABELS = frozenset({"gpu", "browser-capture"})


sys.path.insert(0, str(HERE.parent / "ci"))
from spawn_closure import SpawnIndex  # noqa: E402  tools/ci: the shared spawn-edge source


def _is_cmake(path: str) -> bool:
    return path.endswith(("CMakeLists.txt", ".cmake"))


def _declared_hit(inputs: Iterable[str], changed: Iterable[str]) -> bool:
    inputs = [i.rstrip("/") for i in inputs]
    return any(f == i or f.startswith(i + "/") for f in changed for i in inputs)


def environment_bound(mapped: dict) -> bool:
    return bool(mapped.get("resource_locks")) or bool(ENVIRONMENT_LABELS & set(mapped.get("labels") or []))


def codemodel_rekeyed(head: dict[str, dict], group: dict[str, dict],
                      types: tuple[str, ...] = ("EXECUTABLE",)) -> tuple[set[str], set[str]]:
    """(artifacts re-keyed by a codemodel change, artifacts the group's
    codemodel describes), both relative to the build dir, for targets of
    `types` (executables by default).

    A target is re-keyed when its digest differs from the head job's or it
    is new, and so is every target that depends on one, transitively: the
    digest names its dependencies but does not cover their content."""
    changed = {n for n, t in group.items() if n not in head or head[n].get("digest") != t.get("digest")}
    dependents: dict[str, set[str]] = {}
    for n, t in group.items():
        for d in t.get("dependencies") or []:
            dependents.setdefault(d, set()).add(n)
    stack = list(changed)
    while stack:
        for n in dependents.get(stack.pop(), ()):
            if n not in changed:
                changed.add(n)
                stack.append(n)

    def exes(names: Iterable[str]) -> set[str]:
        return {a.removeprefix("<build>/") for n in names for a in group[n].get("artifacts") or []
                if group[n].get("type") in types}
    return exes(changed), exes(group)


def spawn_scan_of(doc: dict | None, kind: str = "spawns") -> dict | None:
    """The `kind` scan (spawns or data) a checked-in script-input list
    carries: per-executable entries plus the set of executables the scan
    covered, or None when that list did not scan for it (an older tree, or
    an unreadable list)."""
    if not doc or kind not in (doc.get("executables_scanned_for") or []) \
            or not isinstance(doc.get("executables_scanned"), list):
        return None
    entries = dict(doc.get("executables") or {})
    if kind == "data" and not data_scan_saw_readers(entries):
        return None
    return {"entries": entries, "scanned": frozenset(doc["executables_scanned"])}


# A data scan that finds no reads in an executable is trusted only when it
# demonstrably detects the readers it already knows: this share of the
# executables with declared reads must carry a source the scan itself
# matched (`detected_sources`), or every executable falls to the broad rule.
DATA_SCAN_MIN_DETECTED = 0.85


def data_scan_saw_readers(entries: dict[str, dict]) -> bool:
    """Whether a data scan detected its known readers; a list that does not
    record what the scan matched cannot show it."""
    declared = [e for e in entries.values() if e.get("data") == "declared"]
    if not declared or any("detected_sources" not in e for e in declared):
        return False
    seen = sum(1 for e in declared if e["detected_sources"])
    return seen / len(declared) >= DATA_SCAN_MIN_DETECTED


def _scan_entry(executable: str, scan: dict) -> tuple[dict | None, bool]:
    name = os.path.basename(executable)
    name = name[:-4] if name.endswith(".exe") else name
    return scan["entries"].get(name), name in scan["scanned"]


def recorded_outputs(head_bins: dict[str, str] | None, group_bins: dict[str, str] | None,
                     link: Iterable[str]) -> tuple[frozenset[str], frozenset[str]] | None:
    """(executables both jobs recorded a hash for, those whose hash differs),
    or None when either job recorded none."""
    if head_bins is None or group_bins is None:
        return None
    hashed = frozenset(e for e in link if e in head_bins and e in group_bins)
    return hashed, frozenset(e for e in hashed if head_bins[e] != group_bins[e])


def data_hit(executable: str, scan: dict | None, drift: list[str], data: list[str]) -> bool:
    """Whether a drift reaches what one test executable reads from the
    checkout, by its data manifest entry.

    `data` is the drift's runtime-surface files. With no data scan, or an
    executable the scan did not cover, any of them counts. An entry whose
    reads are all declared is hit only through its declared inputs; one
    with undeclared reads (or any other state) by any runtime-surface file;
    `data: none`, or no entry for a scanned executable, by none."""
    if scan is None:
        return bool(data)
    entry, scanned = _scan_entry(executable, scan)
    if entry is None:
        return bool(data) and not scanned
    state = entry.get("data")
    if state == "none":
        return False
    if state == "declared":
        return _declared_hit(entry.get("inputs") or [], drift)
    return bool(data)


def spawn_status(executable: str, scan: dict | None) -> str:
    """`clean`, `undeclared` or `unknown` for one test executable (relative
    to the build dir) under a spawn scan.

    An entry whose `spawns` is absent, `declared` (its edges are in the
    codemodel) or `none` (reviewed: it runs nothing this repo builds) is
    clean; any other value is undeclared. A missing entry is clean only when
    the scan covered that executable; otherwise nothing is known about it."""
    if scan is None:
        return "unknown"
    entry, scanned = _scan_entry(executable, scan)
    if entry is None:
        return "clean" if scanned else "unknown"
    return "clean" if entry.get("spawns") in (None, "declared", "none") else "undeclared"


def classify_source_keys(drift: list[str], group_tests: list[str], test_map: dict[str, dict],
                         head_entries: dict[str, dict] | None, group_entries: dict[str, dict] | None,
                         rebuilt: set[str], all_executables: set[str],
                         codemodel: tuple[set[str], set[str]] | None = None,
                         commit_bound: frozenset[str] = frozenset(),
                         generated_keyed: bool = False,
                         spawns=None,
                         spawnable: frozenset[str] = frozenset(),
                         spawn_scan: dict | None = None,
                         data_scan: dict | None = None,
                         outputs: tuple[frozenset[str], frozenset[str]] | None = None,
                         modules: frozenset[str] = frozenset(),
                         rebuilt_modules: frozenset[str] = frozenset()) -> dict[str, dict]:
    """Per variant: the tests that must run and the share of executables rebuilt.

    A test can run or load other built programs. `spawns` (a
    tools/ci/spawn_closure.py SpawnIndex) gives each executable's spawn
    closure; a test whose closure holds a rebuilt executable or module runs.
    `spawnable` names the executables no test runs as its own (tools,
    helpers, fixtures) and `modules` the loadable modules. A test can reach
    one without any edge, so in the strict variants, when one is rebuilt, a
    test runs unless `spawn_scan` (see spawn_scan_of) vouches for its
    executable, and one the scan found spawning with no reviewed edge always
    runs. `rebuilt_modules` are the modules the drift rebuilds; the CMake
    rules widen it like the executables.

    `rebuilt` is the set of executables (relative to the build dir) the drift
    reaches through the graph; `all_executables` the ones the graph builds.
    The build share counts only the executables the group's own tests run,
    so a graph configured with more targets than the gate (examples) does
    not dilute it. Pure: the graph and git reads happen before."""
    import test_receipts_shadow  # tools/ci: the always-run names and the runtime-surface rule
    always_run = test_receipts_shadow.ALWAYS_RUN_NAME_RE
    surface = test_receipts_shadow.is_runtime_surface
    drift_set = set(drift)
    pins = bool(DEPENDENCY_PIN_PATHS & set(drift))
    cmake = any(_is_cmake(f) for f in drift) or pins
    data = sorted(f for f in drift if surface(f) and not _is_cmake(f) and f != SCRIPT_INPUTS_PATH)
    list_changed = SCRIPT_INPUTS_PATH in drift_set
    out: dict[str, dict] = {}
    counted = {e for name in group_tests for e in (test_map.get(name) or {}).get("executables") or []} & all_executables
    rekeyed, described = codemodel or (set(), set())
    # Executables whose bytes change with no input change (they embed the
    # commit) are rebuilt, and their tests run, in every pair.
    rekeyed = set(rekeyed) | set(commit_bound)
    # Without content-keyed digests a root CMakeLists.txt change (its
    # project(VERSION) reaches generated headers) re-keys everything; with
    # them the digests re-key exactly the consumers.
    stamped = bool(CONFIGURE_STAMP_PATHS & set(drift)) and not generated_keyed
    variants = SOURCE_KEY_VARIANTS + (("cmake-codemodel", "manifest-data") if codemodel is not None else ()) \
        + (("output-key",) if codemodel is not None and outputs is not None else ())
    hashed, changed = outputs or (frozenset(), frozenset())
    for variant in variants:
        # manifest-data is cmake-codemodel with the data rule scoped to each
        # executable's data manifest entry instead of every compiled test.
        # output-key is manifest-data keyed on outputs: an executable whose
        # recorded bytes are the same in both jobs is not rebuilt, whatever
        # its sources or codemodel did; one without a recorded hash in both
        # keeps the source key.
        exact_cmake = variant in ("cmake-codemodel", "manifest-data", "output-key")
        strict = variant in ("strict-data", "cmake-codemodel", "manifest-data", "output-key")
        by_output = variant == "output-key"
        reads = (lambda exes: any(data_hit(e, data_scan, drift, data) for e in exes)) \
            if variant in ("manifest-data", "output-key") else (lambda exes: bool(data))
        # Every executable this variant rebuilds, before narrowing to the
        # ones the group's tests run: a spawned tool counts too.
        if exact_cmake:
            rebuilt_all = all_executables if (pins or stamped) else \
                rebuilt | rekeyed | ((all_executables - described) if cmake else set())
        elif strict:
            rebuilt_all = all_executables if cmake else rebuilt | commit_bound
        else:
            rebuilt_all = set(rebuilt)
        if by_output:
            rebuilt_all = (rebuilt_all - hashed) | changed
        direct = rebuilt_all if by_output else rebuilt
        modules_rebuilt = set(modules) if (pins or stamped or (cmake and not exact_cmake)) else set(rebuilt_modules)
        runtime_rebuilt = rebuilt_all | modules_rebuilt
        # A rebuilt program some test may reach without an edge: tests the
        # spawn scan cannot vouch for run.
        spawn_all = strict and bool((spawnable | modules) & runtime_rebuilt)
        spawn_all_legacy = strict and bool(spawnable & rebuilt_all)
        fallback_only = 0  # compiled tests that run only because of the spawnable fallback
        fallback_only_legacy = 0  # the same under the earlier rule: no scan, every compiled test
        spawn_undeclared = 0  # compiled tests that run because the scan found an unreviewed spawn
        spawn_hit = (lambda exes: spawns is not None and any(spawns.closure(e) & runtime_rebuilt for e in exes)) \
            if strict else (lambda exes: False)
        # The re-key rule a CMake change triggers: everything, or only the
        # executables whose codemodel entry moved (and the undescribed ones).
        cmake_hit = (lambda exes: pins or stamped or bool(set(exes) & rekeyed)
                     or (cmake and not set(exes) <= described)) if exact_cmake \
            else (lambda exes: cmake or bool(set(exes) & commit_bound))
        if by_output:
            # The bytes answer for a hashed executable; the CMake rules only
            # for the rest.
            source_hit = cmake_hit
            cmake_hit = (lambda exes: any(e not in hashed for e in exes)
                         and source_hit([e for e in exes if e not in hashed]))
        run: list[str] = []
        for name in group_tests:
            mapped = test_map.get(name) or {}
            exes = mapped.get("executables") or []
            if strict and (always_run.search(name) or environment_bound(mapped)):
                run.append(name)
            elif exes:
                keyed = (not set(exes) <= all_executables or (strict and (cmake_hit(exes) or reads(exes)))
                         or set(exes) & direct or drift_set & set(mapped.get("sources") or []) or spawn_hit(exes))
                statuses = {spawn_status(e, spawn_scan) for e in exes} if strict else set()
                undeclared = "undeclared" in statuses
                fallback = spawn_all and "unknown" in statuses
                if keyed or undeclared or fallback:
                    run.append(name)
                if undeclared and not keyed:
                    spawn_undeclared += 1
                if fallback and not (keyed or undeclared):
                    fallback_only += 1
                if spawn_all_legacy and not keyed:
                    fallback_only_legacy += 1
            elif group_entries is not None and name in group_entries and head_entries is not None:
                entry = group_entries[name]
                if (head_entries.get(name) != entry or _declared_hit(entry.get("inputs") or [], drift)
                        or (variant == "list-level" and list_changed)
                        or (strict and "hermetic" not in (mapped.get("labels") or []))):
                    run.append(name)
            else:
                run.append(name)  # unknown inputs: never skippable
        total = len(counted)
        rebuilt_set = rebuilt_all & counted
        out[variant] = {"run": sorted(run), "executables_total": total, "executables_rebuilt": len(rebuilt_set),
                        "cmake_changed": cmake, "data_changed": data[:20],
                        "spawnable_rebuilt": sorted((spawnable | modules) & runtime_rebuilt)[:20],
                        # The spawnable fallback's cost: tests that run only
                        # because a rebuilt program might reach them unseen,
                        # now and under the rule before the spawn scan.
                        "spawnable_fallback": spawn_all, "fallback_only_tests": fallback_only,
                        "fallback_only_tests_legacy": fallback_only_legacy,
                        "spawn_scanned": spawn_scan is not None, "spawn_undeclared_tests": spawn_undeclared,
                        "data_scanned": data_scan is not None}
        if codemodel is not None:
            # The same counts over the executables the recorded build
            # describes, where the variants can be compared exactly, and for
            # the binary-hash control every executable the variant rebuilds,
            # including spawned tools and others no group test runs.
            out[variant]["described_total"] = len(counted & described)
            out[variant]["described_rebuilt"] = len(rebuilt_set & described)
            out[variant]["rebuilt"] = sorted(rebuilt_all)
    return out


# The recorded graph. The replay's Ninja graph comes from one configured
# build, whose executable names drift as tests are regrouped. Each job's
# reuse record names the executables it actually linked, their direct
# objects and the archive members each link pulled, so the executables a
# drift rebuilds are taken from the group's own record: an object or member
# is reached when its SOURCE (recovered from the object path) is a drifted
# source, or compiles a header the drift reaches. The older graph is used
# only for that source -> headers step, which follows source paths, not
# executable names. Anything the graph cannot place (a source added since
# the graph was built, an unknown archive member) counts as reached when its
# own source drifted (matched by name for a member) or when any header
# drifted, since a header is the only drift that reaches a source the graph
# never compiled. Toolchain, prebuilt and FetchContent archives are pinned; a
# CMake-level change to them reaches executables through the codemodel.

COMPILE_SUFFIXES = (".cpp", ".cc", ".cxx", ".c", ".mm", ".m")
CODE_SUFFIXES = COMPILE_SUFFIXES + (".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".inc", ".def")


def object_source(obj: str) -> str | None:
    """The repo-relative source a CMake object path compiles, or None.

    `<dir>/CMakeFiles/<target>.dir/<path>.o` compiles `<dir>/<path>`, with
    `__/` standing for `../`."""
    head, sep, rest = obj.partition("CMakeFiles/")
    if not sep or "/" not in rest or not rest.endswith(".o"):
        return None
    path = os.path.normpath(os.path.join(head, rest.split("/", 1)[1][:-2].replace("__/", "../")))
    return None if path.startswith("..") or os.path.isabs(path) else path


class GraphIndex:
    """Source-level facts from a Ninja graph: which compiled sources the
    drift reaches, and which sources each archive member was built from."""

    def __init__(self, graph, source_root: Path, build_dir: Path) -> None:
        root = os.path.realpath(source_root) + os.sep
        self.graph, self.root, self.build = graph, root, os.path.realpath(build_dir)
        self.obj_source: dict[str, str] = {}
        for src, outs in graph.src_to_out.items():
            if src.startswith(root) and src.endswith(COMPILE_SUFFIXES):
                for o in outs:
                    if o.endswith(".o"):
                        self.obj_source[self._rel(o)] = src[len(root):]
        self.sources = set(self.obj_source.values())
        self.archive_members: dict[str, dict[str, set[str]]] = {}
        for i, outs in graph.fwd.items():
            src = self.obj_source.get(self._rel(i))
            if src is None:
                continue
            for o in outs:
                if o.endswith(".a"):
                    self.archive_members.setdefault(self._rel(o), {}).setdefault(os.path.basename(i), set()).add(src)

    def _rel(self, path: str) -> str:
        return os.path.relpath(self.graph.norm(path), self.build)

    def affected_sources(self, drift: list[str]) -> set[str]:
        reached = self.graph.affected_outputs([os.path.join(self.root, f) for f in drift])
        out = {self.obj_source[r] for r in (self._rel(p) for p in reached) if r in self.obj_source}
        return out | {f for f in drift if f.endswith(COMPILE_SUFFIXES)}


def recorded_rebuilt(link: dict[str, dict], drift: list[str], index: "GraphIndex",
                     affected: set[str] | None = None) -> set[str]:
    """Executables (relative to the build dir) a drift rebuilds, from one
    job's recorded link members. Pure given `index`."""
    affected = index.affected_sources(drift) if affected is None else affected
    header_drift = any(f.endswith(CODE_SUFFIXES) and not f.endswith(COMPILE_SUFFIXES) for f in drift)
    drifted = set(drift)
    drifted_objects = {os.path.basename(f) + ".o" for f in drift if f.endswith(COMPILE_SUFFIXES)}
    out: set[str] = set()
    for exe, rec in link.items():
        hit = False
        for obj in rec.get("objects") or []:
            src = object_source(obj)
            if src is not None and src in index.sources:
                hit = src in affected
            else:
                hit = src in drifted or header_drift
            if hit:
                break
        if not hit:
            for archive, members in (rec.get("members") or {}).items():
                if os.path.isabs(archive) or archive.startswith("_deps/"):
                    continue
                known = index.archive_members.get(archive) or {}
                for m in members:
                    srcs = known.get(m)
                    if (srcs & affected) if srcs else (m in drifted_objects or header_drift):
                        hit = True
                        break
                if hit:
                    break
        if hit:
            out.add(exe)
    return out


def load_graph(build_dir: Path | None, pickle_path: Path | None):
    """A Ninja graph with header deps: from a pickled affected_tests_shadow
    Graph, or parsed from a configured build dir (slow on a full tree)."""
    import affected_tests_shadow as shadow  # tools/ci: the Ninja graph the affected-test shadow uses
    if pickle_path is not None:
        import pickle
        sys.modules.setdefault("ats", shadow)  # pickles made from a copy of the module
        with open(pickle_path, "rb") as handle:
            return pickle.load(handle)
    edges = shadow.parse_build_ninja((build_dir / "build.ninja").read_text(encoding="utf-8", errors="replace"))
    deps = shadow.parse_ninja_deps(subprocess.run(["ninja", "-C", str(build_dir), "-t", "deps"], check=True,
                                                  capture_output=True, text=True).stdout)
    return shadow.Graph(build_dir, edges, deps)


def annotate_source_keys(corpus_dir: Path, repo: Path, graph, graph_source_root: Path, graph_build_dir: Path,
                         test_map: dict[str, dict], only_runs: set[str] | None = None,
                         gh: "GitHub | None" = None, legacy_rules: bool = False) -> dict:
    """Write the source-key variants into each pair that has a valid head run
    with per-test records; with `gh`, also the cmake-codemodel variant for
    pairs whose head and group jobs both recorded a codemodel. Returns counts
    for the manifest."""
    import reuse_policy_replay as rpr
    corpus = rpr.Corpus.load(corpus_dir)
    collector = Collector.__new__(Collector)
    collector.repo, collector.cache = repo, corpus_dir / "cache"
    collector._git_lock, collector._commit_cache = threading.Lock(), {}
    collector.gh = gh
    entries_cache: dict[str, dict | None] = {}
    with_codemodel = with_recorded = with_v2 = with_scan = 0
    index = GraphIndex(graph, graph_source_root, graph_build_dir) if gh is not None else None

    unread: list[str] = []

    docs_cache: dict[str, dict | None] = {}

    def doc_at(sha: str) -> dict | None:
        if sha not in docs_cache:
            docs_cache[sha] = collector.script_inputs_at(sha)
            if docs_cache[sha] is None:
                unread.append(sha)
        return docs_cache[sha]

    def entries(sha: str) -> dict | None:
        doc = doc_at(sha)
        return None if doc is None else doc.get("tests", {})

    build_real = os.path.realpath(graph_build_dir)
    # Executables the graph builds; a test mapped to anything else is unknown.
    built = {os.path.relpath(graph.norm(o), build_real) for outs in graph.fwd.values() for o in outs}
    # Executables whose recorded bytes differed between a head and a group
    # job that checked out the same tree: they embed the commit, so no key
    # over their inputs can stand for them.
    commit_bound: set[str] = set()
    learned_from = 0
    if gh is not None:
        for pair in corpus.pairs:
            head_row = next((h for h in pair.get("heads") or [] if h.get("drift_files") == []), None)
            if head_row is None or (only_runs is not None and str(pair["group_run_id"]) not in only_runs):
                continue
            hb = collector.reuse_record(head_row["run_id"])["binaries"]
            gb = collector.reuse_record(str(pair["group_run_id"]))["binaries"]
            if hb is not None and gb is not None:
                learned_from += 1
                commit_bound |= {e for e in set(hb) & set(gb) if hb[e] != gb[e]}
    commit_bound_set = frozenset(commit_bound)
    all_exes = {e for v in test_map.values() for e in v.get("executables") or []} & built
    done = 0
    for pair in corpus.pairs:
        if only_runs is not None and str(pair["group_run_id"]) not in only_runs:
            continue
        group = corpus.run(pair["group_run_id"])
        head_row = next((h for h in pair.get("heads") or [] if h.get("drift_files") is not None), None)
        if group is None or validate_run(group) or head_row is None:
            continue
        head = corpus.run(head_row["run_id"])
        tests = corpus.tests(group["run_id"])
        if head is None or validate_run(head) or not tests:
            continue
        drift = head_row["drift_files"]
        changed_abs = [os.path.realpath(os.path.join(graph_source_root, f)) for f in drift]
        reached = graph.affected_outputs(changed_abs)
        rebuilt = {os.path.relpath(p, build_real) for p in reached if p.startswith(build_real + os.sep)}
        codemodel = None
        group_record = None
        if gh is not None:
            head_cm, group_record = collector.codemodel_targets(head["run_id"]), collector.reuse_record(group["run_id"])
            if head_cm is not None and group_record["targets"] is not None:
                codemodel = codemodel_rekeyed(head_cm, group_record["targets"])
                with_codemodel += 1
        test_ids = [t["test_id"] for t in tests]
        head_entries, group_entries = entries(head["checkout_sha"]), entries(group["checkout_sha"])
        pair["source_key"] = classify_source_keys(
            drift, test_ids, test_map, head_entries, group_entries, rebuilt, all_exes, codemodel)
        if codemodel is not None and group_record["link"] and group_record["executables"]:
            # The same variants with executables, test mapping and rebuilds
            # taken from the group job's own record instead of the graph's build.
            link = group_record["link"]
            recorded_map = {name: {**(test_map.get(name) or {}), "sources": [],
                                   "executables": [e] if (e := group_record["executables"].get(name)) in link else []}
                            for name in test_ids}
            # Both jobs recorded content-keyed digests and the group declared
            # its commit-bound registrations: the blunt rules retire for this
            # pair. Anything less keeps them.
            head_record = collector.reuse_record(head["run_id"])
            v2 = (not legacy_rules and content_keyed(head_record) and content_keyed(group_record)
                  and group_record.get("declared_commit_bound") is not None)
            bound = frozenset(group_record["declared_commit_bound"]) if v2 else commit_bound_set
            spawns = SpawnIndex(group_record["targets"])
            # A module is rebuilt when its codemodel entry moved, or when the
            # drift reaches its recorded link members; a record from before
            # modules were recorded leaves the Ninja graph's build of it, and
            # (unknown to that graph) any code drift.
            mods_rekeyed, modules = codemodel_rekeyed(head_cm, group_record["targets"], ("MODULE_LIBRARY",))
            code_drift = any(f.endswith(CODE_SUFFIXES) for f in drift)
            linked_rebuilt = recorded_rebuilt(link, drift, index)
            rebuilt_modules = {m for m in modules if m in mods_rekeyed or (
                m in linked_rebuilt if m in link else m in rebuilt if m in built else code_drift)}
            recorded = classify_source_keys(
                drift, test_ids, recorded_map, head_entries, group_entries,
                linked_rebuilt, set(link), codemodel, bound, generated_keyed=v2,
                spawns=spawns, spawnable=frozenset(set(link) - set(group_record["executables"].values())),
                spawn_scan=spawn_scan_of(doc_at(group["checkout_sha"])),
                data_scan=spawn_scan_of(doc_at(group["checkout_sha"]), "data"),
                modules=frozenset(modules), rebuilt_modules=frozenset(rebuilt_modules),
                outputs=recorded_outputs(head_record["binaries"], group_record["binaries"], link))
            test_exes = set(group_record["executables"].values())
            spawned_by_tests = {d for e in test_exes for d in spawns.closure(e)}
            with_v2 += int(v2)
            with_scan += int(recorded["strict-data"]["spawn_scanned"])
            # The executable-level control: every executable whose recorded
            # bytes differ between the head and group jobs must be rebuilt.
            head_bins, group_bins = head_record["binaries"], group_record["binaries"]
            for variant in [v for v in ("strict-data", "cmake-codemodel", "manifest-data", "output-key") if v in recorded]:
                if head_bins is not None and group_bins is not None:
                    both = (set(head_bins) & set(group_bins) & set(link))
                    changed = {e for e in both if head_bins[e] != group_bins[e]}
                    recorded[variant]["binaries_compared"] = len(both)
                    recorded[variant]["binaries_changed"] = len(changed)
                    unreached = sorted(changed - set(recorded[variant]["rebuilt"]))
                    recorded[variant]["unreached_changed_binaries"] = unreached
                    # Where to look, not a second gate: one a group test runs,
                    # one a test spawns, or one neither reaches.
                    recorded[variant]["unreached_kinds"] = {
                        e: "backs_test" if e in test_exes else "spawnable" if e in spawned_by_tests else "neither"
                        for e in unreached}
                    # The inverse: rebuilt although the bytes came out the
                    # same, the policy's over-approximation.
                    recorded[variant]["rebuilt_identical_binaries"] = len((both - changed) & set(recorded[variant]["rebuilt"]))
            pair["source_key"]["strict-data-recorded"] = recorded["strict-data"]
            pair["source_key"]["cmake-codemodel-recorded"] = recorded["cmake-codemodel"]
            pair["source_key"]["manifest-data-recorded"] = recorded["manifest-data"]
            if "output-key" in recorded:
                pair["source_key"]["output-key-recorded"] = recorded["output-key"]
            with_recorded += 1
        pair["source_key_head_run_id"] = head_row["run_id"]
        done += 1
    write_jsonl(corpus_dir / "pairs.jsonl", corpus.pairs)
    # A list that cannot be read makes every script test unknown (it runs);
    # name the commits so a low number is traceable to its cause.
    return {"pairs_annotated": done, "pairs_with_codemodel": with_codemodel, "pairs_with_recorded_graph": with_recorded,
            "commit_bound_executables": sorted(commit_bound), "commit_bound_learned_from_pairs": learned_from,
            "pairs_content_keyed": with_v2, "pairs_spawn_scanned": with_scan,
            "script_lists_unread": unread[:20], "script_lists_unread_count": len(unread)}
