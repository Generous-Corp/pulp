#!/usr/bin/env python3
"""Validate the physical paced probe and its planted numerical failure."""

import argparse
import csv
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="pulp-paced-probe-control-") as parent:
        for negative in (False, True):
            directory = Path(parent) / ("negative" if negative else "baseline")
            command = [args.probe, "--blocks=16", "--warmup=8", "--lead=2",
                       f"--output-dir={directory}"]
            if negative:
                command.append("--negative-control")
            result = subprocess.run(command, text=True, capture_output=True, timeout=60)
            assert result.returncode == int(negative), (result.returncode, result.stdout, result.stderr)
            receipt = json.loads((directory / "receipt.json").read_text())
            assert receipt["schema"] == "pulp.gpu-audio-paced-convolution.v1"
            assert receipt["status"] == ("failed" if negative else "completed")
            assert receipt["performance_verdict"] == "unassigned"
            assert receipt["negative_control"] == negative
            assert receipt["oracle_failed_blocks"] == int(negative)
            assert receipt["produced_blocks_before_stop"] > 0
            assert receipt["run_identity"]
            assert receipt["declared_slots"] == receipt["provider_slots"] > 0
            assert receipt["declared_lead_blocks"] == receipt["lead_blocks"] == 2
            assert 0 < receipt["high_water_in_flight"] <= receipt["declared_slots"]
            assert receipt["terminal_records"] == receipt["retired_success"] + receipt["retired_failure"]
            assert receipt["retired_success"] > 0
            assert receipt["authenticated_terminal_records"] == receipt["terminal_records"]
            assert receipt["gpu_receipt_authenticated"] is True
            assert receipt["terminal_record_count"] == receipt["admissions_enqueued"]
            assert receipt["admissions_attempted"] == receipt["admissions_enqueued"] + receipt["admissions_dropped"]
            assert receipt["admissions_dropped"] == 0
            assert receipt["trace_dropped"] == 0
            assert receipt["trace_attempted"] == (receipt["trace_enqueued"] + receipt["trace_dropped"] +
                                                    receipt["trace_sampled_out"] + receipt["trace_invalid"])
            assert receipt["fallback_blocks"] >= receipt["miss_blocks"]
            assert receipt["late_completions"] >= 0
            with (directory / "blocks.csv").open() as stream:
                records = list(csv.DictReader(stream))
            assert len(records) == receipt["total_callbacks"] == 26
            measured = [row for row in records if row["measured"] == "1"]
            assert len(measured) == 16
            assert sum(int(row["miss_counter_delta"]) for row in measured) == receipt["measured_miss_counter_delta"]
            for ordinal, row in enumerate(records):
                assert int(row["callback_sequence"]) == ordinal
                assert row["source_sequence"] == (str(ordinal - 2) if ordinal >= 2 else "")
                assert int(row["scheduled_ns"]) <= int(row["callback_begin_ns"]) <= int(row["callback_end_ns"])
                assert row["finite"] == "1"
        long_directory = Path(parent) / "long"
        long_result = subprocess.run(
            [args.probe, "--blocks=1000", "--warmup=64", "--lead=2",
             f"--output-dir={long_directory}"],
            text=True, capture_output=True, timeout=120)
        assert long_result.returncode == 0, (long_result.returncode, long_result.stdout,
                                             long_result.stderr)
        long_receipt = json.loads((long_directory / "receipt.json").read_text())
        assert long_receipt["status"] == "completed"
        assert long_receipt["measured_blocks"] == 1000
        assert long_receipt["terminal_records"] > 256
        assert long_receipt["terminal_records"] == long_receipt["admissions_enqueued"]
        assert long_receipt["admissions_dropped"] == 0
        assert long_receipt["trace_dropped"] == 0
        assert long_receipt["gpu_receipt_authenticated"] is True
    print("paced convolution probe and numerical negative control passed")


if __name__ == "__main__":
    main()
