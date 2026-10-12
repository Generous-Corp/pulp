#!/usr/bin/env python3
"""Compute the weekly PR-head selection receipt from live PR file lists."""
from __future__ import annotations
import argparse, json, subprocess
from pathlib import Path

KNOWN = {
    "wide-non-native-selftest", "sample-region-compat-baseline",
    "no-editor-plugin-link-floor-artifacts",
}

def gh(repo: str, path: str):
    p = subprocess.run(["gh", "api", f"repos/{repo}/{path}", "--paginate"], text=True,
                       capture_output=True, encoding="utf-8", check=True)
    return json.loads(p.stdout)

def selected(files: list[str]) -> set[str]:
    out = set()
    for path in files:
        if path.startswith("test/fixtures/no_editor_link_floor/"):
            out.add("no-editor-plugin-link-floor-artifacts")
        if path.startswith("tools/scripts/") or path.startswith("tools/ci/"):
            out.add("wide-non-native-selftest")
        if path.startswith("test/fixtures/sample-region-compat/") or path.startswith("test/sample_region_compat/") or path in {
            "core/host/include/pulp/host/custom_node_type.hpp",
            "core/host/include/pulp/host/signal_graph.hpp",
        }:
            out.add("sample-region-compat-baseline")
    return out & KNOWN

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args()
    prs = gh(args.repo, "pulls?state=closed&per_page=100&sort=updated&direction=desc")
    receipt = {}
    for pr in prs:
        files = gh(args.repo, f"pulls/{pr['number']}/files?per_page=100")
        names = sorted(selected([f["filename"] for f in files]))
        if names:
            receipt[str(pr["number"])] = names
    args.output.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0
if __name__ == "__main__":
    raise SystemExit(main())
