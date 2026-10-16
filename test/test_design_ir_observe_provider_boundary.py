#!/usr/bin/env python3
"""Keep the import observer on Pulp's narrow render observer boundary.

The importer tool may use Pulp's render abstractions, but provider headers and
opaque Dawn handles belong in the render implementation. These negative
controls fail if the tool starts reaching into Skia/Dawn/WebGPU again.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

INCLUDES = re.compile(r'^\s*#\s*(?:include|import)\s*[<"]([^>"]+)[>"]', re.MULTILINE)
PROVIDER_INCLUDE = re.compile(r'(^|/)(dawn|webgpu|skia)/|^(include|modules|src)/')
FORBIDDEN = (
    "wgpu::",
    "dawn::",
    "skgpu::",
    "dawn_device_handle",
    "dawn_instance_handle",
    "dawn_queue_handle",
    "static_cast<wgpu",
)
REQUIRED = (
    '"render_observer_private.hpp"',
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
    errors.extend(f"forbidden provider include: {header}" for header in INCLUDES.findall(source)
                  if PROVIDER_INCLUDE.search(header))
    errors.extend(f"missing observer API use: {token}" for token in REQUIRED if token not in source)
    repository_root = args.source.resolve().parents[2]
    for include_dir in (repository_root / "core").glob("*/include"):
        for header in include_dir.rglob("*"):
            if header.suffix not in {".h", ".hpp"} or not header.is_file():
                continue
            contents = header.read_text(encoding="utf-8")
            if (header.name in {"render_observer.hpp", "render_observer_private.hpp"}
                    or "RenderToolchainVersions" in contents or "namespace render_observer" in contents):
                errors.append(f"observer seam must stay private, found exported header: {header}")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("design-ir-observe provider boundary: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
