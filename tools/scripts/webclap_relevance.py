#!/usr/bin/env python3
"""Return success when a changed path can affect the WebCLAP proof lane."""

from __future__ import annotations

import sys


RELEVANT_PREFIXES = (
    ".github/actions/",
    "cmake/",
    "core/",
    "examples/pulp-gain/",
    "examples/pulp-pluck/",
    "examples/super-convolver/",
    "examples/web-demos/",
    "external/",
    "packages/pulp-web-player/",
    "tools/cmake/",
    "tools/deps/",
)

RELEVANT_FILES = {
    ".github/workflows/wclap-cloudflare.yml",
    "CMakeLists.txt",
    "package.json",
    "package-lock.json",
    "tools/scripts/webclap_relevance.py",
    "tools/scripts/test_webclap_relevance.py",
}


def is_relevant(path: str) -> bool:
    normalized = path.strip()
    if normalized.startswith("./"):
        normalized = normalized[2:]
    return normalized in RELEVANT_FILES or normalized.startswith(RELEVANT_PREFIXES)


def main() -> int:
    return 0 if any(is_relevant(line) for line in sys.stdin) else 1


if __name__ == "__main__":
    raise SystemExit(main())
