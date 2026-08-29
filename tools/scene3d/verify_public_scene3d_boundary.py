#!/usr/bin/env python3
import argparse
import re
import sys
from pathlib import Path


FORBIDDEN_PATTERNS = [
    (re.compile(r"#\s*include\s*[<\"][^>\"]*(fastgltf|cgltf|tinygltf)[^>\"]*[>\"]"),
     "glTF parser dependency include"),
    (re.compile(r"\b(fastgltf|cgltf|tinygltf)::"),
     "glTF parser namespace"),
    (re.compile(r"#\s*include\s*[<\"][^>\"]*(webgpu|dawn|wgpu)[^>\"]*[>\"]"),
     "Dawn/WebGPU include"),
    (re.compile(r"\b(wgpu::|WGPU[A-Za-z_]*|dawn::)"),
     "Dawn/WebGPU type"),
    (re.compile(r"#\s*include\s*[<\"][^>\"]*(Sk[A-Za-z_]*|skia)[^>\"]*[>\"]"),
     "Skia include"),
    (re.compile(r"\bSk[A-Z][A-Za-z_]*"),
     "Skia type"),
    (re.compile(r"#\s*include\s*[<\"][^>\"]*pulp/view/[^>\"]*[>\"]"),
     "view module include"),
    (re.compile(r"\bpulp::view\b"),
     "view module namespace"),
    (re.compile(r"\b(v8::|quickjs|qjs_|JSContextRef|JSGlobalContextRef|JavaScriptCore)\b"),
     "JS engine type"),
    (re.compile(r"\b(three\.|threejs|web-compat|widget_bridge)\b"),
     "live Three.js/web-compat surface"),
]

EXPECTED_PUBLIC_HEADERS = [
    "core/scene/include/pulp/scene/bake_preflight.hpp",
    "core/scene/include/pulp/scene/draco_scene_adapter.hpp",
    "core/scene/include/pulp/scene/gltf_loader.hpp",
    "core/scene/include/pulp/scene/material_key.hpp",
    "core/scene/include/pulp/scene/render_packet.hpp",
    "core/scene/include/pulp/scene/renderer3d_characterization.hpp",
    "core/scene/include/pulp/scene/scene_data.hpp",
    "core/scene/include/pulp/scene/scene_graph.hpp",
    "core/scene/include/pulp/scene/scene_stats.hpp",
    "core/scene/include/pulp/scene/sidecar.hpp",
    "core/render/include/pulp/render/renderer3d.hpp",
]


def strip_comments(text: str):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//.*", "", text)


def default_headers(repo_root: Path):
    return [repo_root / relative for relative in EXPECTED_PUBLIC_HEADERS]


def verify_default_header_set(repo_root: Path):
    expected = {repo_root / relative for relative in EXPECTED_PUBLIC_HEADERS}
    discovered = set((repo_root / "core/scene/include/pulp/scene").glob("*.hpp"))
    discovered.update([
        repo_root / "core/render/include/pulp/render/renderer3d.hpp",
    ])

    errors = []
    for path in sorted(expected - discovered):
        errors.append(f"expected public Scene3D header missing: {path}")
    for path in sorted(discovered - expected):
        errors.append(f"unexpected public Scene3D header: {path}")
    return errors


def main():
    parser = argparse.ArgumentParser(
        description="Verify public Scene3D headers keep parser/GPU/JS implementation types private.")
    parser.add_argument("--repo-root", type=Path, default=Path("."))
    parser.add_argument("--header", action="append", type=Path,
                        help="Header to scan. Defaults to core/scene public headers and renderer3d.hpp.")
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    headers = args.header or default_headers(repo_root)

    errors = [] if args.header else verify_default_header_set(repo_root)
    checked = []
    for header in headers:
        path = header if header.is_absolute() else repo_root / header
        try:
            text = strip_comments(path.read_text(encoding="utf-8"))
        except OSError as exc:
            errors.append(f"{path}: {exc}")
            continue

        checked.append(path)
        for pattern, reason in FORBIDDEN_PATTERNS:
            match = pattern.search(text)
            if match:
                errors.append(
                    f"{path}: forbidden {reason}: {match.group(0)!r}")

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print(f"public_scene3d_boundary_verified={len(checked)} headers")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
