#!/usr/bin/env python3
"""Run the cheap, script-owned drift checks on HEAD merged with main.

The author refreshes a branch with ``git merge origin/main``.  This helper lets
the local gates inspect that same hypothetical tree before the merge exists,
so generated script manifests cannot silently validate only the old branch.
Compiled/build checks intentionally do not run here.

``--overall-bound`` caps checkout and checker execution against a shared elapsed
budget. Git metadata operations (including merge-tree/commit-tree and pruning)
and cleanup are outside that enforcement; this is not a whole-command deadline.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path


STALE = "your branch is stale against main; merge origin/main and regenerate"
CONFLICT = 3


def git(root: Path, *args: str, check: bool = False) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["git", "-C", str(root), *args], text=True, encoding="utf-8",
                          capture_output=True, check=check)


# A check that could not run (missing interpreter, a timeout on a loaded host)
# says nothing about the branch, so it must never read as a stale verdict.
UNAVAILABLE = None


# A check that could not run at all exits with this code, distinct from 1
# (stale) and from 3 (the merge conflicts), so a caller can never read "did
# not run" as "passed".
NOT_CHECKED = 4
# The overall time bound (--overall-bound) ran out before every check finished.
# Distinct from NOT_CHECKED: the hook treats it as advisory, never a pass and
# never a refusal. A check that finished with a failure still outranks it.
BOUND_EXCEEDED = 5
BOUND = "bound"
BASE_CHECK_TIMEOUT = 60.0
LOAD_SCALE_SECONDS = 20.0
MAX_CHECK_TIMEOUT = 240.0
ENV_VAR = "PULP_MERGE_TREE_CHECK_TIMEOUT"


def _load_per_core() -> tuple[float, float]:
    """(1-minute load average, load per core); (0.0, 0.0) when unreadable."""
    try:
        load = os.getloadavg()[0]
    except (OSError, AttributeError):
        return 0.0, 0.0
    return load, load / max(1, os.cpu_count() or 1)


def _check_timeout() -> float:
    """Seconds one check may take. PULP_MERGE_TREE_CHECK_TIMEOUT overrides;
    otherwise a 60 s floor, growing by 80 s per extra load-per-core above one
    (180 s at 2.5 per core), capped at 240 s, so a loaded shared host does not
    turn every check into a timeout. A timed-out check is retried once at
    double the limit (see _run)."""
    override = os.environ.get(ENV_VAR)
    if override:
        try:
            return float(override)
        except ValueError:
            pass
    _, per_core = _load_per_core()
    scaled = LOAD_SCALE_SECONDS * max(1.0, 4.0 * per_core - 1.0)
    return min(MAX_CHECK_TIMEOUT, max(BASE_CHECK_TIMEOUT, scaled))


def _run(cmd: list[str], cwd: Path, env: dict[str, str], timeout: float | None = None,
         deadline: float | None = None) -> int | str | None:
    """Run one check. A timeout is retried once with double the limit; a second
    timeout (or a start failure) is UNAVAILABLE, which callers treat as not
    checked, never as a pass. `deadline` (a time.monotonic() value) caps every
    attempt at the time left; running out of it returns BOUND, with no retry."""
    if timeout is None:
        timeout = _check_timeout()
    proc = None
    for limit in (timeout, timeout * 2):
        start = time.monotonic()
        bounded = False
        if deadline is not None:
            remaining = deadline - start
            if remaining <= 0:
                return BOUND
            if limit >= remaining:
                limit, bounded = remaining, True
        try:
            proc = subprocess.run(cmd, cwd=cwd, env=env, text=True, encoding="utf-8",
                                  timeout=limit, capture_output=True)
            break
        except (OSError, subprocess.TimeoutExpired) as exc:
            elapsed = time.monotonic() - start
            load, _ = _load_per_core()
            if bounded and isinstance(exc, subprocess.TimeoutExpired):
                print(f"merge-tree script-state: {cmd[0]} stopped at the overall bound "
                      f"(elapsed {elapsed:.1f}s, 1-min load {load:.1f})", file=sys.stderr)
                return BOUND
            retrying = isinstance(exc, subprocess.TimeoutExpired) and limit == timeout
            print(f"merge-tree script-state: {cmd[0]} unavailable or timed out: {exc} "
                  f"(elapsed {elapsed:.1f}s, limit {limit:.0f}s, 1-min load {load:.1f}); "
                  + (f"retrying once with limit {limit * 2:.0f}s" if retrying else
                     f"raise it with {ENV_VAR}=<seconds>, e.g. {ENV_VAR}=240"),
                  file=sys.stderr)
            if not retrying:
                return UNAVAILABLE
    assert proc is not None
    if proc.stdout:
        print(proc.stdout, end="", file=sys.stderr)
    if proc.stderr:
        print(proc.stderr, end="", file=sys.stderr)
    # A failure remains a failure even when a build-free helper explains that a
    # compiled inventory is not checked locally. Never let that advisory text
    # mask a different script-state failure in the same run.
    return proc.returncode


def add_worktree(root: Path, path: Path, commit: str,
                 timeout: float | None) -> subprocess.CompletedProcess[str] | None:
    """`git worktree add --detach`, with group teardown on timeout/cancellation.
    Reap before the caller removes the worktree. None on timeout."""
    proc = subprocess.Popen(
        ["git", "-C", str(root), "worktree", "add", "--detach", str(path), commit],
        text=True, encoding="utf-8", stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, start_new_session=True)
    try:
        out, err = proc.communicate(timeout=timeout)
    except BaseException as exc:
        # Cancellation must unwind through here before outer worktree cleanup.
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
        proc.communicate()
        if isinstance(exc, subprocess.TimeoutExpired):
            return None
        raise
    return subprocess.CompletedProcess(proc.args, proc.returncode, out, err)


REMOVE_TRIES = 5
REMOVE_PAUSE = 0.4


def remove_worktree(root: Path, path: Path) -> None:
    """Remove our own worktree at `path`, including one whose add was killed.

    Git leaves an interrupted add locked ("initializing"); `prune` skips it and
    a single --force will not remove it. Unlock, remove with --force twice,
    retry the directory removal (a file written as it starts makes it fail
    once), then prune. Only `path`, our own mkdtemp directory, is touched."""
    def git_quiet(*a: str) -> None:
        subprocess.run(["git", "-C", str(root), *a], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    git_quiet("worktree", "unlock", str(path))
    git_quiet("worktree", "remove", "--force", "--force", str(path))
    for _ in range(REMOVE_TRIES):
        shutil.rmtree(path, ignore_errors=True)
        if not path.exists():
            break
        time.sleep(REMOVE_PAUSE)
    git_quiet("worktree", "prune", "--expire", "now")


# Include helper scripts conservatively: the manifest imports shared schema
# and Git-history utilities as well as the agent_capability_* modules.
# The hook's equivalent selector is covered by the same fixture matrix.
CAPABILITY_INPUTS = re.compile(
    r"^(core/[^/]+/include/|tools/scripts/|tools/agent-capabilities/|docs/status/agent-capabilit"
    r"|tools/dsp_vocabulary\.py$|docs/status/forge-catalog\.json$|test/test_agent_capability_compile\.cpp$)")


def _touches_capability_inputs(root: Path, base: str, head: str) -> bool:
    """Same paths as the hook's own capability gate. Unreadable means yes."""
    res = git(root, "diff", "--name-only", f"{base}...{head}")
    if res.returncode:
        return True
    return any(CAPABILITY_INPUTS.match(line) for line in res.stdout.splitlines())


def _bound_exceeded(bound: float, unrun: list[str]) -> int:
    names = f" (not finished: {', '.join(unrun)})" if unrun else ""
    print(f"merge-tree script-state: NOT CHECKED: the {bound:.0f}s overall bound ran out{names}. "
          "ADVISORY, NOT A PASS: CI builds the same merged tree on pull_request. Run "
          "`python3 tools/scripts/merge_tree_script_state.py` on a quieter host to check it now.",
          file=sys.stderr)
    return BOUND_EXCEEDED


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo-root", type=Path, default=Path.cwd())
    ap.add_argument("--base", default="origin/main")
    ap.add_argument("--head", default="HEAD")
    ap.add_argument("--only-if-capability-inputs", action="store_true",
                    help="run the agent-capability manifest check only when base...head touches "
                         "its inputs (the pre-push hook passes this; gates.sh always runs it)")
    ap.add_argument("--overall-bound", type=float, default=0.0, metavar="SECONDS",
                    help="cap checkout and checker execution with this shared elapsed budget "
                         f"and exit {BOUND_EXCEEDED} (not checked, advisory); "
                         "metadata operations and cleanup are not bounded; 0 means no bound")
    args = ap.parse_args(argv)
    deadline = time.monotonic() + args.overall_bound if args.overall_bound > 0 else None
    root = args.repo_root.resolve()
    base = git(root, "rev-parse", "--verify", args.base)
    head = git(root, "rev-parse", "--verify", args.head)
    if base.returncode or head.returncode:
        print("merge-tree script-state: cannot resolve base or head", file=sys.stderr)
        return 2
    base_sha, head_sha = base.stdout.strip(), head.stdout.strip()
    merged = git(root, "merge-tree", "--write-tree", base_sha, head_sha)
    if merged.returncode != 0 or not merged.stdout.strip().splitlines():
        print(f"merge-tree script-state: {STALE} (would be DIRTY)", file=sys.stderr)
        return CONFLICT
    tree = merged.stdout.strip().splitlines()[0]
    commit = subprocess.run(
        ["git", "-C", str(root), "commit-tree", tree, "-p", base_sha, "-p", head_sha],
        input="merge-tree script-state\n", text=True, encoding="utf-8", capture_output=True,
        env={**os.environ, "GIT_AUTHOR_NAME": "pulp-gates", "GIT_AUTHOR_EMAIL": "gates@localhost",
             "GIT_COMMITTER_NAME": "pulp-gates", "GIT_COMMITTER_EMAIL": "gates@localhost"},
        check=False,
    )
    if commit.returncode:
        print("merge-tree script-state: could not materialize merge tree", file=sys.stderr)
        return 2
    synthetic = commit.stdout.strip()
    # Remove only stale temporary worktrees from this checker before adding a
    # new one. The prune is metadata-only and never deletes a live worktree.
    subprocess.run(["git", "-C", str(root), "worktree", "prune", "--expire", "now"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    tmp = Path(tempfile.mkdtemp(prefix="pulp-merge-tree-", dir=root.parent))
    added_worktree = False

    def cleanup() -> None:
        nonlocal added_worktree
        if added_worktree:
            remove_worktree(root, tmp)
            added_worktree = False
        else:
            shutil.rmtree(tmp, ignore_errors=True)

    def on_signal(signum: int, _frame: object) -> None:
        # add_worktree tears down its child before the finally block cleans up.
        raise SystemExit(128 + signum)

    old_handlers = {s: signal.signal(s, on_signal) for s in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
    try:
        # Set before the add: an interrupted add leaves a locked record that
        # cleanup() must still remove.
        added_worktree = True
        added = add_worktree(root, tmp, synthetic,
                             None if deadline is None else max(0.1, deadline - time.monotonic()))
        if added is None:
            return _bound_exceeded(args.overall_bound, [])
        if added.returncode:
            print(added.stderr, file=sys.stderr)
            return 2
        env = os.environ.copy()
        env["PULP_GATES_NO_CONFIGURE"] = "1"
        # Cheapest first. A bound hit drops only what is last in line, so the
        # checks the hook cannot re-run itself (ledger, unregistered tests) go
        # first; families --static and the capability check are re-run by the
        # hook when this one does not finish.
        checks: list[tuple[str, list[str]]] = []
        ledger = tmp / "tools/scripts/vellum_inheritance_ledger_check.py"
        if ledger.exists():
            checks.append(("vellum-inheritance-ledger", [sys.executable, str(ledger),
                            "--root", str(tmp), "--base", base_sha]))
        checks.append(("unregistered-tests", [sys.executable, str(tmp / "tools/scripts/unregistered_tests_check.py"),
                                              "--repo-root", str(tmp)]))
        checks.append(("changed-surface-script-families-drift",
                       [sys.executable, str(tmp / "tools/scripts/changed_surface_script_families.py"),
                        "--repo-root", str(tmp), "--static", "--base", base_sha, "--head", synthetic]))
        # The ctest script-input inventory is compiled/build-owned and has no
        # build-free equivalent. Run it only when a configured build is
        # explicitly supplied; never substitute its schema-only validator.
        # A configured build belongs to the checkout it was configured in: its
        # registered commands carry that checkout's absolute paths, which the
        # lane reads as paths outside the (temporary) checkout and reports as
        # manifest drift. So the check runs here only for a build configured
        # under this root -- which the caller's PULP_GATES_BUILD_DIR never is --
        # and is otherwise skipped, named. gates.sh runs the same lane against
        # the caller's build in its own pass.
        configured = os.environ.get("PULP_GATES_BUILD_DIR", "")
        configured_path = Path(configured).expanduser().resolve() if configured else None
        if (configured_path is not None and (configured_path / "CMakeCache.txt").exists()
                and configured_path.is_relative_to(tmp.resolve())):
            checks.append(("script-test-inputs-drift", [sys.executable,
                            str(tmp / "tools/scripts/gates_script_inputs.py"),
                            "--base", base_sha, "--repo-root", str(tmp),
                            "--build-dir", str(configured_path)]))
        else:
            print("merge-tree script-state: script-test-inputs-drift (no build-free variant; "
                  "a build configured outside the merge-tree root cannot be checked here; skipped)",
                  file=sys.stderr)
        # The capability check is ~10 s and can only change when the range
        # touches its inputs: the same trigger the hook uses for its own run.
        if args.only_if_capability_inputs and not _touches_capability_inputs(root, base_sha, head_sha):
            print("merge-tree script-state: agent-capability-manifest (the range touches none of its "
                  "inputs; skipped)", file=sys.stderr)
        else:
            checks.append(("agent-capability-manifest",
                           [sys.executable, str(tmp / "tools/scripts/agent_capability_manifest.py"), "--check"]))
        failed: list[str] = []
        unavailable: list[str] = []
        unrun: list[str] = []
        for name, cmd in checks:
            if not Path(cmd[1]).exists():
                continue
            print(f"merge-tree script-state: {name}", file=sys.stderr)
            if unrun:  # the bound already ran out; do not start another check
                unrun.append(name)
                continue
            rc = _run(cmd, tmp, env, deadline=deadline)
            if rc == BOUND:
                unrun.append(name)
            elif rc is UNAVAILABLE:
                unavailable.append(name)
            elif rc:
                failed.append(name)
        if failed:
            print(f"merge-tree script-state: {STALE}", file=sys.stderr)
            return 1
        if unavailable:
            # Could not establish: not a pass. gates.sh and pre-push fail on it.
            print("merge-tree script-state: NOT CHECKED (could not run: "
                  f"{', '.join(unavailable)}); this is not a stale-branch verdict, and "
                  "not a pass. Re-run on a less loaded host or raise "
                  "PULP_MERGE_TREE_CHECK_TIMEOUT.", file=sys.stderr)
            return NOT_CHECKED
        if unrun:
            return _bound_exceeded(args.overall_bound, unrun)
        return 0
    finally:
        cleanup()
        for s, handler in old_handlers.items():
            signal.signal(s, handler)


if __name__ == "__main__":
    raise SystemExit(main())
