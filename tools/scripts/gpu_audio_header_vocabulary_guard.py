#!/usr/bin/env python3
"""Keep backend types out of the installed GPU-audio headers.

The node, transport, status, capability and program contracts under
core/gpu_audio/include are backend-neutral: a consumer must be able to compile
against them without any graphics backend. This guard fails when an installed
header names a WebGPU type (`wgpu...`, `WGPU...`), the Dawn namespace
(`dawn::`), a Metal type (`MTL...`, metal-cpp `MTL::`, `CAMetal...`), a
Vulkan type (`Vk...`), a Direct3D or DXGI interface (`ID3D...`, `IDXGI...`),
or includes a backend SDK header (Metal, QuartzCore, Vulkan, WebGPU, Dawn,
Direct3D, DXGI). Prose that mentions a backend by name is allowed;
identifiers and includes are not.

Exit status: 0 clean, 1 a header names a backend type, 2 nothing was scanned.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_INCLUDE = ROOT / "core" / "gpu_audio" / "include"
FORBIDDEN = re.compile(
    r"\b[wW][gG][pP][uU]\w*|\bdawn::"  # WebGPU / Dawn
    r"|\bMTL[A-Z]\w*|\bMTL::|\bCA::|\bCAMetal\w*|\b__bridge\b"  # Metal, metal-cpp
    r"|\bVk[A-Z]\w*"  # Vulkan types
    r"|\bID3D\w*|\bIDXGI\w*|\bIDirect3D\w*|\bD3D1[0-2]_\w*"  # Direct3D / DXGI
    # A backend SDK header included or imported by an installed header.
    r"|^\s*#\s*(?:include|import)\s*[<\"](?:Metal/|MetalKit/|QuartzCore/|vulkan/|webgpu/|"
    r"dawn/|d3d1[0-2]|dxgi)"
)


def strip_comments(text: str) -> str:
    """Drop // and /* */ comments so documentation may name a backend."""
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def scan(include_root: pathlib.Path) -> tuple[int, list[str]]:
    headers = sorted(p for p in include_root.rglob("*") if p.suffix in {".h", ".hpp"})
    hits = []
    for header in headers:
        code = strip_comments(header.read_text(encoding="utf-8"))
        for number, line in enumerate(code.splitlines(), start=1):
            match = FORBIDDEN.search(line)
            if match:
                hits.append(f"{header.relative_to(include_root)}:{number}: {match.group(0)}")
    return len(headers), hits


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--include-root", type=pathlib.Path, default=DEFAULT_INCLUDE)
    args = parser.parse_args(argv)
    count, hits = scan(args.include_root)
    if count == 0:
        print(f"gpu-audio header vocabulary: no headers under {args.include_root}", file=sys.stderr)
        return 2
    if hits:
        print("gpu-audio header vocabulary: backend types in installed headers:", file=sys.stderr)
        for hit in hits:
            print(f"  {hit}", file=sys.stderr)
        return 1
    print(f"gpu-audio header vocabulary: {count} headers backend-neutral")
    return 0


if __name__ == "__main__":
    sys.exit(main())
