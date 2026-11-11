#!/usr/bin/env python3
"""Check that the recorded evidence source is reachable and byte-identical."""

from __future__ import annotations

import hashlib
import json
import subprocess
from pathlib import Path


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
MANIFEST = HERE / "manifest.json"


def git(*args: str) -> str:
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True).strip()


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
source = manifest["tested_source_commit"]
head = git("rev-parse", "HEAD")
main = git("rev-parse", "origin/main")

for commit in (source, head, main):
    subprocess.run(["git", "cat-file", "-e", f"{commit}^{{commit}}"], cwd=ROOT, check=True)

source_is_ancestor_of_main = subprocess.run(
    ["git", "merge-base", "--is-ancestor", source, main], cwd=ROOT
).returncode == 0
source_is_ancestor_of_head = subprocess.run(
    ["git", "merge-base", "--is-ancestor", source, head], cwd=ROOT
).returncode == 0
if not source_is_ancestor_of_main or not source_is_ancestor_of_head:
    raise SystemExit("recorded source is not reachable from origin/main and the checked-out head")

for relative, expected in manifest["source_files"].items():
    current = ROOT / relative
    if digest(current.read_bytes()) != expected["sha256"]:
        raise SystemExit(f"working-tree digest mismatch: {relative}")
    recorded = subprocess.check_output(
        ["git", "show", f"{source}:{relative}"], cwd=ROOT
    )
    if digest(recorded) != expected["sha256"]:
        raise SystemExit(f"source commit digest mismatch: {relative}")

result = {
    "tested_source_commit": source,
    "origin_main": main,
    "source_is_ancestor_of_origin_main": source_is_ancestor_of_main,
    "source_is_ancestor_of_checked_out_head": source_is_ancestor_of_head,
    "source_files_match": True,
}
(HERE / "source-ancestry-check.json").write_text(
    json.dumps(result, indent=2) + "\n", encoding="utf-8"
)
print(json.dumps(result, sort_keys=True))
