#!/usr/bin/env python3

import argparse
import json
import subprocess
import sys


SCENARIOS = {
    "baseline": ("shared_io_oracle", True, 11, (11, 11, 11, 0, 22, 22)),
    "wrong-output": ("oracle_negative_control_detected", False, 1, (2, 2, 1, 0, 4, 4)),
    "refuse-allocation": ("expected_prepare_refusal", None, 0, (1, 1, 0, 0, 2, 2)),
    "refuse-input-import": ("expected_prepare_refusal", None, 0, (2, 2, 0, 0, 2, 4)),
    "refuse-output-import": ("expected_prepare_refusal", None, 0, (2, 2, 0, 0, 3, 4)),
    "reject-before-submit": ("expected_submit_refusal", None, 0, (2, 2, 0, 0, 4, 4)),
    "poison-after-submit": (
        "expected_failure_quarantine_and_drain", None, 1, (2, 2, 0, 1, 4, 4)
    ),
    "delay-completion": (
        "expired_delivery_late_clean_drain", None, 1, (2, 2, 1, 0, 4, 4)
    ),
    "plant-write-buffer": (
        "transfer_counter_negative_control_detected", True, 1, (2, 2, 1, 0, 4, 4)
    ),
    "plant-copy-buffer": (
        "transfer_counter_negative_control_detected", True, 1, (2, 2, 1, 0, 4, 4)
    ),
    "plant-map-async": (
        "transfer_counter_negative_control_detected", True, 1, (2, 2, 1, 0, 4, 4)
    ),
    "invalid-command-after-submit": (
        "expected_failure_quarantine_and_drain", None, 1, (2, 2, 0, 1, 4, 4)
    ),
    "synthetic-queue-error": (
        "expected_failure_quarantine_and_drain", None, 1, (2, 2, 0, 1, 4, 4)
    ),
    "synthetic-queue-cancel": (
        "expected_failure_quarantine_and_drain", None, 1, (2, 2, 0, 1, 4, 4)
    ),
    "force-loss-before-submit": (
        "expected_failure_quarantine_and_drain", None, 1, (2, 2, 0, 1, 4, 4)
    ),
    "force-loss-between-submit-and-registration": (
        "expected_failure_quarantine_and_drain", None, 1, (2, 2, 0, 1, 4, 4)
    ),
    "force-loss-after-registration": (
        "expected_failure_quarantine_and_drain", None, 1, (2, 2, 0, 1, 4, 4)
    ),
    "terminal-busy-retry": (
        "terminal_busy_retried_by_drain", None, 1, (2, 2, 1, 0, 4, 4)
    ),
    "real-device-repetition": ("shared_io_oracle", True, 64, (128, 128, 64, 0, 256, 256)),
    "native-input-oom": ("expected_prepare_refusal", None, 0, (2, 2, 0, 0, 2, 4)),
    "native-output-oom": ("expected_prepare_refusal", None, 0, (2, 2, 0, 0, 3, 4)),
}


def run(probe: str, scenario: str, timeout: float) -> dict:
    completed = subprocess.run(
        [probe, "--strict", f"--scenario={scenario}"],
        check=False,
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"{scenario}: exit={completed.returncode} stdout={completed.stdout!r} "
            f"stderr={completed.stderr!r}"
        )
    lines = [line for line in completed.stdout.splitlines() if line.strip()]
    if len(lines) != 1:
        raise RuntimeError(f"{scenario}: expected one receipt, got {lines!r}")
    receipt = json.loads(lines[0])
    if receipt.get("schema") != "pulp.gpu-dawn-shared-io-provider.v2":
        raise RuntimeError(f"{scenario}: wrong schema")
    if receipt.get("scenario") != scenario or receipt.get("status") != "passed":
        raise RuntimeError(f"{scenario}: failed receipt {receipt!r}")
    expected_reason, expected_oracle, expected_submissions, expected_lifecycle = SCENARIOS[scenario]
    if receipt.get("reason") != expected_reason or receipt.get("oracle") is not expected_oracle:
        raise RuntimeError(f"{scenario}: wrong planted-control disposition {receipt!r}")
    if receipt.get("proc_table_installs") != 1:
        raise RuntimeError(f"{scenario}: proc table was not installed exactly once")
    if receipt.get("render_concurrency") != "not_exercised":
        raise RuntimeError(f"{scenario}: render-concurrency boundary missing")
    if (
        receipt.get("architecture") != "arm64"
        or receipt.get("adapter_backend") != "metal"
        or receipt.get("adapter_vendor_id") != 0x106B
        or not receipt.get("hardware_model")
        or not receipt.get("os_release")
        or not receipt.get("adapter_name")
    ):
        raise RuntimeError(f"{scenario}: physical machine/adapter receipt incomplete {receipt!r}")
    transfer_fields = (
        "write_buffer_calls",
        "write_buffer_bytes",
        "copy_buffer_to_buffer_calls",
        "copy_buffer_to_buffer_bytes",
        "map_async_calls",
        "map_async_bytes",
    )
    expected_transfer = {
        "plant-write-buffer": (1, 4, 0, 0, 0, 0),
        "plant-copy-buffer": (0, 0, 1, 4, 0, 0),
        "plant-map-async": (0, 0, 0, 0, 1, 4),
    }.get(scenario, (0, 0, 0, 0, 0, 0))
    actual_transfer = tuple(receipt.get(field) for field in transfer_fields)
    if actual_transfer != expected_transfer:
        raise RuntimeError(
            f"{scenario}: transfer counts {actual_transfer!r} != {expected_transfer!r}"
        )
    if (
        receipt.get("expected_submissions") != expected_submissions
        or receipt.get("queue_submit_calls") != expected_submissions
        or receipt.get("submitted_command_buffers") != expected_submissions
    ):
        raise RuntimeError(f"{scenario}: submission accounting mismatch {receipt!r}")
    lifecycle = tuple(
        receipt.get(field)
        for field in (
            "slots_created",
            "slots_destroyed",
            "retired_success",
            "retired_failure",
            "disposals_observed",
            "host_frees",
        )
    )
    if lifecycle != expected_lifecycle:
        raise RuntimeError(
            f"{scenario}: lifecycle accounting {lifecycle!r} != {expected_lifecycle!r}"
        )
    expected_imports = {
        "baseline": (22, 22, 22),
        "real-device-repetition": (256, 256, 256),
        "refuse-allocation": (2, 2, 2),
        "refuse-input-import": (4, 2, 2),
        "refuse-output-import": (4, 3, 3),
        "native-input-oom": (4, 3, 2),
        "native-output-oom": (4, 4, 3),
    }.get(scenario, (4, 4, 4))
    actual_imports = tuple(
        receipt.get(field) for field in ("allocations", "import_attempts", "import_successes")
    )
    if actual_imports != expected_imports:
        raise RuntimeError(f"{scenario}: import-stage accounting mismatch {receipt!r}")
    expected_busy_retries = 1 if scenario == "terminal-busy-retry" else 0
    if receipt.get("terminal_busy_retries") != expected_busy_retries:
        raise RuntimeError(f"{scenario}: terminal Busy retry evidence mismatch {receipt!r}")
    expected_fault_injections = 1 if scenario.startswith("native-") else 0
    if receipt.get("fault_injections") != expected_fault_injections:
        raise RuntimeError(f"{scenario}: native fault injection accounting mismatch {receipt!r}")
    expected_drains = (
        12 if scenario == "baseline"
        else 128 if scenario == "real-device-repetition"
        else 3 if scenario.startswith("refuse-") or scenario.startswith("native-")
        else 2
    )
    if receipt.get("drain_calls") != expected_drains or receipt.get("failed_drains") != 0:
        raise RuntimeError(f"{scenario}: two-phase drain accounting mismatch {receipt!r}")
    return receipt


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    args = parser.parse_args()

    try:
        for scenario in SCENARIOS:
            run(args.probe, scenario, 20.0)

        try:
            subprocess.run(
                [args.probe, "--strict", "--scenario=watchdog-hang"],
                check=False,
                capture_output=True,
                text=True,
                timeout=0.25,
            )
        except subprocess.TimeoutExpired:
            pass
        else:
            raise RuntimeError("watchdog-hang: planted hang escaped the child watchdog")

        # The killed negative-control child must not poison a fresh exact-provider child.
        run(args.probe, "baseline", 20.0)
    except (json.JSONDecodeError, OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"gpu shared-I/O verifier failed: {error}", file=sys.stderr)
        return 1

    print(
        f"gpu shared-I/O verifier passed: {len(SCENARIOS)} scenarios plus in-flight watchdog control"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
