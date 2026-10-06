#!/usr/bin/env python3
import argparse
import sys
from pathlib import Path


FORBIDDEN_TOKENS = {
    "pulp_view": "view module",
    "pulp-view": "view module",
    "libpulp-view": "view module",
    "widget_bridge": "runtime WebGPU bridge",
    "web-compat": "web-compat JS asset",
    "three.webgpu": "Three.js runtime asset",
    "three.core": "Three.js runtime asset",
    "three.iife": "Three.js runtime asset",
    "threejs-native-demo": "live Three.js demo",
    "libnode": "V8/Node runtime",
    "v8_": "V8 runtime",
    "quickjs": "QuickJS runtime",
    "javascriptcore": "JavaScriptCore runtime",
    "JavaScriptCore.framework": "JavaScriptCore runtime",
}

REQUIRED_TOKENS = {
    "core/scene/CMakeFiles/pulp-scene3d-inspect.dir/link.txt": {
        "libpulp-scene": "Scene3D CPU module",
        "libfastgltf": "native glTF parser",
        "libsimdjson": "fastgltf JSON backend",
    },
    "core/scene/CMakeFiles/pulp-scene3d-sidecar.dir/link.txt": {
        "libpulp-scene": "Scene3D CPU module",
        "libfastgltf": "native glTF parser",
        "libsimdjson": "fastgltf JSON backend",
    },
    "core/scene/CMakeFiles/pulp-scene3d-bake-preflight.dir/link.txt": {
        "libpulp-scene": "Scene3D CPU module",
        "libfastgltf": "native glTF parser",
        "libsimdjson": "fastgltf JSON backend",
    },
    "core/render/CMakeFiles/pulp-renderer3d-probe.dir/link.txt": {
        "libpulp-render": "native render module",
        "libpulp-scene": "Scene3D CPU module",
        "libfastgltf": "native glTF parser",
        "libsimdjson": "fastgltf JSON backend",
        "libwgpu_native": "native WebGPU runtime",
        "libskia": "native image/decode/render support",
    },
    "core/render/CMakeFiles/pulp-scene3d-inspect-native.dir/link.txt": {
        "libpulp-render": "native render module",
        "libpulp-scene": "Scene3D CPU module",
        "libfastgltf": "native glTF parser",
        "libsimdjson": "fastgltf JSON backend",
        "libwgpu_native": "native WebGPU runtime",
        "libskia": "native image/decode/render support",
    },
    "test/CMakeFiles/pulp-test-scene3d.dir/link.txt": {
        "libpulp-scene": "Scene3D CPU module",
        "libfastgltf": "native glTF parser",
        "libsimdjson": "fastgltf JSON backend",
    },
    "test/CMakeFiles/pulp-test-renderer3d.dir/link.txt": {
        "libpulp-render": "native render module",
        "libpulp-scene": "Scene3D CPU module",
        "libfastgltf": "native glTF parser",
        "libsimdjson": "fastgltf JSON backend",
        "libwgpu_native": "native WebGPU runtime",
        "libskia": "native image/decode/render support",
    },
}


# Spellings a required token may take on a link line. The `webgpu` target is
# IMPORTED_NO_SONAME on Linux (wgpu-native's .so has no SONAME), so CMake links
# it as `-L<dir> -lwgpu_native` there instead of naming the library's path.
REQUIRED_TOKEN_SPELLINGS = {
    "libwgpu_native": ("libwgpu_native", "-lwgpu_native"),
}


def default_link_files(build_dir: Path):
    return [
        Path("core/scene/CMakeFiles/pulp-scene3d-inspect.dir/link.txt"),
        Path("core/scene/CMakeFiles/pulp-scene3d-sidecar.dir/link.txt"),
        Path("core/scene/CMakeFiles/pulp-scene3d-bake-preflight.dir/link.txt"),
        Path("core/render/CMakeFiles/pulp-renderer3d-probe.dir/link.txt"),
        Path("core/render/CMakeFiles/pulp-scene3d-inspect-native.dir/link.txt"),
        Path("test/CMakeFiles/pulp-test-scene3d.dir/link.txt"),
        Path("test/CMakeFiles/pulp-test-renderer3d.dir/link.txt"),
    ]


def read_link_file(path: Path):
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        raise RuntimeError(f"{path}: {exc}") from exc


def scan_forbidden(path: Path, text: str):
    lowered = text.lower()
    hits = []
    for token, reason in FORBIDDEN_TOKENS.items():
        if token.lower() in lowered:
            hits.append((token, reason))
    return hits


def required_tokens_for(path: Path, build_dir: Path):
    try:
        relative = path.relative_to(build_dir)
    except ValueError:
        return {}
    return REQUIRED_TOKENS.get(relative.as_posix(), {})


def scan_required(path: Path, text: str, build_dir: Path):
    required = required_tokens_for(path, build_dir)
    if not required:
        return []
    lowered = text.lower()
    misses = []
    for token, reason in required.items():
        spellings = REQUIRED_TOKEN_SPELLINGS.get(token, (token,))
        if not any(spelling.lower() in lowered for spelling in spellings):
            misses.append((token, reason))
    return misses


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Verify native Scene3D/Renderer3D targets do not link live "
            "JS/view surfaces."))
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument(
        "--link-file",
        action="append",
        type=Path,
        help="Additional or replacement link.txt path. May be repeated.")
    args = parser.parse_args()

    link_files = args.link_file or default_link_files(args.build_dir)
    errors = []
    checked = []
    for path in link_files:
        if not path.is_absolute():
            path = args.build_dir / path
        try:
            text = read_link_file(path)
        except RuntimeError as exc:
            errors.append(str(exc))
            continue
        checked.append(path)
        for token, reason in scan_forbidden(path, text):
            errors.append(f"{path}: forbidden {reason} token {token!r}")
        for token, reason in scan_required(path, text, args.build_dir):
            errors.append(f"{path}: missing required {reason} token {token!r}")

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    relative = []
    for path in checked:
        try:
            relative.append(str(path.relative_to(args.build_dir)))
        except ValueError:
            relative.append(str(path))
    print("native_renderer_boundary_verified="
          f"{len(checked)} link files: {', '.join(relative)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
