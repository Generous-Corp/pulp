#!/usr/bin/env python3
"""Self-tests for the bounded G-12/R2 receipt wrapper."""

from __future__ import annotations

import copy
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("graph_performance_qualification",
                                              ROOT / "tools" / "graph_performance_qualification.py")
assert SPEC and SPEC.loader
gq = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gq)


def fake_receipt(directory: Path) -> Path:
    stdout = directory / "raw-output.txt"
    stderr = directory / "raw-stderr.txt"
    stdout.write_text("graph_r2_measurement_v1\n", encoding="utf-8")
    stderr.write_text("", encoding="utf-8")
    cells = []
    for shape, scale, block in sorted(gq.EXPECTED_KEYS):
        output_bits = gq.expected_output_bits(shape, scale)
        cells.append({
            "shape": shape, "scale": scale, "block": block,
            "activation": {"activation_p50_ns": 1.0, "activation_p99_sample_ns": 2.0,
                            "activation_max_ns": 3.0},
            "timing": {"p50_ns": 1.0, "p99_sample_ns": 2.0, "max_ns": 3.0,
                       "jitter_ns": 1.0, "observed_wcet_proxy_ns": 3.0},
            "warmup": 500, "iterations": 100000, "samples": 31,
            "oracle": "pass", "process_allocations": 0,
            "output_bits": output_bits, "expected_output_bits": output_bits,
        })
    receipt = {
        "schema": gq.SCHEMA,
        "fixture": gq.FIXTURE_VERSION,
        "status": "PARTIAL",
        "qualification_scope": "R2 bounded graph runtime measurement",
        "qualification_boundary": "bounded slice only",
        "source_sha": "a" * 40,
        "executable": "build/test/pulp-graph-r2-measurement",
        "executable_sha256": "b" * 64,
        "platform": {"system": "test", "release": "test", "machine": "test"},
        "compiler": {"path": "test", "id": "test", "version": "test"},
        "build_profile": {"CMAKE_BUILD_TYPE": "Release", "PULP_ENABLE_GPU": "OFF",
                           "PULP_BUILD_EXAMPLES": "OFF",
                           "PULP_GRAPH_DIRECT_COPY_BENCHMARK": "ON"},
        "matrix": {"shapes": list(gq.SHAPES), "scales": list(gq.SCALES),
                    "blocks": list(gq.BLOCKS), "expected_cell_count": 40,
                    "cells": cells},
        "run": {"returncode": 0, "warmup_blocks": 500, "timed_blocks": 100000,
                "timing_samples": 31},
        "artifacts": {
            "raw_stdout": {"path": stdout.name, "sha256": gq.sha256_file(stdout)},
            "raw_stderr": {"path": stderr.name, "sha256": gq.sha256_file(stderr)},
        },
    }
    receipt["receipt_sha256"] = gq.receipt_digest(receipt)
    path = directory / "receipt.json"
    path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return path


class GraphPerformanceQualification(unittest.TestCase):
    def test_expected_matrix_has_forty_cells(self):
        self.assertEqual(len(gq.EXPECTED_KEYS), 40)
        self.assertEqual(len(gq.parse_output("\n".join(
            ["shape=%s scale=%d block=%d activation_p50_ns=1 activation_p99_sample_ns=2 "
             "activation_max_ns=3 scratch_slots=1" % key for key in sorted(gq.EXPECTED_KEYS)] +
            ["shape=%s scale=%d block=%d warmup=500 iterations=100000 samples=31 p50_ns=1 "
             "p99_sample_ns=2 max_ns=3 jitter_ns=1 observed_wcet_proxy_ns=3 oracle=pass "
             "process_allocations=0 output_bits=0x%x" % (*key, gq.expected_output_bits(key[0], key[1]))
             for key in sorted(gq.EXPECTED_KEYS)]))), 40)

    def test_receipt_and_resigned_planted_negatives(self):
        with tempfile.TemporaryDirectory(prefix="pulp-r2-test-") as temporary:
            receipt_path = fake_receipt(Path(temporary))
            gq.validate_receipt(receipt_path)
            gq.run_controls(receipt_path)

    def test_mutated_output_and_allocation_are_rejected_semantically(self):
        with tempfile.TemporaryDirectory(prefix="pulp-r2-test-") as temporary:
            directory = Path(temporary)
            receipt_path = fake_receipt(directory)
            receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
            for cell, field, value in ((receipt["matrix"]["cells"][0], "expected_output_bits",
                                         receipt["matrix"]["cells"][0]["expected_output_bits"] ^ 1),
                                        (receipt["matrix"]["cells"][0], "process_allocations", 1)):
                mutated = copy.deepcopy(receipt)
                mutated["matrix"]["cells"][0][field] = value
                mutated["receipt_sha256"] = gq.receipt_digest(mutated)
                receipt_path.write_text(json.dumps(mutated, indent=2, sort_keys=True) + "\n",
                                        encoding="utf-8")
                with self.assertRaisesRegex(gq.ReceiptError,
                                           "(fixture expected output|allocations)"):
                    gq.validate_receipt(receipt_path)


if __name__ == "__main__":
    unittest.main()
