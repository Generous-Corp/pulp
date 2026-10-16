#!/usr/bin/env python3
"""The real pinned image must reject a modified linked-runtime control."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from verify_wgpu_native_staged import (diagnostic_exit_disposition, parse_output,
                                       validate_loss_evidence)


ADAPTER_SOURCE = Path(__file__).with_name("pulp_wgpu_native_staged_adapter.cpp")
VERIFIER_SOURCE = Path(__file__).with_name("verify_wgpu_native_staged.py")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--typed-only", action="store_true")
    for name in ("manifest", "asset", "source-tree", "linked-runtime", "executable",
                 "verifier"):
        parser.add_argument(f"--{name}")
    args = parser.parse_args()
    for returncode, expected in ((0, "passed"), (77, "provider_unavailable"),
                                 (16, "diagnostic_failure")):
        if diagnostic_exit_disposition(returncode) != expected:
            print(f"typed exit disposition control failed: {returncode}", file=sys.stderr)
            return 1
    valid = parse_output(
        "device_loss_callback=registered device_loss_scenario=triggered "
        "device_loss_reason=destroyed device_loss_terminal_records=1 "
        "device_loss_no_output_leases=true device_loss_reprime=unavailable\n"
        "device_loss_proof=callback_terminal_no_output_lease "
        "callback_fallback_proof=unavailable\n")
    if validate_loss_evidence(valid) != 1:
        print("typed loss evidence positive control failed", file=sys.stderr)
        return 1
    for field, value in (("device_loss_terminal_records", "0"),
                         ("device_loss_no_output_leases", "false"),
                         ("device_loss_reprime", "reprimed"),
                         ("device_loss_proof", "other")):
        invalid = dict(valid)
        invalid[field] = value
        try:
            validate_loss_evidence(invalid)
        except ValueError:
            pass
        else:
            print(f"typed loss evidence negative control failed: {field}", file=sys.stderr)
            return 1
    if args.typed_only:
        source = ADAPTER_SOURCE.read_text(encoding="utf-8")
        verifier_source = VERIFIER_SOURCE.read_text(encoding="utf-8")
        if ("run_loss_capability_probe()" not in source or
                "loss_probe_provider.release();" not in source or
                "kill(child, SIGKILL);" not in source or
                "poll(&descriptor, 1, 8'000)" not in source or
                "setpgid(0, 0)" not in source or
                "kill(-child, SIGKILL)" not in source or
                'open("/dev/null", O_WRONLY)' not in source or
                "FD_CLOEXEC" not in source or
                "O_NONBLOCK" not in source or
                "PR_SET_PDEATHSIG" not in source or
                "getppid() == 1" not in source or
                "wait_for_child(child, child_status)" not in source):
            print("typed unavailable probe watchdog control failed", file=sys.stderr)
            return 1
        if ("start_new_session=True" not in verifier_source or
                "os.killpg(process.pid, signal.SIGKILL)" not in verifier_source):
            print("verifier timeout process-group control failed", file=sys.stderr)
            return 1
        if "const auto abandon_loss_session" not in source or source.count(
            "return abandon_loss_session();"
        ) < 4:
            print("typed loss-session teardown control failed", file=sys.stderr)
            return 1
        if "loss_session.release();" not in source:
            print("typed successful-loss teardown control failed", file=sys.stderr)
            return 1
        if ("auto session = std::make_unique<SharedIoProgramSession>();" not in source or
                "const auto fail_session" not in source or
                source.count("return fail_session(") < 10 or
                "session.release();" not in source or
                "provider.release();" not in source):
            print("typed initial-session teardown control failed", file=sys.stderr)
            return 1
        if "loss_provider.release();" not in source:
            print("typed loss-provider teardown control failed", file=sys.stderr)
            return 1
        if ("const auto loss_probe_result = run_loss_capability_probe();" not in source or
                "loss_probe_result.callback_registered" not in source or
                "loss_probe_result.reason" not in source):
            print("typed capability-probe result control failed", file=sys.stderr)
            return 1
        return 0
    if any(getattr(args, name.replace("-", "_")) is None
           for name in ("manifest", "asset", "source-tree", "linked-runtime",
                        "executable", "verifier")):
        parser.error("all identity-control arguments are required unless --typed-only is used")
    with tempfile.TemporaryDirectory(prefix="pulp-wgpu-identity-negative-") as scratch:
        mutated = Path(scratch) / Path(args.linked_runtime).name
        receipt = Path(scratch) / "receipt.json"
        shutil.copy2(args.linked_runtime, mutated)
        with mutated.open("ab") as stream:
            stream.write(b"planted-runtime-mutation")
        run = subprocess.run(
            [sys.executable, args.verifier, "--manifest", args.manifest,
             "--asset", args.asset, "--source-tree", args.source_tree,
             "--linked-runtime", str(mutated), "--executable", args.executable,
             "--receipt", str(receipt)],
            capture_output=True, text=True, encoding="utf-8", timeout=30, check=False)
        if (run.returncode != 1 or "linked_runtime_not_pinned" not in run.stderr or
                receipt.exists()):
            print(f"negative control failed: {run.returncode} {run.stderr}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
