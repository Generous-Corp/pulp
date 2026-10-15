#!/usr/bin/env python3
"""Keep the import observer on Pulp's narrow render observer boundary.

The importer tool may use Pulp's render abstractions, but provider headers and
opaque Dawn handles belong in the render implementation. These negative
controls fail if the tool starts reaching into Skia/Dawn/WebGPU again.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

FORBIDDEN = (
    "include/core/SkMilestone.h",
    "dawn/dawn_version.h",
    "webgpu/webgpu_cpp.h",
    "webgpu/webgpu.h",
    "wgpu::",
    "dawn_device_handle",
    "dawn_instance_handle",
    "dawn_queue_handle",
    "static_cast<wgpu",
)
REQUIRED = (
    "pulp/render/render_observer.hpp",
    "render_observer::inject_device_loss",
    "render_observer::device_lost",
    "render_observer::toolchain_versions",
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    errors = [f"forbidden provider reach-through: {token}" for token in FORBIDDEN if token in source]
    errors.extend(f"missing observer API use: {token}" for token in REQUIRED if token not in source)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("design-ir-observe provider boundary: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
