"""Pytest fixtures for the Pulp sandbox E2E harness.

Binary discovery (in priority order):

1. Env-var overrides:
     PULP_CPP_BINARY_FOR_TEST  — the C++ pulp-cpp delegate
     PULP_RS_BINARY_FOR_TEST   — the Rust pulp binary

2. Build-artifact paths relative to the repo root:
     build/tools/cli/pulp-cpp                                 (C++, post-swap)
     build/tools/cli/pulp                                     (C++, pre-swap fallback)
     experimental/pulp-rs/target/release/pulp                 (Rust, post-swap)
     experimental/pulp-rs/target/debug/pulp                   (Rust, fallback)
     experimental/pulp-rs/target/release/pulp-rs              (Rust, pre-swap fallback)
     experimental/pulp-rs/target/debug/pulp-rs                (Rust, pre-swap fallback)

3. Fallback to the installed binary:
     ~/.pulp/bin/pulp-cpp                                     (C++, post-swap)
     ~/.pulp/bin/pulp                                         (C++, pre-swap fallback)

If neither candidate is found, tests that need the binary are skipped
with a clear message. Tests never run against "whatever happens to
be on PATH" — that's what the harness is defending against.
"""

from __future__ import annotations

import os
import shutil
from pathlib import Path
from typing import Iterator

import pytest

from pulp_sandbox import Sandbox


# ----- repo-root discovery ---------------------------------------------------


def _find_repo_root() -> Path:
    """Walk upward from this file until we find the ``.git`` directory
    (or a ``.git`` file, for worktrees)."""
    current = Path(__file__).resolve().parent
    for candidate in (current, *current.parents):
        if (candidate / ".git").exists():
            return candidate
    # Fall back to four levels up (tools/sandbox-e2e/conftest.py ->
    # tools/sandbox-e2e -> tools -> repo).
    return Path(__file__).resolve().parents[2]


REPO_ROOT = _find_repo_root()


# ----- binary discovery ------------------------------------------------------


def _discover_cpp_binary() -> Path | None:
    """Locate the C++ ``pulp-cpp`` binary under test. See module docstring."""
    override = os.environ.get("PULP_CPP_BINARY_FOR_TEST")
    if override:
        candidate = Path(override).expanduser()
        return candidate if candidate.exists() else None
    candidates = [
        REPO_ROOT / "build" / "tools" / "cli" / "pulp-cpp",
        REPO_ROOT / "build" / "tools" / "cli" / "pulp",
        Path.home() / ".pulp" / "bin" / "pulp-cpp",
        Path.home() / ".pulp" / "bin" / "pulp",
    ]
    for c in candidates:
        if c.exists():
            return c
    return None


def _discover_rust_binary() -> Path | None:
    """Locate the Rust ``pulp`` binary. Pre-swap ``pulp-rs`` paths remain
    as fallbacks so older branches can still run the harness."""
    override = os.environ.get("PULP_RS_BINARY_FOR_TEST")
    if override:
        candidate = Path(override).expanduser()
        return candidate if candidate.exists() else None

    # Build artifacts, then a sibling-worktree fallback (pre-merge).
    candidates = [
        REPO_ROOT / "experimental" / "pulp-rs" / "target" / "release" / "pulp",
        REPO_ROOT / "experimental" / "pulp-rs" / "target" / "debug" / "pulp",
        REPO_ROOT / "experimental" / "pulp-rs" / "target" / "release" / "pulp-rs",
        REPO_ROOT / "experimental" / "pulp-rs" / "target" / "debug" / "pulp-rs",
    ]
    # If the repo doesn't have experimental/ (it's on a branch where
    # the Rust work hasn't landed), look for sibling worktrees.
    if not (REPO_ROOT / "experimental").exists():
        for sibling in REPO_ROOT.parent.glob("pulp*"):
            for rel in (
                "experimental/pulp-rs/target/release/pulp",
                "experimental/pulp-rs/target/debug/pulp",
                "experimental/pulp-rs/target/release/pulp-rs",
                "experimental/pulp-rs/target/debug/pulp-rs",
            ):
                p = sibling / rel
                if p.exists():
                    candidates.append(p)

    for c in candidates:
        if c.exists():
            return c
    return None


# ----- fixtures --------------------------------------------------------------


@pytest.fixture(scope="session")
def cpp_binary() -> Path:
    path = _discover_cpp_binary()
    if path is None:
        pytest.skip(
            "C++ pulp-cpp binary not found. Set PULP_CPP_BINARY_FOR_TEST "
            "or build build/tools/cli/pulp-cpp."
        )
    return path


@pytest.fixture(scope="session")
def rust_binary() -> Path:
    path = _discover_rust_binary()
    if path is None:
        pytest.skip(
            "Rust pulp binary not found. Set PULP_RS_BINARY_FOR_TEST "
            "or `cargo build --release` under experimental/pulp-rs/."
        )
    return path


@pytest.fixture(scope="session")
def repo_root() -> Path:
    return REPO_ROOT


@pytest.fixture(scope="session")
def claude_commands_dir(repo_root: Path) -> Path:
    """Location of the plugin's slash-command markdown files."""
    d = repo_root / ".claude" / "commands"
    if not d.is_dir():
        pytest.skip(f"no .claude/commands directory at {d}")
    return d


@pytest.fixture(scope="session")
def stub_pulp_cpp_src(repo_root: Path) -> Path:
    """Path to the shell stub used for delegation tests."""
    p = repo_root / "tools" / "sandbox-e2e" / "fixtures" / "stub_pulp_cpp.sh"
    if not p.exists():
        pytest.fail(f"stub pulp-cpp fixture missing at {p}")
    return p


@pytest.fixture()
def sandbox() -> Iterator[Sandbox]:
    """Per-test sandbox. Teardown runs the contamination audit —
    any write to the user's real ``~/.pulp`` fails the test here."""
    sbx = Sandbox()
    sbx.setup()
    try:
        yield sbx
        # Contamination check must run before teardown so the failure
        # surfaces as a test failure, not a teardown error.
        sbx.assert_no_contamination()
    finally:
        sbx.teardown()


# ----- reporting hook --------------------------------------------------------


def pytest_report_header(config: pytest.Config) -> list[str]:
    cpp = _discover_cpp_binary()
    rs = _discover_rust_binary()
    return [
        f"pulp sandbox e2e: C++ binary = {cpp or '(not found)'}",
        f"pulp sandbox e2e: Rust binary = {rs or '(not found)'}",
        f"pulp sandbox e2e: repo root   = {REPO_ROOT}",
    ]


# Make pulp_sandbox importable regardless of where pytest is invoked
# from. pytest auto-adds conftest's dir to sys.path, but a developer
# running `pytest tools/sandbox-e2e/` from the repo root should get
# the same behavior as `pytest` from inside the dir.
def pytest_configure(config: pytest.Config) -> None:  # noqa: D401
    import sys as _sys

    harness_dir = str(Path(__file__).resolve().parent)
    if harness_dir not in _sys.path:
        _sys.path.insert(0, harness_dir)
    _ = shutil  # silence unused-import if any platform drops the branch
