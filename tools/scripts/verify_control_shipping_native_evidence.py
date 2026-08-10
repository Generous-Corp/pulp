#!/usr/bin/env python3
"""Validate and aggregate native control-shipping scanner reports.

This helper does not scan binaries itself. It validates reports emitted by the
installed Pulp SDK's canonical check_control_shipping_artifact.cmake scanner so
CI cannot replace native nm/readelf/dumpbin/lipo evidence with format labels.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


LEG_SCHEMA = "dev.pulp.control/native-shipping-leg@1"
AGGREGATE_SCHEMA = "dev.pulp.control/native-shipping-evidence@1"
REPORT_SCHEMA = "dev.pulp.control/shipping-report@1"
AAX_UNAVAILABLE_REASONS = {
    "aax-sdk-secret-unavailable",
    "aax-sdk-secret-withheld-untrusted-event",
}

EXPECTED_SCANNERS = {
    "darwin": ("nm", "otool", "lipo"),
    "linux": ("nm", "readelf", "readelf"),
    "windows": ("dumpbin", "dumpbin", "dumpbin"),
}

EXPECTED_LEGS = {
    "macos-universal": ("darwin", {"arm64", "x86_64"}),
    "linux-x64": ("linux", {"x86_64"}),
    "linux-arm64": ("linux", {"arm64"}),
    "windows-x64": ("windows", {"x86_64"}),
}

EXPECTED_FORMATS = {
    "macos-universal": {
        "Standalone",
        "VST3",
        "CLAP",
        "LV2",
        "AUv2",
        "AUv3Framework",
        "AUv3Extension",
        "AUv3Host",
    },
    "linux-x64": {"Standalone", "VST3", "CLAP", "LV2"},
    "linux-arm64": {"Standalone", "VST3", "CLAP", "LV2"},
    "windows-x64": {"Standalone", "VST3", "CLAP", "LV2"},
}

ARCH_ALIASES = {
    "amd64": "x86_64",
    "x64": "x86_64",
    "x86-64": "x86_64",
    "aarch64": "arm64",
    "aa64": "arm64",
}


class EvidenceError(RuntimeError):
    pass


def _normalize_arch(value: str) -> str:
    lowered = value.strip().lower()
    return ARCH_ALIASES.get(lowered, lowered)


def _csv(value: str) -> list[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


def _load_object(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise EvidenceError(f"cannot read JSON {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise EvidenceError(f"expected JSON object: {path}")
    return value


def verify_leg(args: argparse.Namespace) -> dict[str, Any]:
    root = Path(args.report_root).resolve()
    if not root.is_dir():
        raise EvidenceError(f"report root does not exist: {root}")
    if args.platform not in EXPECTED_SCANNERS:
        raise EvidenceError(f"unsupported platform: {args.platform}")

    required_formats = set(_csv(args.required_formats))
    required_arches = {_normalize_arch(item) for item in _csv(args.required_architectures)}
    if not required_formats or not required_arches:
        raise EvidenceError("required formats and architectures must be nonempty")

    pattern = f"{args.target}.*.control-shipping-report.json"
    report_paths = sorted(root.rglob(pattern))
    if not report_paths:
        raise EvidenceError(f"no canonical scanner reports matched {pattern} under {root}")

    by_format: dict[str, tuple[Path, dict[str, Any]]] = {}
    expected_symbol, expected_dependency, expected_architecture = EXPECTED_SCANNERS[args.platform]
    for path in report_paths:
        report = _load_object(path)
        artifact_format = report.get("format")
        if not isinstance(artifact_format, str) or not artifact_format:
            raise EvidenceError(f"report has no format: {path}")
        if artifact_format in by_format:
            previous_path, previous_report = by_format[artifact_format]
            if report != previous_report:
                raise EvidenceError(
                    f"conflicting {artifact_format} reports: {previous_path} and {path}"
                )
            # AUv3 packaging embeds byte-identical framework/extension sidecars
            # into the host app. They are copies of the already-scanned report,
            # not independent proof or an ambiguity.
            continue
        by_format[artifact_format] = (path, report)

    missing = sorted(required_formats - by_format.keys())
    if missing:
        raise EvidenceError(f"missing real format report(s): {', '.join(missing)}")

    selected: list[dict[str, Any]] = []
    for artifact_format in sorted(required_formats):
        path, report = by_format[artifact_format]
        if report.get("schema") != REPORT_SCHEMA:
            raise EvidenceError(f"unsupported report schema in {path}")
        if report.get("verdict") != "pass":
            raise EvidenceError(f"scanner did not pass for {artifact_format}: {path}")
        if report.get("profile") != "production-stripped":
            raise EvidenceError(f"{artifact_format} is not production-stripped: {path}")
        if report.get("platform") != args.platform:
            raise EvidenceError(
                f"{artifact_format} platform is {report.get('platform')!r}, expected {args.platform!r}"
            )
        scanners = (
            report.get("symbol_scanner"),
            report.get("dependency_scanner"),
            report.get("architecture_scanner"),
        )
        if scanners != (expected_symbol, expected_dependency, expected_architecture):
            raise EvidenceError(
                f"{artifact_format} used scanners {scanners!r}, expected "
                f"{(expected_symbol, expected_dependency, expected_architecture)!r}"
            )
        actual_arches_value = report.get("architectures")
        if not isinstance(actual_arches_value, list) or not all(
            isinstance(value, str) for value in actual_arches_value
        ):
            raise EvidenceError(f"{artifact_format} has invalid architectures: {path}")
        actual_arches = {_normalize_arch(value) for value in actual_arches_value}
        if not required_arches.issubset(actual_arches):
            raise EvidenceError(
                f"{artifact_format} architectures {sorted(actual_arches)} do not cover "
                f"{sorted(required_arches)}"
            )
        artifact = report.get("artifact")
        if not isinstance(artifact, str) or not Path(artifact).is_file():
            raise EvidenceError(f"{artifact_format} report does not name a live native artifact: {path}")
        size = report.get("artifact_size_bytes")
        if not isinstance(size, int) or size <= 0:
            raise EvidenceError(f"{artifact_format} report has invalid artifact size: {path}")
        if Path(artifact).stat().st_size != size:
            raise EvidenceError(f"{artifact_format} report artifact size is stale: {path}")
        selected.append(
            {
                "format": artifact_format,
                "artifact": artifact,
                "artifact_size_bytes": size,
                "architectures": sorted(actual_arches),
                "report": str(path),
                "scanners": {
                    "symbols": expected_symbol,
                    "dependencies": expected_dependency,
                    "architecture": expected_architecture,
                },
            }
        )

    if args.aax_mode == "required":
        if "AAX" not in required_formats:
            raise EvidenceError("AAX required mode must include AAX in required formats")
        aax = {"status": "proven", "proof": True, "format": "AAX"}
    elif args.aax_mode == "unavailable":
        if not args.aax_unavailable_reason:
            raise EvidenceError("AAX unavailable mode requires a machine-readable reason code")
        if "AAX" in by_format:
            raise EvidenceError("AAX report exists but the leg declared AAX unavailable")
        aax = {
            "status": "unavailable",
            "proof": False,
            "reason_code": args.aax_unavailable_reason,
        }
    else:
        if args.aax_unavailable_reason:
            raise EvidenceError("AAX reason is only valid with unavailable mode")
        aax = {"status": "not-assessed", "proof": False}

    evidence = {
        "schema": LEG_SCHEMA,
        "leg": args.leg,
        "platform": args.platform,
        "required_architectures": sorted(required_arches),
        "required_formats": sorted(required_formats),
        "reports": selected,
        "aax": aax,
        "verdict": "pass",
    }
    Path(args.output).write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return evidence


def aggregate(args: argparse.Namespace) -> dict[str, Any]:
    root = Path(args.evidence_root).resolve()
    paths = sorted(root.rglob("*.native-shipping-leg.json"))
    legs: dict[str, dict[str, Any]] = {}
    for path in paths:
        evidence = _load_object(path)
        if evidence.get("schema") != LEG_SCHEMA or evidence.get("verdict") != "pass":
            raise EvidenceError(f"invalid or failed leg evidence: {path}")
        leg = evidence.get("leg")
        if not isinstance(leg, str) or leg not in EXPECTED_LEGS:
            raise EvidenceError(f"unknown native shipping leg {leg!r}: {path}")
        if leg in legs:
            raise EvidenceError(f"duplicate native shipping leg: {leg}")
        platform, expected_arches = EXPECTED_LEGS[leg]
        actual_arches = {_normalize_arch(value) for value in evidence.get("required_architectures", [])}
        if evidence.get("platform") != platform or actual_arches != expected_arches:
            raise EvidenceError(f"native shipping leg identity mismatch: {leg}")
        required_formats = evidence.get("required_formats")
        if not isinstance(required_formats, list) or not all(
            isinstance(value, str) for value in required_formats
        ):
            raise EvidenceError(f"native shipping leg has invalid required formats: {leg}")
        actual_formats = set(required_formats)
        allowed_formats = (EXPECTED_FORMATS[leg], EXPECTED_FORMATS[leg] | {"AAX"})
        if actual_formats not in allowed_formats:
            raise EvidenceError(f"native shipping leg format coverage mismatch: {leg}")
        reports = evidence.get("reports")
        if not isinstance(reports, list) or not all(isinstance(value, dict) for value in reports):
            raise EvidenceError(f"native shipping leg has invalid reports: {leg}")
        report_formats = {value.get("format") for value in reports}
        if report_formats != actual_formats:
            raise EvidenceError(f"native shipping leg report coverage mismatch: {leg}")
        symbol_scanner, dependency_scanner, architecture_scanner = EXPECTED_SCANNERS[platform]
        expected_scanners = {
            "symbols": symbol_scanner,
            "dependencies": dependency_scanner,
            "architecture": architecture_scanner,
        }
        for report in reports:
            if report.get("scanners") != expected_scanners:
                raise EvidenceError(f"native shipping leg scanner mismatch: {leg}")
            report_arches = {
                _normalize_arch(value)
                for value in report.get("architectures", [])
                if isinstance(value, str)
            }
            if not expected_arches.issubset(report_arches):
                raise EvidenceError(f"native shipping leg report architecture mismatch: {leg}")
            if not isinstance(report.get("artifact_size_bytes"), int) or report["artifact_size_bytes"] <= 0:
                raise EvidenceError(f"native shipping leg report has invalid artifact size: {leg}")
        legs[leg] = evidence

    missing = sorted(EXPECTED_LEGS.keys() - legs.keys())
    if missing:
        raise EvidenceError(f"missing native shipping leg evidence: {', '.join(missing)}")

    aax = legs["macos-universal"].get("aax")
    if not isinstance(aax, dict):
        raise EvidenceError("macOS universal leg has no AAX disposition")
    if aax.get("status") == "proven":
        if aax.get("proof") is not True:
            raise EvidenceError("AAX proven status is missing proof=true")
        if set(legs["macos-universal"]["required_formats"]) != (
            EXPECTED_FORMATS["macos-universal"] | {"AAX"}
        ):
            raise EvidenceError("AAX proven status is missing the real AAX report")
    elif aax.get("status") == "unavailable":
        if aax.get("proof") is not False or aax.get("reason_code") not in AAX_UNAVAILABLE_REASONS:
            raise EvidenceError("AAX unavailable status is not an explicit secret disposition")
        if set(legs["macos-universal"]["required_formats"]) != EXPECTED_FORMATS["macos-universal"]:
            raise EvidenceError("AAX unavailable status cannot include an AAX report")
    else:
        raise EvidenceError("AAX must be proven or explicitly unavailable")

    result = {
        "schema": AGGREGATE_SCHEMA,
        "complete": True,
        "aax": aax,
        "legs": [legs[name] for name in sorted(legs)],
        "verdict": "pass",
    }
    Path(args.output).write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return result


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser()
    commands = root.add_subparsers(dest="command", required=True)

    leg = commands.add_parser("verify-leg")
    leg.add_argument("--report-root", required=True)
    leg.add_argument("--target", default="PulpSDKSmokeProbe")
    leg.add_argument("--leg", required=True)
    leg.add_argument("--platform", required=True)
    leg.add_argument("--required-architectures", required=True)
    leg.add_argument("--required-formats", required=True)
    leg.add_argument("--aax-mode", choices=("required", "unavailable", "not-assessed"), default="not-assessed")
    leg.add_argument("--aax-unavailable-reason")
    leg.add_argument("--output", required=True)
    leg.set_defaults(handler=verify_leg)

    combined = commands.add_parser("aggregate")
    combined.add_argument("--evidence-root", required=True)
    combined.add_argument("--output", required=True)
    combined.set_defaults(handler=aggregate)
    return root


def main() -> int:
    args = parser().parse_args()
    try:
        result = args.handler(args)
    except EvidenceError as exc:
        print(f"control native shipping evidence: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
