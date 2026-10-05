#!/usr/bin/env python3
"""Verifies the native renderer link-boundary checker rejects drift."""

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


LINK_FILES = (
    "core/scene/CMakeFiles/pulp-scene3d-inspect.dir/link.txt",
    "core/scene/CMakeFiles/pulp-scene3d-sidecar.dir/link.txt",
    "core/scene/CMakeFiles/pulp-scene3d-bake-preflight.dir/link.txt",
    "core/render/CMakeFiles/pulp-renderer3d-probe.dir/link.txt",
    "core/render/CMakeFiles/pulp-scene3d-inspect-native.dir/link.txt",
    "test/CMakeFiles/pulp-test-scene3d.dir/link.txt",
    "test/CMakeFiles/pulp-test-renderer3d.dir/link.txt",
)


def read_text(path):
    return path.read_text(encoding="utf-8")


def write_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def copy_build_links(source_build_dir, target_build_dir):
    for relative in LINK_FILES:
        src = source_build_dir / relative
        dst = target_build_dir / relative
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)


def run_verifier(verifier, build_dir):
    return subprocess.run(
        [
            sys.executable,
            str(verifier),
            "--build-dir",
            str(build_dir),
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def append_to_link(build_dir, relative, token):
    path = build_dir / relative
    write_text(path, read_text(path) + f" {token}\n")


def remove_required_token(build_dir, relative, *spellings):
    path = build_dir / relative
    text = read_text(path)
    if not any(spelling.lower() in text.lower() for spelling in spellings):
        raise ValueError(f"{relative} does not contain required token {spellings[0]!r}")
    for spelling in spellings:
        text = text.replace(spelling, "")
    write_text(path, text)


WEBGPU_LINK_FILES = (
    "core/render/CMakeFiles/pulp-renderer3d-probe.dir/link.txt",
    "core/render/CMakeFiles/pulp-scene3d-inspect-native.dir/link.txt",
    "test/CMakeFiles/pulp-test-renderer3d.dir/link.txt",
)


def mutate_no_soname_webgpu_spelling(build_dir):
    # The Linux spelling: the runtime named by -l rather than by its path.
    for relative in WEBGPU_LINK_FILES:
        path = build_dir / relative
        text = re.sub(r"\S*libwgpu_native\S*", "-lwgpu_native", read_text(path))
        if "-lwgpu_native" not in text:
            raise ValueError(f"{relative} does not link the WebGPU runtime")
        write_text(path, text)


def remove_link_file(build_dir, relative):
    (build_dir / relative).unlink()


def mutate_forbidden_view_token(build_dir):
    append_to_link(
        build_dir,
        "core/scene/CMakeFiles/pulp-scene3d-inspect.dir/link.txt",
        "libpulp-view",
    )


def mutate_forbidden_widget_token(build_dir):
    append_to_link(
        build_dir,
        "core/render/CMakeFiles/pulp-renderer3d-probe.dir/link.txt",
        "widget_bridge",
    )


def mutate_missing_scene_parser_token(build_dir):
    remove_required_token(
        build_dir,
        "core/scene/CMakeFiles/pulp-scene3d-inspect.dir/link.txt",
        "libfastgltf",
    )


def mutate_missing_render_webgpu_token(build_dir):
    remove_required_token(
        build_dir,
        "core/render/CMakeFiles/pulp-renderer3d-probe.dir/link.txt",
        "libwgpu_native",
        "-lwgpu_native",
    )


def mutate_missing_no_soname_webgpu_token(build_dir):
    mutate_no_soname_webgpu_spelling(build_dir)
    mutate_missing_render_webgpu_token(build_dir)


def mutate_missing_required_link_file(build_dir):
    remove_link_file(
        build_dir,
        "test/CMakeFiles/pulp-test-renderer3d.dir/link.txt",
    )


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
    print(f"native_renderer_boundary_contract_case={name}")


def main():
    parser = argparse.ArgumentParser(
        description="Verify native renderer link-boundary drift is rejected.")
    parser.add_argument("--boundary-verifier", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    args = parser.parse_args()

    if not args.boundary_verifier.exists():
        print(f"boundary_verifier_exists=false path={args.boundary_verifier}")
        return 2
    for relative in LINK_FILES:
        if not (args.build_dir / relative).exists():
            print(f"native_boundary_link_exists=false path={args.build_dir / relative}")
            return 2

    cases = [
        ("valid-current-link-boundary", None, 0, "native_renderer_boundary_verified=7 link files"),
        ("valid-no-soname-webgpu-link", mutate_no_soname_webgpu_spelling, 0, "native_renderer_boundary_verified=7 link files"),
        ("forbidden-view-link-token", mutate_forbidden_view_token, 1, "forbidden view module token"),
        ("forbidden-widget-link-token", mutate_forbidden_widget_token, 1, "forbidden runtime WebGPU bridge token"),
        ("missing-scene-parser-token", mutate_missing_scene_parser_token, 1, "missing required native glTF parser token"),
        ("missing-render-webgpu-token", mutate_missing_render_webgpu_token, 1, "missing required native WebGPU runtime token"),
        ("missing-no-soname-webgpu-token", mutate_missing_no_soname_webgpu_token, 1, "missing required native WebGPU runtime token"),
        ("missing-required-link-file", mutate_missing_required_link_file, 1, "No such file or directory"),
    ]
    errors = []

    with tempfile.TemporaryDirectory() as tmp:
        temp_root = Path(tmp)
        for name, mutate, expected_code, expected_text in cases:
            case_build = temp_root / name
            copy_build_links(args.build_dir, case_build)
            if mutate is not None:
                mutate(case_build)
            expect_case(
                name,
                run_verifier(args.boundary_verifier, case_build),
                expected_code,
                expected_text,
                errors,
            )

    if errors:
        for error in errors:
            print(f"error={error}", file=sys.stderr)
        return 1

    print("native_renderer_boundary_contract_verified=true")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(exc, file=sys.stderr)
        raise SystemExit(1)
