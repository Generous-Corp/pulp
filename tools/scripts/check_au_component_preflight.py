#!/usr/bin/env python3
"""Preflight a built or installed AU v2 component before a DAW-bench run."""

from __future__ import annotations

import argparse
import json
import math
import os
import plistlib
import shutil
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any


HOST_PROBE_TIMEOUT = 10.0


@dataclass(frozen=True)
class CheckResult:
    label: str
    ok: bool
    detail: str
    code: str | None = None
    remediation: str | None = None


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
        return CheckResult(name, False, f"missing or non-string {name}",
                           "COMPONENT_METADATA_INVALID", "Fix the AudioComponents metadata and rebuild.")
    if expected is not None and value != expected:
        return CheckResult(name, False, f"expected {expected!r}, found {value!r}",
                           "COMPONENT_METADATA_INVALID", "Use the expected AU type, subtype, or manufacturer.")
    return CheckResult(name, True, value)


def _check_string_field(data: dict[str, Any], name: str) -> CheckResult:
    value = data.get(name)
    if not isinstance(value, str) or not value:
        return CheckResult(name, False, f"missing or non-string {name}",
                           "COMPONENT_METADATA_INVALID", "Fix the bundle's required Info.plist field.")
    return CheckResult(name, True, value)


def _check_component_version(component: dict[str, Any]) -> CheckResult:
    value = component.get("version")
    if not isinstance(value, int) or value < 0:
        return CheckResult("version", False, "missing or non-negative integer version",
                           "COMPONENT_METADATA_INVALID", "Fix AudioComponents[0].version in the bundle metadata.")
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
        results.append(CheckResult("Info.plist", False, error or "unknown plist error",
                                   "COMPONENT_METADATA_INVALID", "Fix or restore Contents/Info.plist."))
        return results
    results.append(CheckResult("Info.plist", True, str(plist_path)))
    for name in ("CFBundleIdentifier", "CFBundleName", "CFBundleShortVersionString"):
        results.append(_check_string_field(data, name))

    component, error = _first_audio_component(data)
    if error or component is None:
        results.append(CheckResult("AudioComponents[0]", False, error or "unknown component error",
                                   "COMPONENT_METADATA_INVALID", "Add a valid AudioComponents entry to Info.plist."))
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
        results.append(CheckResult("CFBundleExecutable", False, "missing or non-string",
                                   "COMPONENT_METADATA_INVALID", "Set CFBundleExecutable to the AU binary name."))

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
    proc = _run_probe([codesign, "--verify", "--deep", "--strict", "--verbose=2", str(bundle)])
    output = proc.stdout.strip()
    if proc.returncode == 124:
        return CheckResult("codesign", False, output, "HOST_PROBE_TIMEOUT",
                           "Retry after codesign is responsive.")
    if proc.returncode != 0:
        return CheckResult("codesign", False, output or "codesign verify failed")
    return CheckResult("codesign", True, output or "valid")


def _check_signing_identity(bundle: Path, identity: str) -> CheckResult:
    codesign = shutil.which("codesign")
    if codesign is None:
        return CheckResult("signing identity", False, "codesign not found")
    with tempfile.TemporaryDirectory(prefix="pulp-au-signing-check-") as temp_dir:
        temp_bundle = Path(temp_dir) / bundle.name
        try:
            shutil.copytree(bundle, temp_bundle, symlinks=True)
        except OSError as exc:
            return CheckResult("signing identity", False, f"cannot copy bundle: {exc}")
        proc = _run_probe([
            codesign, "--force", "--deep", "--sign", identity,
            "--timestamp=none", "--options", "runtime", str(temp_bundle),
        ])
    output = proc.stdout.strip()
    if proc.returncode == 124:
        return CheckResult("signing identity", False, output, "HOST_PROBE_TIMEOUT",
                           "Retry after codesign is responsive.")
    if proc.returncode != 0:
        if "errSecInternalComponent" in output:
            output += (
                "\nprivate key access failed; unlock the keychain or configure "
                "the key partition list for Apple signing tools"
            )
        return CheckResult("signing identity", False, output or "codesign signing failed")
    return CheckResult("signing identity", True, identity)


def _check_gatekeeper(bundle: Path) -> CheckResult:
    spctl = shutil.which("spctl")
    if spctl is None:
        return CheckResult("gatekeeper", False, "spctl not found")
    proc = _run_probe([spctl, "--assess", "--type", "execute", "--verbose=4", str(bundle)])
    output = proc.stdout.strip()
    if proc.returncode == 124:
        return CheckResult("gatekeeper", False, output, "HOST_PROBE_TIMEOUT",
                           "Retry after spctl is responsive.")
    if proc.returncode != 0:
        policy_detail = _check_distribution_policy(bundle)
        if policy_detail:
            output = "\n".join(part for part in (output, policy_detail) if part)
        return CheckResult("gatekeeper", False, output or "spctl rejected component")
    return CheckResult("gatekeeper", True, output or "accepted")


def _check_distribution_policy(bundle: Path) -> str:
    syspolicy = shutil.which("syspolicy_check")
    if syspolicy is None:
        return ""
    proc = _run_probe([syspolicy, "distribution", str(bundle)])
    return proc.stdout.strip()


def _check_symbol(executable: Path, symbol: str) -> CheckResult:
    nm = shutil.which("nm")
    if nm is None:
        return CheckResult("factory symbol", False, "nm not found")
    proc = _run_probe([nm, "-gU", str(executable)])
    if proc.returncode != 0:
        if proc.returncode == 124:
            return CheckResult("factory symbol", False, proc.stdout.strip(), "HOST_PROBE_TIMEOUT",
                               "Retry after nm is responsive.")
        return CheckResult("factory symbol", False, proc.stdout.strip() or "nm failed")
    needle = symbol if symbol.startswith("_") else f"_{symbol}"
    if needle not in proc.stdout:
        return CheckResult("factory symbol", False, f"{needle} not exported")
    return CheckResult("factory symbol", True, needle)


def _auval_inventory(output: str) -> tuple[list[str], list[str]]:
    """Return all component rows and rows whose manufacturer is not Apple."""
    component_lines: list[str] = []
    non_apple_lines: list[str] = []
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
    return component_lines, non_apple_lines


def _check_auval_list(args: argparse.Namespace) -> CheckResult:
    auval = shutil.which("auval")
    if auval is None:
        return CheckResult("auval list", False, "auval not found", "TOOL_MISSING",
                           "Install the macOS AudioUnit validation tools.")
    if not (args.expect_type and args.expect_subtype and args.expect_manufacturer):
        return CheckResult("auval list", False, "type, subtype, and manufacturer are required",
                           "EXPECTATION_MISSING", "Pass --expect-type, --expect-subtype, and --expect-manufacturer.")

    try:
        proc = subprocess.run(
            [auval, "-a"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
            timeout=args.auval_timeout,
        )
    except subprocess.TimeoutExpired as exc:
        partial = exc.output.decode(errors="replace") if isinstance(exc.output, bytes) else (exc.output or "")
        return CheckResult(
            "auval list", False,
            f"timed out after {args.auval_timeout:g}s" + (f"\n{partial}" if partial else ""),
            "AUVAL_TIMEOUT",
            "Do not retry an unbounded scan; isolate the target with --run-auval and inspect the registrar domain/logs.",
        )
    needle = f"{args.expect_type} {args.expect_subtype} {args.expect_manufacturer}"
    if proc.returncode != 0:
        tail = "\n".join(proc.stdout.splitlines()[-12:])
        return CheckResult("auval list", False, tail or "auval -a failed", "AUVAL_LIST_FAILED",
                           "Run the host diagnosis and inspect AudioComponentRegistrar logs.")
    for line in proc.stdout.splitlines():
        if line.strip().split()[:3] == [args.expect_type, args.expect_subtype, args.expect_manufacturer]:
            return CheckResult("auval list", True, line.strip(), "COMPONENT_VISIBLE")
    components, non_apple = _auval_inventory(proc.stdout)
    summary = _summarize_auval_list(proc.stdout)
    if components and not non_apple:
        return CheckResult(
            "auval list", False, f"{needle} not listed; {summary}",
            "SYSTEM_REGISTRAR_APPLE_ONLY",
            "Run again with --diagnose-host; restart the AudioComponentRegistrar or reboot after Rosetta is installed, then rescan.",
        )
    return CheckResult(
        "auval list", False, f"{needle} not listed; {summary}",
        "COMPONENT_NOT_REGISTERED",
        "Run again with --diagnose-host; verify installation path, metadata, architecture, signing, and registrar state.",
    )


def _summarize_auval_list(output: str) -> str:
    component_lines, non_apple_lines = _auval_inventory(output)
    if not component_lines:
        return "auval -a listed no components"
    if not non_apple_lines:
        return f"auval -a listed {len(component_lines)} Apple component(s) and no non-Apple components"
    sample = "; ".join(non_apple_lines[:3])
    suffix = "" if len(non_apple_lines) <= 3 else f"; +{len(non_apple_lines) - 3} more"
    return f"auval -a listed {len(component_lines)} component(s), including non-Apple: {sample}{suffix}"


def _run_probe(command: list[str], *, timeout: float | None = None) -> subprocess.CompletedProcess[str]:
    limit = HOST_PROBE_TIMEOUT if timeout is None else timeout
    try:
        return subprocess.run(command, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, check=False, timeout=limit)
    except subprocess.TimeoutExpired as exc:
        partial = exc.output.decode(errors="replace") if isinstance(exc.output, bytes) else (exc.output or "")
        return subprocess.CompletedProcess(command, 124, f"timed out after {limit:g}s\n{partial}")


def _check_host_architecture(bundle: Path) -> list[CheckResult]:
    results: list[CheckResult] = []
    uname = _run_probe(["uname", "-m"])
    arch = uname.stdout.strip() or "unknown"
    results.append(CheckResult("host architecture", uname.returncode == 0, arch,
                               "HOST_ARCHITECTURE" if uname.returncode == 0 else "HOST_ARCHITECTURE_UNKNOWN"))
    plist, _ = _read_plist(bundle / "Contents" / "Info.plist")
    executable_name = plist.get("CFBundleExecutable") if plist else None
    target = (bundle / "Contents" / "MacOS" / executable_name
              if isinstance(executable_name, str) and executable_name else None)
    if target is None:
        results.append(CheckResult("binary architecture", False, "executable not found",
                                   "BINARY_MISSING", "Fix CFBundleExecutable and rebuild the component."))
    else:
        file_tool = shutil.which("file")
        if not file_tool:
            results.append(CheckResult("binary architecture", False, "file not found",
                                       "TOOL_MISSING", "Install the macOS file tool or provide a complete host toolchain."))
        else:
            probe = _run_probe([file_tool, str(target)])
            detail = probe.stdout.strip() or "file returned no architecture"
            if probe.returncode == 124:
                results.append(CheckResult("binary architecture", False, detail,
                                           "HOST_PROBE_TIMEOUT", "Retry after checking the host tools are responsive."))
            else:
                has_arm64 = "arm64" in detail or "arm64e" in detail
                has_x86 = "x86_64" in detail
                compatible = (probe.returncode == 0 and "Mach-O" in detail and
                              ((arch == "arm64" and (has_arm64 or has_x86)) or
                               (arch == "x86_64" and has_x86)))
                code = "BINARY_ARCHITECTURE_COMPATIBLE" if compatible else "BINARY_ARCHITECTURE_MISMATCH"
                remediation = None if compatible else "Build an arm64/universal AU, or install Rosetta for an x86_64-only AU."
                results.append(CheckResult("binary architecture", compatible, detail, code, remediation))
    if arch == "arm64":
        rosetta = _run_probe(["arch", "-x86_64", "true"])
        if rosetta.returncode == 0:
            results.append(CheckResult("Rosetta", True, "available", "ROSETTA_AVAILABLE"))
        else:
            results.append(CheckResult("Rosetta", False, rosetta.stdout.strip() or "unavailable",
                                       "HOST_ROSETTA_MISSING",
                                       "Install Rosetta with: softwareupdate --install-rosetta --agree-to-license; restart the registrar before rescanning."))
    elif arch == "x86_64":
        results.append(CheckResult("Rosetta", True, "not required on non-arm64 host", "ROSETTA_NOT_REQUIRED"))
    else:
        results.append(CheckResult("Rosetta", False, "host architecture unavailable", "HOST_ARCHITECTURE_UNKNOWN",
                                   "Retry after uname -m is responsive."))
    return results


def _registrar_rosetta_failure(output: str) -> bool:
    needle = "Rosetta runtime is not available"
    try:
        decoded = json.loads(output)
    except json.JSONDecodeError:
        decoded = []
    records = decoded if isinstance(decoded, list) else [decoded]
    for record in records:
        if not isinstance(record, dict):
            continue
        process = str(record.get("process", ""))
        image = str(record.get("processImagePath", "")).rsplit("/", 1)[-1]
        message = str(record.get("eventMessage", ""))
        if (process == "AudioComponentRegistrar" or image == "AudioComponentRegistrar") and needle in message:
            return True
    for line in output.splitlines():
        if needle not in line:
            continue
        # Compact output is accepted only when the registrar is the process
        # prefix; this excludes the `log show` command's argv.
        if "AudioComponentRegistrar[" in line:
            return True
    return False


def _check_registrar() -> list[CheckResult]:
    results: list[CheckResult] = []
    launchctl = shutil.which("launchctl")
    if not launchctl:
        return [CheckResult("AudioComponentRegistrar", False, "launchctl not found", "REGISTRAR_TOOL_MISSING",
                            "Run this check on macOS with launchctl available." )]
    domain = _run_probe([launchctl, "managername"])
    manager = domain.stdout.strip()
    if domain.returncode == 0 and manager:
        results.append(CheckResult("registrar domain", True, manager, "REGISTRAR_DOMAIN"))
    elif domain.returncode == 124:
        results.append(CheckResult("registrar domain", False, domain.stdout.strip(), "HOST_PROBE_TIMEOUT",
                                   "Retry after launchctl is responsive."))
    else:
        results.append(CheckResult("registrar domain", False, domain.stdout.strip() or "launchctl managername failed",
                                   "REGISTRAR_DOMAIN_UNKNOWN",
                                   "Run this check from the same Aqua or user launchd domain as the DAW."))

    uid = str(os.getuid())
    if domain.returncode != 0:
        manager = ""
    targets: list[str] = []
    if manager == "Aqua":
        targets.append(f"gui/{uid}/com.apple.audio.AudioComponentRegistrar")
    elif manager:
        targets.append(f"user/{uid}/com.apple.audio.AudioComponentRegistrar")
    else:
        targets.append("system/com.apple.audio.AudioComponentRegistrar")
    probes = [(target, _run_probe([launchctl, "print", target])) for target in targets]
    present = next(((target, probe) for target, probe in probes if probe.returncode == 0), None)
    if present is not None:
        target, _ = present
        results.append(CheckResult("AudioComponentRegistrar", True, f"{target} present", "REGISTRAR_PRESENT"))
    elif any(probe.returncode == 124 for _, probe in probes):
        results.append(CheckResult("AudioComponentRegistrar", False, "launchctl probe timed out",
                                   "HOST_PROBE_TIMEOUT", "Retry after launchctl is responsive."))
    else:
        # The registrar can be a short-lived on-demand process rather than a
        # continuously printed launchd service. Use a process probe before
        # diagnosing the host as unavailable.
        pgrep = shutil.which("pgrep")
        running = _run_probe([pgrep, "-x", "AudioComponentRegistrar"]) if pgrep else None
        if not manager and running is not None and running.returncode == 0:
            results.append(CheckResult("AudioComponentRegistrar", True, "process present (on-demand)",
                                       "REGISTRAR_PRESENT"))
        elif manager:
            results.append(CheckResult("AudioComponentRegistrar", False,
                                       f"{targets[0]} unavailable in registrar domain {manager}",
                                       "REGISTRAR_DOMAIN_UNAVAILABLE",
                                       "Rescan or restart the registrar in the same Aqua/user domain as the DAW."))
        else:
            results.append(CheckResult("AudioComponentRegistrar", False, probes[-1][1].stdout.strip() or "service not found",
                                       "SYSTEM_REGISTRAR_UNAVAILABLE",
                                       "Restart the registrar or reboot macOS, then rerun this preflight."))
    log = shutil.which("log")
    if not log:
        results.append(CheckResult("registrar logs", False, "log tool not found", "REGISTRAR_LOG_TOOL_MISSING",
                                   "Run this check on macOS with the unified log tool available."))
    else:
        logs = _run_probe([log, "show", "--last", "10m", "--style", "json",
                           "--predicate", 'process == "AudioComponentRegistrar" AND eventMessage CONTAINS[c] "Rosetta runtime is not available"'])
        text = logs.stdout.strip()
        if logs.returncode == 124:
            results.append(CheckResult("registrar logs", False, text, "HOST_PROBE_TIMEOUT",
                                       "Retry after the unified log service is responsive."))
        elif _registrar_rosetta_failure(text):
            results.append(CheckResult("registrar logs", False, "Rosetta runtime is not available",
                                       "REGISTRAR_ROSETTA_FAILURE",
                                       "Install Rosetta and restart the registrar before rescanning."))
        else:
            results.append(CheckResult("registrar logs", True, "no recent Rosetta failure observed", "REGISTRAR_LOGS_CLEAR"))
    return results


def _diagnose_host(args: argparse.Namespace) -> list[CheckResult]:
    results = _check_host_architecture(args.bundle)
    results.extend(_check_registrar())
    return results


def _run_auval(args: argparse.Namespace) -> list[CheckResult]:
    auval = shutil.which("auval")
    if auval is None:
        return [CheckResult("auval", False, "auval not found")]
    if not (args.expect_type and args.expect_subtype and args.expect_manufacturer):
        return [CheckResult("auval", False, "type, subtype, and manufacturer are required")]

    results: list[CheckResult] = []
    for index in range(args.auval_repeat):
        detail = f"run {index + 1}/{args.auval_repeat}"
        try:
            proc = subprocess.run(
                [auval, "-v", args.expect_type, args.expect_subtype, args.expect_manufacturer],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=args.auval_timeout,
            )
        except subprocess.TimeoutExpired:
            results.append(CheckResult(
                "auval", False, f"{detail} timed out after {args.auval_timeout:g}s",
                "AUVAL_TIMEOUT",
                "Inspect registrar logs and isolate unrelated third-party components before retrying.",
            ))
            continue
        passed = any(line.strip().rstrip(".") in ("AU VALIDATION SUCCEEDED", "AU VALIDATION PASSED")
                     for line in proc.stdout.splitlines())
        if proc.returncode != 0 or not passed:
            tail = "\n".join(proc.stdout.splitlines()[-12:])
            results.append(CheckResult("auval", False, f"{detail} failed\n{tail}",
                                       "AUVAL_FAILED", "Inspect the typed bundle and registrar checks before retrying."))
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
                **({"code": result.code} if result.code else {}),
                **({"remediation": result.remediation} if result.remediation else {}),
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
    parser.add_argument("--check-signing-identity", default=None,
                        help="copy the bundle to a temp dir and verify this identity can sign it")
    parser.add_argument("--check-gatekeeper", action="store_true",
                        help="also require spctl --assess --type execute to accept the bundle")
    parser.add_argument("--check-auval-list", action="store_true",
                        help="also require auval -a to list the expected type/subtype/manufacturer")
    parser.add_argument("--diagnose-host", action="store_true",
                        help="inspect Apple Silicon/Rosetta and AudioComponentRegistrar when diagnosing AU discovery")
    parser.add_argument("--run-auval", action="store_true",
                        help="also run auval -v using the expected type/subtype/manufacturer")
    parser.add_argument("--auval-repeat", type=int, default=2,
                        help="number of auval runs required when --run-auval is set")
    parser.add_argument("--auval-timeout", type=float, default=30.0,
                        help="maximum seconds allowed for each auval invocation (default: 30)")
    args = parser.parse_args(argv)

    if args.auval_repeat < 1:
        parser.error("--auval-repeat must be at least 1")
    if not math.isfinite(args.auval_timeout) or args.auval_timeout <= 0:
        parser.error("--auval-timeout must be greater than 0")

    results = _check_bundle(args)
    if args.check_codesign:
        results.append(_check_codesign(args.bundle))
    if args.check_signing_identity:
        results.append(_check_signing_identity(args.bundle, args.check_signing_identity))
    if args.check_gatekeeper:
        results.append(_check_gatekeeper(args.bundle))
    if args.check_auval_list:
        auval_result = _check_auval_list(args)
        results.append(auval_result)
        if args.diagnose_host or (not auval_result.ok and auval_result.code == "SYSTEM_REGISTRAR_APPLE_ONLY"):
            results.extend(_diagnose_host(args))
    elif args.diagnose_host:
        results.extend(_diagnose_host(args))
    if args.run_auval:
        results.extend(_run_auval(args))

    if args.format == "json":
        print(_render_json(results), end="")
    else:
        print(_render(results))
    return 0 if all(result.ok for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
