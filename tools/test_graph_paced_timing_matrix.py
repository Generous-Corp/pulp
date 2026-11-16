#!/usr/bin/env python3
"""Self-test for the shared graph paced-timing matrix runner.

Runs without a C++ build: the in-process ``python-reference`` fixture emits the
same raw cell format as the C++ harness, so the parser, deadline-miss counter,
admission gate, receipt schema and validator are all exercised end to end. Set
PULP_PACED_MATRIX_BUILD_DIR to a Release build to also run the planted negative
through a real C++ fixture.
"""

from __future__ import annotations

import copy
import json
import os
import platform
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import graph_paced_timing_matrix as matrix  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
# The CPU count must match the host identity class the receipt records.
NCPU = os.cpu_count() or 1
QUIET = lambda: ([0.0, 0.0, 0.0], NCPU)  # noqa: E731
LOADED = lambda: ([1000.0, 900.0, 800.0], NCPU)  # noqa: E731


def clean_repo(parent: Path) -> Path:
    """A committed throwaway git repository whose working tree starts clean."""
    repo = parent / "repo"
    repo.mkdir()
    git = ["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@example.invalid",
           "-c", "commit.gpgsign=false"]
    subprocess.run(git + ["init", "-q"], check=True)
    (repo / ".gitignore").write_text("ignored/\n", encoding="utf-8")
    (repo / "source.cpp").write_text("int main() {}\n", encoding="utf-8")
    subprocess.run(git + ["add", "."], check=True)
    subprocess.run(git + ["commit", "-q", "-m", "fixture"], check=True)
    return repo


def record(output: Path, *, mode="short", fixtures=("python-reference",), callbacks=8,
           planted_sleep_us=0, planted_every=1, load_reader=QUIET, build_dir=None,
           source_root=ROOT):
    return matrix.record_matrix(
        mode=mode, source_root=source_root, build_dir=build_dir or (output / "no-build"),
        output_dir=output, fixtures=list(fixtures), callbacks=callbacks, frames_filter=None,
        planted_sleep_us=planted_sleep_us, planted_every=planted_every,
        max_load_per_cpu=matrix.DEFAULT_MAX_LOAD_PER_CPU, timeout_seconds=600,
        load_reader=load_reader)


class ReceiptSchemaTest(unittest.TestCase):
    def test_short_receipt_schema_and_validation(self):
        with tempfile.TemporaryDirectory() as tmp:
            receipt, code = record(Path(tmp))
            self.assertEqual(code, 0)
            self.assertEqual(set(receipt), matrix.REQUIRED_TOP)
            self.assertEqual(receipt["schema"], matrix.SCHEMA)
            self.assertEqual(receipt["status"], "COMPLETE")
            self.assertIs(receipt["timing_qualified"], False)
            self.assertIn("short_mode", receipt["disqualifiers"])
            self.assertEqual(len(receipt["cells"]), 1)
            cell = receipt["cells"][0]
            self.assertEqual(set(cell), matrix.REQUIRED_CELL)
            self.assertEqual(cell["callbacks"], 8)
            for key in ("p50", "p99", "max"):
                self.assertIn(key, cell["metrics"]["duration_ns"])
            self.assertEqual(set(receipt["admission"]["before"]), {"load", "verdict", "admitted", "reason"})
            self.assertIsNotNone(receipt["admission"]["after"])
            identity = json.dumps(receipt["host_identity_class"])
            node = platform.node()
            if node:
                self.assertNotIn(node, identity, "receipt must not carry the hostname")
            matrix.validate_receipt(Path(tmp) / "receipt.json")

    def test_validator_rejects_tampered_raw_and_resigned_metrics(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            receipt, _ = record(out)
            path = out / "receipt.json"
            original = path.read_text(encoding="utf-8")
            raw = out / receipt["cells"][0]["raw_stdout"]["path"]
            saved = raw.read_text(encoding="utf-8")
            raw.write_text(saved.replace("P 0 0 ", "P 0 1 ", 1), encoding="utf-8")
            with self.assertRaises(matrix.MatrixError):
                matrix.validate_receipt(path)
            raw.write_text(saved, encoding="utf-8")
            for mutate in (
                lambda r: r["cells"][0]["metrics"].__setitem__("deadline_misses", 3),
                lambda r: r.__setitem__("timing_qualified", True),
                lambda r: r["cells"].append(copy.deepcopy(r["cells"][0])),
                lambda r: r["disqualifiers"].remove("short_mode"),
            ):
                changed = json.loads(original)
                mutate(changed)
                changed["receipt_sha256"] = matrix.receipt_digest(changed)
                path.write_text(json.dumps(changed), encoding="utf-8")
                with self.assertRaises(matrix.MatrixError):
                    matrix.validate_receipt(path)
            path.write_text(original, encoding="utf-8")
            matrix.validate_receipt(path)


class RawBindingTest(unittest.TestCase):
    """Every recorded cell field must agree with the digest-bound raw output."""

    def assert_resigned_tamper_rejected(self, path, original, mutate, message):
        changed = json.loads(original)
        mutate(changed)
        changed["receipt_sha256"] = matrix.receipt_digest(changed)
        path.write_text(json.dumps(changed), encoding="utf-8")
        try:
            with self.assertRaisesRegex(matrix.MatrixError, message):
                matrix.validate_receipt(path)
        finally:
            path.write_text(original, encoding="utf-8")

    def test_resigned_cell_metadata_tamper_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            record(out)
            path = out / "receipt.json"
            original = path.read_text(encoding="utf-8")
            cell = lambda r: r["cells"][0]  # noqa: E731
            tampers = {
                "compiled_source": lambda r: cell(r).__setitem__("compiled_source", "a" * 40),
                "build_dirty": lambda r: cell(r).__setitem__("build_dirty", 1),
                "build_type": lambda r: cell(r).__setitem__("build_type", "Release"),
                "returncode": lambda r: cell(r).__setitem__("returncode", 1),
                "extras": lambda r: cell(r).__setitem__("extras", {"walk_fallbacks": "0"}),
                "variant": lambda r: cell(r).__setitem__("variant", "other"),
            }
            for field, mutate in tampers.items():
                with self.subTest(field=field):
                    message = "unexpected matrix cell" if field == "variant" else \
                        f"cell {field} disagrees with raw output"
                    self.assert_resigned_tamper_rejected(path, original, mutate, message)
            matrix.validate_receipt(path)

    def test_stale_or_dirty_disqualifier_cannot_be_erased(self):
        # A raw cell built from a dirty tree at another commit stays disqualified
        # even when the receipt claims the clean head.
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            receipt, _ = record(out)
            path = out / "receipt.json"
            original = path.read_text(encoding="utf-8")
            self.assertTrue(any(r.startswith("stale_binary:") for r in receipt["disqualifiers"]))
            self.assert_resigned_tamper_rejected(
                path, original,
                lambda r: (r["cells"][0].__setitem__("compiled_source", r["source"]["sha"]),
                           r["disqualifiers"].remove(next(d for d in r["disqualifiers"]
                                                          if d.startswith("stale_binary:")))),
                "cell compiled_source disagrees with raw output")

    def test_planted_fault_cannot_be_erased_from_the_receipt(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            record(out, planted_sleep_us=1000, planted_every=4)
            path = out / "receipt.json"
            original = path.read_text(encoding="utf-8")
            self.assert_resigned_tamper_rejected(
                path, original,
                lambda r: (r["planted"].__setitem__("sleep_us", 0),
                           r["disqualifiers"].remove("planted_fault")),
                "planted fault disagrees with raw output")

    def test_process_trailer_must_agree_with_catch2_summary(self):
        fixture = matrix.FIXTURES["g5-graph-event"]
        body = matrix.reference_cell_output("routed", 48000, 128, 8, 0, 1).replace(
            "fixture=python-reference", "fixture=g5-graph-event")
        trailers = ("PACED_SOURCE sha=" + "a" * 40 + " dirty=0 tracked_changes=0 untracked_count=0 "
                    "untracked_digest=" + "0" * 64 + "\n"
                    "PACED_ADMISSION load1=0.0 logical_cpus=8 max_load_per_cpu=0.25\n"
                    "PACED_BUILD cmake_build_type=python pulp_sanitizer=none\n")
        passing = body + "All tests passed (10 assertions in 1 test case)\n"
        self.assertEqual(matrix.raw_cell_view(
            fixture, passing + "PACED_PROCESS returncode=0\n" + trailers)["returncode"], 0)
        with self.assertRaisesRegex(matrix.MatrixError, "Catch2 summary"):
            matrix.raw_cell_view(fixture, passing + "PACED_PROCESS returncode=1\n" + trailers)
        with self.assertRaisesRegex(matrix.MatrixError, "Catch2 summary"):
            matrix.raw_cell_view(fixture, body + "PACED_PROCESS returncode=0\n" + trailers)
        with self.assertRaisesRegex(matrix.MatrixError, "process trailer"):
            matrix.raw_cell_view(fixture, passing + trailers)

    def test_receipt_source_and_build_profile_are_bound_to_raw_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            record(out)
            path = out / "receipt.json"
            original = path.read_text(encoding="utf-8")
            self.assert_resigned_tamper_rejected(
                path, original, lambda r: r["source"].__setitem__("sha", "a" * 40),
                "source state disagrees with raw output")
            self.assert_resigned_tamper_rejected(
                path, original,
                lambda r: r["build_profile"].__setitem__("CMAKE_BUILD_TYPE", "Release"),
                "build profile disagrees with raw output")
            self.assert_resigned_tamper_rejected(
                path, original,
                lambda r: r["build_profile"].__setitem__("PULP_SANITIZER", "address"),
                "build profile disagrees with raw output")
            matrix.validate_receipt(path)


class AdmissionTest(unittest.TestCase):
    def test_qualification_refuses_loaded_host_before_any_cell(self):
        with tempfile.TemporaryDirectory() as tmp:
            # A clean source isolates the admission refusal from the clean-source refusal.
            receipt, code = record(Path(tmp) / "out", mode="qualification",
                                   fixtures=matrix.QUALIFYING_FIXTURES,
                                   callbacks=matrix.QUALIFICATION_CALLBACKS, load_reader=LOADED,
                                   source_root=clean_repo(Path(tmp)))
            self.assertEqual(code, matrix.REFUSED_EXIT)
            self.assertEqual(receipt["status"], "REFUSED")
            self.assertEqual(receipt["cells"], [])
            self.assertEqual(receipt["admission"]["before"]["verdict"], "refuse")
            self.assertIs(receipt["timing_qualified"], False)
            self.assertIn("admission_before_failed", receipt["disqualifiers"])
            matrix.validate_receipt(Path(tmp) / "out" / "receipt.json")

    def test_admission_threshold_boundary_and_unknown_load(self):
        self.assertTrue(matrix.admission_verdict([2.0, 0, 0], 8, 0.25)["admitted"])
        self.assertFalse(matrix.admission_verdict([2.01, 0, 0], 8, 0.25)["admitted"])
        self.assertEqual(matrix.admission_verdict([], 8, 0.25)["verdict"], "unknown")

    def test_read_load_keeps_full_precision_at_the_boundary(self):
        # 7.004 on 28 CPUs exceeds the 0.25 x 28 = 7.0 limit; a rounded sample
        # (7.0) would be admitted.
        from unittest import mock
        with mock.patch.object(matrix.os, "getloadavg", return_value=(7.004, 1.0, 1.0)), \
                mock.patch.object(matrix.os, "cpu_count", return_value=28):
            load, ncpu = matrix.read_load()
        self.assertEqual(load[0], 7.004)
        verdict = matrix.admission_verdict(load, ncpu, 0.25)
        self.assertFalse(verdict["admitted"])
        self.assertEqual(verdict["verdict"], "refuse")

    def test_short_mode_flags_but_still_runs_on_loaded_host(self):
        with tempfile.TemporaryDirectory() as tmp:
            receipt, code = record(Path(tmp), load_reader=LOADED)
            self.assertEqual(code, 0)
            self.assertEqual(len(receipt["cells"]), 1)
            self.assertIn("admission_before_failed", receipt["disqualifiers"])

    def test_qualification_refuses_reference_fixture_short_counts_and_planted_faults(self):
        with tempfile.TemporaryDirectory() as tmp:
            for kwargs in ({"fixtures": ("python-reference",)},
                           {"fixtures": matrix.QUALIFYING_FIXTURES, "callbacks": 64},
                           {"fixtures": matrix.QUALIFYING_FIXTURES, "planted_sleep_us": 10}):
                args = {"callbacks": matrix.QUALIFICATION_CALLBACKS, **kwargs}
                with self.assertRaises(matrix.MatrixError):
                    record(Path(tmp), mode="qualification", **args)


class SourceStateTest(unittest.TestCase):
    """An untracked, non-ignored file can be compiled in, so it makes the source dirty."""

    def test_untracked_file_marks_source_dirty_and_refuses_qualification(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = clean_repo(Path(tmp))
            build, out = repo / "build", repo / "receipts"
            build.mkdir()
            (build / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Release\n", encoding="utf-8")
            (repo / "ignored").mkdir()
            (repo / "ignored" / "scratch.o").write_text("x", encoding="utf-8")
            clean = matrix._source_state(repo, (build, out))
            self.assertFalse(clean["dirty"], clean)  # ignored + run-owned dirs do not count
            self.assertEqual((clean["tracked_changes"], clean["untracked_count"]), (0, 0))
            (repo / "planted_input.hpp").write_text("#define PLANTED 1\n", encoding="utf-8")
            dirty = matrix._source_state(repo, (build, out))
            self.assertTrue(dirty["dirty"])
            self.assertEqual(dirty["tracked_changes"], 0)
            self.assertEqual(dirty["untracked_paths"], ["planted_input.hpp"])
            with self.assertRaisesRegex(matrix.MatrixError, "clean source.*1 untracked"):
                record(out, mode="qualification", fixtures=matrix.QUALIFYING_FIXTURES,
                       callbacks=matrix.QUALIFICATION_CALLBACKS, source_root=repo, build_dir=build)

    def test_short_receipt_records_untracked_files_and_validation_binds_them(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = clean_repo(Path(tmp))
            (repo / "planted_input.hpp").write_text("x\n", encoding="utf-8")
            out = Path(tmp) / "out"
            receipt, _ = record(out, source_root=repo)
            self.assertTrue(receipt["source"]["dirty"])
            self.assertEqual(receipt["source"]["untracked_count"], 1)
            self.assertIn("dirty_source", receipt["disqualifiers"])
            path = out / "receipt.json"
            matrix.validate_receipt(path)
            original = path.read_text(encoding="utf-8")
            for mutate, message in (
                (lambda r: r["source"].update(dirty=False), "marked clean"),
                (lambda r: r["source"].update(untracked_count=0, untracked_paths=[]),
                 "untracked file digest"),
                (lambda r: r["source"].update(untracked_paths=["other.hpp"],
                                              untracked_digest=matrix.hashlib.sha256(
                                                  b"other.hpp").hexdigest()),
                 "source state disagrees with raw output"),
            ):
                changed = json.loads(original)
                mutate(changed)
                changed["disqualifiers"] = matrix.disqualifiers_for(changed)
                changed["timing_qualified"] = not changed["disqualifiers"]
                changed["receipt_sha256"] = matrix.receipt_digest(changed)
                path.write_text(json.dumps(changed), encoding="utf-8")
                with self.assertRaisesRegex(matrix.MatrixError, message):
                    matrix.validate_receipt(path)
            path.write_text(original, encoding="utf-8")


def resign(receipt):
    receipt["receipt_sha256"] = matrix.receipt_digest(receipt)
    return receipt


class TrustedFieldTamperTest(unittest.TestCase):
    """Fields a verdict depends on are recomputed, never trusted, even when re-signed."""

    def tampered(self, path, original, mutate, message):
        changed = json.loads(original)
        mutate(changed)
        path.write_text(json.dumps(resign(changed)), encoding="utf-8")
        try:
            with self.assertRaisesRegex(matrix.MatrixError, message):
                matrix.validate_receipt(path)
        finally:
            path.write_text(original, encoding="utf-8")

    def test_admission_verdict_is_recomputed_from_the_load_sample(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            receipt, _ = record(out, load_reader=LOADED)
            self.assertIn("admission_after_failed", receipt["disqualifiers"])
            path = out / "receipt.json"
            original = path.read_text(encoding="utf-8")

            def flip_after(r):  # the failing load sample stays as recorded
                r["admission"]["after"].update(admitted=True, verdict="admit", reason="")
                r["disqualifiers"].remove("admission_after_failed")
            self.tampered(path, original, flip_after, "after admission verdict disagrees")

            def flip_before(r):
                r["admission"]["before"].update(admitted=True, verdict="admit", reason="")
                r["disqualifiers"].remove("admission_before_failed")
            self.tampered(path, original, flip_before, "before admission verdict disagrees")

            def relax_threshold(r):  # consistent verdicts under a relaxed threshold
                r["admission"]["max_load_per_cpu"] = 1e6
                for name in ("before", "after"):
                    r["admission"][name] = matrix.admission_verdict(
                        r["admission"][name]["load"], r["admission"]["logical_cpus"], 1e6)
                r["disqualifiers"] = matrix.disqualifiers_for(r)
            self.tampered(path, original, relax_threshold, "admission inputs disagree with raw output")

            def lower_load(r):
                for name in ("before", "after"):
                    r["admission"][name] = matrix.admission_verdict(
                        [0.0, 0.0, 0.0], r["admission"]["logical_cpus"], r["admission"]["max_load_per_cpu"])
                r["disqualifiers"] = matrix.disqualifiers_for(r)
            self.tampered(path, original, lower_load, "admission inputs disagree with raw output")

            self.tampered(path, original,
                          lambda r: r["admission"].__setitem__("logical_cpus", 10_000),
                          "CPU count disagrees with host identity")
            matrix.validate_receipt(path)

    def test_totals_registry_fields_and_mode_are_recomputed(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            record(out)
            path = out / "receipt.json"
            original = path.read_text(encoding="utf-8")
            self.tampered(path, original, lambda r: r["totals"].__setitem__("deadline_misses", 0)
                          if r["totals"]["deadline_misses"] else r["totals"].__setitem__("deadline_misses", 7),
                          "totals disagree")
            self.tampered(path, original, lambda r: r["cells"][0].__setitem__("source_file", "x.cpp"),
                          "fixture registry fields")
            self.tampered(path, original, lambda r: r.__setitem__("mode", "qualification"),
                          "qualification receipt has short-mode inputs")
            self.tampered(path, original, lambda r: r["admission"].__setitem__("rule", "anything"),
                          "admission rule text changed")


FAKE_FIXTURE = """#!{python}
# Stand-in for a compiled cell. PULP_FAKE_SCHED picks how it treats the
# scheduling request: "honour" behaves like the C++ harness (a real-time
# request is obtained, the malformed one refused); "fallback" ignores the
# request and reports default scheduling, which the runner must reject;
# "deny" refuses every real-time request, like a host without real-time rights.
import os, sys
sys.path.insert(0, {tools!r})
import graph_paced_timing_matrix as m
env = os.environ
requested = env.get("PULP_PACED_SCHED", "none")
behaviour = env.get("PULP_FAKE_SCHED", "honour")
reference = m.format_sched_line("none", "unknown", "not_requested")
if behaviour == "fallback" or requested == "none":
    sched = m.format_sched_line("none", "timeshare", "not_requested")
elif requested == "realtime" and behaviour != "deny":
    sched = m.format_sched_line("realtime", "time_constraint", "obtained", period_ns=2666666,
                                computation_ns=1333333, constraint_ns=2666666, preemptible=1,
                                priority=97)
else:
    sched = m.format_sched_line(requested, "timeshare", "refused",
                                "errno_1" if behaviour == "deny" else "kern_return_4", priority=31)
if sched.split()[3] == "status=refused":
    lines = m.reference_cell_output(env["PULP_PACED_VARIANT"], int(env["PULP_PACED_RATE"]),
                                    int(env["PULP_PACED_FRAMES"]), 8, 0, 1).splitlines()[:2]
    lines[1] = lines[1].replace("callbacks=8", "callbacks=" + env["PULP_PACED_CALLBACKS"])
    text = "\\n".join(lines + [sched, "PACED_END rows=0 oracle_failures=0"]) + "\\n"
    text = text.replace("fixture=python-reference", "fixture={fixture}")
    sys.stdout.write(text + "test cases: 1 | 1 failed\\n")
    sys.exit(1)
text = m.reference_cell_output(env["PULP_PACED_VARIANT"], int(env["PULP_PACED_RATE"]),
                               int(env["PULP_PACED_FRAMES"]), int(env["PULP_PACED_CALLBACKS"]), 0, 1)
text = text.replace("fixture=python-reference", "fixture={fixture}").replace(reference, sched)
text = text.replace("build_type=python", "build_type={stamp}")
sys.stdout.write(text + "All tests passed (1 assertion in 1 test case)\\n")
"""


def install_fake(build: Path, fixture: str, stamp: str = "Release") -> None:
    exe = build / matrix.FIXTURES[fixture].executable
    exe.parent.mkdir(parents=True, exist_ok=True)
    exe.write_text(FAKE_FIXTURE.format(python=sys.executable, tools=str(ROOT / "tools"), stamp=stamp,
                                       fixture=fixture), encoding="utf-8")
    exe.chmod(0o755)
    (build / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Release\nPULP_SANITIZER:STRING=\n",
                                          encoding="utf-8")


class CompiledBuildTypeTest(unittest.TestCase):
    """A Release cache over a binary stamped Debug (reconfigured, not rebuilt) cannot qualify."""

    def run_fake(self, tmp, stamp):
        build = tmp / "build"
        install_fake(build, "g6-voice-bank", stamp)
        out = tmp / "out"
        receipt, code = matrix.record_matrix(
            mode="short", source_root=ROOT, build_dir=build, output_dir=out, fixtures=["g6-voice-bank"],
            callbacks=8, frames_filter={128}, planted_sleep_us=0, planted_every=1,
            max_load_per_cpu=matrix.DEFAULT_MAX_LOAD_PER_CPU, timeout_seconds=120, load_reader=QUIET)
        self.assertEqual(code, 0)
        return receipt, out / "receipt.json"

    def test_debug_stamp_under_release_cache_is_disqualified_when_recorded_and_validated(self):
        tag = "build_type_mismatch:g6-voice-bank/eight-slot/48000/128"
        with tempfile.TemporaryDirectory() as tmp:
            receipt, path = self.run_fake(Path(tmp), "Debug")
            self.assertEqual(receipt["build_profile"]["CMAKE_BUILD_TYPE"], "Release")
            self.assertEqual(receipt["cells"][0]["build_type"], "Debug")
            self.assertIn(tag, receipt["disqualifiers"])
            matrix.validate_receipt(path)
            original = path.read_text(encoding="utf-8")
            for mutate, message in (
                (lambda r: r["disqualifiers"].remove(tag), "disqualifiers disagree"),
                (lambda r: (r["cells"][0].__setitem__("build_type", "Release"),
                            r["disqualifiers"].remove(tag)), "cell build_type disagrees with raw output"),
            ):
                changed = json.loads(original)
                mutate(changed)
                path.write_text(json.dumps(resign(changed)), encoding="utf-8")
                with self.assertRaisesRegex(matrix.MatrixError, message):
                    matrix.validate_receipt(path)

    def test_release_stamp_control_has_no_build_type_disqualifier(self):
        with tempfile.TemporaryDirectory() as tmp:
            receipt, path = self.run_fake(Path(tmp), "Release")
            self.assertFalse([d for d in receipt["disqualifiers"] if d.startswith("build_type_mismatch:")])
            matrix.validate_receipt(path)


class MissCounterTest(unittest.TestCase):
    CONTROL_ATTEMPTS = 4

    def test_deadline_rule_boundary(self):
        period = 4096 * 1_000_000_000 // 48000
        exact = [(0, 0, 4096 * 1_000_000_000 // 48000, 1)]
        # end - target == one block (rounded down to ns) is on time ...
        self.assertEqual(matrix.compute_metrics(exact, 48000, 4096)["deadline_misses"], 0)
        # ... one nanosecond past the exact block boundary is a miss.
        late = [(0, 0, period + 1, 1)]
        self.assertEqual(matrix.compute_metrics(late, 48000, 4096)["deadline_misses"], 1)

    @unittest.skipIf(os.environ.get("PULP_SELFTEST_SKIP_WALL_CLOCK") == "1",
                     "wall-clock case runs in its own performance-labelled entry")
    def test_planted_sleep_must_register_misses_and_control_must_not(self):
        # 4096 frames at 48 kHz is an 85.3 ms period; a 90 ms planted sleep on
        # every second callback overruns exactly those callbacks.
        with tempfile.TemporaryDirectory() as tmp:
            planted, _ = record(Path(tmp) / "planted", planted_sleep_us=90_000, planted_every=2)
        misses = planted["cells"][0]["metrics"]["deadline_misses"]
        self.assertGreaterEqual(misses, 4, "instrument failed to see a planted overrun")
        self.assertIn("planted_fault", planted["disqualifiers"])
        self.assertGreater(planted["cells"][0]["metrics"]["duration_ns"]["max"], 90_000_000)

        # Negative control: an unplanted run must report no misses. The control
        # records real callbacks, so one slow scheduling slice on a loaded or
        # sanitizer-instrumented runner can add a genuine miss that is host
        # noise, not an instrument false positive. A systematic false positive
        # shows up in every attempt, so require at least one clean run out of
        # CONTROL_ATTEMPTS, and never a planted-fault disqualifier.
        control_misses = []
        for attempt in range(self.CONTROL_ATTEMPTS):
            with tempfile.TemporaryDirectory() as tmp:
                control, _ = record(Path(tmp) / "control")
            self.assertNotIn("planted_fault", control["disqualifiers"])
            control_misses.append(control["cells"][0]["metrics"]["deadline_misses"])
            if control_misses[-1] == 0:
                break
        self.assertEqual(min(control_misses), 0,
                         "negative control: the unplanted run must be clean in at least one "
                         "of %d attempts (misses per attempt: %s)"
                         % (self.CONTROL_ATTEMPTS, control_misses))


class QualificationRuleTest(unittest.TestCase):
    """The rule must be satisfiable; otherwise timing_qualified=false proves nothing."""

    def ideal(self):
        cells = []
        for fixture, variant, rate, frames in matrix.matrix_cells(matrix.QUALIFYING_FIXTURES, None):
            cells.append({"fixture": fixture.name, "variant": variant, "rate": rate, "frames": frames,
                          "callbacks": matrix.QUALIFICATION_CALLBACKS, "returncode": 0,
                          "compiled_source": "a" * 40, "build_dirty": 0, "build_type": "Release",
                          "scheduling": {"status": "obtained", "obtained": "time_constraint"},
                          "metrics": {"thresholds_met": True}})
        return {"mode": "qualification", "status": "COMPLETE",
                "requested": {"callbacks": matrix.QUALIFICATION_CALLBACKS,
                              "fixtures": list(matrix.QUALIFYING_FIXTURES), "frames_filter": None},
                "planted": {"sleep_us": 0, "every": 1},
                "admission": {"max_load_per_cpu": matrix.DEFAULT_MAX_LOAD_PER_CPU,
                              "before": {"admitted": True}, "after": {"admitted": True}},
                "build_profile": {"CMAKE_BUILD_TYPE": "Release", "PULP_SANITIZER": ""},
                "scheduling": {"requested": "realtime", "refusal": None},
                "source": {"sha": "a" * 40, "dirty": False}, "cells": cells}

    def test_ideal_run_qualifies_and_each_single_defect_disqualifies(self):
        self.assertEqual(matrix.disqualifiers_for(self.ideal()), [])
        defects = {
            "admission_after_failed": lambda r: r["admission"]["after"].__setitem__("admitted", False),
            "dirty_source": lambda r: r["source"].__setitem__("dirty", True),
            "not_plain_release": lambda r: r["build_profile"].__setitem__("PULP_SANITIZER", "address"),
            "thresholds_failed:": lambda r: r["cells"][3]["metrics"].__setitem__("thresholds_met", False),
            "stale_binary:": lambda r: r["cells"][0].__setitem__("compiled_source", "b" * 40),
            "short_cell:": lambda r: r["cells"][1].__setitem__("callbacks", 99999),
            "build_type_mismatch:": lambda r: r["cells"][2].__setitem__("build_type", "Debug"),
            "admission_threshold_relaxed": lambda r: r["admission"].__setitem__("max_load_per_cpu", 0.5),
            "scheduling_not_realtime_request": lambda r: r["scheduling"].__setitem__("requested", "none"),
            "scheduling_refused": lambda r: r["scheduling"].__setitem__("refusal", {"reason": "x"}),
            "scheduling_not_obtained:": lambda r: r["cells"][4]["scheduling"].update(
                status="not_requested", obtained="timeshare"),
        }
        for reason, mutate in defects.items():
            receipt = self.ideal()
            mutate(receipt)
            reasons = matrix.disqualifiers_for(receipt)
            self.assertTrue(any(r.startswith(reason) for r in reasons), (reason, reasons))


def fake_record(tmp: Path, *, fixture="g6-voice-bank", scheduling="realtime", behaviour="honour",
                mode="short", callbacks=8, source_root=ROOT, frames_filter=frozenset({128})):
    build = tmp / "build"
    install_fake(build, fixture)
    fixtures = list(matrix.QUALIFYING_FIXTURES) if mode == "qualification" else [fixture]
    previous = os.environ.get("PULP_FAKE_SCHED")
    os.environ["PULP_FAKE_SCHED"] = behaviour
    try:
        return matrix.record_matrix(
            mode=mode, source_root=source_root, build_dir=build, output_dir=tmp / "out",
            fixtures=fixtures, callbacks=callbacks,
            frames_filter=None if mode == "qualification" else set(frames_filter),
            planted_sleep_us=0, planted_every=1, max_load_per_cpu=matrix.DEFAULT_MAX_LOAD_PER_CPU,
            timeout_seconds=120, scheduling=scheduling, load_reader=QUIET)
    finally:
        if previous is None:
            os.environ.pop("PULP_FAKE_SCHED", None)
        else:
            os.environ["PULP_FAKE_SCHED"] = previous


class SchedulingTest(unittest.TestCase):
    """Real-time scheduling is requested, read back, recorded, and refused when unavailable."""

    def test_scheduling_line_must_be_internally_consistent(self):
        good = matrix.format_sched_line("realtime", "time_constraint", "obtained", period_ns=1)
        self.assertEqual(matrix.parse_sched_line(good)["obtained"], "time_constraint")
        for line, message in (
            (matrix.format_sched_line("realtime", "timeshare", "obtained"), "without a real-time class"),
            (matrix.format_sched_line("realtime", "timeshare", "refused"), "refusal without"),
            (matrix.format_sched_line("realtime", "timeshare", "not_requested"), "not_requested disagrees"),
            (matrix.format_sched_line("none", "timeshare", "refused", "x"), "refusal without"),
            (good + "\n" + good, "exactly one scheduling line"),
            ("", "exactly one scheduling line"),
            (good.replace("priority=-1", "priority=x"), "malformed scheduling integer"),
        ):
            with self.subTest(line=line):
                with self.assertRaisesRegex(matrix.MatrixError, message):
                    matrix.parse_sched_line(line)

    def test_obtained_realtime_is_recorded_and_bound(self):
        # Control for the refusal tests below: the same fake, honouring the request.
        with tempfile.TemporaryDirectory() as tmp:
            receipt, code = fake_record(Path(tmp))
            self.assertEqual(code, 0)
            self.assertEqual(receipt["status"], "COMPLETE")
            self.assertEqual(receipt["scheduling"]["requested"], "realtime")
            self.assertIsNone(receipt["scheduling"]["refusal"])
            cell = receipt["cells"][0]
            self.assertEqual((cell["scheduling"]["status"], cell["scheduling"]["obtained"]),
                             ("obtained", "time_constraint"))
            self.assertFalse([d for d in receipt["disqualifiers"] if d.startswith("scheduling_")])
            path = Path(tmp) / "out" / "receipt.json"
            matrix.validate_receipt(path)
            original = path.read_text(encoding="utf-8")
            changed = json.loads(original)
            changed["cells"][0]["scheduling"]["obtained"] = "fifo"
            path.write_text(json.dumps(resign(changed)), encoding="utf-8")
            with self.assertRaisesRegex(matrix.MatrixError, "cell scheduling disagrees with raw output"):
                matrix.validate_receipt(path)

    def test_default_scheduling_never_qualifies(self):
        with tempfile.TemporaryDirectory() as tmp:
            receipt, code = fake_record(Path(tmp), scheduling="none")
            self.assertEqual(code, 0)
            self.assertIn("scheduling_not_realtime_request", receipt["disqualifiers"])
            self.assertIn("scheduling_not_obtained:g6-voice-bank/eight-slot/48000/128",
                          receipt["disqualifiers"])
            path = Path(tmp) / "out" / "receipt.json"
            matrix.validate_receipt(path)
            # Re-signing the request as realtime cannot erase it: the raw line says none.
            changed = json.loads(path.read_text(encoding="utf-8"))
            changed["scheduling"]["requested"] = "realtime"
            changed["disqualifiers"] = matrix.disqualifiers_for(changed)
            path.write_text(json.dumps(resign(changed)), encoding="utf-8")
            with self.assertRaisesRegex(matrix.MatrixError, "requested class"):
                matrix.validate_receipt(path)
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(matrix.MatrixError, "qualification requires realtime"):
                fake_record(Path(tmp), scheduling="none", mode="qualification",
                            callbacks=matrix.QUALIFICATION_CALLBACKS, source_root=clean_repo(Path(tmp)))

    def test_silent_fallback_to_default_scheduling_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(matrix.MatrixError, "scheduling request mismatch"):
                fake_record(Path(tmp), behaviour="fallback")

    def test_refused_request_stops_short_run_with_refused_receipt(self):
        with tempfile.TemporaryDirectory() as tmp:
            receipt, code = fake_record(Path(tmp), scheduling="realtime-invalid")
            self.assertEqual(code, matrix.REFUSED_EXIT)
            self.assertEqual(receipt["status"], "REFUSED")
            self.assertEqual(receipt["cells"], [])
            refusal = receipt["scheduling"]["refusal"]
            self.assertEqual(refusal["scheduling"]["reason"], "kern_return_4")
            self.assertEqual(refusal["fixture"], "g6-voice-bank")
            self.assertNotEqual(refusal["returncode"], 0)
            self.assertIn("scheduling_refused", receipt["disqualifiers"])
            self.assertIs(receipt["timing_qualified"], False)
            path = Path(tmp) / "out" / "receipt.json"
            matrix.validate_receipt(path)
            original = path.read_text(encoding="utf-8")
            tampers = (
                (lambda r: r["scheduling"]["refusal"]["scheduling"].__setitem__("reason", "other"),
                 "refusal disagrees with raw output"),
                (lambda r: r["scheduling"]["refusal"].__setitem__("returncode", 0),
                 "refusing cell must have failed"),
                (lambda r: (r["scheduling"].__setitem__("refusal", None),
                            r.__setitem__("disqualifiers", matrix.disqualifiers_for(r))),
                 "refused receipt must have no cells and a refused admission"),
                (lambda r: r.__setitem__("status", "COMPLETE"), "a scheduling refusal must stop the run"),
            )
            for mutate, message in tampers:
                with self.subTest(message=message):
                    changed = json.loads(original)
                    mutate(changed)
                    path.write_text(json.dumps(resign(changed)), encoding="utf-8")
                    with self.assertRaisesRegex(matrix.MatrixError, message):
                        matrix.validate_receipt(path)
            path.write_text(original, encoding="utf-8")
            raw = Path(tmp) / "out" / refusal["raw_stdout"]["path"]
            raw.write_text(raw.read_text(encoding="utf-8").replace("kern_return_4", "kern_return_5"),
                           encoding="utf-8")
            with self.assertRaisesRegex(matrix.MatrixError, "refusal raw output digest mismatch"):
                matrix.validate_receipt(path)

    def test_unavailable_realtime_refuses_qualification_at_the_first_cell(self):
        with tempfile.TemporaryDirectory() as tmp:
            # "deny" models a host that refuses real-time scheduling (for example
            # Linux without CAP_SYS_NICE). Only the first fixture has an
            # executable, so a run that continued past the refusal would fail on
            # the missing second one instead of returning REFUSED.
            receipt, code = fake_record(Path(tmp), fixture="g5-graph-event", behaviour="deny",
                                        mode="qualification", callbacks=matrix.QUALIFICATION_CALLBACKS,
                                        source_root=clean_repo(Path(tmp)))
            self.assertEqual(code, matrix.REFUSED_EXIT)
            self.assertEqual(receipt["status"], "REFUSED")
            self.assertEqual(receipt["mode"], "qualification")
            self.assertEqual(receipt["cells"], [])
            self.assertEqual(receipt["scheduling"]["refusal"]["fixture"], "g5-graph-event")
            self.assertEqual(receipt["scheduling"]["refusal"]["scheduling"]["requested"], "realtime")
            self.assertIs(receipt["timing_qualified"], False)
            matrix.validate_receipt(Path(tmp) / "out" / "receipt.json")
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(matrix.MatrixError, "qualification requires realtime"):
                fake_record(Path(tmp), fixture="g5-graph-event", scheduling="realtime-invalid",
                            mode="qualification", callbacks=matrix.QUALIFICATION_CALLBACKS,
                            source_root=clean_repo(Path(tmp)))

@unittest.skipUnless(os.environ.get("PULP_PACED_MATRIX_BUILD_DIR"),
                     "set PULP_PACED_MATRIX_BUILD_DIR to a Release build to run the C++ planted negative")
class CompiledPlantedNegativeTest(unittest.TestCase):
    def test_compiled_fixture_sees_planted_overrun(self):
        build = Path(os.environ["PULP_PACED_MATRIX_BUILD_DIR"]).resolve()
        with tempfile.TemporaryDirectory() as tmp:
            receipt, code = matrix.record_matrix(
                mode="short", source_root=ROOT, build_dir=build, output_dir=Path(tmp),
                fixtures=["g5-graph-event"], callbacks=8, frames_filter={4096},
                planted_sleep_us=90_000, planted_every=2, max_load_per_cpu=0.25,
                timeout_seconds=300)
        self.assertEqual(code, 0)
        for cell in receipt["cells"]:
            self.assertGreaterEqual(cell["metrics"]["deadline_misses"], 4)
            self.assertEqual(cell["metrics"]["oracle_failures"], 0)

    def test_compiled_fixtures_obtain_realtime_and_refuse_a_rejected_request(self):
        build = Path(os.environ["PULP_PACED_MATRIX_BUILD_DIR"]).resolve()
        for fixture, frames in (("g5-graph-event", 128), ("g12-activation", 128)):
            with self.subTest(fixture=fixture), tempfile.TemporaryDirectory() as tmp:
                common = dict(mode="short", source_root=ROOT, build_dir=build, fixtures=[fixture],
                              callbacks=16, frames_filter={frames}, planted_sleep_us=0,
                              planted_every=1, max_load_per_cpu=0.25, timeout_seconds=300)
                control, code = matrix.record_matrix(output_dir=Path(tmp) / "control",
                                                     scheduling="realtime", **common)
                if platform.system() == "Darwin":
                    self.assertEqual(code, 0)
                    for cell in control["cells"]:
                        self.assertEqual(cell["scheduling"]["obtained"], "time_constraint")
                    matrix.validate_receipt(Path(tmp) / "control" / "receipt.json")
                refused, code = matrix.record_matrix(output_dir=Path(tmp) / "refused",
                                                     scheduling="realtime-invalid", **common)
                self.assertEqual(code, matrix.REFUSED_EXIT)
                self.assertEqual(refused["status"], "REFUSED")
                self.assertEqual(refused["cells"], [])
                self.assertEqual(refused["scheduling"]["refusal"]["scheduling"]["status"], "refused")
                matrix.validate_receipt(Path(tmp) / "refused" / "receipt.json")

    def test_g12_build_stamp_is_not_an_oracle_failure_but_audio_is(self):
        build = Path(os.environ["PULP_PACED_MATRIX_BUILD_DIR"]).resolve()
        with tempfile.TemporaryDirectory() as tmp:
            receipt, _ = matrix.record_matrix(
                mode="short", source_root=ROOT, build_dir=build, output_dir=Path(tmp),
                fixtures=["g12-activation"], callbacks=16, frames_filter={128},
                planted_sleep_us=0, planted_every=1, max_load_per_cpu=0.25, timeout_seconds=300)
            fixture = matrix.FIXTURES["g12-activation"]
            for cell in receipt["cells"]:
                self.assertEqual(cell["metrics"]["oracle_failures"], 0)
                raw = (Path(tmp) / cell["raw_stdout"]["path"]).read_text(encoding="utf-8")
                for stamp in ("build_dirty=0", "build_dirty=1"):
                    view = matrix.raw_cell_view(fixture, raw.replace(
                        f"build_dirty={cell['build_dirty']}", stamp, 1))
                    self.assertEqual(view["metrics"]["oracle_failures"], 0)
                corrupted = raw.replace("bits=3f000000:", "bits=3ec00000:", 1)
                self.assertEqual(matrix.raw_cell_view(fixture, corrupted)["metrics"]["oracle_failures"],
                                 cell["callbacks"])


if __name__ == "__main__":
    unittest.main()
