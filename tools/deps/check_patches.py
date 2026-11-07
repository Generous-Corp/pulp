#!/usr/bin/env python3
"""Verify that vendored dependencies differ from upstream only by recorded patches.

``tools/deps/patches.json`` names, for each dependency carried in the tree, the
upstream ref, the files that are touched, the SHA-256 each file has at that
upstream ref, and the ordered patch series applied on top.

For every in-tree dependency this script:

1. copies the touched files into a scratch directory,
2. reverse-applies the patch series from the last patch to the first, and
3. requires each file to match its recorded upstream SHA-256.

It fails on the first patch that does not reverse cleanly. It also fails when
the reversed files do not match upstream, which means someone edited a vendored
file without recording a patch. Patch files that touch a file outside the
dependency's ``files`` list, patch files missing from disk, and patch files on
disk that no manifest entry lists are errors too.

Usage:
    python3 tools/deps/check_patches.py                # check the working tree
    python3 tools/deps/check_patches.py --verify-upstream
        # also download each file at its upstream commit and compare hashes

Exit status: 0 when every dependency verifies, 1 otherwise, 2 on a usage or
manifest error.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = REPO_ROOT / "tools" / "deps" / "patches.json"
PATCH_DIR = Path("tools") / "deps" / "patches"

_PLUS_LINE = re.compile(r"^\+\+\+ (?:b/)?(\S+)", re.MULTILINE)


class ManifestError(Exception):
    pass


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def normalize_eol(data: bytes) -> bytes:
    """Fold CRLF to LF.

    A Windows checkout may convert line endings. Upstream hashes and patch
    files are recorded with LF, so both sides are compared after this fold.
    """
    return data.replace(b"\r\n", b"\n")


def load_manifest(path: Path) -> dict:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ManifestError(f"cannot read {path}: {exc}") from exc
    if data.get("schema_version") != 1 or not isinstance(data.get("dependencies"), list):
        raise ManifestError(f"{path}: expected schema_version 1 and a dependencies list")
    names = [d.get("name") for d in data["dependencies"]]
    if len(set(names)) != len(names) or not all(names):
        raise ManifestError(f"{path}: dependency names must be present and unique")
    return data


def patch_targets(patch_text: str) -> set[str]:
    return set(_PLUS_LINE.findall(patch_text))


def _reverse_apply(scratch: Path, patch: Path) -> str | None:
    """Reverse-apply one patch in scratch. Returns an error text or None."""
    local = scratch.parent / f"{scratch.name}.{patch.name}"
    local.write_bytes(normalize_eol(patch.read_bytes()))
    # Stop git from finding a repository above the scratch directory (for
    # example when TMPDIR sits inside a checkout): it would then resolve the
    # patch paths against that repository and skip files outside this directory.
    env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(scratch.parent))
    try:
        proc = subprocess.run(
            ["git", "apply", "-R", "-p1", "--whitespace=nowarn", str(local)],
            cwd=scratch, capture_output=True, text=True, check=False, env=env,
        )
    finally:
        local.unlink(missing_ok=True)
    if proc.returncode != 0:
        return (proc.stderr or proc.stdout).strip() or "git apply failed"
    return None


def check_dependency(dep: dict, repo_root: Path) -> list[str]:
    """Return a list of failure messages for one dependency (empty when ok)."""
    name = dep["name"]
    if not dep.get("in_tree", True):
        problems = []
        for key in ("fork_repo", "fork_ref", "fork_commit", "upstream_repo", "upstream_ref"):
            if not dep.get(key):
                problems.append(f"{name}: fetched dependency is missing '{key}'")
        if not dep.get("carried_commits"):
            problems.append(f"{name}: fetched fork must list its carried_commits")
        ctp = dep.get("configure_time_patches")
        if ctp:
            cmake = repo_root / ctp.get("file", "")
            if not cmake.is_file():
                problems.append(f"{name}: configure-time patch file {ctp.get('file')} does not exist")
            else:
                text = cmake.read_text(encoding="utf-8", errors="replace")
                for fn in ctp.get("functions", []):
                    if not re.search(r"^function\(\s*" + re.escape(fn) + r"\b", text, re.MULTILINE):
                        problems.append(f"{name}: {ctp['file']} no longer defines {fn}()")
        return problems

    problems: list[str] = []
    files = dep.get("files") or []
    patches = dep.get("patches") or []
    allowed = {f["path"] for f in files}
    if not files:
        return [f"{name}: in-tree dependency lists no files"]

    # Every patch must exist and touch only listed files.
    patch_paths = []
    for rel in patches:
        p = repo_root / rel
        if not p.is_file():
            problems.append(f"{name}: patch file {rel} does not exist")
            continue
        outside = patch_targets(p.read_text(encoding="utf-8", errors="replace")) - allowed
        if outside:
            problems.append(f"{name}: {rel} touches files not listed for {name}: {sorted(outside)}")
        patch_paths.append(p)
    if problems:
        return problems

    scratch = Path(tempfile.mkdtemp(prefix="pulp-check-patches-"))
    try:
        for f in files:
            src = repo_root / f["path"]
            if not src.is_file():
                return [f"{name}: vendored file {f['path']} is missing"]
            dst = scratch / f["path"]
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(normalize_eol(src.read_bytes()))
        for rel, p in reversed(list(zip(patches, patch_paths))):
            err = _reverse_apply(scratch, p)
            if err:
                return [f"{name}: patch {rel} does not reverse cleanly against the tree "
                        f"(the file was edited outside the recorded series): {err}"]
        for f in files:
            got = sha256_bytes((scratch / f["path"]).read_bytes())
            if got != f["upstream_sha256"]:
                problems.append(
                    f"{name}: {f['path']} does not match upstream {dep.get('upstream_ref')} "
                    f"after reversing {len(patches)} patch(es): expected {f['upstream_sha256']}, "
                    f"got {got}. Record the edit as a patch in tools/deps/patches.json.")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    return problems


def check_orphans(manifest: dict, repo_root: Path) -> list[str]:
    listed = {rel for d in manifest["dependencies"] for rel in d.get("patches", [])}
    root = repo_root / PATCH_DIR
    if not root.is_dir():
        return []
    on_disk = {p.relative_to(repo_root).as_posix() for p in root.rglob("*.patch")}
    return [f"patch file {rel} is not listed in patches.json" for rel in sorted(on_disk - listed)]


def verify_upstream(dep: dict) -> list[str]:
    """Download each file at the upstream commit and compare its hash."""
    problems = []
    repo = dep.get("upstream_repo", "")
    commit = dep.get("upstream_commit")
    m = re.match(r"https://github\.com/([^/]+/[^/.]+)(?:\.git)?$", repo)
    if not dep.get("in_tree", True) or not m or not commit:
        return problems
    for f in dep["files"]:
        url = f"https://raw.githubusercontent.com/{m.group(1)}/{commit}/{f['upstream_path']}"
        try:
            with urllib.request.urlopen(url, timeout=60) as resp:
                got = sha256_bytes(resp.read())
        except OSError as exc:
            problems.append(f"{dep['name']}: could not fetch {url}: {exc}")
            continue
        if got != f["upstream_sha256"]:
            problems.append(f"{dep['name']}: recorded upstream_sha256 for {f['path']} does not "
                            f"match {url} ({got})")
    return problems


def cross_check_pins(manifest: dict, repo_root: Path) -> list[str]:
    """A recorded commit must agree with tools/deps/manifest.json when that row has one."""
    path = repo_root / "tools" / "deps" / "manifest.json"
    if not path.is_file():
        return []
    rows = {r["name"]: r for r in json.loads(path.read_text(encoding="utf-8"))["dependencies"]}
    problems = []
    for dep in manifest["dependencies"]:
        row = rows.get(dep["name"])
        if row is None:
            problems.append(f"{dep['name']}: no row in tools/deps/manifest.json")
            continue
        pinned = (row.get("upstream") or {}).get("commit")
        want = dep.get("upstream_commit") if dep.get("in_tree", True) else dep.get("fork_commit")
        if pinned and want and pinned != want:
            problems.append(f"{dep['name']}: patches.json records {want} but "
                            f"tools/deps/manifest.json pins {pinned}")
    return problems


def run(manifest_path: Path, repo_root: Path, upstream: bool) -> int:
    try:
        manifest = load_manifest(manifest_path)
    except ManifestError as exc:
        print(f"check_patches: {exc}", file=sys.stderr)
        return 2
    failures = check_orphans(manifest, repo_root) + cross_check_pins(manifest, repo_root)
    for dep in manifest["dependencies"]:
        errs = check_dependency(dep, repo_root)
        if upstream and not errs:
            errs = verify_upstream(dep)
        if errs:
            failures.extend(errs)
        else:
            kind = "in tree" if dep.get("in_tree", True) else "fetched fork"
            print(f"check_patches: {dep['name']}: ok ({kind}, {len(dep.get('patches', []))} patch(es))")
    for msg in failures:
        print(f"check_patches: FAIL {msg}", file=sys.stderr)
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    ap.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    ap.add_argument("--verify-upstream", action="store_true",
                    help="download each file at its upstream commit and compare hashes")
    args = ap.parse_args(argv)
    return run(args.manifest.resolve(), args.repo_root.resolve(), args.verify_upstream)


if __name__ == "__main__":
    sys.exit(main())
