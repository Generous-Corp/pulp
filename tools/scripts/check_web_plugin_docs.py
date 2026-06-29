#!/usr/bin/env python3
"""Lint the web-plugin support docs against the actual tree.

docs/reference/web-plugin-support.md (and the web-plugins guide) make concrete
claims — "build with pulp_add_wam_plugin", "the runtime lives in
core/format/src/wasm/wam-runtime.mjs", "the browser fixture is at
examples/web-demos/wasm-build/browser-test/". This lint fails if any
backtick-quoted repo path in those docs does not exist, or if a named build
helper is not actually defined, so the docs cannot drift into claiming a web
capability the repo does not have.

Usage: python3 tools/scripts/check_web_plugin_docs.py [--root <repo>]
Exit 0 on success, 1 on a false claim.
"""
from __future__ import annotations
import argparse
import re
import sys
from pathlib import Path

# Docs whose backtick repo-paths must resolve.
DOCS = [
    "docs/reference/web-plugin-support.md",
    "docs/guides/web-plugins.md",
]

# Build helpers that must be defined where the docs say (helper -> file).
REQUIRED_HELPERS = {
    "pulp_add_wam_plugin": "tools/cmake/PulpWam.cmake",
}

# A backtick span looks like a repo path if it contains a slash and a known
# source/dir token. We only validate spans that clearly name a tree path, so
# prose like `wam_init` or `WamEnv` is ignored.
PATH_SPAN = re.compile(r"`([^`]+)`")
LOOKS_LIKE_PATH = re.compile(
    r"^(core|tools|examples|docs|test|ci|ship|apple)/[\w./*-]+$"
)


def repo_paths_in(text: str) -> list[str]:
    out = []
    for span in PATH_SPAN.findall(text):
        span = span.strip().rstrip("/")
        if LOOKS_LIKE_PATH.match(span) and "*" not in span:
            out.append(span)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None)
    args = ap.parse_args()
    root = Path(args.root) if args.root else Path(__file__).resolve().parents[2]

    errors: list[str] = []

    for doc in DOCS:
        doc_path = root / doc
        if not doc_path.exists():
            errors.append(f"missing doc: {doc}")
            continue
        text = doc_path.read_text()
        for ref in repo_paths_in(text):
            if not (root / ref).exists():
                errors.append(f"{doc}: references nonexistent path `{ref}`")

    for helper, file in REQUIRED_HELPERS.items():
        fp = root / file
        if not fp.exists():
            errors.append(f"helper file missing: {file} (for `{helper}`)")
        elif helper not in fp.read_text():
            errors.append(f"`{helper}` not defined in {file}")

    if errors:
        print("check_web_plugin_docs: FAIL")
        for e in errors:
            print(f"  - {e}")
        return 1
    print("check_web_plugin_docs: OK — web-plugin docs match the tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
