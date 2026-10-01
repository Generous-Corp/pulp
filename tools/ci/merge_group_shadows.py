#!/usr/bin/env python3
"""Run every merge-group shadow instrument from one workflow step.

The merge-group `macos` job carries four shadow instruments, each of which
annotates and changes no outcome:

- binary identity vs the PR head's receipt   (tools/ci/binary_identity_shadow.py)
- the graph's affected-test set              (tools/ci/affected_tests_shadow.py)
- flake exoneration of a failed ctest        (tools/ci/flake_exoneration_shadow.py)
- per-test content-hash receipts             (tools/ci/test_receipts_shadow.py)

Three separate workflow steps meant three places in build.yml that every
neighbouring change conflicted with. This runner is the one step: it calls
each instrument in isolation, names any that could not run (an exception or
a non-zero exit is reported, never propagated), and always exits 0. Gating
per instrument: identity and affected tests need a successful build and the
ctest inventory; exoneration runs only when ctest's outcome was failure;
receipts run whenever ctest ran (passed or failed) and write
`<work-dir>/test-receipts.json`, which the workflow uploads as the
`test-receipts-macos` artifact, and every entry's key to
`<work-dir>/test-keys.json`, which the per-job reuse record carries. Binary
identity runs first and leaves its per-binary hashes in
`<work-dir>/our-identity.json` so neither receipts nor the reuse record hash
the test binaries twice.

    merge_group_shadows.py run --build-dir B --source-root S --repository O/R \\
        --merge-sha SHA --token T --junit J --selected-json I \\
        --ctest-outcome success|failure|skipped [--hours 24] [--work-dir W] [--run-id N]
"""
from __future__ import annotations

import argparse
import importlib
import os
import subprocess
import sys
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))


def _first_parent(source_root: str, sha: str) -> str | None:
    proc = subprocess.run(["git", "-C", source_root, "rev-parse", "--verify", "--quiet", f"{sha}^1"],
                          capture_output=True, text=True)
    return proc.stdout.strip() or None


def load_module(module_name: str):
    return importlib.import_module(module_name)


def run_one(label: str, module_name: str, argv: list[str]) -> str:
    """Import and run one instrument's main(argv); return a one-word status."""
    try:
        module = load_module(module_name)
        rc = module.main([module_name] + argv)
        return f"rc={rc}"
    except SystemExit as exc:  # argparse or an explicit exit inside the instrument
        return f"exit={exc.code}"
    except Exception as exc:  # noqa: BLE001 - shadow: report, never propagate
        print(f"merge-group shadows: {label} could not run: {exc!r}", file=sys.stderr)
        traceback.print_exc(file=sys.stderr)
        return "error"


def plan(a: argparse.Namespace) -> list[tuple[str, str, list[str]] | tuple[str, str, None]]:
    """The instruments to run and their argv; None means skipped, with the reason as argv."""
    steps: list = []
    steps.append(("binary-identity", "binary_identity_shadow",
                  ["measure", "--build-dir", a.build_dir, "--source-root", a.source_root,
                   "--merge-sha", a.merge_sha, "--repository", a.repository, "--token", a.token,
                   "--work-dir", a.work_dir]))
    base = _first_parent(a.source_root, a.merge_sha)
    if a.ctest_outcome == "skipped":
        steps.append(("affected-tests", "affected_tests_shadow", None))
    elif base is None:
        steps.append(("affected-tests", "affected_tests_shadow", None))
    else:
        steps.append(("affected-tests", "affected_tests_shadow",
                      ["--build-dir", a.build_dir, "--source-root", a.source_root, "--base", base,
                       "--head", a.merge_sha, "--selected-json", a.selected_json, "--junit", a.junit,
                       "--event", "merge_group"]))
    if a.ctest_outcome == "failure":
        steps.append(("flake-exoneration", "flake_exoneration_shadow",
                      ["--repo", a.repository, "--junit", a.junit, "--head", a.merge_sha,
                       "--hours", str(a.hours)]))
    else:
        steps.append(("flake-exoneration", "flake_exoneration_shadow", None))
    if a.ctest_outcome in ("success", "failure"):
        steps.append(("test-receipts", "test_receipts_shadow",
                      ["run", "--build-dir", a.build_dir, "--source-root", a.source_root,
                       "--repository", a.repository, "--merge-sha", a.merge_sha, "--token", a.token,
                       "--junit", a.junit, "--selected-json", a.selected_json, "--run-id", a.run_id,
                       "--receipts-out", os.path.join(a.work_dir, "test-receipts.json"),
                       "--identity-json", os.path.join(a.work_dir, "our-identity.json"),
                       "--keys-out", os.path.join(a.work_dir, "test-keys.json")]))
    else:
        steps.append(("test-receipts", "test_receipts_shadow", None))
    return steps


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--build-dir", required=True)
    r.add_argument("--source-root", required=True)
    r.add_argument("--repository", required=True)
    r.add_argument("--merge-sha", required=True)
    r.add_argument("--token", default=os.environ.get("GITHUB_TOKEN", ""))
    r.add_argument("--junit", required=True)
    r.add_argument("--selected-json", required=True)
    r.add_argument("--ctest-outcome", choices=("success", "failure", "skipped", "cancelled"), required=True)
    r.add_argument("--hours", type=int, default=24)
    r.add_argument("--work-dir", default=os.environ.get("RUNNER_TEMP", "/tmp"))
    r.add_argument("--run-id", default=os.environ.get("GITHUB_RUN_ID", ""))
    a = ap.parse_args(argv[1:])
    results = []
    for label, module_name, args in plan(a):
        if args is None:
            results.append(f"{label}=skipped")
            continue
        results.append(f"{label}={run_one(label, module_name, args)}")
    print("merge-group shadows: " + " ".join(results))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
