#!/usr/bin/env python3
"""Preflight a built or installed AU v2 component before a DAW-bench run."""

from __future__ import annotations

import argparse
import json
import os
import plistlib
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class CheckResult:
    label: str
    ok: bool
    detail: str


def _read_plist(path: Path) -> tuple[dict[str, Any] | None, str | None]:
    try:
        with path.open("rb") as f:
            data = plistlib.load(f)
    except OSError as exc:
        return None, f"cannot read Info.plist: {exc}"
    except Exception as exc:
        return None, f"cannot parse Info.plist: {exc}"
    if not isinstance(data, dict):
        return None, "Info.plist root is not a dictionary"
    return data, None


def _first_audio_component(data: dict[str, Any]) -> tuple[dict[str, Any] | None, str | None]:
    components = data.get("AudioComponents")
    if not isinstance(components, list) or not components:
        return None, "Info.plist has no AudioComponents[0]"
    component = components[0]
    if not isinstance(component, dict):
        return None, "AudioComponents[0] is not a dictionary"
    return component, None


def _check_field(component: dict[str, Any], name: str, expected: str | None) -> CheckResult:
    value = component.get(name)
    if not isinstance(value, str) or not value:
        return CheckResult(name, False, f"missing or non-string {name}")
    if expected is not None and value != expected:
        return CheckResult(name, False, f"expected {expected!r}, found {value!r}")
    return CheckResult(name, True, value)


def _check_string_field(data: dict[str, Any], name: str) -> CheckResult:
    value = data.get(name)
    if not isinstance(value, str) or not value:
        return CheckResult(name, False, f"missing or non-string {name}")
    return CheckResult(name, True, value)


def _check_component_version(component: dict[str, Any]) -> CheckResult:
    value = component.get("version")
    if not isinstance(value, int) or value < 0:
        return CheckResult("version", False, "missing or non-negative integer version")
    return CheckResult("version", True, str(value))


def _mode(path: Path) -> str:
    try:
        return oct(path.stat().st_mode & 0o777)
    except OSError:
        return "missing"


def _check_permissions(bundle: Path, executable_name: str | None) -> CheckResult:
    paths = [
        bundle,
        bundle / "Contents",
        bundle / "Contents" / "Info.plist",
        bundle / "Contents" / "MacOS",
    ]
    if executable_name:
        paths.append(bundle / "Contents" / "MacOS" / executable_name)

    missing = [path for path in paths if not path.exists()]
    if missing:
        return CheckResult(
            "permissions",
            False,
            "missing " + ", ".join(path.as_posix() for path in missing),
        )

    unreadable = [path for path in paths if not os.access(path, os.R_OK)]
    unsearchable = [
        path for path in (bundle, bundle / "Contents", bundle / "Contents" / "MacOS")
        if not os.access(path, os.X_OK)
    ]
    if unreadable or unsearchable:
        detail = []
        if unreadable:
            detail.append("unreadable: " + ", ".join(path.as_posix() for path in unreadable))
        if unsearchable:
            detail.append("unsearchable: " + ", ".join(path.as_posix() for path in unsearchable))
        return CheckResult("permissions", False, "; ".join(detail))

    modes = ", ".join(f"{path.name or path.as_posix()}={_mode(path)}" for path in paths)
    return CheckResult("permissions", True, modes)


def _check_bundle(args: argparse.Namespace) -> list[CheckResult]:
    bundle = args.bundle
    results: list[CheckResult] = []
    results.append(CheckResult("bundle", bundle.is_dir(), str(bundle)))
    plist_path = bundle / "Contents" / "Info.plist"
    executable_name: str | None = None

    data, error = _read_plist(plist_path)
    if error or data is None:
        results.append(CheckResult("Info.plist", False, error or "unknown plist error"))
        return results
    results.append(CheckResult("Info.plist", True, str(plist_path)))
    for name in ("CFBundleIdentifier", "CFBundleName", "CFBundleShortVersionString"):
        results.append(_check_string_field(data, name))

    component, error = _first_audio_component(data)
    if error or component is None:
        results.append(CheckResult("AudioComponents[0]", False, error or "unknown component error"))
        return results
    results.append(CheckResult("AudioComponents[0]", True, "present"))
    results.append(_check_field(component, "name", None))
    results.append(_check_field(component, "description", None))
    results.append(_check_component_version(component))

    for name, expected in (
        ("type", args.expect_type),
        ("subtype", args.expect_subtype),
        ("manufacturer", args.expect_manufacturer),
        ("factoryFunction", args.expect_factory),
    ):
        results.append(_check_field(component, name, expected))

    cf_executable = data.get("CFBundleExecutable")
    if isinstance(cf_executable, str) and cf_executable:
        executable_name = cf_executable
        executable = bundle / "Contents" / "MacOS" / cf_executable
        results.append(CheckResult("CFBundleExecutable", executable.is_file(), str(executable)))
    else:
        results.append(CheckResult("CFBundleExecutable", False, "missing or non-string"))

    if args.expect_symbol:
        if executable_name is None:
            results.append(CheckResult("factory symbol", False, "no executable to inspect"))
        else:
            results.append(_check_symbol(bundle / "Contents" / "MacOS" / executable_name,
                                         args.expect_symbol))

    if args.check_permissions:
        results.append(_check_permissions(bundle, executable_name))

    return results


def _check_codesign(bundle: Path) -> CheckResult:
    codesign = shutil.which("codesign")
    if codesign is None:
        return CheckResult("codesign", False, "codesign not found")
    proc = subprocess.run(
        [codesign, "--verify", "--deep", "--strict", "--verbose=2", str(bundle)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    output = proc.stdout.strip()
    if proc.returncode != 0:
        return CheckResult("codesign", False, output or "codesign verify failed")
    return CheckResult("codesign", True, output or "valid")


def _check_symbol(executable: Path, symbol: str) -> CheckResult:
    nm = shutil.which("nm")
    if nm is None:
        return CheckResult("factory symbol", False, "nm not found")
    proc = subprocess.run(
        [nm, "-gU", str(executable)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if proc.returncode != 0:
        return CheckResult("factory symbol", False, proc.stdout.strip() or "nm failed")
    needle = symbol if symbol.startswith("_") else f"_{symbol}"
    if needle not in proc.stdout:
        return CheckResult("factory symbol", False, f"{needle} not exported")
    return CheckResult("factory symbol", True, needle)


def _check_auval_list(args: argparse.Namespace) -> CheckResult:
    auval = shutil.which("auval")
    if auval is None:
        return CheckResult("auval list", False, "auval not found")
    if not (args.expect_type and args.expect_subtype and args.expect_manufacturer):
        return CheckResult("auval list", False, "type, subtype, and manufacturer are required")

    proc = subprocess.run(
        [auval, "-a"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    needle = f"{args.expect_type} {args.expect_subtype} {args.expect_manufacturer}"
    if proc.returncode != 0:
        tail = "\n".join(proc.stdout.splitlines()[-12:])
        return CheckResult("auval list", False, tail or "auval -a failed")
    for line in proc.stdout.splitlines():
        if line.strip().startswith(needle):
            return CheckResult("auval list", True, line.strip())
    return CheckResult("auval list", False,
                       f"{needle} not listed; {_summarize_auval_list(proc.stdout)}")


def _summarize_auval_list(output: str) -> str:
    component_lines = []
    non_apple_lines = []
    for raw_line in output.splitlines():
        line = raw_line.strip()
        parts = line.split()
        if len(parts) < 3:
            continue
        if len(parts[0]) != 4 or not (1 <= len(parts[1]) <= 4) or len(parts[2]) != 4:
            continue
        component_lines.append(line)
        if parts[2] != "appl":
            non_apple_lines.append(line)

    if not component_lines:
        return "auval -a listed no components"
    if not non_apple_lines:
        return f"auval -a listed {len(component_lines)} Apple component(s) and no non-Apple components"
    sample = "; ".join(non_apple_lines[:3])
    suffix = "" if len(non_apple_lines) <= 3 else f"; +{len(non_apple_lines) - 3} more"
    return f"auval -a listed {len(component_lines)} component(s), including non-Apple: {sample}{suffix}"


def _run_auval(args: argparse.Namespace) -> list[CheckResult]:
    auval = shutil.which("auval")
    if auval is None:
        return [CheckResult("auval", False, "auval not found")]
    if not (args.expect_type and args.expect_subtype and args.expect_manufacturer):
        return [CheckResult("auval", False, "type, subtype, and manufacturer are required")]

    results: list[CheckResult] = []
    for index in range(args.auval_repeat):
        proc = subprocess.run(
            [auval, "-v", args.expect_type, args.expect_subtype, args.expect_manufacturer],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        detail = f"run {index + 1}/{args.auval_repeat}"
        if proc.returncode != 0 or "PASS" not in proc.stdout:
            tail = "\n".join(proc.stdout.splitlines()[-12:])
            results.append(CheckResult("auval", False, f"{detail} failed\n{tail}"))
        else:
            results.append(CheckResult("auval", True, detail))
    return results


def _render(results: list[CheckResult]) -> str:
    lines: list[str] = []
    for result in results:
        mark = "PASS" if result.ok else "FAIL"
        lines.append(f"{mark}: {result.label}: {result.detail}")
    return "\n".join(lines)


def _render_json(results: list[CheckResult]) -> str:
    ok = all(result.ok for result in results)
    data = {
        "ok": ok,
        "checks": [
            {
                "label": result.label,
                "ok": result.ok,
                "detail": result.detail,
            }
            for result in results
        ],
    }
    return json.dumps(data, indent=2, sort_keys=True) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path,
                        help="path to an installed or built .component bundle")
    parser.add_argument("--format", choices=("text", "json"), default="text",
                        help="output format")
    parser.add_argument("--expect-type", default=None,
                        help="expected AudioComponents[0].type, e.g. aumf")
    parser.add_argument("--expect-subtype", default=None,
                        help="expected AudioComponents[0].subtype")
    parser.add_argument("--expect-manufacturer", default=None,
                        help="expected AudioComponents[0].manufacturer")
    parser.add_argument("--expect-factory", default=None,
                        help="expected AudioComponents[0].factoryFunction")
    parser.add_argument("--expect-symbol", default=None,
                        help="factory symbol expected in Contents/MacOS/<executable>")
    parser.add_argument("--check-permissions", action="store_true",
                        help="verify the current process can read/traverse the component bundle")
    parser.add_argument("--check-codesign", action="store_true",
                        help="also run codesign --verify --deep --strict on the component bundle")
    parser.add_argument("--check-auval-list", action="store_true",
                        help="also require auval -a to list the expected type/subtype/manufacturer")
    parser.add_argument("--run-auval", action="store_true",
                        help="also run auval -v using the expected type/subtype/manufacturer")
    parser.add_argument("--auval-repeat", type=int, default=2,
                        help="number of auval runs required when --run-auval is set")
    args = parser.parse_args(argv)

    if args.auval_repeat < 1:
        parser.error("--auval-repeat must be at least 1")

    results = _check_bundle(args)
    if args.check_codesign:
        results.append(_check_codesign(args.bundle))
    if args.check_auval_list:
        results.append(_check_auval_list(args))
    if args.run_auval:
        results.extend(_run_auval(args))

    if args.format == "json":
        print(_render_json(results), end="")
    else:
        print(_render(results))
    return 0 if all(result.ok for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
