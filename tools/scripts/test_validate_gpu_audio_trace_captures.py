#!/usr/bin/env python3
"""Focused tests for the GPU-audio offline Perfetto capture validator."""

from __future__ import annotations

import hashlib
import importlib.util
import os
import stat
import sqlite3
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).with_name("validate_gpu_audio_trace_captures.py")
SPEC = importlib.util.spec_from_file_location("validate_gpu_audio_trace_captures", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


FAKE_PROCESSOR = """#!/usr/bin/env python3
import os
import sys
from pathlib import Path

if sys.argv[1:] == ['--version']:
    print('Perfetto ' + os.environ.get('GPU_AUDIO_TRACE_FAKE_VERSION', 'v57.2-test'))
    raise SystemExit(0)

query = Path(sys.argv[3]).read_text(encoding='utf-8')
audit = os.environ.get('GPU_AUDIO_TRACE_QUERY_AUDIT')
if audit:
    with Path(audit).open('a', encoding='utf-8') as handle:
        handle.write(query)
        handle.write('\\n-- query boundary --\\n')

positive = {
    'session_rows': 1,
    'block_rows': 2,
    'admission_rows': 2,
    'terminal_rows': 2,
    'eligible_rows': 2,
    'delivery_rows': 2,
    'recovery_rows': 0,
    'capture_issue_rows': 0,
    'full_lifecycle_issue_rows': 0,
    'full_lifecycle_generation_rows': 1,
    'lifecycle_violation_rows': 0,
    'duplicate_terminal_rows': 0,
    'duplicate_delivery_rows': 0,
    'admission_violation_rows': 0,
    'duplicate_admission_rows': 0,
    'unavailable_gpu_null_rows': 2,
    'unavailable_gpu_nonnull_rows': 0,
}
negative = {
    'queue_dropped_issue_rows': 1,
    'full_lifecycle_issue_rows': 2,
    'lifecycle_violation_rows': 2,
    'missing_gpu_terminal_rows': 1,
    'missing_delivery_rows': 1,
    'orphan_terminal_rows': 1,
    'delivery_without_eligibility_rows': 1,
    'duplicate_terminal_rows': 1,
    'duplicate_delivery_rows': 1,
    'missing_terminal_for_admission_rows': 1,
    'terminal_without_admission_rows': 1,
    'duplicate_admission_rows': 1,
}
metrics = positive if Path(sys.argv[4]).name.startswith('positive') else negative
if os.environ.get('GPU_AUDIO_TRACE_FAKE_BAD_POSITIVE') and metrics is positive:
    metrics = dict(metrics)
    metrics['session_rows'] = 0
for name, value in metrics.items():
    print(f'pulp_gpu_audio_validation:{name}={value}')
"""


class ValidateGpuAudioTraceCapturesTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.definitions = self.root / "definitions.sql"
        self.definitions.write_text("-- checked-in definitions remain immutable\n", encoding="utf-8")
        self.positive = self.root / "positive.pftrace"
        self.negative = self.root / "negative.pftrace"
        self.positive.write_bytes(b"positive")
        self.negative.write_bytes(b"negative")
        self.processor = self.root / "trace_processor_shell"
        self.processor.write_text(FAKE_PROCESSOR, encoding="utf-8")
        self.processor.chmod(self.processor.stat().st_mode | stat.S_IXUSR)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_controls_pass_and_combined_query_leaves_definitions_unchanged(self) -> None:
        original_digest = hashlib.sha256(self.definitions.read_bytes()).hexdigest()
        audit = self.root / "queries.sql"
        with mock.patch.dict(os.environ, {"GPU_AUDIO_TRACE_QUERY_AUDIT": str(audit)}):
            positive, negative = MODULE.validate_captures(
                self.processor, self.definitions, self.positive, self.negative,
            )

        self.assertEqual(positive["block_rows"], 2)
        self.assertGreaterEqual(negative["queue_dropped_issue_rows"], 1)
        self.assertEqual(hashlib.sha256(self.definitions.read_bytes()).hexdigest(), original_digest)
        combined = audit.read_text(encoding="utf-8")
        self.assertIn("-- checked-in definitions remain immutable", combined)
        self.assertIn("pulp_gpu_audio_validation:session_rows=", combined)
        self.assertIn("pulp_gpu_audio_validation:queue_dropped_issue_rows=", combined)

    def test_positive_control_mismatch_fails_closed(self) -> None:
        with mock.patch.dict(os.environ, {"GPU_AUDIO_TRACE_FAKE_BAD_POSITIVE": "1"}):
            with self.assertRaisesRegex(MODULE.ValidationError, "session_rows=0"):
                MODULE.validate_captures(
                    self.processor, self.definitions, self.positive, self.negative,
                )

    def test_nonexecutable_processor_is_refused_before_query(self) -> None:
        self.processor.chmod(stat.S_IRUSR | stat.S_IWUSR)
        with self.assertRaisesRegex(MODULE.ValidationError, "not executable"):
            MODULE.validate_captures(
                self.processor, self.definitions, self.positive, self.negative,
            )

    def test_missing_or_duplicate_metrics_fail_closed(self) -> None:
        with self.assertRaisesRegex(MODULE.ValidationError, "required metric"):
            MODULE._parse_metrics("", {"block_rows"})
        with self.assertRaisesRegex(MODULE.ValidationError, "duplicate metric"):
            MODULE._parse_metrics("pulp_gpu_audio_validation:block_rows=2\n" * 2,
                                  {"block_rows"})

    def test_similar_version_prefix_does_not_satisfy_pin(self) -> None:
        with mock.patch.dict(os.environ, {"GPU_AUDIO_TRACE_FAKE_VERSION": "v57.20-test"}):
            with self.assertRaisesRegex(MODULE.ValidationError, "Pulp-pinned Perfetto"):
                MODULE._require_pinned_processor(self.processor)

    def test_wrong_processor_version_is_refused_before_query(self) -> None:
        with mock.patch.dict(os.environ, {"GPU_AUDIO_TRACE_FAKE_VERSION": "v58.0-test"}):
            with self.assertRaisesRegex(MODULE.ValidationError, "Pulp-pinned Perfetto v57.2"):
                MODULE.validate_captures(
                    self.processor, self.definitions, self.positive, self.negative,
                )


class GpuAudioTraceSqlTests(unittest.TestCase):
    """Exercise the checked-in relational views with an EXTRACT_ARG fixture.

    Only Perfetto's CREATE syntax is translated. Production SQL expressions
    and joins execute unchanged; real Perfetto captures test the import layer.
    """

    def setUp(self) -> None:
        self.db = sqlite3.connect(":memory:")
        self.arguments: dict[int, dict] = {}
        self.db.create_function("EXTRACT_ARG", 2,
                                lambda row, key: self.arguments[row].get(key))
        self.db.executescript("""
            CREATE TABLE slice (id INT, ts INT, name TEXT, dur INT,
                                arg_set_id INT, track_id INT, category TEXT);
            CREATE TABLE thread_track (id INT, utid INT);
            CREATE TABLE thread (utid INT, upid INT);
            CREATE TABLE stats (name TEXT, severity TEXT, value INT);
            INSERT INTO thread_track VALUES (1, 1), (2, 2);
            INSERT INTO thread VALUES (1, 10), (2, 20);
        """)
        sql = (SCRIPT.parents[2] / ".agents/skills/trace-sql/pulp_gpu_audio_blocks.sql").read_text()
        self.db.executescript(sql.replace("CREATE OR REPLACE PERFETTO VIEW", "CREATE VIEW"))
        self.add("session", success_stride=1, capture_admissions=1,
                 cpu_clock="worker.monotonic", event_time="drain", gpu_clock_mapped=0)
        for sequence in (0, 1):
            self.add("admission", sequence=sequence)
            self.add("eligible", sequence=sequence)
            self.add("terminal", sequence=sequence, gpu_work_admitted=1, output_eligible=0,
                     gpu_terminal="completed_accepted", outcome="success", gpu_reason="none",
                     gpu_elapsed_available=0, gpu_elapsed_ns=-1)
            self.add("delivery", sequence=sequence, output_eligible=1,
                     delivery="gpu_delivered", delivery_reason="none")
        self.counter = self.add("counters", admissions_attempted=2, admissions_enqueued=2,
                                admissions_dropped=0, admissions_drained=2, attempted=6,
                                sampled_out=0, invalid=0, enqueued=6, dropped=0, drained=6)

    def tearDown(self) -> None:
        self.db.close()

    def add(self, event: str, *, track: int = 1, **fields) -> int:
        row = len(self.arguments) + 1
        self.arguments[row] = {f"debug.{key}": value for key, value in
                               dict(schema=2, engine_id=7, generation=1, **fields).items()}
        self.db.execute("INSERT INTO slice VALUES (?, ?, ?, 0, ?, ?, 'gpu')",
                        (row, row * 100, f"gpu.audio.{event}", row, track))
        return row

    def rows(self, view: str) -> list[tuple]:
        return self.db.execute(f"SELECT * FROM pulp_gpu_audio_{view}").fetchall()

    def require_unqualified(self) -> None:
        self.assertEqual(self.rows("full_lifecycle_generations"), [])

    def test_complete_independent_producer_facts_qualify(self) -> None:
        self.assertEqual(self.rows("capture_issues"), [])
        self.assertEqual(self.rows("full_lifecycle_issues"), [])
        self.assertEqual(self.rows("full_lifecycle_generations"), [(10, 7, 1)])
        self.assertEqual(self.db.execute("SELECT sequence, gpu_elapsed_ns FROM "
                                        "pulp_gpu_audio_blocks ORDER BY sequence").fetchall(),
                         [(0, None), (1, None)])

    def test_missing_terminal_is_detected_even_when_delivery_remains(self) -> None:
        self.db.execute("DELETE FROM slice WHERE name = 'gpu.audio.terminal' AND id = 4")
        self.require_unqualified()
        self.assertIn("missing_gpu_terminal", [row[0] for row in self.rows("lifecycle_violations")])
        self.assertIn("gpu_delivery_without_acceptance",
                      [row[0] for row in self.rows("lifecycle_violations")])

    def test_missing_eligibility_and_missing_delivery_are_independent(self) -> None:
        for event, expected in (("eligible", "delivery_without_eligibility"),
                                ("delivery", "missing_delivery")):
            with self.subTest(event=event):
                self.db.execute("SAVEPOINT missing")
                self.db.execute("DELETE FROM slice WHERE name = ?", (f"gpu.audio.{event}",))
                self.require_unqualified()
                self.assertIn(expected, [row[0] for row in self.rows("lifecycle_violations")])
                self.db.execute("ROLLBACK TO missing")
                self.db.execute("RELEASE missing")

    def test_each_identity_census_rejects_duplicates(self) -> None:
        for event, view in (("admission", "admissions"), ("eligible", "eligible"),
                            ("terminal", "terminals"), ("delivery", "deliveries")):
            with self.subTest(event=event):
                self.db.execute("SAVEPOINT duplicate")
                self.db.execute("INSERT INTO slice SELECT id + 100, ts + 1, name, dur, "
                                "arg_set_id, track_id, category FROM slice WHERE name = ?",
                                (f"gpu.audio.{event}",))
                self.assertEqual(len(self.rows(f"duplicate_{view}")), 2)
                self.require_unqualified()
                self.db.execute("ROLLBACK TO duplicate")
                self.db.execute("RELEASE duplicate")

    def test_missing_required_annotations_cannot_pass_through_sql_null(self) -> None:
        for row, field in ((1, "capture_admissions"), (1, "success_stride"),
                           (self.counter, "drained"), (self.counter, "admissions_drained"),
                           (4, "gpu_work_admitted"), (4, "gpu_terminal"),
                           (4, "gpu_elapsed_available"), (4, "gpu_reason"), (5, "output_eligible"),
                           (5, "delivery_reason"),
                           (5, "delivery"), (2, "sequence")):
            with self.subTest(row=row, field=field):
                value = self.arguments[row].pop(f"debug.{field}")
                self.require_unqualified()
                self.arguments[row][f"debug.{field}"] = value

    def test_engine_identity_never_joins_across_processes(self) -> None:
        self.db.execute("UPDATE slice SET track_id = 2 WHERE name = 'gpu.audio.terminal'")
        self.require_unqualified()
        issues = [row[0] for row in self.rows("lifecycle_violations")]
        self.assertIn("missing_gpu_terminal", issues)
        self.assertIn("orphan_terminal", issues)

    def test_final_counter_snapshot_is_not_independent_maxima(self) -> None:
        fields = {key.removeprefix("debug."): value for key, value in
                  self.arguments[self.counter].items()
                  if key not in ("debug.schema", "debug.engine_id", "debug.generation")}
        self.add("counters", **dict(fields, drained=5))
        self.assertIn("queue_not_drained", [row[0] for row in self.rows("capture_issues")])
        self.require_unqualified()

    def test_loss_and_legacy_schema_fail_closed(self) -> None:
        self.arguments[4]["debug.schema"] = 1
        self.require_unqualified()
        self.arguments[4]["debug.schema"] = 2
        self.db.execute("INSERT INTO stats VALUES ('traced_buf_write_wrap_count', 'info', 1)")
        self.assertIn("processor_data_loss", [row[0] for row in self.rows("capture_issues")])
        self.require_unqualified()

    def test_valid_labels_cannot_hide_contradictory_terminal_outcomes(self) -> None:
        for disposition in ("provider_failed", "late_rejected", "stale_rejected",
                            "device_lost", "cancelled_teardown"):
            with self.subTest(disposition=disposition):
                self.arguments[4]["debug.gpu_terminal"] = disposition
                self.require_unqualified()

    def test_empty_generation_does_not_borrow_another_generations_positive_control(self) -> None:
        session = self.add("session", success_stride=1, capture_admissions=1,
                           cpu_clock="worker.monotonic", event_time="drain", gpu_clock_mapped=0)
        counter = self.add("counters", admissions_attempted=0, admissions_enqueued=0,
                           admissions_dropped=0, admissions_drained=0, attempted=0,
                           sampled_out=0, invalid=0, enqueued=0, dropped=0, drained=0)
        self.arguments[session]["debug.generation"] = 2
        self.arguments[counter]["debug.generation"] = 2
        self.assertEqual(self.rows("full_lifecycle_generations"), [(10, 7, 1)])

    def test_recovery_requires_a_new_epoch_and_quiescence(self) -> None:
        recovery = self.add("recovery", sequence=0, next_generation=2, quiescent=1)
        for name in ("attempted", "enqueued", "drained"):
            self.arguments[self.counter][f"debug.{name}"] += 1
        self.assertEqual(self.rows("full_lifecycle_issues"), [])
        self.arguments[recovery]["debug.quiescent"] = 0
        self.require_unqualified()
        self.arguments[recovery]["debug.quiescent"] = 1
        self.arguments[recovery]["debug.next_generation"] = 1
        self.require_unqualified()

    def test_fallback_does_not_rewrite_terminal_fact(self) -> None:
        self.arguments[4].update({"debug.gpu_terminal": "late_rejected",
                                  "debug.outcome": "late_rejected",
                                  "debug.gpu_reason": "deadline_exceeded"})
        self.arguments[5].update({"debug.delivery": "silence_delivered",
                                  "debug.delivery_reason": "deadline_exceeded"})
        self.assertEqual(self.rows("full_lifecycle_issues"), [])
        self.assertEqual(self.db.execute("SELECT gpu_terminal, delivery FROM "
                                        "pulp_gpu_audio_blocks WHERE sequence = 0").fetchone(),
                         ("late_rejected", "silence_delivered"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
