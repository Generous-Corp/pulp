#!/usr/bin/env python3
"""Verifies the public Scene3D boundary checker rejects implementation leaks."""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


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


LEAK_CASES = [
    ("parser-include", '#include <fastgltf/core.hpp>\n', "glTF parser dependency include"),
    ("parser-namespace", "fastgltf::Asset* parser_asset;\n", "glTF parser namespace"),
    ("webgpu-include", "#include <webgpu/webgpu_cpp.h>\n", "Dawn/WebGPU include"),
    ("webgpu-type", "wgpu::Device device;\n", "Dawn/WebGPU type"),
    ("skia-include", "#include <skia/core/SkImage.h>\n", "Skia include"),
    ("skia-type", "SkImage* image;\n", "Skia type"),
    ("view-include", "#include <pulp/view/widget_bridge.hpp>\n", "view module include"),
    ("view-namespace", "pulp::view::WidgetBridge* bridge;\n", "view module namespace"),
    ("js-engine", "v8::Isolate* isolate;\n", "JS engine type"),
    ("live-threejs", "const char* marker = \"web-compat widget_bridge three.js\";\n", "live Three.js/web-compat surface"),
]


def read_text(path):
    return path.read_text(encoding="utf-8")


def write_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def run_verifier(verifier, *args):
    return subprocess.run(
        [sys.executable, str(verifier), *[str(arg) for arg in args]],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def copy_expected_headers(repo_root, tmp_root):
    for relative in EXPECTED_PUBLIC_HEADERS:
        src = repo_root / relative
        dst = tmp_root / relative
        write_text(dst, read_text(src))


def expect_case(name, result, expected_code, expected_text, errors):
    sys.stdout.write(result.stdout)
    if result.returncode != expected_code:
        errors.append(
            f"{name}: expected exit {expected_code}, got {result.returncode}"
        )
    if expected_text not in result.stdout:
        errors.append(
            f"{name}: expected diagnostic containing {expected_text!r}"
        )
    print(f"public_scene3d_boundary_contract_case={name}")


def run_leak_case(verifier, base_header, tmpdir, name, leak_text, expected_text):
    header = Path(tmpdir) / f"{name}.hpp"
    write_text(header, read_text(base_header) + "\n" + leak_text)
    return run_verifier(verifier, "--repo-root", Path(tmpdir), "--header", header), expected_text


def main():
    parser = argparse.ArgumentParser(
        description="Verify public Scene3D boundary checker rejects drift.")
    parser.add_argument("--boundary-verifier", type=Path, required=True)
    parser.add_argument("--repo-root", type=Path, required=True)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    for label, path in (
            ("boundary_verifier", args.boundary_verifier),
            ("repo_root", repo_root)):
        if not path.exists():
            print(f"{label}_exists=false path={path}")
            return 2

    errors = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp_root = Path(tmp)
        copy_expected_headers(repo_root, tmp_root)

        expect_case(
            "valid-current-boundary",
            run_verifier(args.boundary_verifier, "--repo-root", repo_root),
            0,
            "public_scene3d_boundary_verified=11 headers",
            errors,
        )

        missing_root = tmp_root / "missing"
        shutil.copytree(tmp_root, missing_root)
        (missing_root / EXPECTED_PUBLIC_HEADERS[0]).unlink()
        expect_case(
            "missing-public-header",
            run_verifier(args.boundary_verifier, "--repo-root", missing_root),
            1,
            "expected public Scene3D header missing",
            errors,
        )

        extra_root = tmp_root / "extra"
        shutil.copytree(tmp_root, extra_root)
        write_text(
            extra_root / "core/scene/include/pulp/scene/extra_public.hpp",
            "#pragma once\n",
        )
        expect_case(
            "extra-public-header",
            run_verifier(args.boundary_verifier, "--repo-root", extra_root),
            1,
            "unexpected public Scene3D header",
            errors,
        )

        base_header = repo_root / "core/scene/include/pulp/scene/scene_data.hpp"
        for name, leak_text, expected_text in LEAK_CASES:
            result, expected = run_leak_case(
                args.boundary_verifier,
                base_header,
                tmp,
                name,
                leak_text,
                expected_text,
            )
            expect_case(name, result, 1, expected, errors)

    if errors:
        for error in errors:
            print(f"error={error}", file=sys.stderr)
        return 1

    print("public_scene3d_boundary_contract_verified=true")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(exc, file=sys.stderr)
        raise SystemExit(1)
