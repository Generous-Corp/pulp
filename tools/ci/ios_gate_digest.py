#!/usr/bin/env python3
"""Content digest of everything the iOS compile gate can read — shadow mode.

The iOS compile gate (`test/cmake/test_ios_compile_gate.sh`) is the largest
piece of the required `macos` gate's Build step (6–13 min per job, three to
four times the whole macOS Ninja build), and it re-runs on every PR-head push
and every merge group whose diff the path allowlist cannot clear — 77% of
merge groups and 100% of PR-head runs, although 64% of consecutive PR-head
pushes and 23% of merge groups leave every file it compiles untouched.

This tool gives each gate run an exact identity: a SHA-256 over the blob ids
of every tracked file the gate's three configures can reach, plus the Apple
toolchain that compiles them. Two trees with the same digest cannot produce a
different gate result. The workflow records a successful run under its digest
(a workflow artifact named `ios-gate-ok-<digest>`) and, before the next run,
looks the digest up. In SHADOW mode it only reports what it would have done
(`would_skip` when a passing run with the same digest exists, else `run`); the
gate still runs. Nothing is skipped and no gating changes.

Two proxies read from the job annotations (`pulp-ios-gate-shadow/v1`):
- would_skip ÷ gate runs, merge group and PR head separately;
- the safety control: would_skip verdicts whose real gate run then FAILED,
  which must be zero before any switch-on is proposed.

Input set. The gate configures with PULP_BUILD_TESTS=OFF for the two Ninja
SDK legs and PULP_BUILD_EXAMPLES=ON + PULP_ENABLE_GPU=ON for the Xcode GPU
leg, so its inputs are the product sources, examples, every CMake file,
dependency pins, the Skia fetch script, the gate's own scripts, and the
test/docs paths that non-test CMake names (`IOS_COMPILE_REQUIRED_PATTERNS`).
The set below is deliberately a SUPERSET of what the configures list: a path
that is in the set but unread costs a needless re-run, a path that is read but
not in the set would let a change through, so the set errs wide. `*.md` files
are excluded everywhere: no CMake or compiler reads them.

Usage:
    ios_gate_digest.py compute --repo . [--rev HEAD] [--toolchain-id ID]
    ios_gate_digest.py lookup --repository O/R --token T --digest D
    ios_gate_digest.py note --verdict would_skip|run|ran_ok|ran_failed --digest D
                            [--source-run-id N] [--event E]
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import subprocess
import sys
import urllib.parse
import urllib.request
from pathlib import Path

SCHEMA = "pulp-ios-gate-shadow/v1"
DIGEST_VERSION = "1"
ARTIFACT_PREFIX = "ios-gate-ok-"

# Paths the gate's configures can read (see the module docstring). Matched
# with fnmatch against the repository-relative path; `**` here means "any
# path under", so patterns are checked both as written and as a prefix.
INPUT_PATTERNS = (
    "core/**",
    "examples/**",
    "apple/**",
    "inspect/**",
    "external/**",
    "cmake/**",
    "tools/cmake/**",
    "tools/deps/**",
    "CMakeLists.txt",
    "**/CMakeLists.txt",
    "**/*.cmake",
    "setup.sh",
    "tools/scripts/fetch_skia_for_release.py",
    "test/cmake/test_ios_*",
)
EXCLUDE_PATTERNS = ("*.md", "**/*.md")


def _required_patterns() -> tuple[str, ...]:
    """`IOS_COMPILE_REQUIRED_PATTERNS` from classify_changes.py: the test/docs
    paths the iOS configures name. Read from the sibling module so the two
    lists cannot drift; absent (a partial checkout) they are simply not added,
    which only widens nothing and is reported by `--print-inputs`."""
    here = Path(__file__).resolve().parent.parent / "scripts"
    sys.path.insert(0, str(here))
    try:
        import classify_changes  # type: ignore
        return tuple(classify_changes.IOS_COMPILE_REQUIRED_PATTERNS)
    except Exception:  # pragma: no cover - partial checkouts
        return ()
    finally:
        sys.path.pop(0)


def _match(path: str, pattern: str) -> bool:
    if pattern.endswith("/**"):
        return path.startswith(pattern[:-2])
    if fnmatch.fnmatchcase(path, pattern):
        return True
    # `**/x` also matches a top-level `x`.
    return pattern.startswith("**/") and fnmatch.fnmatchcase(path, pattern[3:])


def is_input(path: str, patterns: tuple[str, ...] | None = None) -> bool:
    if any(_match(path, p) for p in EXCLUDE_PATTERNS):
        return False
    pats = patterns if patterns is not None else INPUT_PATTERNS + _required_patterns()
    return any(_match(path, p) for p in pats)


def tracked_blobs(repo: Path, rev: str) -> list[tuple[str, str]]:
    """(path, blob sha) for every tracked file at `rev`, submodule gitlinks
    included as their commit id (a moved gitlink is a changed input)."""
    out = subprocess.run(["git", "-C", str(repo), "ls-tree", "-r", "--full-tree", rev],
                         capture_output=True, text=True, check=True).stdout
    rows = []
    for line in out.splitlines():
        meta, _, path = line.partition("\t")
        parts = meta.split()
        if len(parts) >= 3:
            rows.append((path, parts[2]))
    return rows


def toolchain_id() -> str:
    """Apple toolchain identity. Off macOS, or without Xcode, `unknown`, which
    still yields a valid (if never matching cross-host) digest."""
    parts = []
    for cmd in (["xcodebuild", "-version"],
                ["xcrun", "--sdk", "iphoneos", "--show-sdk-version"],
                ["xcrun", "--sdk", "iphonesimulator", "--show-sdk-version"]):
        try:
            parts.append(" ".join(subprocess.run(cmd, capture_output=True, text=True,
                                                 check=True, timeout=60).stdout.split()))
        except Exception:
            parts.append("unknown")
    return " | ".join(parts)


def compute(repo: Path, rev: str = "HEAD", toolchain: str | None = None,
            patterns: tuple[str, ...] | None = None) -> dict:
    blobs = [(p, b) for p, b in tracked_blobs(repo, rev) if is_input(p, patterns)]
    tc = toolchain if toolchain is not None else toolchain_id()
    h = hashlib.sha256()
    h.update(f"{SCHEMA} digest-v{DIGEST_VERSION}\n".encode())
    h.update(f"toolchain\t{tc}\n".encode())
    for path, blob in sorted(blobs):
        h.update(f"{blob}\t{path}\n".encode())
    return {"digest": h.hexdigest(), "inputs": len(blobs), "toolchain": tc,
            "rev": subprocess.run(["git", "-C", str(repo), "rev-parse", rev],
                                  capture_output=True, text=True, check=True).stdout.strip()}


def artifact_name(digest: str) -> str:
    return ARTIFACT_PREFIX + digest


def lookup(repository: str, token: str, digest: str, fetch=None) -> dict:
    """Is there an unexpired `ios-gate-ok-<digest>` artifact? `fetch(url, token)`
    returns the parsed JSON page; injected by tests."""
    fetch = fetch or _fetch_json
    name = artifact_name(digest)
    url = (f"https://api.github.com/repos/{repository}/actions/artifacts?"
           + urllib.parse.urlencode({"name": name, "per_page": 100}))
    page = fetch(url, token)
    live = [a for a in page.get("artifacts", []) if a.get("name") == name and not a.get("expired")]
    live.sort(key=lambda a: a.get("created_at") or "")
    if not live:
        return {"found": False, "digest": digest, "candidates": len(page.get("artifacts", []))}
    src = live[-1]
    return {"found": True, "digest": digest,
            "source_run_id": (src.get("workflow_run") or {}).get("id"),
            "created_at": src.get("created_at"), "candidates": len(live)}


def _fetch_json(url: str, token: str) -> dict:
    req = urllib.request.Request(url, headers={
        "Authorization": f"Bearer {token}", "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28"})
    with urllib.request.urlopen(req, timeout=60) as resp:  # noqa: S310 - fixed GitHub host
        return json.load(resp)


def note(verdict: str, digest: str, event: str | None, source_run_id: int | None) -> str:
    """The annotation line the proxies read. Always a `notice`: shadow mode
    never fails a job."""
    record = {"schema": SCHEMA, "mode": "shadow", "verdict": verdict, "digest": digest,
              "event": event, "source_run_id": source_run_id}
    return f"::notice title=ios-gate-shadow::{json.dumps(record, sort_keys=True)}"


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("compute")
    c.add_argument("--repo", default=".")
    c.add_argument("--rev", default="HEAD")
    c.add_argument("--toolchain-id", default=None)
    c.add_argument("--print-inputs", action="store_true")
    l = sub.add_parser("lookup")
    l.add_argument("--repository", required=True)
    l.add_argument("--token", default=os.environ.get("GITHUB_TOKEN", ""))
    l.add_argument("--digest", required=True)
    n = sub.add_parser("note")
    n.add_argument("--verdict", required=True, choices=("would_skip", "run", "ran_ok", "ran_failed"))
    n.add_argument("--digest", required=True)
    n.add_argument("--event", default=os.environ.get("GITHUB_EVENT_NAME"))
    n.add_argument("--source-run-id", type=int, default=None)
    a = ap.parse_args(argv[1:])
    if a.cmd == "compute":
        repo = Path(a.repo)
        result = compute(repo, a.rev, a.toolchain_id)
        if a.print_inputs:
            for p, _ in sorted(tracked_blobs(repo, a.rev)):
                if is_input(p):
                    print(p, file=sys.stderr)
        print(json.dumps(result, sort_keys=True))
        return 0
    if a.cmd == "lookup":
        if not a.token:
            print("lookup: no token", file=sys.stderr)
            return 2
        try:
            print(json.dumps(lookup(a.repository, a.token, a.digest), sort_keys=True))
        except Exception as exc:  # the API is not the gate; report and continue
            print(json.dumps({"found": False, "digest": a.digest, "error": str(exc)}))
            return 3
        return 0
    print(note(a.verdict, a.digest, a.event, a.source_run_id))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
