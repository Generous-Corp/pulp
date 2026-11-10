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
    path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def fake_receipt(directory: Path) -> Path:
    source = directory / "source-fixture.cpp"
    source.write_text("fixture source\n", encoding="utf-8")
    executable = directory / "benchmark-executable"
    executable.write_bytes(b"retained executable fixture")
    stdout = directory / "raw-output.txt"
    stderr = directory / "raw-stderr.txt"
    stderr.write_text("", encoding="utf-8")
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
    stdout.write_text("\n".join(lines) + "\n", encoding="utf-8")
    cells = gq.parse_output(stdout.read_text(encoding="utf-8"))
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
            receipt = json.loads(path.read_text(encoding="utf-8"))
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
                receipt = json.loads(path.read_text(encoding="utf-8"))
                target = receipt["matrix"]["cells"][0]
                target["memory"][field] = value
                raw = path.parent / "raw-output.txt"
                lines = raw.read_text(encoding="utf-8").splitlines()
                for i, line in enumerate(lines):
                    if line.startswith("shape=chain scale=1 block=16 ") and "oracle=" not in line:
                        tokens = line.split()
                        lines[i] = " ".join(f"{field}={value}" if token.startswith(field + "=") else token
                                            for token in tokens)
                raw.write_text("\n".join(lines) + "\n", encoding="utf-8")
                receipt["artifacts"]["raw_stdout"]["sha256"] = gq.sha256_file(raw)
                write_receipt(path, receipt)
                with self.assertRaisesRegex(gq.ReceiptError, "memory/reuse topology"):
                    gq.validate_receipt(path)

    def test_all_required_raw_fields_and_cells_are_enforced(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = fake_receipt(Path(temporary))
            original = (path.parent / "raw-output.txt").read_text(encoding="utf-8")
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
                    receipt = json.loads(path.read_text(encoding="utf-8"))
                    cell = receipt["matrix"]["cells"][0]
                    measured = cell[section] if section == "timing" else cell["preparation"][section]
                    measured["batch_mean_samples_ns" if section == "timing" else "samples_ns"][0] = invalid
                    write_receipt(path, receipt)
                    with self.assertRaisesRegex(gq.ReceiptError, "finite and nonnegative"):
                        gq.validate_receipt(path)
            with tempfile.TemporaryDirectory() as temporary:
                path = fake_receipt(Path(temporary))
                receipt = json.loads(path.read_text(encoding="utf-8"))
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
            source.write_text("fixture source\n", encoding="utf-8")
            subprocess.run(["git", "init", "-q", str(directory)], check=True)
            subprocess.run(["git", "-C", str(directory), "add", "test"], check=True)
            subprocess.run(["git", "-C", str(directory), "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                            "-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null", "commit", "-qm", "fixture"], check=True)
            receipt = json.loads(path.read_text(encoding="utf-8"))
            receipt["source_sha"] = gq._source_sha(directory)
            write_receipt(path, receipt)
            gq.validate_receipt(path, directory)
            changed = copy.deepcopy(receipt)
            fixture = directory / "source-fixture.cpp"
            fixture.write_text("different fixture\n", encoding="utf-8")
            changed["source_fixture"]["sha256"] = gq.sha256_file(fixture)
            changed["artifacts"]["source_fixture"]["sha256"] = gq.sha256_file(fixture)
            write_receipt(path, changed)
            with self.assertRaisesRegex(gq.ReceiptError, "source commit"):
                gq.validate_receipt(path, directory)
            fixture.write_text("fixture source\n", encoding="utf-8")
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
            receipt = json.loads(path.read_text(encoding="utf-8"))
            receipt["measurement_contract"]["timing"] = "callback p99 and WCET"
            write_receipt(path, receipt)
            with self.assertRaisesRegex(gq.ReceiptError, "contract"):
                gq.validate_receipt(path)




def fake_activation_receipt(directory: Path) -> Path:
    """Literal synthetic pilot: never evidence of actual timing qualification."""
    source = directory / "source-fixture.inc"
    source.write_text("synthetic activation source\n")
    committed_source = directory / "test/fixtures/g5_graph_event_rt_cases.inc"
    committed_source.parent.mkdir(parents=True)
    committed_source.write_bytes(source.read_bytes())
    subprocess.run(["git", "init", "-q", str(directory)], check=True)
    subprocess.run(["git", "-C", str(directory), "add", "test"], check=True)
    subprocess.run(["git", "-C", str(directory), "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                    "-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null", "commit", "-qm", "fixture"], check=True)
    binary = directory / "activation-executable"
    binary.write_bytes(b"synthetic executable for receipt corruption controls")
    def artifact(p):
        return {"path": p.name, "sha256": gq.sha256_file(p)}
    source_sha = gq._source_sha(directory)
    cells = []
    # Literal dimensions and float words are independent of validator constants.
    for rate in (44100, 48000, 96000):
        for frames in (32, 64, 128, 256):
            for backend in ("direct", "routed"):
                rows = [f"G12_ROW index={i} sample={i * frames} "
                        f"target_ns={1000000000 + frames * i * 1000000000 // rate} "
                        f"start_ns={1000000100 + frames * i * 1000000000 // rate} "
                        f"end_ns={1000001100 + frames * i * 1000000000 // rate} "
                        f"audio_class={0 if i < 8 else 1} bits={'3e800000' if i < 8 else '3f000000'}:{frames}"
                        for i in range(16)]
                request = 1000001101 + frames * 7 * 1000000000 // rate
                meta = (f"G12_CELL rate={rate} frames={frames} backend={backend} callbacks=16 "
                        f"compiled_source={source_sha} build_type=Release build_dirty=0 "
                        f"prepare_start_ns=100 prepare_end_ns=200 request_ns={request} return_ns={request + 100} "
                        f"committed=1 first_new=8 first_new_sample={8 * frames} "
                        "latency_samples=0 snapshot_latency=0 snapshot_nodes=3 snapshot_edges=2 "
                        "node_latencies=0,0,0 edge_delays=0,0 nodes=3 edges=2 voices=0 grains=0 probes=0 "
                        f"executor_blocks={16 if backend == 'routed' else 0} parallel_levels=0 parallel_enabled=0 walk_fallbacks=0 execution_failures=0 "
                        f"node_audio_bytes={24 * frames} automation_bytes=0 delay_bytes=0 spectral_bytes=0 "
                        f"prepared_bytes={24 * frames} row_stride=72 row_bytes=1152 audio_bytes={64 * frames} "
                        f"io_bytes={8 * frames} harness_payload_bytes={1152 + 72 * frames} misses=0")
                raw = directory / f"{rate}-{frames}-{backend}.txt"
                raw.write_text("\n".join(["g12_paced_activation_v1", meta, *rows]) + "\n")
                err = raw.with_suffix(".stderr")
                err.write_text("")
                cell = gq.parse_activation_output(raw.read_text())
                cell.pop("callbacks")
                cell.update(returncode=0, raw_stdout=artifact(raw), raw_stderr=artifact(err))
                cells.append(cell)
    receipt = {"schema": "pulp.graph_paced_activation_receipt.v1", "fixture": "g12_paced_activation_v1",
               "status": "PARTIAL", "mode": "pilot", "timing_qualified": False,
               "measurement_contract": copy.deepcopy(gq.ACTIVATION_CONTRACT), "callback_count": 16,
               "source_sha": gq._source_sha(directory),
               "source_fixture": {"path": "test/fixtures/g5_graph_event_rt_cases.inc", "sha256": gq.sha256_file(source)},
               "executable_sha256": gq.sha256_file(binary),
               "build_profile": {"CMAKE_BUILD_TYPE": "Release", "PULP_SANITIZER": ""},
               "matrix": {"rates": [44100, 48000, 96000], "frames": [32, 64, 128, 256],
                          "backends": ["direct", "routed"], "expected_cell_count": 24},
               "artifacts": {"source_fixture": artifact(source), "executable": artifact(binary)}, "cells": cells}
    path = directory / "activation.json"
    write_receipt(path, receipt)
    return path


class PacedActivationQualification(unittest.TestCase):
    def test_complete_pilot_remains_unqualified_with_independent_raw_metrics(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = fake_activation_receipt(Path(tmp))
            gq.validate_receipt(path, path.parent)
            value = json.loads(path.read_text())
            self.assertEqual(len(value["cells"]), 24)
            self.assertFalse(value["timing_qualified"])
            self.assertEqual(value["cells"][0]["metrics"]["p99_duration_ns"], 1000)
            self.assertEqual(value["cells"][0]["metrics"]["p99_absolute_wake_ns"], 100)
            self.assertEqual(value["cells"][0]["metrics"]["deadline_misses"], 0)

    def test_resigned_metadata_summary_matrix_and_provenance_negatives_restore(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = fake_activation_receipt(Path(tmp))
            original = json.loads(path.read_text())
            mutations = [
                ("missing cell", lambda r: r["cells"].pop()),
                ("duplicate cell", lambda r: r["cells"].__setitem__(1, copy.deepcopy(r["cells"][0]))),
                ("short count", lambda r: r.__setitem__("callback_count", 15)),
                ("qualify pilot", lambda r: r.__setitem__("timing_qualified", True)),
                ("short qualify", lambda r: r.update(mode="qualification", timing_qualified=True)),
                ("raw percentile", lambda r: r["cells"][0]["metrics"].__setitem__("p99_duration_ns", 999)),
                ("raw miss", lambda r: r["cells"][0]["metrics"].__setitem__("deadline_misses", 1)),
                ("stale executable", lambda r: r["cells"][0]["metadata"].__setitem__("compiled_source", "0" * 40)),
                ("raw PDC", lambda r: r["cells"][0]["metadata"].__setitem__("snapshot_latency", 1)),
                ("raw memory", lambda r: r["cells"][0]["metadata"].__setitem__("prepared_bytes", 1)),
                ("source", lambda r: r["source_fixture"].__setitem__("sha256", "0" * 64)),
                ("binary", lambda r: r.__setitem__("executable_sha256", "0" * 64)),
                ("source commit", lambda r: r.__setitem__("source_sha", "0" * 40)),
            ]
            for name, mutate in mutations:
                with self.subTest(name=name):
                    changed = copy.deepcopy(original)
                    mutate(changed)
                    write_receipt(path, changed)
                    with self.assertRaises((gq.ReceiptError, subprocess.CalledProcessError)):
                        gq.validate_receipt(path, path.parent)
                    write_receipt(path, original)
                    gq.validate_receipt(path, path.parent)

    def test_resigned_raw_audio_index_schedule_pdc_and_resource_controls(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = fake_activation_receipt(Path(tmp))
            original = json.loads(path.read_text())
            raw_path = path.parent / original["cells"][0]["raw_stdout"]["path"]
            raw = raw_path.read_text()
            mutations = (
                ("wrong gain", "3f000000:32", "3ec00000:32"),
                ("mixed", "3f000000:32", "3e800000:1,3f000000:31"),
                ("silent", "3f000000:32", "0:32"),
                ("stale", "3f000000:32", "3e800000:32"),
                ("wrong sample", "index=8 sample=256", "index=8 sample=257"),
                ("wrong index", "index=8 sample=256", "index=9 sample=256"),
                ("omitted commit", "committed=1", "committed=0"),
                ("wrong transition", "first_new=8 ", "first_new=9 "),
                ("PDC", "node_latencies=0,0,0", "node_latencies=0,1,0"),
                ("actual backend", "executor_blocks=0", "executor_blocks=16"),
                ("dirty source", "build_dirty=0", "build_dirty=1"),
                ("debug", "build_type=Release", "build_type=Debug"),
                ("memory", "node_audio_bytes=768", "node_audio_bytes=769"),
                ("harness", "audio_bytes=2048", "audio_bytes=2049"),
                ("schedule", "target_ns=1000000000", "target_ns=999999900"),
                ("short audio", "3f000000:32", "3f000000:31"),
            )
            for name, old, new in mutations:
                with self.subTest(name=name):
                    self.assertIn(old, raw)
                    broken = raw.replace(old, new, 1)
                    raw_path.write_text(broken)
                    changed = copy.deepcopy(original)
                    changed["cells"][0]["raw_stdout"]["sha256"] = gq.sha256_file(raw_path)
                    # Recompute typed summaries too when possible: semantic
                    # corruption must fail before raw/receipt inequality alone.
                    with self.assertRaises(gq.ReceiptError):
                        gq.parse_activation_output(broken)
                    write_receipt(path, changed)
                    with self.assertRaises(gq.ReceiptError):
                        gq.validate_receipt(path, path.parent)
                    raw_path.write_text(raw)
                    write_receipt(path, original)
                    gq.validate_receipt(path, path.parent)
            lines = raw.splitlines()
            for broken in ("\n".join(lines[:-1]) + "\n", raw + lines[-1] + "\n"):
                with self.assertRaises(gq.ReceiptError):
                    gq.parse_activation_output(broken)

    def test_paced_controls_restore_receipt_and_raw_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = fake_activation_receipt(Path(tmp))
            files = {p: p.read_bytes() for p in path.parent.iterdir() if p.is_file()}
            gq.run_controls(path, path.parent)
            for artifact, raw in files.items():
                self.assertEqual(artifact.read_bytes(), raw)

    def test_exact_strict_timing_boundaries_and_nearest_rank(self):
        # 48 kHz x128: callback budget 8,000,000/3 ns; strict .7 budget,
        # wake < 6,250/3 ns. Values straddle the thresholds by one nanosecond.
        base = {"p99_duration_ns": 1866666, "p99_absolute_wake_ns": 2083, "deadline_misses": 0}
        self.assertTrue(gq.activation_thresholds(base, 48000, 128))
        for field, value in (("p99_duration_ns", 1866667), ("p99_absolute_wake_ns", 2084),
                             ("deadline_misses", 1)):
            broken = dict(base, **{field: value})
            self.assertFalse(gq.activation_thresholds(broken, 48000, 128))
        self.assertEqual(gq.activation_percentile(list(range(1, 101))), 99)
        self.assertEqual(gq.activation_percentile(list(range(1, 129))), 127)

    def test_fully_resigned_source_and_binary_artifact_controls(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = fake_activation_receipt(Path(tmp))
            original = json.loads(path.read_text())
            for name in ("source_fixture", "executable"):
                artifact_path = path.parent / original["artifacts"][name]["path"]
                saved = artifact_path.read_bytes()
                artifact_path.write_bytes(b"changed artifact")
                changed = copy.deepcopy(original)
                changed["artifacts"][name]["sha256"] = gq.sha256_file(artifact_path)
                if name == "source_fixture":
                    # Even with both source hashes replaced, the Git source
                    # commit remains an independent authority.
                    changed["source_fixture"]["sha256"] = gq.sha256_file(artifact_path)
                write_receipt(path, changed)
                with self.assertRaises(gq.ReceiptError):
                    gq.validate_receipt(path, path.parent)
                artifact_path.write_bytes(saved)
                write_receipt(path, original)
                gq.validate_receipt(path, path.parent)

if __name__ == "__main__":
    unittest.main()
