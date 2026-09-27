#!/usr/bin/env python3
"""Fixture-repo tests for the pre-push main-refresh telemetry and policy.

Each case builds a real Git repository with an `origin/main` that advanced
after the feature branch forked, then feeds Git's pushed-ref record to
`refresh_push_check.py` (and, for the wiring cases, to `.githooks/pre-push`
itself) with a fake `gh` standing in for GitHub. The fake is the only network:
it answers from a JSON fixture, fails, or hangs, so fail-open and the time
budget are exercised deterministically.

Every case asserts the telemetry line it expects, not just an exit code, so a
check that silently stopped classifying (and so logged nothing and warned
nothing) cannot pass as "allowed".
"""

from __future__ import annotations

import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/scripts/refresh_push_check.py"
HOOK = ROOT / ".githooks/pre-push"
HOOK_LIB = ROOT / ".githooks/lib"

FAKE_GH = r"""#!/usr/bin/env python3
import json, os, sys, time
mode = os.environ.get("FAKE_GH_MODE", "ok")
query = " ".join(sys.argv[1:])
if mode == "fail":
    print("network unreachable", file=sys.stderr); sys.exit(1)
if mode == "hang":
    time.sleep(30)
fixture = json.loads(open(os.environ["FAKE_GH_FIXTURE"]).read())
key = "required" if "isRequired" in query else "pr"
print(json.dumps(fixture[key]))
"""

ZERO = "0" * 40


def pr_fixture(*, mergeable="MERGEABLE", gate="IN_PROGRESS", failing=None, required=True, ejected_head=None, head="HEAD"):
    contexts = [
        {
            "__typename": "CheckRun",
            "name": "macos",
            "status": gate,
            "conclusion": None if gate != "COMPLETED" else "SUCCESS",
            "checkSuite": {"workflowRun": {"workflow": {"name": "Build and Test"}}},
        }
    ]
    req_contexts = []
    if failing:
        contexts.append({"__typename": "CheckRun", "name": failing, "status": "COMPLETED", "conclusion": "FAILURE"})
        req_contexts.append({"__typename": "CheckRun", "name": failing, "conclusion": "FAILURE", "isRequired": required})
    timeline = [{"beforeCommit": {"oid": ejected_head}}] if ejected_head else []
    pr = {
        "number": 4242,
        "mergeable": mergeable,
        "headRefOid": head,
        "timelineItems": {"nodes": timeline},
        "commits": {"nodes": [{"commit": {"statusCheckRollup": {"contexts": {"nodes": contexts}}}}]},
    }
    return {
        "pr": {"data": {"repository": {"pullRequests": {"nodes": [pr]}}}},
        "required": {
            "data": {
                "repository": {
                    "pullRequest": {
                        "commits": {"nodes": [{"commit": {"statusCheckRollup": {"contexts": {"nodes": req_contexts}}}}]}
                    }
                }
            }
        },
    }


class Fixture:
    def __init__(self, tmp: Path):
        self.tmp = tmp
        self.origin = tmp / "origin.git"
        self.repo = tmp / "work"
        self.log = tmp / "state" / "refresh-pushes.jsonl"
        self.gh = tmp / "fake-gh"
        self.gh.write_text(FAKE_GH)
        self.gh.chmod(self.gh.stat().st_mode | stat.S_IEXEC)
        self.fixture_file = tmp / "gh-fixture.json"
        self.set_pr(pr_fixture())

    def git(self, *args: str, cwd: Path | None = None, check: bool = True) -> str:
        env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t",
                   GIT_COMMITTER_EMAIL="t@t", GIT_CONFIG_NOSYSTEM="1", HOME=str(self.tmp))
        for k in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE"):
            env.pop(k, None)
        res = subprocess.run(["git", *args], cwd=cwd or self.repo, capture_output=True, text=True, env=env)
        if check and res.returncode != 0:
            raise AssertionError(f"git {' '.join(args)} failed: {res.stderr}")
        return res.stdout.strip()

    def write(self, rel: str, text: str) -> None:
        p = self.repo / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)

    def commit(self, rel: str, text: str, msg: str) -> str:
        self.write(rel, text)
        self.git("add", rel)
        self.git("commit", "-q", "-m", msg)
        return self.git("rev-parse", "HEAD")

    def set_pr(self, fixture: dict) -> None:
        self.fixture_file.write_text(json.dumps(fixture))

    def setup(self, *, conflict: bool = False) -> str:
        """origin/main advanced after `feat` forked; returns feat's remote head."""
        self.git("init", "-q", "--bare", str(self.origin), cwd=self.tmp)
        self.git("init", "-q", "-b", "main", str(self.repo), cwd=self.tmp)
        self.write(".shipyard/config.toml", '[merge]\nrefresh_branch = "only-if-conflicting"\n')
        self.git("add", ".shipyard/config.toml")
        self.commit("shared.txt", "base\n", "base")
        self.git("remote", "add", "origin", str(self.origin))
        self.git("push", "-q", "origin", "main")
        self.git("checkout", "-q", "-b", "feat")
        feat = self.commit("shared.txt" if conflict else "feat.txt", "feature\n", "feature work")
        self.git("push", "-q", "origin", "feat")
        self.git("checkout", "-q", "main")
        self.commit("shared.txt" if conflict else "main.txt", "main moved\n", "main moves")
        self.git("push", "-q", "origin", "main")
        self.git("checkout", "-q", "feat")
        self.git("fetch", "-q", "origin")
        return feat

    def records(self, remote_sha: str) -> str:
        head = self.git("rev-parse", "HEAD")
        return f"refs/heads/feat {head} refs/heads/feat {remote_sha}\n"

    def env(self, **extra: str) -> dict:
        env = {k: v for k, v in os.environ.items() if not k.startswith(("PULP_", "SHIPYARD_", "GIT_"))}
        env.update(
            PULP_REFRESH_PUSH_LOG=str(self.log),
            PULP_REFRESH_PUSH_GH=str(self.gh),
            FAKE_GH_FIXTURE=str(self.fixture_file),
            PYTHONDONTWRITEBYTECODE="1",
        )
        env.update(extra)
        return env

    def run(self, remote_sha: str, **extra: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--root", str(self.repo),
             "--remote-url", "git@github.com:Generous-Corp/pulp.git"],
            input=self.records(remote_sha), capture_output=True, text=True, env=self.env(**extra), timeout=60,
        )

    def run_hook(self, remote_sha: str, **extra: str) -> subprocess.CompletedProcess:
        # The hook resolves helpers relative to the repo it runs in. Mirror
        # only what this path needs; the fixture has no versioning config, so
        # the hook stops after its pushed-ref guard, as on an old checkout.
        shutil.copytree(HOOK_LIB, self.repo / ".githooks/lib", dirs_exist_ok=True)
        shutil.copy2(HOOK, self.repo / ".githooks/pre-push")
        (self.repo / "tools/scripts").mkdir(parents=True, exist_ok=True)
        shutil.copy2(SCRIPT, self.repo / "tools/scripts/refresh_push_check.py")
        env = self.env(**extra)
        return subprocess.run(
            ["bash", str(self.repo / ".githooks/pre-push"), "origin", "git@github.com:Generous-Corp/pulp.git"],
            cwd=self.repo, input=self.records(remote_sha), capture_output=True, text=True, env=env, timeout=60,
        )

    def entries(self) -> list[dict]:
        if not self.log.exists():
            return []
        return [json.loads(l) for l in self.log.read_text().splitlines() if l.strip()]


def pure_refresh(fx: Fixture) -> str:
    feat = fx.setup()
    fx.git("merge", "-q", "--no-edit", "origin/main")
    fx.set_pr(pr_fixture(head=feat))
    return feat


# ── cases ──────────────────────────────────────────────────────────────────


def case_pure_refresh_logged_and_warned(fx: Fixture) -> None:
    feat = pure_refresh(fx)
    res = fx.run(feat)
    assert res.returncode == 0, res.stderr
    assert "pure main-refresh of PR #4242" in res.stderr, res.stderr
    (e,) = fx.entries()
    assert e["pure_refresh"] is True and e["decision"] == "warned", e
    assert e["pr"] == 4242 and e["gate_in_flight"] is True and e["policy"] == "only-if-conflicting", e
    assert e["head_before"] == feat and e["main_merges"] == 1 and e["lookup"] == "ok", e


def case_conflict_resolution_logged_not_warned(fx: Fixture) -> None:
    feat = fx.setup(conflict=True)
    r = subprocess.run(["git", "merge", "--no-edit", "origin/main"], cwd=fx.repo, capture_output=True, text=True)
    assert r.returncode != 0, "fixture must conflict"
    fx.write("shared.txt", "resolved\n")
    fx.git("add", "shared.txt")
    fx.git("commit", "-q", "-m", "resolve conflict")
    fx.set_pr(pr_fixture(head=feat))
    res = fx.run(feat)
    assert res.returncode == 0, res.stderr
    assert "pure main-refresh" not in res.stderr, res.stderr
    (e,) = fx.entries()
    assert e["pure_refresh"] is False and e["decision"] == "allowed:not-pure", e
    assert e["pr"] == 4242, e


def case_evil_merge_not_pure(fx: Fixture) -> None:
    """A clean merge that ALSO smuggles an edit into the merge commit."""
    feat = fx.setup()
    fx.git("merge", "-q", "--no-commit", "origin/main")
    fx.write("feat.txt", "edited during the merge\n")
    fx.git("add", "feat.txt")
    fx.git("commit", "-q", "--no-edit")
    res = fx.run(feat)
    (e,) = fx.entries()
    assert e["pure_refresh"] is False and "pure main-refresh" not in res.stderr, (e, res.stderr)


def case_refresh_plus_own_commit_not_pure(fx: Fixture) -> None:
    feat = pure_refresh(fx)
    fx.commit("feat.txt", "more feature\n", "more work")
    res = fx.run(feat)
    (e,) = fx.entries()
    assert e["pure_refresh"] is False and e["other_commits"] == 1, e
    assert "pure main-refresh" not in res.stderr


def case_normal_commit_skipped(fx: Fixture) -> None:
    feat = fx.setup()
    fx.commit("feat.txt", "more feature\n", "more work")
    res = fx.run(feat, FAKE_GH_MODE="fail")
    assert res.returncode == 0 and res.stderr == "", res.stderr
    assert fx.entries() == [], fx.entries()


def case_refuse_mode_blocks(fx: Fixture) -> None:
    feat = pure_refresh(fx)
    res = fx.run(feat, PULP_REFRESH_PUSH_POLICY="refuse")
    assert res.returncode == 1, (res.returncode, res.stderr)
    assert "PULP_ALLOW_REFRESH_PUSH=1" in res.stderr
    (e,) = fx.entries()
    assert e["decision"] == "refused" and e["mode"] == "refuse", e


def case_override_allows(fx: Fixture) -> None:
    feat = pure_refresh(fx)
    res = fx.run(feat, PULP_REFRESH_PUSH_POLICY="refuse", PULP_ALLOW_REFRESH_PUSH="1")
    assert res.returncode == 0, res.stderr
    (e,) = fx.entries()
    assert e["decision"] == "overridden", e


def case_off_mode_logs_silently(fx: Fixture) -> None:
    feat = pure_refresh(fx)
    res = fx.run(feat, PULP_REFRESH_PUSH_POLICY="off")
    assert res.returncode == 0 and "pure main-refresh" not in res.stderr, res.stderr
    (e,) = fx.entries()
    assert e["decision"] == "off" and e["pure_refresh"] is True, e


def case_justified_refreshes_allowed(fx: Fixture) -> None:
    feat = pure_refresh(fx)
    for fixture, want in (
        (pr_fixture(head=feat, failing="macos-required", required=True), "allowed:failing-required-check"),
        (pr_fixture(head=feat, ejected_head=feat), "allowed:ejected-at-head"),
        (pr_fixture(head=feat, mergeable="CONFLICTING"), "allowed:conflicting"),
    ):
        fx.set_pr(fixture)
        res = fx.run(feat, PULP_REFRESH_PUSH_POLICY="refuse")
        assert res.returncode == 0, (want, res.stderr)
        assert fx.entries()[-1]["decision"] == want, (want, fx.entries()[-1])
    # A failing check that is NOT required does not justify the refresh.
    fx.set_pr(pr_fixture(head=feat, failing="advisory-lint", required=False))
    res = fx.run(feat, PULP_REFRESH_PUSH_POLICY="refuse")
    assert res.returncode == 1 and fx.entries()[-1]["decision"] == "refused", fx.entries()[-1]


def case_network_failure_fails_open(fx: Fixture) -> None:
    feat = pure_refresh(fx)
    res = fx.run(feat, PULP_REFRESH_PUSH_POLICY="refuse", FAKE_GH_MODE="fail")
    assert res.returncode == 0, res.stderr
    (e,) = fx.entries()
    assert e["decision"] == "allowed:lookup-failed" and e["lookup"].startswith("failed:"), e


def case_network_hang_is_bounded(fx: Fixture) -> None:
    feat = pure_refresh(fx)
    start = time.monotonic()
    res = fx.run(feat, PULP_REFRESH_PUSH_POLICY="refuse", FAKE_GH_MODE="hang", PULP_REFRESH_PUSH_TIMEOUT="1")
    elapsed = time.monotonic() - start
    assert res.returncode == 0, res.stderr
    assert elapsed < 6, f"lookup budget not enforced: {elapsed:.1f}s"
    (e,) = fx.entries()
    assert e["lookup"] == "failed:timeout" and e["decision"] == "allowed:lookup-failed", e


def case_hook_wiring(fx: Fixture) -> None:
    """The real hook runs the check, honours refuse, and the skip knob demotes it."""
    feat = pure_refresh(fx)
    res = fx.run_hook(feat)
    assert res.returncode == 0, res.stderr
    assert "pure main-refresh of PR #4242" in res.stderr, res.stderr
    res = fx.run_hook(feat, PULP_REFRESH_PUSH_POLICY="refuse")
    assert res.returncode == 1, (res.returncode, res.stderr)
    res = fx.run_hook(feat, PULP_REFRESH_PUSH_POLICY="refuse", PULP_SKIP_PREPUSH="1")
    assert res.returncode == 0 and "demoted to advisory" in res.stderr, res.stderr
    decisions = [e["decision"] for e in fx.entries()]
    assert decisions == ["warned", "refused", "refused"], decisions


CASES = [v for k, v in sorted(globals().items()) if k.startswith("case_")]


def main() -> int:
    failures = []
    for case in CASES:
        with tempfile.TemporaryDirectory(prefix="refresh-push-") as d:
            try:
                case(Fixture(Path(d)))
                print(f"PASS {case.__name__}")
            except AssertionError as exc:
                failures.append(case.__name__)
                print(f"FAIL {case.__name__}: {exc}")
    # Control: a suite that collected nothing would pass vacuously.
    if len(CASES) < 12:
        print(f"FAIL collected only {len(CASES)} cases")
        return 1
    print(f"{len(CASES) - len(failures)}/{len(CASES)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
