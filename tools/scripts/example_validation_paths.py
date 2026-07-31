#!/usr/bin/env python3
"""Classify paths that can affect example configuration or compilation."""

from __future__ import annotations

import sys


PREFIXES = (
    "examples/",
    "external/",
    "core/format/",
    "tools/cmake/",
    "tools/deps/",
    ".github/actions/install-linux-build-deps/",
)

EXACT = {
    ".github/workflows/examples-validation.yml",
    "setup.sh",
    "test/au_bundle_lifecycle.cpp",
    "test/clap_bundle_lifecycle.cpp",
    "test/vst3_bundle_lifecycle.cpp",
    "tools/ci/governed-build.sh",
    "tools/ci/install_linux_build_deps.py",
    "tools/ci/linux_build_deps.json",
    "tools/ci/lib/auval-exec-check.sh",
    "tools/ci/run-auval-component.sh",
    "tools/scripts/example_validation_paths.py",
    "tools/scripts/fetch_skia_for_release.py",
}


def affects_examples(path: str) -> bool:
    normalized = path.strip().removeprefix("./")
    return (
        normalized in EXACT
        or normalized.startswith(PREFIXES)
        or "/include/" in normalized
        or normalized.endswith(".cmake")
        or normalized.endswith("CMakeLists.txt")
    )


def main() -> int:
    # Consume the whole stream so a match cannot close `git diff`'s pipe early
    # under a caller's `set -o pipefail`.
    matched = False
    for line in sys.stdin:
        matched = affects_examples(line) or matched
    return 0 if matched else 10


if __name__ == "__main__":
    raise SystemExit(main())
