#!/usr/bin/env python3
"""PUB-04 real-host proof checker for the Sample Region native formats.

The native package driver (or a checked-in REAPER driver) emits one JSON object
prefixed by ``[sample-region-f4]`` after it has driven the installed, signed
plugin.  This script deliberately does not turn a scan/validator result into a
host pass: every required host observation must be present in the same receipt.
It can execute a driver with ``--run-command`` or validate a captured log with
``--receipt``.  The driver owns the GUI/REAPER interaction; this checker owns
the fail-closed evidence contract.
"""
from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

EXIT_PASS, EXIT_FAIL, EXIT_SKIP, EXIT_INCONCLUSIVE = 0, 1, 2, 3
PREFIX = "[sample-region-f4] "
FORMATS = ("au", "vst3", "clap")
EXPECTED_BUNDLE_ID = "com.pulp.sample-region-allpass"
REQUIRED_TRUE = (
    "installed", "signed", "automation", "state_save", "state_reload",
    "audio", "reload", "zero_pdc", "parameter_identity",
)


@dataclass(frozen=True)
class Verdict:
    code: int
    reason: str
    receipt: dict[str, Any] | None = None


def _json_lines(text: str) -> list[dict[str, Any]]:
    found: list[dict[str, Any]] = []
    for line in text.splitlines():
        if not line.startswith(PREFIX):
            continue
        try:
            value = json.loads(line[len(PREFIX):])
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            found.append(value)
    return found


def validate_receipt(receipt: dict[str, Any], *, expected_format: str | None = None) -> Verdict:
    """Validate one atomic, host-bound PUB-04 receipt.

    A receipt is intentionally stricter than a collection of independent
    validator results.  It must identify the exact host and package and prove
    all observations from one installed instance.  Missing or false fields are
    INCONCLUSIVE because they are missing evidence; explicit ``error`` is a
    FAIL because the host reported a failed assertion.
    """
    if receipt.get("error"):
        return Verdict(EXIT_FAIL, f"host reported failure: {receipt['error']}", receipt)
    if receipt.get("packet") != "PKT-F4-01":
        return Verdict(EXIT_INCONCLUSIVE, "receipt is not Packet F4 / PKT-F4-01", receipt)
    fmt = receipt.get("format")
    if fmt not in FORMATS or (expected_format and fmt != expected_format):
        return Verdict(EXIT_INCONCLUSIVE, f"invalid or unexpected format: {fmt!r}", receipt)
    for key in ("host", "host_version", "plugin_path", "bundle_id", "host_instance"):
        if not isinstance(receipt.get(key), str) or not receipt[key]:
            return Verdict(EXIT_INCONCLUSIVE, f"missing exact {key}", receipt)
    if not Path(receipt["plugin_path"]).is_absolute():
        return Verdict(EXIT_INCONCLUSIVE, "plugin_path is not absolute", receipt)
    if receipt.get("host") != "REAPER":
        return Verdict(EXIT_INCONCLUSIVE, "PUB-04 requires a real REAPER host receipt", receipt)
    if receipt.get("bundle_id") != EXPECTED_BUNDLE_ID:
        return Verdict(EXIT_INCONCLUSIVE, f"unexpected bundle id: {receipt.get('bundle_id')!r}", receipt)
    missing = [key for key in REQUIRED_TRUE if receipt.get(key) is not True]
    if missing:
        return Verdict(EXIT_INCONCLUSIVE, "missing proof fields: " + ", ".join(missing), receipt)
    if not isinstance(receipt.get("host_parameter_ids"), list) or not receipt["host_parameter_ids"]:
        return Verdict(EXIT_INCONCLUSIVE, "host-observed parameter identity list is missing", receipt)
    if not isinstance(receipt.get("parameter_ids"), list) or not receipt["parameter_ids"]:
        return Verdict(EXIT_INCONCLUSIVE, "parameter identity list is missing", receipt)
    if receipt.get("host_parameter_ids") != receipt.get("parameter_ids"):
        return Verdict(EXIT_FAIL, "host parameter identity was not bound to receipt order", receipt)
    if receipt.get("parameter_ids") != receipt.get("parameter_order"):
        return Verdict(EXIT_FAIL, "parameter identity/order mismatch", receipt)
    if receipt.get("pdc_samples") not in (0, 0.0):
        return Verdict(EXIT_FAIL, f"non-zero PDC reported: {receipt.get('pdc_samples')}", receipt)
    if not isinstance(receipt.get("audio_peak"), (int, float)) or receipt["audio_peak"] <= 0:
        return Verdict(EXIT_INCONCLUSIVE, "audio proof has no positive measured peak", receipt)
    if receipt.get("reload_generation") != receipt.get("saved_generation"):
        return Verdict(EXIT_FAIL, "reload generation did not restore saved state", receipt)
    if not receipt.get("state_hash_equal") or not receipt.get("state_before_sha256") or not receipt.get("state_after_sha256"):
        return Verdict(EXIT_INCONCLUSIVE, "state chunk hashes are missing or unequal", receipt)
    if not receipt.get("wav_exists") or not receipt.get("wav_sha256") or receipt.get("audio_oracle_pass") is not True:
        return Verdict(EXIT_INCONCLUSIVE, "canonical processed audio oracle evidence is missing", receipt)
    if not isinstance(receipt.get("automation_points"), list) or len(receipt["automation_points"]) < 2:
        return Verdict(EXIT_INCONCLUSIVE, "host automation observations are missing", receipt)
    if not receipt.get("pdc_api"):
        return Verdict(EXIT_INCONCLUSIVE, "host PDC query source is missing", receipt)
    return Verdict(EXIT_PASS, f"PASS {fmt}: signed installed REAPER host proof", receipt)


def analyze_output(text: str, *, expected_format: str | None = None) -> Verdict:
    receipts = _json_lines(text)
    if not receipts:
        return Verdict(EXIT_INCONCLUSIVE, "no [sample-region-f4] receipt emitted")
    return validate_receipt(receipts[-1], expected_format=expected_format)


def run_driver(command: str, *, timeout: float, expected_format: str | None = None) -> Verdict:
    try:
        proc = subprocess.run(
            shlex.split(command), capture_output=True, text=True, timeout=timeout,
            env=os.environ.copy(), check=False,
        )
    except FileNotFoundError as exc:
        return Verdict(EXIT_INCONCLUSIVE, f"host driver unavailable: {exc}")
    except subprocess.TimeoutExpired:
        return Verdict(EXIT_INCONCLUSIVE, "real-host driver timed out")
    output = proc.stdout + "\n" + proc.stderr
    verdict = analyze_output(output, expected_format=expected_format)
    if proc.returncode and verdict.code == EXIT_PASS:
        return Verdict(EXIT_FAIL, f"host driver exited {proc.returncode}", verdict.receipt)
    return verdict


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__)
    source = ap.add_mutually_exclusive_group(required=True)
    source.add_argument("--receipt", type=Path, help="captured REAPER driver output")
    source.add_argument("--run-command", help="exact host-driver command to execute")
    ap.add_argument("--format", choices=FORMATS, required=True)
    ap.add_argument("--timeout", type=float, default=300.0)
    return ap


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.receipt:
        try:
            text = args.receipt.read_text(encoding="utf-8")
        except OSError as exc:
            print(f"INCONCLUSIVE: cannot read receipt: {exc}")
            return EXIT_INCONCLUSIVE
        verdict = analyze_output(text, expected_format=args.format)
    else:
        verdict = run_driver(args.run_command, timeout=args.timeout,
                             expected_format=args.format)
    print(verdict.reason)
    return verdict.code


if __name__ == "__main__":
    raise SystemExit(main())
