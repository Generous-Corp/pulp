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
from unittest import mock

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
# The reverter's ORIGINATING BRANCH, still on the remote. Its merge-base with the
# landing IS the landing, so the reverting bytes were already in the branch before
# it was pushed — which is what makes this guard's placement (pre-push) the right
# one, and is provable rather than assumed.
REVERTER_BRANCH = "48007987ee2f5a1ece95adb3173e3f55033c7149"

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


def _have_reverter_branch() -> bool:
    try:
        subprocess.run(
            ["git", "cat-file", "-e", f"{REVERTER_BRANCH}^{{commit}}"],
            cwd=REPO_ROOT, capture_output=True, check=True,
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


def test_predicate_revert_disguised_by_real_work() -> None:
    """The realistic form: the revert rides along with genuine changes.

    This is what makes the shape hard to see in review — the diff has real work
    in it. The extra paths must not excuse the revert, and the cost short-circuit
    (which skips a landing whose paths are not all in the push) must not drop it.
    """
    print("\n[predicate] a revert bundled with unrelated work still blocks")
    proposed = {f: f"pre_{i}" for i, f in enumerate(FILES)}
    proposed["core/genuinely_new.cpp"] = "brand_new"
    proposed["core/other.cpp"] = "also_edited"
    v = G.GuardBackstop().check(proposed, [MERGE], now=NOW)
    check("blocked despite the extra work", v.blocked is True, f"reason={v.reason}")
    check("the landing's paths are a subset of the push (so it is examined)",
          MERGE.touched_paths().issubset(set(proposed)))


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

    def as_instant(arg: str) -> datetime | None:
        """Parse, or report unparseable as a failure — never raise.

        A non-absolute cutoff is exactly what this test exists to catch, so it
        must land as a FAIL. Letting fromisoformat raise aborts the whole suite
        at this point and hides every later test, which is precisely when the
        remaining coverage matters most.
        """
        try:
            return datetime.fromisoformat(arg)
        except ValueError:
            return None

    arg = G._since_arg(72.0, now=NOW)
    check("no 'ago' phrasing", "ago" not in arg, arg)
    check("parses back as a real instant",
          as_instant(arg) == NOW - timedelta(hours=72), arg)
    fractional = G._since_arg(1.5, now=NOW)
    check("a fractional window stays exact",
          as_instant(fractional) == NOW - timedelta(hours=1.5), fractional)

    if not _have_history():
        print("  SKIP  history not present for the live approxidate comparison")
        return
    # A shallow checkout (CI's default fetch-depth=1 on the build job) grafts
    # the first-parent walk at the tip, so `git log --first-parent` sees a
    # single commit regardless of --since. The live demonstration below needs
    # real depth to distinguish the float trap from the integer form; the
    # arithmetic assertions above already pin the contract. Skip only the live
    # comparison when history is truncated — same spirit as the guard above.
    shallow = subprocess.run(
        ["git", "rev-parse", "--is-shallow-repository"],
        cwd=REPO_ROOT, capture_output=True, text=True).stdout.strip()
    if shallow == "true":
        print("  SKIP  shallow checkout — live approxidate comparison needs full history")
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


def test_recent_landings_honors_the_window() -> None:
    """The git walk itself must honor the cutoff — in both directions.

    The rest of the suite pins `_since_arg`'s output as a string. That is
    white-box: it cannot fail if the walk stops passing the cutoff to git, and
    it cannot fail in the direction that actually takes the guard offline. This
    drives the real walk over the real landing and asserts the boundary moves
    with `now`, which is the only thing that distinguishes a working window from
    a filter that silently matches everything (or nothing).
    """
    print("\n[window] the git walk excludes a landing older than the cutoff")
    if not _have_history():
        print("  SKIP  history not present")
        return
    record = G.landing_record(REPO_ROOT, LANDING)
    paths = sorted(record.touched_paths())
    at = record.merged_at
    inside = G.recent_landings(REPO_ROOT, REVERTER, since_hours=72.0,
                               paths=paths, now=at + timedelta(hours=1))
    check("inside the window: the landing is found",
          LANDING in {m.merge_id for m in inside})
    # Same commit, same paths, same call — only `now` moves past the window.
    outside = G.recent_landings(REPO_ROOT, REVERTER, since_hours=72.0,
                                paths=paths, now=at + timedelta(hours=73))
    check("outside the window: the landing is gone",
          LANDING not in {m.merge_id for m in outside},
          f"found={[m.merge_id[:8] for m in outside]}")


def test_real_incident_replay() -> None:
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


def test_real_branch_would_have_been_blocked_at_push() -> None:
    """The claim that matters: this guard, on that branch, at that moment.

    Replays the ORIGINATING BRANCH exactly as it stood against main's tip when it
    was pushed. Anything less leaves the useful question unanswered — a guard that
    only recognizes the shape after it has landed on main is a post-mortem, not a
    gate. The branch's merge-base with the landing is the landing itself, so the
    reverting bytes were in the branch pre-push and a pre-push check can see them.

    Skips if the branch has since been deleted from the remote; the assertion is
    about a real ref, so a fabricated stand-in would prove nothing.
    """
    print("\n[real] the originating branch would have been blocked at push time")
    if not _have_reverter_branch():
        print("  SKIP  the originating branch is no longer fetchable")
        return
    when = subprocess.run(
        ["git", "log", "-1", "--format=%cI", REVERTER_BRANCH],
        cwd=REPO_ROOT, capture_output=True, text=True,
    ).stdout.strip()
    at = datetime.fromisoformat(when)

    # The branch's base was main's tip at the time — which was the landing.
    mb = subprocess.run(
        ["git", "merge-base", REVERTER_BRANCH, LANDING],
        cwd=REPO_ROOT, capture_output=True, text=True,
    ).stdout.strip()
    check("the branch was up to date with the landing", mb == LANDING, mb)

    v = G.check_push(REPO_ROOT, base_ref=LANDING, head_ref=REVERTER_BRANCH,
                     since_hours=72.0, now=at)
    check("BLOCKED at push time", v.blocked is True, f"reason={v.reason}")
    check("names the landing it would have erased", v.merge_id == LANDING)
    check("names all 5 paths", len(v.reverted_paths) == 5, f"got {v.reverted_paths}")


def test_real_incident_end_to_end() -> None:
    """The whole guard, via its public entry point, on the real incident.

    Anchored to the moment the revert actually happened. Wall-clock time would
    put a historical landing outside the recency window and pass for the wrong
    reason — an outcome indistinguishable from the guard working.
    """
    print("\n[real] check_push blocks the real incident, at the real moment")
    if not _have_history():
        print("  SKIP  incident commits not present in this checkout")
        return
    when = subprocess.run(
        ["git", "log", "-1", "--format=%cI", REVERTER],
        cwd=REPO_ROOT, capture_output=True, text=True,
    ).stdout.strip()
    at = datetime.fromisoformat(when)

    v = G.check_push(REPO_ROOT, base_ref=LANDING, head_ref=REVERTER,
                     since_hours=72.0, now=at)
    check("BLOCKED end-to-end", v.blocked is True, f"reason={v.reason}")
    check("names the real landing", v.merge_id == LANDING, v.merge_id)
    check("names all 5 real paths", len(v.reverted_paths) == 5,
          f"got {v.reverted_paths}")

    # Same commits, but with the window measured from now: the landing is long
    # past, so this must pass — and NOT because the guard is broken.
    old = G.check_push(REPO_ROOT, base_ref=LANDING, head_ref=REVERTER,
                       since_hours=72.0)
    check("an old landing is outside the window and passes",
          old.blocked is False, f"reason={old.reason}")


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
    # A throwaway fixture repo must never inherit the developer's signing config:
    # with commit.gpgsign on and the key unusable, every commit here dies and the
    # whole suite aborts on a traceback rather than reporting a result.
    _git_in(repo, "config", "commit.gpgsign", "false")
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


def test_e2e_hint_mode_downgrades_content_block() -> None:
    print("\n[e2e] hint mode downgrades a content block after history resolves")
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


def test_e2e_fails_closed_on_bad_repo() -> None:
    print("\n[e2e] fails closed when git cannot answer")
    with tempfile.TemporaryDirectory() as empty:
        r = _run(
            [sys.executable, os.path.join(REPO_ROOT, "tools/scripts/silent_revert_guard.py"),
             "--repo", empty, "--base", "main", "--mode=report"],
            empty,
        )
        check("exit 2 on a non-repo", r.returncode == 2, f"rc={r.returncode}")
        check("never reports empty success", "HISTORY UNAVAILABLE" in r.stdout,
              f"out={r.stdout}")
        check("prints immutable receipt", '"status":"history_unavailable"' in r.stdout,
              f"out={r.stdout}")
        hint = _run(
            [sys.executable, os.path.join(REPO_ROOT, "tools/scripts/silent_revert_guard.py"),
             "--repo", empty, "--base", "main", "--mode=hint"],
            empty,
        )
        check("hint preserves its always-zero contract", hint.returncode == 0,
              f"rc={hint.returncode}")
        check("hint still reports unavailable history loudly",
              "HISTORY UNAVAILABLE" in hint.stdout and
              '"status":"history_unavailable"' in hint.stdout, hint.stdout)
        direct = G.check_push(empty, "main")
        check("direct API callers fail closed", direct.blocked,
              f"verdict={direct}")
        check("direct API retains unavailable classification",
              direct.history_status == "history_unavailable",
              f"verdict={direct}")


def test_e2e_intent_cannot_bypass_unavailable_base() -> None:
    print("\n[e2e] revert intent cannot bypass unavailable base history")
    controls = (
        ("explicit revert subject", 'Revert "landing"', "report", 2),
        (
            "skip trailer",
            'rollback\n\nSilent-Revert: skip reason="deliberate"',
            "hint",
            0,
        ),
    )
    for name, message, mode, expected_rc in controls:
        with tempfile.TemporaryDirectory() as repo:
            _fixture(repo)
            _git_in(repo, "checkout", "-q", "-b", "feature")
            _write(repo, "c.txt", name + "\n")
            _git_in(repo, "add", "c.txt")
            _git_in(repo, "commit", "-q", "-m", message)
            r = _run(
                [
                    sys.executable,
                    os.path.join(
                        REPO_ROOT, "tools/scripts/silent_revert_guard.py"
                    ),
                    "--repo", repo, "--base", "missing-base", f"--mode={mode}",
                ],
                repo,
            )
            check(f"{name}: unavailable history preserves {mode} exit contract",
                  r.returncode == expected_rc,
                  f"rc={r.returncode} out={r.stdout}")
            check(f"{name}: unavailable history remains loud",
                  "HISTORY UNAVAILABLE" in r.stdout, r.stdout)
            check(f"{name}: receipt resolves HEAD",
                  '"resolved_head":null' not in r.stdout, r.stdout)


def test_post_resolution_failure_receipt_is_consistent() -> None:
    print("\n[e2e] post-resolution Git failure updates immutable receipt")
    with tempfile.TemporaryDirectory() as repo:
        _fixture(repo)
        failure = "post-resolution failure " * 80
        controls = []
        with mock.patch.object(
            G, "_has_revert_intent_checked", return_value=(False, failure)
        ):
            controls.append(("intent", G.check_push(repo, "main")))
        with mock.patch.object(
            G, "_proposed_from_comparison", return_value=({}, failure)
        ):
            controls.append(("proposed", G.check_push(repo, "main")))
        with mock.patch.object(
            G, "_proposed_from_comparison", return_value=({"a.txt": "x"}, "")
        ), mock.patch.object(
            G, "_recent_landings_checked",
            return_value=([], failure, "command_failed"),
        ):
            controls.append(("recent-landings", G.check_push(repo, "main")))
        for name, verdict in controls:
            check(f"{name}: verdict reports command_failed",
                  verdict.history_status == "command_failed")
            check(f"{name}: receipt reports command_failed",
                  verdict.provenance is not None and
                  verdict.provenance.status == "command_failed")
            check(f"{name}: receipt stderr is bounded and normalized",
                  verdict.provenance is not None and
                  len(verdict.provenance.stderr) <= 512 and
                  "  " not in verdict.provenance.stderr)
            check(f"{name}: receipt retains resolved HEAD",
                  verdict.provenance is not None and
                  verdict.provenance.resolved_head is not None)


def test_e2e_shallow_history_fails_only_for_nonempty_diff() -> None:
    print("\n[e2e] shallow history cannot clear a nonempty proposed diff")
    with tempfile.TemporaryDirectory() as directory:
        origin = os.path.join(directory, "origin")
        shallow = os.path.join(directory, "shallow")
        os.mkdir(origin)
        _fixture(origin)
        clone = subprocess.run(
            ["git", "clone", "-q", "--depth=1", f"file://{origin}", shallow],
            capture_output=True, text=True,
        )
        check("shallow fixture cloned", clone.returncode == 0, clone.stderr)
        clean = _run(
            [
                sys.executable,
                os.path.join(REPO_ROOT, "tools/scripts/silent_revert_guard.py"),
                "--repo", shallow, "--base", "main", "--mode=report",
            ],
            shallow,
        )
        check("empty shallow diff remains a valid success", clean.returncode == 0,
              f"rc={clean.returncode} out={clean.stdout}")
        _git_in(shallow, "config", "user.email", "t@example.com")
        _git_in(shallow, "config", "user.name", "t")
        _git_in(shallow, "config", "commit.gpgsign", "false")
        _git_in(shallow, "checkout", "-q", "-b", "feature")
        _write(shallow, "a.txt", "feature bytes\n")
        _git_in(shallow, "add", "a.txt")
        _git_in(shallow, "commit", "-q", "-m", "feature")
        changed = _run(
            [
                sys.executable,
                os.path.join(REPO_ROOT, "tools/scripts/silent_revert_guard.py"),
                "--repo", shallow, "--base", "main", "--mode=report",
            ],
            shallow,
        )
        check("nonempty shallow diff exits 2", changed.returncode == 2,
              f"rc={changed.returncode} out={changed.stdout}")
        check("shallow limitation is explicit",
              "shallow history has missing hidden objects" in changed.stdout,
              changed.stdout)
        check("receipt classifies history unavailable",
              '"status":"history_unavailable"' in changed.stdout,
              changed.stdout)

        old_origin = os.path.join(directory, "old-origin")
        old_shallow = os.path.join(directory, "old-shallow")
        os.mkdir(old_origin)
        _git_in(old_origin, "init", "-q", "-b", "main")
        _git_in(old_origin, "config", "user.email", "t@example.com")
        _git_in(old_origin, "config", "user.name", "t")
        _git_in(old_origin, "config", "commit.gpgsign", "false")

        def dated_commit(repo: str, message: str, when: str) -> None:
            env = dict(os.environ)
            env["GIT_AUTHOR_DATE"] = when
            env["GIT_COMMITTER_DATE"] = when
            completed = subprocess.run(
                ["git", "commit", "-q", "-m", message], cwd=repo,
                capture_output=True, text=True, env=env,
            )
            if completed.returncode != 0:
                raise RuntimeError(completed.stderr)

        _write(old_origin, "seed.txt", "grandparent\n")
        _git_in(old_origin, "add", ".")
        dated_commit(
            old_origin, "newer-dated grandparent", datetime.now(UTC).isoformat()
        )
        grandparent_sha = _run(
            ["git", "rev-parse", "HEAD"], old_origin
        ).stdout.strip()
        _write(old_origin, "a.txt", "original a\n")
        _write(old_origin, "b.txt", "original b\n")
        _git_in(old_origin, "add", ".")
        dated_commit(old_origin, "old boundary", "2020-01-01T00:00:00Z")
        _write(old_origin, "a.txt", "improved a\n")
        _write(old_origin, "b.txt", "improved b\n")
        _git_in(old_origin, "add", ".")
        dated_commit(
            old_origin, "recent landing",
            datetime.now(UTC).isoformat(),
        )
        recent_landing_sha = _run(
            ["git", "rev-parse", "HEAD"], old_origin
        ).stdout.strip()
        subprocess.run(
            ["git", "clone", "-q", "--depth=2", f"file://{old_origin}", old_shallow],
            check=True,
        )
        _git_in(old_shallow, "config", "user.email", "t@example.com")
        _git_in(old_shallow, "config", "user.name", "t")
        _git_in(old_shallow, "config", "commit.gpgsign", "false")
        _git_in(old_shallow, "checkout", "-q", "-b", "feature")
        _write(old_shallow, "a.txt", "original a\n")
        _write(old_shallow, "b.txt", "original b\n")
        _git_in(old_shallow, "add", "a.txt", "b.txt")
        _git_in(old_shallow, "commit", "-q", "-m", "silent revert")
        missing_newer_parent = _run(
            [
                sys.executable,
                os.path.join(REPO_ROOT, "tools/scripts/silent_revert_guard.py"),
                "--repo", old_shallow, "--base", "main", "--mode=report",
            ],
            old_shallow,
        )
        check("old-dated boundary cannot hide newer missing parent",
              missing_newer_parent.returncode == 2,
              f"rc={missing_newer_parent.returncode} "
              f"out={missing_newer_parent.stdout}")
        check("non-monotonic missing ancestry is classified unavailable",
              '"status":"history_unavailable"' in missing_newer_parent.stdout,
              missing_newer_parent.stdout)
        _git_in(
            old_shallow, "fetch", "-q", "--depth=1", "origin", grandparent_sha
        )
        locally_complete = _run(
            [
                sys.executable,
                os.path.join(REPO_ROOT, "tools/scripts/silent_revert_guard.py"),
                "--repo", old_shallow, "--base", "main", "--mode=report",
            ],
            old_shallow,
        )
        check("locally complete shallow graph blocks normally",
              locally_complete.returncode == 1,
              f"rc={locally_complete.returncode} out={locally_complete.stdout}")
        check("old-dated middle commit does not hide the recent landing",
              recent_landing_sha in locally_complete.stdout,
              locally_complete.stdout)


def main() -> int:
    test_predicate_blocks_wholesale_revert()
    test_predicate_revert_disguised_by_real_work()
    test_predicate_clean_cases()
    test_predicate_window()
    test_predicate_added_file_tombstone()
    test_predicate_noop_landing()
    test_since_arg_is_absolute()
    test_recent_landings_path_filter()
    test_recent_landings_honors_the_window()
    test_real_incident_replay()
    test_real_incident_end_to_end()
    test_real_branch_would_have_been_blocked_at_push()
    test_e2e_blocks_silent_revert()
    test_e2e_hint_mode_downgrades_content_block()
    test_e2e_behind_base_is_clean()
    test_e2e_explicit_revert_is_clean()
    test_e2e_skip_trailer_is_clean()
    test_e2e_normal_work_is_clean()
    test_e2e_fails_closed_on_bad_repo()
    test_e2e_intent_cannot_bypass_unavailable_base()
    test_post_resolution_failure_receipt_is_consistent()
    test_e2e_shallow_history_fails_only_for_nonempty_diff()

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
