#!/usr/bin/env python3
"""Emit release-cli.yml's build/smoke matrix legs, filtered by active_platforms.

release-cli.yml's ``build-cli`` and ``smoke-cli`` jobs take their
``matrix.include`` from this script (via ``fromJSON`` of a
``resolve-macos-runner`` output) instead of a hardcoded YAML list. That makes
the set of legs a RELEASE builds derive from the same field —
``release_product_matrix.json``'s ``active_platforms`` — that the publish-time
exact-asset verifier derives its contract from, so the legs that build and the
assets the finalizer demands can never disagree. Shipping a platform (or
pausing one) is a one-line edit to that JSON field.

The per-leg configuration below is workflow plumbing, not shipping policy:
which runner image, which artifact extension, which container. Notes that used
to live next to the YAML include entries:

* ``darwin-x64`` is CROSS-COMPILED on the healthy Apple-Silicon runner, never
  the flaky native ``macos-15-intel`` image (which CPU-pegs and never shipped
  an artifact — nightly-intel.yml's universal cross-check exists precisely
  because the native lane is unreliable). The ``macos-15-xcompile`` sentinel
  routes through resolve_release_runners.py's map to the dedicated release
  Tart pool. When active, it is held to the same reliability class as
  ``darwin-arm64``; whether it is active is decided by ``active_platforms``
  (see docs/guides/intel-support.md and the subset staleness check).
* ``linux-x64`` builds inside an ``ubuntu:22.04`` container so Pulp's own
  objects don't re-leak the runner's glibc ~2.39 — matching the portable
  Skia/Dawn/V8 deps' 2.34 floor. The container applies to the BUILD leg only;
  the smoke leg deliberately runs on the bare runner image, proving the
  artifact works outside the container that built it.

The runner-label→actual-runner routing stays in resolve_release_runners.py;
each leg's ``runs-on`` indexes that map by platform, unchanged.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from release_artifact_contents import DEFAULT_MATRIX_PATH, ProductMatrix

# Per-platform leg configuration, keyed by the platform names used in
# release_product_matrix.json. Dict order is the canonical leg order.
BUILD_LEGS: dict[str, dict[str, str]] = {
    "darwin-arm64": {"os": "macos-15", "artifact": "pulp"},
    "darwin-x64": {"os": "macos-15-xcompile", "artifact": "pulp"},
    "linux-x64": {
        "os": "ubuntu-24.04",
        "artifact": "pulp",
        "container": "ubuntu:22.04",
    },
    "linux-arm64": {"os": "ubuntu-24.04-arm", "artifact": "pulp"},
    "windows-x64": {"os": "windows-latest", "artifact": "pulp.exe"},
    "windows-arm64": {"os": "windows-11-arm", "artifact": "pulp.exe"},
}


class MatrixLegError(RuntimeError):
    pass


def _legs(
    matrix: ProductMatrix, *, smoke: bool
) -> list[dict[str, str]]:
    missing = set(matrix.platforms) - set(BUILD_LEGS)
    if missing:
        raise MatrixLegError(
            "release platform(s) with no matrix leg configuration: "
            f"{sorted(missing)} — add them to BUILD_LEGS in {__file__}"
        )
    unknown = set(BUILD_LEGS) - set(matrix.platforms)
    if unknown:
        raise MatrixLegError(
            "matrix leg(s) for platforms outside the release inventory: "
            f"{sorted(unknown)}"
        )
    active = set(matrix.active_platforms)
    legs = []
    for platform, config in BUILD_LEGS.items():
        if platform not in active:
            continue
        leg = {"platform": platform, **config}
        if smoke:
            # The smoke job re-runs each artifact on a fresh runner; the
            # linux-x64 build container must NOT leak into it (see module
            # docstring).
            leg.pop("container", None)
        legs.append(leg)
    return legs


def build_include(matrix: ProductMatrix) -> list[dict[str, str]]:
    return _legs(matrix, smoke=False)


def smoke_include(matrix: ProductMatrix) -> list[dict[str, str]]:
    return _legs(matrix, smoke=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--matrix", type=Path, default=DEFAULT_MATRIX_PATH)
    parser.add_argument(
        "--github-output",
        action="store_true",
        help="Print key=value lines for $GITHUB_OUTPUT",
    )
    args = parser.parse_args(argv)
    matrix = ProductMatrix.load(args.matrix)
    build = build_include(matrix)
    smoke = smoke_include(matrix)
    active = list(matrix.active_platforms)
    if args.github_output:
        print(f"active_platforms={json.dumps(active)}")
        print(f"build_include={json.dumps(build)}")
        print(f"smoke_include={json.dumps(smoke)}")
    else:
        print(
            json.dumps(
                {
                    "active_platforms": active,
                    "build_include": build,
                    "smoke_include": smoke,
                },
                indent=2,
            )
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
