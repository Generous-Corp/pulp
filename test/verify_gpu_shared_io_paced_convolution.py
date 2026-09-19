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
    print("paced convolution probe and numerical negative control passed")


if __name__ == "__main__":
    main()
