#!/usr/bin/env python3
"""Are Pulp's test binaries reproducible across gate VMs? Shadow measurement.

A merge group can only skip the tests of an executable whose bytes it can
prove equal to a passing PR-head build's (per-binary receipt reuse: the looser,
still-safe equality behind Chromium's "same change, moved base" tryjob reuse).
That needs one precondition nobody has measured: the same sources, built on
two different VMs (PR head on one host, merge group on another), produce the
same sha256 for every test binary. Any linker-embedded path, timestamp or
UUID breaks it, and has to be normalised before reuse can be designed.

This tool runs in the merge-group `macos` job after the build:
1. computes this build's per-binary identity (`protected_merge_receipt.py`'s
   `artifact_identity`, the same list a receipt carries);
2. finds the PR head's receipt (`protected-validation-macos-<head>-<base>`
   for the merge commit's second and first parent) and, when it exists,
   downloads it through the receipt module's own verifying `download`. Only
   heads that ran the full suite issue a receipt, so a head without one is
   read from its reuse record instead (`reuse-record-macos`, which every
   pull-request `macos` job writes with every test executable's sha256,
   `tools/ci/reuse_record.py`), taken only from this repository's
   `build.yml` pull_request run for that exact head;
3. compares the two lists path by path and annotates
   `pulp-binary-identity-shadow/v1`: tree_identical (merge tree == head tree),
   compared, identical, differing (with up to 20 example paths), or
   `waiting_on_receipt` when the PR head published none.

It skips nothing and changes no outcome. `--report` reads the annotations of
recent merge-group runs and prints the aggregate; until n >= 20 tree-identical
groups have been compared it says so ("waiting on receipts, n=K").

Proxy: identical ÷ compared on tree-identical groups (must approach 100%).
Control: a group whose sources changed shows differing > 0 for the binaries
that link them.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "scripts"))
import protected_merge_receipt as pmr  # noqa: E402

SCHEMA = "pulp-binary-identity-shadow/v1"
TITLE = "binary-identity-shadow"


def compare(ours: dict, theirs: dict) -> dict:
    """Per-path sha256 comparison of two artifact identities (`files` lists)."""
    a = {f["path"]: f["sha256"] for f in ours.get("files", [])}
    b = {f["path"]: f["sha256"] for f in theirs.get("files", [])}
    common = sorted(set(a) & set(b))
    identical = [p for p in common if a[p] == b[p]]
    differing = [p for p in common if a[p] != b[p]]
    return {"compared": len(common), "identical": len(identical), "differing": len(differing),
            "only_here": len(set(a) - set(b)), "only_there": len(set(b) - set(a)),
            "differing_examples": differing[:20],
            "identical_share": (len(identical) / len(common)) if common else None}


RECORD_ARTIFACT = "reuse-record-macos"
WORKFLOW_PATH = ".github/workflows/build.yml"
MAX_RECORD_BYTES = 64 * 1024 * 1024
API = "https://api.github.com"


def record_files(identity: dict) -> dict:
    """A reuse record's identity.json as a receipt-shaped `files` list: the
    build-tree executables only, paths relative to the build directory."""
    files = [{"path": key[len("<build>/"):], "sha256": rec["sha256"]}
             for key, rec in (identity.get("executables") or {}).items()
             if key.startswith("<build>/") and rec.get("sha256")]
    return {"files": sorted(files, key=lambda f: f["path"])}


def head_record_identity(repository: str, head: str, token: str,
                         fetch=None, download=None) -> tuple[dict | None, str]:
    """(identity, source run) from the newest trusted reuse record of `head`:
    this repository's build.yml, a pull_request run whose head is `head`.
    (None, reason) when there is none."""
    import io
    import zipfile

    fetch = fetch or pmr.api_json
    download = download or (lambda url, tok: pmr.download_archive(url, tok, MAX_RECORD_BYTES))
    runs = fetch(f"{API}/repos/{repository}/actions/runs?head_sha={head}&event=pull_request&per_page=50",
                 token).get("workflow_runs", [])
    trusted = [r for r in runs if r.get("path") == WORKFLOW_PATH and r.get("head_sha") == head
               and (r.get("repository") or {}).get("full_name") == repository
               and (r.get("head_repository") or {}).get("full_name") == repository]
    if not trusted:
        return None, f"no trusted build.yml pull_request run for {head}"
    for run in sorted(trusted, key=lambda r: r.get("id", 0), reverse=True):
        arts = fetch(f"{API}/repos/{repository}/actions/runs/{run['id']}/artifacts?per_page=100",
                     token).get("artifacts", [])
        arts = [x for x in arts if not x.get("expired") and
                (x.get("name") == RECORD_ARTIFACT or x.get("name", "").startswith(RECORD_ARTIFACT + "-attempt-"))]
        for art in sorted(arts, key=lambda x: x.get("id", 0), reverse=True):
            with zipfile.ZipFile(io.BytesIO(download(art["archive_download_url"], token))) as zf:
                ident = json.loads(zf.read("identity.json"))
            files = record_files(ident)
            if files["files"]:
                return files, str(run["id"])
    return None, f"no reuse record with executable hashes for {head}"


def git(repo: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True, check=True).stdout.strip()


def merge_parents(repo: Path, sha: str) -> tuple[str, str] | None:
    parts = git(repo, "rev-list", "--parents", "-n", "1", sha).split()
    return (parts[1], parts[2]) if len(parts) == 3 else None


def tree_identical(repo: Path, merge: str, head: str) -> bool:
    return git(repo, "rev-parse", f"{merge}^{{tree}}") == git(repo, "rev-parse", f"{head}^{{tree}}")


def note(record: dict) -> str:
    return f"::notice title={TITLE}::{json.dumps(record, sort_keys=True)}"


def cmd_measure(a: argparse.Namespace) -> int:
    repo = Path(a.source_root).resolve()
    build_dir = Path(a.build_dir).resolve()
    record: dict = {"schema": SCHEMA, "mode": "shadow", "merge_sha": a.merge_sha}
    parents = merge_parents(repo, a.merge_sha)
    if not parents:
        record.update({"verdict": "not_a_two_parent_merge"})
        print(note(record))
        return 0
    base, head = parents
    record.update({"base": base, "head": head, "tree_identical": tree_identical(repo, a.merge_sha, head)})
    try:
        inventory = pmr.ctest_inventory(build_dir)
        ours = pmr.artifact_identity(build_dir, inventory)
    except pmr.ReceiptError as exc:
        print(f"binary-identity shadow: no verdict, own identity unavailable: {exc}", file=sys.stderr)
        return 2
    record["our_binaries"] = len(ours["files"])
    receipt_path = Path(a.work_dir) / "head-receipt.json"
    Path(a.work_dir).mkdir(parents=True, exist_ok=True)
    # The per-test receipts instrument reuses these hashes instead of reading
    # every test binary a second time.
    (Path(a.work_dir) / "our-identity.json").write_text(json.dumps(ours, sort_keys=True), encoding="utf-8")
    artifact_name = f"protected-validation-macos-{head}-{base}"
    dl = subprocess.run([sys.executable, str(HERE.parent / "scripts" / "protected_merge_receipt.py"), "download",
                         "--repository", a.repository, "--target", "macos", "--token", a.token,
                         "--artifact-name", artifact_name, "--base-sha", base, "--head-sha", head,
                         "--output", str(receipt_path)], capture_output=True, text=True)
    if dl.returncode != 0:
        receipt_reason = (dl.stderr or dl.stdout).strip().splitlines()[-1:][0] if (dl.stderr or dl.stdout).strip() else "download failed"
        try:
            theirs, source_run = head_record_identity(a.repository, head, a.token)
        except Exception as exc:  # noqa: BLE001 - a failed lookup is reported, never trusted
            theirs, source_run = None, f"reuse record lookup failed: {pmr.describe_lookup_error(exc)}"
        if theirs is None:
            record.update({"verdict": "waiting_on_receipt", "artifact": artifact_name,
                           "reason": f"{receipt_reason}; {source_run}"})
            print(f"binary-identity shadow: waiting on receipt {artifact_name}: {record['reason']}")
            print(note(record))
            return 0
        record.update({"verdict": "compared", "source": "reuse-record", "source_run": source_run,
                       **compare(ours, theirs)})
        print(f"binary-identity shadow: tree_identical={record['tree_identical']} compared={record['compared']} "
              f"identical={record['identical']} differing={record['differing']} (head's reuse record)")
        print(note(record))
        return 0
    try:
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        theirs = receipt.get("artifact") or receipt.get("artifact_identity") or {}
        if not theirs.get("files"):
            raise ValueError("receipt carries no per-binary identity")
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        record.update({"verdict": "receipt_unreadable", "reason": str(exc)})
        print(note(record))
        return 0
    record.update({"verdict": "compared", "source": "receipt", **compare(ours, theirs)})
    print(f"binary-identity shadow: tree_identical={record['tree_identical']} compared={record['compared']} "
          f"identical={record['identical']} differing={record['differing']}")
    print(note(record))
    return 0


def _gh(repo: str, path: str) -> dict | list | None:
    proc = subprocess.run([os.environ.get("PULP_GH_CLI", "ghapp"), "api", path], capture_output=True, text=True)
    if proc.returncode != 0 or not proc.stdout.strip():
        return None
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError:
        return None


def cmd_report(a: argparse.Namespace) -> int:
    runs = (_gh(a.repository, f"repos/{a.repository}/actions/workflows/build.yml/runs?event=merge_group&per_page=100") or {}).get("workflow_runs", [])
    records = []
    for r in runs:
        jobs = (_gh(a.repository, f"repos/{a.repository}/actions/runs/{r['id']}/jobs?per_page=100") or {}).get("jobs", [])
        for j in jobs:
            if j.get("name") != "macos":
                continue
            for ann in _gh(a.repository, f"repos/{a.repository}/check-runs/{j['id']}/annotations") or []:
                if ann.get("title") == TITLE:
                    try:
                        records.append(json.loads(ann["message"]))
                    except (json.JSONDecodeError, KeyError):
                        pass
    compared = [x for x in records if x.get("verdict") == "compared" and x.get("tree_identical")]
    waiting = sum(1 for x in records if x.get("verdict") == "waiting_on_receipt")
    n = len(compared)
    if n < a.min_n:
        print(f"binary-identity: waiting on receipts, n={n} tree-identical comparisons of {a.min_n} needed "
              f"({waiting} groups found no receipt; {len(records)} annotations read)")
        return 0
    identical = sum(x["identical"] for x in compared)
    total = sum(x["compared"] for x in compared)
    print(f"binary-identity: n={n} tree-identical groups, identical {identical} / {total} binaries "
          f"({100.0 * identical / total:.2f}%); groups with any difference: {sum(1 for x in compared if x['differing'])}")
    examples = sorted({p for x in compared for p in x.get("differing_examples", [])})[:20]
    if examples:
        print("differing paths (examples): " + ", ".join(examples))
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("measure")
    m.add_argument("--build-dir", required=True)
    m.add_argument("--source-root", required=True)
    m.add_argument("--merge-sha", required=True)
    m.add_argument("--repository", required=True)
    m.add_argument("--token", default=os.environ.get("GITHUB_TOKEN", ""))
    m.add_argument("--work-dir", default=os.environ.get("RUNNER_TEMP", "/tmp"))
    m.set_defaults(func=cmd_measure)
    r = sub.add_parser("report")
    r.add_argument("--repository", required=True)
    r.add_argument("--min-n", type=int, default=20)
    r.set_defaults(func=cmd_report)
    a = ap.parse_args(argv[1:])
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
