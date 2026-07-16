#!/usr/bin/env python3
"""Contract tests for the silent-revert guard.

Two halves, and the second matters as much as the first:

  * It BLOCKS the real shape — replayed against this repository's own history,
    with real blob shas, not a synthetic fixture.
  * It does NOT cry wolf — a backstop that fires on honest work is worse than the
    bug it prevents, so the clean cases are enumerated deliberately: new bytes,
    unrelated files, partial reverts, a behind-the-base branch, an explicit
    revert, and old history.

Run: python3 tools/scripts/test_silent_revert_guard.py
"""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from datetime import datetime, timedelta, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import silent_revert_guard as G  # noqa: E402

UTC = timezone.utc
NOW = datetime(2026, 7, 15, 12, 0, 0, tzinfo=UTC)
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# The real incident, as it sits in this repository's history: a landing whose
# five files were byte-exactly restored to their pre-landing blobs by the commit
# that followed it. Both are SINGLE-PARENT squashes — which is why the guard
# walks --first-parent and not --merges.
LANDING = "0bb759ecc4d96656f068e4458e5ed059a3eaccec"
REVERTER = "f37d4c09c2327ff14779f9bb92587fb083dfe442"

_failures: list[str] = []
_passes = 0


def check(name: str, cond: bool, detail: str = "") -> None:
    global _passes
    if cond:
        _passes += 1
        print(f"  ok    {name}")
    else:
        _failures.append(f"{name} {detail}".strip())
        print(f"  FAIL  {name} {detail}")


def _have_history() -> bool:
    try:
        subprocess.run(
            ["git", "cat-file", "-e", f"{REVERTER}^{{commit}}"],
            cwd=REPO_ROOT,
            capture_output=True,
            check=True,
        )
        return True
    except Exception:
        return False


def _mk(merge_id: str, changes: dict, hours_ago: float = 1.0) -> G.MergeRecord:
    return G.MergeRecord(
        merge_id=merge_id, merged_at=NOW - timedelta(hours=hours_ago), changes=changes
    )


FILES = [f"core/f{i}.cpp" for i in range(5)]
MERGE = _mk(
    "MERGE-1",
    {f: {"pre": f"pre_{i}", "post": f"post_{i}"} for i, f in enumerate(FILES)},
)


def test_predicate_blocks_wholesale_revert() -> None:
    print("\n[predicate] blocks a wholesale byte-exact revert")
    proposed = {f: f"pre_{i}" for i, f in enumerate(FILES)}
    v = G.GuardBackstop().check(proposed, [MERGE], now=NOW)
    check("blocked", v.blocked is True)
    check("names all 5 paths", sorted(v.reverted_paths) == sorted(FILES))
    check("names the landing", v.merge_id == "MERGE-1")


def test_predicate_clean_cases() -> None:
    print("\n[predicate] does not cry wolf on honest work")
    controls: list[tuple[str, dict]] = []
    for i, f in enumerate(FILES):
        controls.append((f"new bytes in {f}", {f: f"edited_{i}"}))
    for i in range(5):
        controls.append((f"unrelated file {i}", {f"core/unrelated_{i}.cpp": f"u_{i}"}))
    controls.append(("partial revert (1 of 5)", {FILES[0]: "pre_0"}))
    controls.append(
        ("partial revert (2 of 5)", {FILES[0]: "pre_0", FILES[1]: "pre_1"})
    )
    for name, proposed in controls:
        v = G.GuardBackstop().check(proposed, [MERGE], now=NOW)
        check(f"clean: {name}", v.blocked is False, f"reason={v.reason}")
    check("control count is 12", len(controls) == 12, f"got {len(controls)}")


def test_predicate_window() -> None:
    print("\n[predicate] old history is a deliberate operation, not the accident")
    proposed = {f: f"pre_{i}" for i, f in enumerate(FILES)}
    old = _mk(
        "OLD",
        {f: {"pre": f"pre_{i}", "post": f"post_{i}"} for i, f in enumerate(FILES)},
        hours_ago=200.0,
    )
    v = G.GuardBackstop(recent_window_hours=72.0).check(proposed, [old], now=NOW)
    check("outside the window is clean", v.blocked is False)


def test_predicate_added_file_tombstone() -> None:
    print("\n[predicate] a landing that ADDED a file is reverted by deleting it")
    m = _mk("ADD", {"core/new.cpp": {"pre": None, "post": "post_new"}})
    check("delete reverts an add", G.is_byte_exact_revert(m, {"core/new.cpp": None}))
    check(
        "keeping the file is clean",
        not G.is_byte_exact_revert(m, {"core/new.cpp": "post_new"}),
    )


def test_predicate_noop_landing() -> None:
    print("\n[predicate] a no-op landing cannot be reverted")
    m = _mk("NOOP", {"core/a.cpp": {"pre": "same", "post": "same"}})
    check("no-op is clean", not G.is_byte_exact_revert(m, {"core/a.cpp": "same"}))
    check("empty landing is clean", not G.is_byte_exact_revert(_mk("E", {}), {}))


def test_since_arg_is_absolute() -> None:
    """Pin the approxidate trap: the window must never be a float duration.

    git's approxidate accepts `"72.0 hours ago"` and silently means something
    else — measured here, it matched the ENTIRE history, while `"120.0 hours
    ago"` matched nothing. It never errors, so a float window fails silently in
    either direction. An absolute ISO cutoff is the only honest form.
    """
    print("\n[window] the --since cutoff is an absolute timestamp, not approxidate")
    arg = G._since_arg(72.0, now=NOW)
    check("no 'ago' phrasing", "ago" not in arg, arg)
    check("parses back as a real instant",
          datetime.fromisoformat(arg) == NOW - timedelta(hours=72), arg)
    check("a fractional window stays exact",
          datetime.fromisoformat(G._since_arg(1.5, now=NOW)) == NOW - timedelta(hours=1.5))

    if not _have_history():
        print("  SKIP  history not present for the live approxidate comparison")
        return
    # The trap itself, demonstrated live against this repo.
    def count(since: str) -> int:
        return len(subprocess.run(
            ["git", "log", "--first-parent", f"--since={since}", "--format=%H"],
            cwd=REPO_ROOT, capture_output=True, text=True).stdout.split())
    total = count("1970-01-01T00:00:00+00:00")
    float_form = count("72.0 hours ago")
    int_form = count("72 hours ago")
    check("float duration is silently wrong (matches everything)",
          float_form == total and int_form != total,
          f"float={float_form} int={int_form} total={total}")
    # And the absolute form the guard actually uses lands between the two.
    iso_form = count(G._since_arg(72.0))
    check("the absolute cutoff agrees with the integer form",
          abs(iso_form - int_form) <= 1, f"iso={iso_form} int={int_form}")


def test_recent_landings_path_filter() -> None:
    """The path filter must not hide a landing that could actually fire."""
    print("\n[window] the path filter keeps only landings that share a path")
    if not _have_history():
        print("  SKIP  history not present")
        return
    paths = sorted(G.landing_record(REPO_ROOT, LANDING).touched_paths())
    at = G.landing_record(REPO_ROOT, LANDING).merged_at
    # A window anchored just after the landing, scoped to its own paths.
    found = G.recent_landings(REPO_ROOT, REVERTER, since_hours=1.0,
                              paths=paths, now=at + timedelta(minutes=30))
    check("the landing is found when its paths are in scope",
          LANDING in {m.merge_id for m in found}, f"found={[m.merge_id[:8] for m in found]}")
    # Scoped to an unrelated path, it must drop out.
    none = G.recent_landings(REPO_ROOT, REVERTER, since_hours=1.0,
                             paths=["does/not/exist.txt"], now=at + timedelta(minutes=30))
    check("an unrelated scope finds nothing", LANDING not in {m.merge_id for m in none})


def test_real_6082_replay() -> None:
    """The real shape, replayed from this repo's history with real blob shas."""
    print("\n[real] replays the actual incident in this repository's history")
    if not _have_history():
        print("  SKIP  incident commits not present in this checkout (shallow clone)")
        return

    landing = G.landing_record(REPO_ROOT, LANDING)
    check("landing record built from real git", landing is not None)
    assert landing is not None
    check("landing touched 5 real paths", len(landing.changes) == 5, f"got {len(landing.changes)}")

    # The reverter's real tree, at exactly the paths the landing touched.
    proposed = {
        path: G._blob_sha(REPO_ROOT, REVERTER, path) for path in landing.touched_paths()
    }
    check(
        "real blobs restore the real pre-landing bytes",
        all(proposed[p] == landing.changes[p]["pre"] for p in landing.touched_paths()),
    )
    check("real pre != real post (a genuine change was undone)",
          all(landing.changes[p]["pre"] != landing.changes[p]["post"]
              for p in landing.touched_paths()))

    v = G.GuardBackstop().check(
        proposed, [landing], now=landing.merged_at + timedelta(hours=1)
    )
    check("BLOCKS the real incident", v.blocked is True, f"reason={v.reason}")
    check("names all 5 real paths", len(v.reverted_paths) == 5)

    # The adapter must actually SEE this landing. It is a single-parent squash,
    # so a --merges walk returns nothing and the guard would be blind.
    merges_only = subprocess.run(
        ["git", "log", "--merges", "--format=%H", "-200", LANDING],
        cwd=REPO_ROOT, capture_output=True, text=True,
    ).stdout.split()
    check(
        "the landing is a squash a --merges walk cannot see",
        LANDING not in merges_only,
    )
    first_parent = subprocess.run(
        ["git", "log", "--first-parent", "--format=%H", "-1", LANDING],
        cwd=REPO_ROOT, capture_output=True, text=True,
    ).stdout.split()
    check("a --first-parent walk does see it", LANDING in first_parent)


# ===========================================================================
# END-TO-END — a real throwaway repo, driving the real CLI.
# ===========================================================================
def _run(args: list[str], cwd: str) -> subprocess.CompletedProcess:
    env = dict(os.environ)
    # Hooks export GIT_DIR into a hook environment; a set GIT_DIR would override
    # the throwaway repo's discovery and operate on the caller's repo instead.
    for var in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY",
                "GIT_COMMON_DIR", "GIT_PREFIX", "GIT_NAMESPACE", "GIT_QUARANTINE_PATH"):
        env.pop(var, None)
    return subprocess.run(args, cwd=cwd, capture_output=True, text=True, env=env)


def _git_in(repo: str, *args: str) -> None:
    r = _run(["git", *args], repo)
    if r.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)} failed: {r.stderr}")


def _write(repo: str, path: str, text: str) -> None:
    full = os.path.join(repo, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "w") as fh:
        fh.write(text)


def _fixture(repo: str) -> None:
    """A base branch with a landing that changes two files."""
    _git_in(repo, "init", "-q", "-b", "main")
    _git_in(repo, "config", "user.email", "t@example.com")
    _git_in(repo, "config", "user.name", "t")
    _write(repo, "a.txt", "original a\n")
    _write(repo, "b.txt", "original b\n")
    _git_in(repo, "add", "a.txt", "b.txt")
    _git_in(repo, "commit", "-q", "-m", "base")
    # The landing.
    _write(repo, "a.txt", "improved a\n")
    _write(repo, "b.txt", "improved b\n")
    _git_in(repo, "add", "a.txt", "b.txt")
    _git_in(repo, "commit", "-q", "-m", "landing: improve a and b")


def test_e2e_blocks_silent_revert() -> None:
    print("\n[e2e] blocks a branch that silently restores the old bytes")
    with tempfile.TemporaryDirectory() as repo:
        _fixture(repo)
        _git_in(repo, "checkout", "-q", "-b", "feature")
        _write(repo, "a.txt", "original a\n")
        _write(repo, "b.txt", "original b\n")
        _git_in(repo, "add", "a.txt", "b.txt")
        _git_in(repo, "commit", "-q", "-m", "tweak the generator config")
        r = _run(
            [sys.executable, os.path.join(REPO_ROOT, "tools/scripts/silent_revert_guard.py"),
             "--repo", repo, "--base", "main", "--mode=report"],
            repo,
        )
        check("exit 1", r.returncode == 1, f"rc={r.returncode} out={r.stdout}")
        check("says BLOCKED", "BLOCKED" in r.stdout)
        check("names both paths", "a.txt" in r.stdout and "b.txt" in r.stdout)


def test_e2e_hint_mode_never_fails() -> None:
    print("\n[e2e] hint mode reports but never blocks")
    with tempfile.TemporaryDirectory() as repo:
        _fixture(repo)
        _git_in(repo, "checkout", "-q", "-b", "feature")
        _write(repo, "a.txt", "original a\n")
        _write(repo, "b.txt", "original b\n")
        _git_in(repo, "add", "a.txt", "b.txt")
        _git_in(repo, "commit", "-q", "-m", "tweak")
        r = _run(
            [sys.executable, os.path.join(REPO_ROOT, "tools/scripts/silent_revert_guard.py"),
             "--repo", repo, "--base", "main", "--mode=hint"],
            repo,
        )
        check("exit 0 in hint mode", r.returncode == 0, f"rc={r.returncode}")
        check("still reports the block", "BLOCKED" in r.stdout)


def test_e2e_behind_base_is_clean() -> None:
    """The false positive that would make the guard unusable."""
    print("\n[e2e] a branch merely BEHIND the base is clean")
    with tempfile.TemporaryDirectory() as repo:
        _fixture(repo)
        # Cut the branch from BEFORE the landing, and touch nothing it touched.
        _git_in(repo, "checkout", "-q", "-b", "feature", "HEAD~1")
        _write(repo, "c.txt", "new unrelated file\n")
        _git_in(repo, "add", "c.txt")
        _git_in(repo, "commit", "-q", "-m", "add c")
        # At tip-vs-tip this branch's a.txt/b.txt ARE the pre-landing bytes.
        r = _run(
            [sys.executable, os.path.join(REPO_ROOT, "tools/scripts/silent_revert_guard.py"),
             "--repo", repo, "--base", "main", "--mode=report"],
            repo,
        )
        check("exit 0 — stale base is not a revert", r.returncode == 0,
              f"rc={r.returncode} out={r.stdout}")


def test_e2e_explicit_revert_is_clean() -> None:
    print("\n[e2e] an explicit revert is allowed")
    with tempfile.TemporaryDirectory() as repo:
        _fixture(repo)
        _git_in(repo, "checkout", "-q", "-b", "feature")
        _run(["git", "revert", "--no-edit", "HEAD"], repo)
        r = _run(
            [sys.executable, os.path.join(REPO_ROOT, "tools/scripts/silent_revert_guard.py"),
             "--repo", repo, "--base", "main", "--mode=report"],
            repo,
        )
        check("exit 0 — `git revert` states its intent", r.returncode == 0,
              f"rc={r.returncode} out={r.stdout}")


def test_e2e_skip_trailer_is_clean() -> None:
    print("\n[e2e] the skip trailer is honored")
    with tempfile.TemporaryDirectory() as repo:
        _fixture(repo)
        _git_in(repo, "checkout", "-q", "-b", "feature")
        _write(repo, "a.txt", "original a\n")
        _write(repo, "b.txt", "original b\n")
        _git_in(repo, "add", "a.txt", "b.txt")
        _git_in(repo, "commit", "-q", "-m",
                'roll back the landing\n\nSilent-Revert: skip reason="landing was wrong"')
        r = _run(
            [sys.executable, os.path.join(REPO_ROOT, "tools/scripts/silent_revert_guard.py"),
             "--repo", repo, "--base", "main", "--mode=report"],
            repo,
        )
        check("exit 0 with the trailer", r.returncode == 0,
              f"rc={r.returncode} out={r.stdout}")


def test_e2e_normal_work_is_clean() -> None:
    print("\n[e2e] ordinary work on the landed files is clean")
    with tempfile.TemporaryDirectory() as repo:
        _fixture(repo)
        _git_in(repo, "checkout", "-q", "-b", "feature")
        _write(repo, "a.txt", "improved a, refined further\n")
        _git_in(repo, "add", "a.txt")
        _git_in(repo, "commit", "-q", "-m", "refine a")
        r = _run(
            [sys.executable, os.path.join(REPO_ROOT, "tools/scripts/silent_revert_guard.py"),
             "--repo", repo, "--base", "main", "--mode=report"],
            repo,
        )
        check("exit 0", r.returncode == 0, f"rc={r.returncode} out={r.stdout}")


def test_e2e_degrades_on_bad_repo() -> None:
    print("\n[e2e] degrades to a pass when git cannot answer")
    with tempfile.TemporaryDirectory() as empty:
        r = _run(
            [sys.executable, os.path.join(REPO_ROOT, "tools/scripts/silent_revert_guard.py"),
             "--repo", empty, "--base", "main", "--mode=report"],
            empty,
        )
        check("exit 0 on a non-repo", r.returncode == 0, f"rc={r.returncode}")


def main() -> int:
    test_predicate_blocks_wholesale_revert()
    test_predicate_clean_cases()
    test_predicate_window()
    test_predicate_added_file_tombstone()
    test_predicate_noop_landing()
    test_since_arg_is_absolute()
    test_recent_landings_path_filter()
    test_real_6082_replay()
    test_e2e_blocks_silent_revert()
    test_e2e_hint_mode_never_fails()
    test_e2e_behind_base_is_clean()
    test_e2e_explicit_revert_is_clean()
    test_e2e_skip_trailer_is_clean()
    test_e2e_normal_work_is_clean()
    test_e2e_degrades_on_bad_repo()

    print("")
    if _failures:
        print(f"FAILED — {len(_failures)} assertion(s):")
        for f in _failures:
            print(f"  - {f}")
        return 1
    print(f"PASSED — {_passes} assertions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
