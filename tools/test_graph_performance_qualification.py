#!/usr/bin/env python3
"""Independent corruption controls for the bounded G-12/R2 receipt."""
from __future__ import annotations

import copy
import importlib.util
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("graph_performance_qualification",
                                              ROOT / "tools" / "graph_performance_qualification.py")
assert SPEC and SPEC.loader
gq = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gq)

# Authored fixture counts, independent of the validator's topology arithmetic.
LAYOUTS = (("passthrough", 1, 3, 2, 4, 3), ("chain", 1, 3, 2, 4, 3),
           ("chain", 4, 6, 5, 10, 3), ("chain", 16, 18, 17, 34, 3),
           ("chain", 64, 66, 65, 130, 3), ("fan-out", 4, 6, 8, 10, 6),
           ("fan-out", 16, 18, 32, 34, 18), ("fan-out", 64, 66, 128, 130, 66))


def write_receipt(path: Path, receipt: dict) -> None:
    receipt["receipt_sha256"] = gq.receipt_digest(receipt)
    path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")


def fake_receipt(directory: Path) -> Path:
    source = directory / "source-fixture.cpp"
    source.write_text("fixture source\n")
    executable = directory / "benchmark-executable"
    executable.write_bytes(b"retained executable fixture")
    stdout = directory / "raw-output.txt"
    stderr = directory / "raw-stderr.txt"
    stderr.write_text("")
    lines = [gq.FIXTURE_VERSION, "graph_direct_copy_benchmark_v2 timer_overhead_ns=1.000"]
    samples = ",".join(map(str, range(1, 32)))
    for shape, scale, nodes, edges, logical, slots in LAYOUTS:
        for block in (16, 32, 64, 128, 512):
            prefix = f"shape={shape} scale={scale} block={block}"
            phases = " ".join(f"{phase}_samples_ns={samples} {phase}_allocation_bytes=" +
                              ",".join([str(slots * block * 4 if phase == "pool_allocate" else 100)] * 31)
                              for phase in ("compile", "snapshot_prepare", "pool_allocate"))
            lines.append(f"{prefix} {phases} node_count={nodes} edge_count={edges} "
                         f"logical_port_slots={logical} no_reuse_slots={logical} scratch_slots={slots} "
                         f"reused_slots={logical - slots} sample_bytes=4 scratch_payload_bytes={slots * block * 4} "
                         f"no_reuse_payload_bytes={logical * block * 4}")
            lines.append(f"{prefix} warmup=500 iterations=100000 samples=31 "
                         f"batch_mean_samples_ns={samples} oracle=pass process_allocations=0 "
                         f"timed_allocations=0 timed_allocation_samples=31 "
                         f"output_bits=0x{gq.expected_output_bits(shape, scale):x}")
    stdout.write_text("\n".join(lines) + "\n")
    cells = gq.parse_output(stdout.read_text())
    for cell in cells:
        cell.pop("timer_overhead_ns")
        cell["expected_output_bits"] = gq.expected_output_bits(cell["shape"], cell["scale"])
    receipt = {
        "schema": gq.SCHEMA, "fixture": gq.FIXTURE_VERSION, "status": "PARTIAL",
        "measurement_contract": gq.MEASUREMENT_CONTRACT,
        "source_sha": "a" * 40,
        "source_fixture": {"path": "test/test_graph_r2_measurement.cpp", "sha256": gq.sha256_file(source)},
        "executable": "build/test/pulp-graph-r2-measurement", "executable_sha256": gq.sha256_file(executable),
        "build_profile": {"CMAKE_BUILD_TYPE": "Release", "PULP_ENABLE_GPU": "OFF",
                          "PULP_BUILD_EXAMPLES": "OFF", "PULP_GRAPH_DIRECT_COPY_BENCHMARK": "ON"},
        "matrix": {"shapes": list(gq.SHAPES), "scales": list(gq.SCALES), "blocks": list(gq.BLOCKS),
                   "expected_cell_count": 40, "cells": cells},
        "run": {"returncode": 0, "warmup_blocks": 500, "timed_blocks": 100000,
                "timing_samples": 31, "timer_overhead_ns": 1.0},
        "artifacts": {name: {"path": path.name, "sha256": gq.sha256_file(path)}
                      for name, path in (("raw_stdout", stdout), ("raw_stderr", stderr),
                                         ("source_fixture", source), ("executable", executable))},
    }
    path = directory / "receipt.json"
    write_receipt(path, receipt)
    return path


class GraphPerformanceQualification(unittest.TestCase):
    def test_complete_fixture_and_independent_summary(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = fake_receipt(Path(temporary))
            gq.validate_receipt(path)
            receipt = json.loads(path.read_text())
            self.assertEqual(len(receipt["matrix"]["cells"]), 40)
            timing = receipt["matrix"]["cells"][0]["timing"]
            self.assertEqual({key: timing[key] for key in gq.SUMMARY_FIELDS},
                             {"min_ns": 1, "p50_ns": 16, "p99_sample_ns": 31, "max_ns": 31, "spread_ns": 30})
            self.assertEqual(gq.expected_memory("fan-out", 64, 512)["scratch_payload_bytes"], 135168)
            self.assertEqual(gq.expected_memory("chain", 64, 512)["reused_slots"], 127)

    def test_receipt_and_resigned_planted_negatives(self):
        with tempfile.TemporaryDirectory() as temporary:
            gq.run_controls(fake_receipt(Path(temporary)))

    def test_rejects_fully_resigned_raw_memory_and_reuse_falsehoods(self):
        for field, value in (("node_count", 0), ("edge_count", 0), ("logical_port_slots", 999),
                             ("no_reuse_slots", 0), ("scratch_slots", 0), ("reused_slots", 0),
                             ("sample_bytes", 8), ("scratch_payload_bytes", 0),
                             ("no_reuse_payload_bytes", 0)):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as temporary:
                path = fake_receipt(Path(temporary))
                receipt = json.loads(path.read_text())
                target = receipt["matrix"]["cells"][0]
                target["memory"][field] = value
                raw = path.parent / "raw-output.txt"
                lines = raw.read_text().splitlines()
                for i, line in enumerate(lines):
                    if line.startswith("shape=chain scale=1 block=16 ") and "oracle=" not in line:
                        tokens = line.split()
                        lines[i] = " ".join(f"{field}={value}" if token.startswith(field + "=") else token
                                            for token in tokens)
                raw.write_text("\n".join(lines) + "\n")
                receipt["artifacts"]["raw_stdout"]["sha256"] = gq.sha256_file(raw)
                write_receipt(path, receipt)
                with self.assertRaisesRegex(gq.ReceiptError, "memory/reuse topology"):
                    gq.validate_receipt(path)

    def test_all_required_raw_fields_and_cells_are_enforced(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = fake_receipt(Path(temporary))
            original = (path.parent / "raw-output.txt").read_text()
            lines = original.splitlines()
            for row in (2, 3):
                for token in lines[row].split():
                    with self.subTest(token=token.split("=")[0]):
                        changed = lines.copy()
                        changed[row] = changed[row].replace(token, "", 1)
                        with self.assertRaises(gq.ReceiptError):
                            gq.parse_output("\n".join(changed))
            with self.assertRaisesRegex(gq.ReceiptError, "incomplete"):
                gq.parse_output("\n".join(lines[:2] + lines[4:]))
            with self.assertRaisesRegex(gq.ReceiptError, "duplicate"):
                gq.parse_output(original + lines[2] + "\n")
            with self.assertRaisesRegex(gq.ReceiptError, "duplicate raw field"):
                gq.parse_output(original.replace("scratch_slots=3", "scratch_slots=3 scratch_slots=4", 1))

    def test_timing_samples_and_summaries_cannot_be_resigned_away(self):
        for section in ("timing", *gq.PHASES):
            for invalid in (-1, float("nan"), float("inf"), True):
                with self.subTest(section=section, invalid=invalid), tempfile.TemporaryDirectory() as temporary:
                    path = fake_receipt(Path(temporary))
                    receipt = json.loads(path.read_text())
                    cell = receipt["matrix"]["cells"][0]
                    measured = cell[section] if section == "timing" else cell["preparation"][section]
                    measured["batch_mean_samples_ns" if section == "timing" else "samples_ns"][0] = invalid
                    write_receipt(path, receipt)
                    with self.assertRaisesRegex(gq.ReceiptError, "finite and nonnegative"):
                        gq.validate_receipt(path)
            with tempfile.TemporaryDirectory() as temporary:
                path = fake_receipt(Path(temporary))
                receipt = json.loads(path.read_text())
                cell = receipt["matrix"]["cells"][0]
                measured = cell[section] if section == "timing" else cell["preparation"][section]
                measured["p50_ns"] = 15
                write_receipt(path, receipt)
                with self.assertRaisesRegex(gq.ReceiptError, "summary mismatch"):
                    gq.validate_receipt(path)

    def test_source_commit_and_executable_binding(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            path = fake_receipt(directory)
            source = directory / "test" / "test_graph_r2_measurement.cpp"
            source.parent.mkdir()
            source.write_text("fixture source\n")
            subprocess.run(["git", "init", "-q", str(directory)], check=True)
            subprocess.run(["git", "-C", str(directory), "add", "test"], check=True)
            subprocess.run(["git", "-C", str(directory), "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                            "-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null", "commit", "-qm", "fixture"], check=True)
            receipt = json.loads(path.read_text())
            receipt["source_sha"] = gq._source_sha(directory)
            write_receipt(path, receipt)
            gq.validate_receipt(path, directory)
            changed = copy.deepcopy(receipt)
            fixture = directory / "source-fixture.cpp"
            fixture.write_text("different fixture\n")
            changed["source_fixture"]["sha256"] = gq.sha256_file(fixture)
            changed["artifacts"]["source_fixture"]["sha256"] = gq.sha256_file(fixture)
            write_receipt(path, changed)
            with self.assertRaisesRegex(gq.ReceiptError, "source commit"):
                gq.validate_receipt(path, directory)
            fixture.write_text("fixture source\n")
            write_receipt(path, receipt)
            binary = directory / "benchmark-executable"
            binary.write_bytes(b"different executable")
            receipt["artifacts"]["executable"]["sha256"] = gq.sha256_file(binary)
            write_receipt(path, receipt)
            with self.assertRaisesRegex(gq.ReceiptError, "executable artifact hash"):
                gq.validate_receipt(path)

    def test_contract_does_not_accept_callback_or_live_activation_claims(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = fake_receipt(Path(temporary))
            receipt = json.loads(path.read_text())
            receipt["measurement_contract"]["timing"] = "callback p99 and WCET"
            write_receipt(path, receipt)
            with self.assertRaisesRegex(gq.ReceiptError, "contract"):
                gq.validate_receipt(path)


if __name__ == "__main__":
    unittest.main()
