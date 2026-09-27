#!/usr/bin/env python3
"""Refuse to build a Pulp source checkout that lives in a temporary directory.

A checkout under ``/tmp``, ``/private/tmp`` or ``$TMPDIR`` defeats the shared
compiler cache. ccache rewrites absolute paths to relative ones only below its
``base_dir``; a tree outside it hashes its own absolute paths, so it can only
ever hit entries it wrote itself and every fresh temporary worktree compiles
cold. Temporary checkouts are also the ones nobody reclaims, and they fill the
disk the build hosts share.

The refusal is enforced where builds start (the root CMake configure and
``tools/ci/governed-build.sh``), because a rule that only lives in a document
does not reach an agent that never reads it.

Exit status: 0 when the checkout may be built (warnings may still print),
3 when it is refused. Ephemeral CI jobs (``GITHUB_ACTIONS=true``) and an
explicit ``PULP_ALLOW_TMP_CHECKOUT=1`` are allowed.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Callable, Mapping, Sequence

ALLOW_ENV = "PULP_ALLOW_TMP_CHECKOUT"
REFUSED_EXIT = 3
FIXED_TEMP_ROOTS = ("/tmp", "/private/tmp")


def _real(path: str | Path) -> Path:
    return Path(os.path.realpath(os.path.expanduser(str(path))))


def _is_within(path: Path, root: Path) -> bool:
    return path == root or root in path.parents


def temp_roots(env: Mapping[str, str]) -> list[Path]:
    """Every temporary root a checkout must not live under, resolved."""

    roots = [_real(root) for root in FIXED_TEMP_ROOTS]
    tmpdir = env.get("TMPDIR", "").strip()
    if tmpdir:
        resolved = _real(tmpdir)
        # A TMPDIR of "/" or a home directory would refuse everything.
        if len(resolved.parts) > 2 and resolved != _real(Path.home()):
            roots.append(resolved)
    unique: list[Path] = []
    for root in roots:
        if root not in unique:
            unique.append(root)
    return unique


def temp_root_containing(source: Path, env: Mapping[str, str]) -> Path | None:
    resolved = _real(source)
    for root in temp_roots(env):
        if _is_within(resolved, root):
            return root
    return None


def primary_checkout(source: Path) -> Path | None:
    """The main worktree of the repository ``source`` belongs to, if any."""

    try:
        common = subprocess.run(
            ["git", "-C", str(source), "rev-parse", "--path-format=absolute", "--git-common-dir"],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if common.returncode != 0 or not common.stdout.strip():
        return None
    common_dir = Path(common.stdout.strip())
    return common_dir.parent if common_dir.name == ".git" else None


def suggested_root(source: Path, env: Mapping[str, str]) -> tuple[Path | None, str]:
    declared = env.get("PULP_WORKTREES_ROOT", "").strip()
    if declared:
        return Path(declared), "PULP_WORKTREES_ROOT"
    primary = primary_checkout(source)
    if primary is not None and temp_root_containing(primary, env) is None:
        return primary.parent, "sibling of the primary checkout"
    return None, ""


def ccache_base_dir(run: Callable[[Sequence[str]], str | None]) -> str | None:
    return run(["ccache", "-k", "base_dir"])


def _run_ccache(argv: Sequence[str]) -> str | None:
    if shutil.which(argv[0]) is None:
        return None
    try:
        result = subprocess.run(
            list(argv), capture_output=True, text=True, timeout=10, check=False
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def refusal_message(source: Path, temp_root: Path, env: Mapping[str, str], context: str) -> str:
    root, how = suggested_root(source, env)
    lines = [
        f"{context}: refusing to build a Pulp checkout in a temporary directory.",
        f"  checkout: {_real(source)}",
        f"  temporary root: {temp_root}",
        "  A checkout here misses the shared ccache (its base_dir cannot cover it), so",
        "  every build is cold, and temporary checkouts are never reclaimed.",
    ]
    if root is not None:
        lines += [
            f"  Create worktrees under {root} ({how}) instead:",
            f"    git worktree add \"{root}/<name>\" <branch>",
        ]
    else:
        lines += [
            "  Create worktrees under $PULP_WORKTREES_ROOT, or beside the primary checkout:",
            "    git worktree add \"$PULP_WORKTREES_ROOT/<name>\" <branch>",
        ]
    lines.append(
        f"  A deliberate one-off (a throwaway clean validation) may set {ALLOW_ENV}=1."
    )
    return "\n".join(lines)


def base_dir_warning(source: Path, base_dir: str | None, context: str) -> str | None:
    if not base_dir:
        return None
    resolved_base = _real(base_dir)
    if _is_within(_real(source), resolved_base):
        return None
    return (
        f"{context}: warning: checkout {_real(source)} is outside ccache base_dir "
        f"{resolved_base}; its compiles can only hit cache entries this tree wrote itself. "
        "Move the worktree below base_dir to share the host cache."
    )


def evaluate(
    source: Path,
    env: Mapping[str, str],
    context: str,
    ccache: Callable[[Sequence[str]], str | None] = _run_ccache,
) -> tuple[int, list[str]]:
    """Return (exit status, messages) without side effects."""

    if env.get("GITHUB_ACTIONS", "").lower() == "true":
        return 0, []
    messages: list[str] = []
    temp_root = temp_root_containing(source, env)
    if temp_root is not None:
        if env.get(ALLOW_ENV, "") == "1":
            messages.append(
                f"{context}: note: building a checkout in {temp_root} because {ALLOW_ENV}=1."
            )
        else:
            return REFUSED_EXIT, [refusal_message(source, temp_root, env, context)]
    warning = base_dir_warning(source, ccache_base_dir(ccache), context)
    if warning:
        messages.append(warning)
    return 0, messages


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("source", type=Path, help="source checkout root")
    parser.add_argument("--context", default="pulp", help="prefix for messages")
    args = parser.parse_args(argv)
    status, messages = evaluate(args.source, os.environ, args.context)
    for message in messages:
        print(message, file=sys.stderr)
    return status


if __name__ == "__main__":
    raise SystemExit(main())
