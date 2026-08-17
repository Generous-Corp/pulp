#!/usr/bin/env python3
"""Controls for the checkout preflight.

Both directions are asserted for every check. A preflight that stopped firing
would "pass" a green-path test while silently reverting the checkout to the
diagnose-by-configure loop it exists to replace, and a preflight that fires on a
healthy tree gets muted within a week.

The archive-floor control is a REAL archive compiled above the floor rather than
a stubbed version string, because the thing under test is whether we read
`LC_BUILD_VERSION` out of a `.a` correctly.
"""
from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE / "checkout_preflight.py"
LIB_REL = pathlib.Path("build") / "mac-gpu" / "lib" / "Release"


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run(skia_dir: str | None, *extra: str) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    if skia_dir is None:
        env.pop("SKIA_DIR", None)
    else:
        env["SKIA_DIR"] = skia_dir
    return subprocess.run(
        [sys.executable, str(SCRIPT), *extra],
        text=True, capture_output=True, env=env,
    )


def _archive(path: pathlib.Path, min_os: str) -> bool:
    """Build a real static archive pinned to `min_os`. False if unavailable."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temp:
        src = pathlib.Path(temp) / "c.c"
        src.write_text("int pulp_ctl(void){return 0;}\n")
        obj = pathlib.Path(temp) / "c.o"
        compiled = subprocess.run(
            ["clang", "-c", "-arch", "arm64", f"-mmacosx-version-min={min_os}",
             str(src), "-o", str(obj)],
            capture_output=True,
        )
        if compiled.returncode != 0:
            return False
        return subprocess.run(
            ["ar", "rcs", str(path), str(obj)], capture_output=True
        ).returncode == 0


def test_floor_violating_archive_is_caught() -> int:
    """The failure that cost a night: Dawn above the floor, everything else fine.

    Configure reports this accurately but late, and names the archive rather
    than the cache. The preflight must name the cache AND the fix.
    """
    if sys.platform != "darwin":
        return 0
    with tempfile.TemporaryDirectory(prefix="pulp-preflight-bad-") as temp:
        root = pathlib.Path(temp)
        if not _archive(root / LIB_REL / "libdawn_combined.a", "15.0"):
            return 0  # no toolchain here; nothing to assert
        _archive(root / LIB_REL / "libskia.a", "13.0")
        result = run(str(root))
        check(result.returncode == 1, "a floor-violating cache must exit 1")
        check(
            "libdawn_combined.a is built for macOS 15.0" in result.stderr,
            f"must name the offending archive and its floor:\n{result.stderr}",
        )
        check(
            f"--dest {root}" in result.stderr,
            "the fix must point at the cache the build ACTUALLY uses, not a "
            f"generic command:\n{result.stderr}",
        )
    return 1


def test_conforming_archive_passes() -> int:
    """The control. A check that always fires is not a check."""
    if sys.platform != "darwin":
        return 0
    with tempfile.TemporaryDirectory(prefix="pulp-preflight-good-") as temp:
        root = pathlib.Path(temp)
        if not _archive(root / LIB_REL / "libdawn_combined.a", "13.0"):
            return 0
        _archive(root / LIB_REL / "libskia.a", "13.0")
        result = run(str(root), "--skip-skia")
        check(
            "libdawn_combined.a" not in result.stderr,
            f"--skip-skia must not report Skia problems:\n{result.stderr}",
        )
        result = run(str(root))
        check(
            "is built for macOS" not in result.stderr,
            f"a conforming cache must not be reported as a floor problem:\n{result.stderr}",
        )
    return 1


def test_skia_dir_override_is_reported() -> int:
    """The invisible mechanism: $SKIA_DIR beats the checkout's own cache.

    Worktrees holding good caches were building against a broken one because
    an inherited env pin wins. Nothing else in the build output says so, which
    is why both obvious local remedies could not take effect.
    """
    if sys.platform != "darwin":
        return 0
    with tempfile.TemporaryDirectory(prefix="pulp-preflight-ovr-") as temp:
        root = pathlib.Path(temp)
        if not _archive(root / LIB_REL / "libskia.a", "13.0"):
            return 0
        _archive(root / LIB_REL / "libdawn_combined.a", "13.0")
        out = run(str(root)).stdout
        check("OVERRIDES" in out, f"the override must be stated:\n{out}")
        check(str(root) in out, f"the winning path must be named:\n{out}")
    return 1


def test_unpopulated_cache_is_distinguished_from_a_bad_one() -> int:
    """A headers-only worktree needs a fetch, not a cache refresh diagnosis."""
    if sys.platform != "darwin":
        return 0
    with tempfile.TemporaryDirectory(prefix="pulp-preflight-empty-") as temp:
        (pathlib.Path(temp) / LIB_REL).mkdir(parents=True)
        result = run(temp)
        check(result.returncode == 1, "an unpopulated cache must exit 1")
        check(
            "not populated" in result.stderr,
            f"must say the cache is unpopulated:\n{result.stderr}",
        )
    return 1


def test_missing_skia_dir_is_reported_not_crashed() -> int:
    result = run("/nonexistent/skia-build-path")
    check(result.returncode == 1, "a missing SKIA_DIR must exit 1, not crash")
    check("does not exist" in result.stderr, result.stderr)
    return 1


def test_unfinished_merge_makes_the_checkout_untrustworthy_to_read() -> int:
    """A checkout on a stale branch mid-merge is not a source of truth.

    This exists because the same mistake was made twice in one session: pinned
    values were read from a checkout parked 6,365 commits behind main with an
    unfinished merge, and reported as what the repo declares. Once it nearly
    overrode a peer's correct diagnosis. `grep` never names the branch it
    answered from.
    """
    with tempfile.TemporaryDirectory(prefix="pulp-preflight-merge-") as temp:
        repo = pathlib.Path(temp) / "repo"
        repo.mkdir()
        def git(*a: str) -> subprocess.CompletedProcess[str]:
            return subprocess.run(["git", *a], cwd=repo, text=True, capture_output=True)
        git("init", "--quiet", "--initial-branch=main", ".")
        git("config", "user.email", "t@pulp.invalid")
        git("config", "user.name", "t")
        (repo / "pin.txt").write_text("v1\n")
        git("add", "."); git("commit", "--quiet", "-m", "base")
        git("checkout", "--quiet", "-b", "side")
        (repo / "pin.txt").write_text("side\n")
        git("add", "."); git("commit", "--quiet", "-m", "side")
        git("checkout", "--quiet", "main")
        (repo / "pin.txt").write_text("main\n")
        git("add", "."); git("commit", "--quiet", "-m", "main")
        git("merge", "side")  # conflicts, left unresolved on purpose

        conflicted = git("diff", "--name-only", "--diff-filter=U").stdout.strip()
        check(bool(conflicted), "control invalid: the merge did not conflict")

        result = run(None, "--skip-skia", "--root", str(repo))
        check(result.returncode == 1, "an unfinished merge must exit 1")
        check(
            "UNFINISHED MERGE" in result.stderr,
            f"must name the unfinished merge:\n{result.stderr}",
        )
        check(
            "git show origin/main:" in result.stderr,
            "must give the safe way to read a pinned value",
        )
    return 1


def test_clean_checkout_is_not_flagged_as_untrustworthy() -> int:
    """The control: an ordinary clean checkout must not be called stale."""
    with tempfile.TemporaryDirectory(prefix="pulp-preflight-clean-") as temp:
        repo = pathlib.Path(temp) / "repo"
        repo.mkdir()
        def git(*a: str) -> subprocess.CompletedProcess[str]:
            return subprocess.run(["git", *a], cwd=repo, text=True, capture_output=True)
        git("init", "--quiet", "--initial-branch=main", ".")
        git("config", "user.email", "t@pulp.invalid")
        git("config", "user.name", "t")
        (repo / "pin.txt").write_text("v1\n")
        git("add", "."); git("commit", "--quiet", "-m", "base")
        result = run(None, "--skip-skia", "--root", str(repo))
        check(
            "UNFINISHED MERGE" not in result.stderr,
            f"a clean checkout must not be flagged:\n{result.stderr}",
        )
    return 1


def test_planning_submodule_state_is_reported_both_ways() -> int:
    """An uninitialized submodule reads downstream as a contract violation."""
    repo_root = SCRIPT.resolve().parents[2]
    planning = repo_root / "planning"
    initialized = (planning / ".git").exists() and any(
        c.name != ".git" for c in planning.iterdir()
    ) if planning.exists() else False
    result = run(None, "--skip-skia")
    if initialized:
        check(
            "planning submodule: initialized" in result.stdout,
            f"an initialized submodule must be reported as such:\n{result.stdout}",
        )
    else:
        check(
            "planning submodule is not initialized" in result.stderr,
            f"an uninitialized submodule must be a problem:\n{result.stderr}",
        )
        check(
            "git submodule update --init planning" in result.stderr,
            "must print the exact fix",
        )
    return 1


def main() -> int:
    if not SCRIPT.exists():
        print(f"checkout-preflight: {SCRIPT} missing", file=sys.stderr)
        return 1
    if sys.platform == "darwin" and shutil.which("clang") is None:
        print("checkout-preflight: clang unavailable; archive controls skipped")
    checks = 0
    for name, test in sorted(globals().items()):
        if name.startswith("test_") and callable(test):
            checks += test()
    print(f"checkout-preflight: {checks} preflight controls passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
