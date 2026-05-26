#!/usr/bin/env python3
"""Verify cross-built Pulp macOS arm64 bundle artifacts.

Pulp-owned structural verifier for the Linux-hosted macOS arm64 cross
lane. Runs from either Linux (using osxcross/LLVM tools) or macOS
(using native tools) and asserts the bundle shape contract documented
in planning/2026-05-24-linux-hosted-macos-arm64-cross-lane.md.

This script intentionally has no third-party dependencies so it can run
in advisory CI before any toolchain bootstrap completes.

Checks per artifact kind:
- bundle directory + Contents/Info.plist present
- Mach-O arm64 executable at the expected path
- (optional) AUv3 .appex/.framework nesting and exec arch
- (optional) every LC_RPATH is loader-relative (no absolute paths)

Tool resolution order for `otool`-equivalents:
  $CMAKE_OTOOL, $OTOOL, $OSXCROSS_ROOT/target/bin/*otool, PATH lookups
  for otool / llvm-otool / arm64-apple-darwin-otool /
  aarch64-apple-darwin-otool.

Exit codes:
  0 — all expected artifacts verified
  1 — at least one structural check failed
  2 — required external tool missing (only when --require-clean-rpaths)
"""
from __future__ import annotations

import argparse
import json
import os
import plistlib
import shutil
import subprocess
import sys
from pathlib import Path


def run(cmd: list[str]) -> str:
    return subprocess.check_output(cmd, text=True, stderr=subprocess.STDOUT)


def fail(msg: str, code: int = 1) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    raise SystemExit(code)


def load_plist(path: Path) -> dict:
    with path.open("rb") as f:
        return plistlib.load(f)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Verify cross-built Pulp macOS arm64 bundle artifacts.",
    )
    parser.add_argument(
        "root",
        nargs="?",
        type=Path,
        help="Artifact directory containing the built bundles.",
    )
    parser.add_argument(
        "--artifact-dir",
        dest="artifact_dir",
        type=Path,
        help="Artifact directory. Overrides the positional root.",
    )
    parser.add_argument(
        "--expect",
        default="app:vst3:component:clap",
        help=(
            "Colon- or comma-separated artifact kinds to verify "
            "(app, vst3, component, clap, auv3)."
        ),
    )
    parser.add_argument(
        "--product-name",
        default="PulpGain",
        help="Product name embedded in bundle names and executables.",
    )
    parser.add_argument("--standalone-app-name")
    parser.add_argument("--auv3-app-name")
    parser.add_argument("--auv3-appex-name")
    parser.add_argument("--auv3-framework-name")
    parser.add_argument(
        "--require-clean-rpaths",
        action="store_true",
        help=(
            "Fail if any Mach-O file in a verified bundle contains an "
            "absolute LC_RPATH entry. Requires otool-equivalent on PATH."
        ),
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Emit structured JSON results on stdout (always emitted now).",
    )
    return parser.parse_args(argv)


def expected_artifacts(args: argparse.Namespace) -> dict[str, dict[str, str]]:
    product = args.product_name
    standalone_app = args.standalone_app_name or product
    auv3_app = args.auv3_app_name or f"{product}-AUv3"
    auv3_appex = args.auv3_appex_name or product
    auv3_framework = args.auv3_framework_name or f"{product}AUv3Framework"
    all_artifacts: dict[str, dict[str, str]] = {
        "app": {
            "name": f"{standalone_app}.app",
            "executable": f"Contents/MacOS/{product}",
        },
        "vst3": {
            "name": f"{product}.vst3",
            "executable": f"Contents/MacOS/{product}",
        },
        "component": {
            "name": f"{product}.component",
            "executable": f"Contents/MacOS/{product}",
        },
        "clap": {
            "name": f"{product}.clap",
            "executable": f"Contents/MacOS/{product}",
        },
        "auv3": {
            "name": f"{auv3_app}.app",
            "executable": f"Contents/MacOS/{product}",
            "appex": f"Contents/PlugIns/{auv3_appex}.appex",
            "appex_executable": (
                f"Contents/PlugIns/{auv3_appex}.appex/Contents/MacOS/{product}"
            ),
            "framework": f"Contents/Frameworks/{auv3_framework}.framework",
            "framework_executable": (
                f"Contents/Frameworks/{auv3_framework}.framework/"
                f"Versions/A/{auv3_framework}"
            ),
        },
    }
    raw = args.expect.replace(",", ":")
    kinds = [part for part in raw.split(":") if part]
    expected: dict[str, dict[str, str]] = {}
    for kind in kinds:
        if kind not in all_artifacts:
            fail(
                f"unknown artifact kind {kind!r}; "
                f"expected one of {sorted(all_artifacts)}",
            )
        spec = dict(all_artifacts[kind])
        expected[spec["name"]] = spec
    return expected


def find_otool() -> str | None:
    candidates: list[str | None] = [
        os.environ.get("CMAKE_OTOOL"),
        os.environ.get("OTOOL"),
    ]
    osxcross_root = os.environ.get("OSXCROSS_ROOT")
    if osxcross_root:
        osxcross_bin = Path(osxcross_root) / "target" / "bin"
        if osxcross_bin.is_dir():
            candidates.extend(str(p) for p in sorted(osxcross_bin.glob("*otool")))
    candidates.extend([
        "otool",
        "llvm-otool",
        "arm64-apple-darwin-otool",
        "aarch64-apple-darwin-otool",
    ])
    for candidate in candidates:
        if not candidate:
            continue
        candidate_path = Path(candidate).expanduser()
        if candidate_path.is_file() and os.access(candidate_path, os.X_OK):
            return str(candidate_path)
        found = shutil.which(candidate)
        if found:
            return found
    return None


def rpaths(binary: Path, otool: str) -> list[str]:
    output = run([otool, "-l", str(binary)])
    values: list[str] = []
    in_rpath = False
    for raw in output.splitlines():
        line = raw.strip()
        if line == "cmd LC_RPATH":
            in_rpath = True
            continue
        if in_rpath and line.startswith("path "):
            values.append(line.split()[1])
            in_rpath = False
    return values


def require_macho_arm64(path: Path) -> str:
    if not path.exists():
        fail(f"missing executable {path}")
    file_out = run(["file", str(path)])
    if "Mach-O" not in file_out or "arm64" not in file_out:
        fail(f"{path} is not Mach-O arm64: {file_out.strip()}")
    return file_out.strip()


def mach_o_files(root: Path) -> list[Path]:
    out: list[Path] = []
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        try:
            file_out = run(["file", str(path)])
        except subprocess.CalledProcessError:
            continue
        if "Mach-O" in file_out:
            out.append(path)
    return sorted(out)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    root = args.artifact_dir or args.root
    if root is None:
        fail("no artifact directory provided (pass it positionally or via --artifact-dir)")
    if not root.is_dir():
        fail(f"artifact directory does not exist or is not a directory: {root}")
    expected = expected_artifacts(args)
    otool = find_otool()
    if args.require_clean_rpaths and not otool:
        fail(
            "could not find otool/llvm-otool to check LC_RPATH values; "
            "set OTOOL or OSXCROSS_ROOT, or install llvm-tools.",
            code=2,
        )
    results: dict[str, object] = {"root": str(root), "artifacts": []}
    for name, spec in expected.items():
        bundle = root / name
        if not bundle.exists():
            fail(f"missing {bundle}")
        info = bundle / "Contents" / "Info.plist"
        exe = bundle / spec["executable"]
        if not info.exists():
            fail(f"missing Info.plist in {bundle}")
        meta = load_plist(info)
        file_out = require_macho_arm64(exe)
        nested: dict[str, object] = {}
        if "appex" in spec:
            appex = bundle / spec["appex"]
            framework = bundle / spec["framework"]
            if not appex.is_dir():
                fail(f"missing AUv3 appex: {appex}")
            if not framework.is_dir():
                fail(f"missing AUv3 framework: {framework}")
            nested["appex"] = spec["appex"]
            nested["appex_file"] = require_macho_arm64(
                bundle / spec["appex_executable"]
            )
            nested["framework"] = spec["framework"]
            nested["framework_file"] = require_macho_arm64(
                bundle / spec["framework_executable"]
            )
        exe_rpaths: list[str] = []
        if otool:
            if args.require_clean_rpaths:
                files_to_check = mach_o_files(bundle)
            else:
                files_to_check = [exe]
            rpath_map: dict[str, list[str]] = {}
            for macho in files_to_check:
                rel = macho.relative_to(bundle).as_posix()
                values = rpaths(macho, otool)
                rpath_map[rel] = values
                if macho == exe:
                    exe_rpaths = values
                if args.require_clean_rpaths:
                    absolute = [v for v in values if v.startswith("/")]
                    if absolute:
                        fail(
                            f"{macho} has absolute LC_RPATH entries: {absolute}",
                        )
            if args.require_clean_rpaths:
                nested["rpaths_by_file"] = rpath_map
        results["artifacts"].append({
            "bundle": name,
            "identifier": meta.get("CFBundleIdentifier"),
            "executable": meta.get("CFBundleExecutable"),
            "file": file_out,
            "rpaths": exe_rpaths,
            "nested": nested,
        })
    print(json.dumps(results, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
