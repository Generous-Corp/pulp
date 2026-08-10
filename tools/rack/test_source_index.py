#!/usr/bin/env python3
"""Deterministic tests for the machine-local reading index."""

from __future__ import annotations

import contextlib
import io
import json
import os
import sqlite3
import stat
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import source_index  # noqa: E402


class SourceIndexTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.corpus = os.path.join(self.tmp.name, "corpus")
        self.db = os.path.join(self.corpus, "source-index.sqlite3")
        os.makedirs(os.path.join(self.corpus, "local"))
        os.makedirs(os.path.join(self.corpus, "synth-secrets"))
        self._write(
            "local/strange-electronic-music-systems.txt",
            "Control voltages can be sampled and held to create stepped random "
            "motion.\fA lag processor turns abrupt pitch changes into portamento.",
        )
        self._write(
            "synth-secrets/part-12.md",
            "# Resonant filters\n\nA low-pass filter shapes the brightness of an "
            "oscillator. Resonance emphasizes the cutoff region.",
        )

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def _write(self, relative: str, text: str) -> None:
        path = os.path.join(self.corpus, relative)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as out:
            out.write(text)

    def test_build_query_and_locators(self) -> None:
        summary = source_index.build_index(self.corpus, self.db)
        self.assertEqual((summary.added, summary.updated, summary.unchanged,
                          summary.removed, summary.sources), (2, 0, 0, 0, 2))
        rows = source_index.search(self.db, "sample and hold random voltage")
        self.assertTrue(rows)
        self.assertEqual(rows[0]["work_id"],
                         "allen-strange-electronic-music-systems")
        self.assertEqual(rows[0]["page"], 1)
        self.assertIn("page=1", rows[0]["locator"])
        self.assertRegex(rows[0]["source_sha256"], r"^[0-9a-f]{64}$")
        self.assertRegex(rows[0]["text_sha256"], r"^[0-9a-f]{64}$")

    def test_semantic_alias_finds_different_source_word(self) -> None:
        source_index.build_index(self.corpus, self.db)
        rows = source_index.search(self.db, "glide")
        self.assertTrue(rows)
        self.assertIn("portamento", rows[0]["snippet"].lower())

    def test_parallel_extractions_do_not_repeat_the_same_passage(self) -> None:
        self._write(
            "local/strange-electronic-music-systems-reading-order.txt",
            "Control voltages can be sampled and held to create stepped random "
            "motion.\fA lag processor turns abrupt pitch changes into portamento.",
        )
        source_index.build_index(self.corpus, self.db)
        rows = source_index.search(self.db, "sample and hold random voltage")
        strange = [row for row in rows if row["work_id"] ==
                   "allen-strange-electronic-music-systems"]
        self.assertEqual(len(strange), 1)

    def test_section_and_source_filter_are_preserved(self) -> None:
        source_index.build_index(self.corpus, self.db)
        rows = source_index.search(
            self.db, "brightness resonance", source="sound-on-sound-synth-secrets"
        )
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["section"], "Resonant filters")
        self.assertEqual(rows[0]["source_key"], "synth-secrets/part-12.md")

    def test_incremental_update_removal_and_rebuild_are_deterministic(self) -> None:
        first = source_index.build_index(self.corpus, self.db)
        second = source_index.build_index(self.corpus, self.db)
        self.assertEqual(second.unchanged, first.sources)
        before = source_index.search(self.db, "resonance")

        self._write(
            "synth-secrets/part-12.md",
            "# Resonant filters\n\nFeedback around a filter raises resonance.",
        )
        os.unlink(os.path.join(
            self.corpus, "local/strange-electronic-music-systems.txt"))
        changed = source_index.build_index(self.corpus, self.db)
        self.assertEqual((changed.updated, changed.removed, changed.sources),
                         (1, 1, 1))
        self.assertFalse(source_index.search(self.db, "sample and hold"))

        incremental = source_index.search(self.db, "resonance")
        rebuilt = source_index.build_index(self.corpus, self.db, rebuild=True)
        after = source_index.search(self.db, "resonance")
        self.assertEqual(rebuilt.added, 1)
        fields = ("source_key", "locator", "page", "section", "text_sha256")
        self.assertEqual([{k: row[k] for k in fields} for row in incremental],
                         [{k: row[k] for k in fields} for row in after])
        self.assertNotEqual(before[0]["text_sha256"], after[0]["text_sha256"])

    def test_query_syntax_is_data_and_result_count_is_bounded(self) -> None:
        source_index.build_index(self.corpus, self.db)
        rows = source_index.search(self.db, 'filter OR "unterminated : *', limit=999)
        self.assertLessEqual(len(rows), source_index.MAX_RESULTS)

    def test_decoder_debris_does_not_create_search_evidence(self) -> None:
        self._write(
            "local/broken-recovery.txt",
            "modu·ati·n ··ltag· rand·m ····uenc·r ··lter " * 100,
        )
        source_index.build_index(self.corpus, self.db)
        rows = source_index.search(self.db, "random sequencer filter")
        self.assertNotIn("local/broken-recovery.txt",
                         {row["source_key"] for row in rows})
        unsearchable = source_index.status(self.db)["unsearchable_sources"]
        self.assertIn("local/broken-recovery.txt",
                      {row["source_key"] for row in unsearchable})

    def test_self_declared_imperfect_recovery_is_quarantined_whole(self) -> None:
        self._write(
            "local/imperfect.txt",
            "# Recovered source\nMachine output, imperfect.\n\n"
            "A convincing fragment mentions oscillator feedback and random clocks.",
        )
        source_index.build_index(self.corpus, self.db)
        rows = source_index.search(self.db, "oscillator feedback random clocks")
        self.assertNotIn("local/imperfect.txt", {row["source_key"] for row in rows})

    def test_recovered_page_markers_are_real_locators(self) -> None:
        self._write(
            "local/recovered-notes.txt",
            "Recovery notes.\n=== page 47 ===\nA clean oscillator technique uses "
            "feedback to shape a waveform.",
        )
        source_index.build_index(self.corpus, self.db)
        rows = source_index.search(self.db, "feedback waveform",
                                   source="recovered-notes")
        self.assertEqual(rows[0]["page"], 47)
        self.assertIn("page=47", rows[0]["locator"])

    def test_newer_schema_fails_closed(self) -> None:
        os.makedirs(os.path.dirname(self.db), exist_ok=True)
        db = sqlite3.connect(self.db)
        db.execute("PRAGMA user_version = 99")
        db.close()
        with self.assertRaisesRegex(source_index.SourceIndexError, "newer"):
            source_index.connect(self.db)

    def test_query_without_build_fails_instead_of_reporting_no_matches(self) -> None:
        with self.assertRaisesRegex(source_index.SourceIndexError, "run.*build"):
            source_index.search(self.db, "oscillator")

    def test_missing_corpus_fails_instead_of_building_an_empty_index(self) -> None:
        missing = os.path.join(self.tmp.name, "not-there")
        with self.assertRaisesRegex(source_index.SourceIndexError, "no reading corpus"):
            source_index.build_index(missing)

    def test_cli_json_is_bounded_and_machine_readable(self) -> None:
        source_index.build_index(self.corpus, self.db)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            rc = source_index.main([
                "--corpus", self.corpus, "--db", self.db,
                "query", "filter cutoff", "--json", "--limit", "1",
            ])
        self.assertEqual(rc, 0)
        rows = json.loads(output.getvalue())
        self.assertEqual(len(rows), 1)
        self.assertIn("locator", rows[0])
        self.assertNotIn("path", rows[0])

    def test_database_defaults_inside_non_shipping_corpus(self) -> None:
        self.assertEqual(source_index.default_db_path(self.corpus), self.db)
        summary = source_index.build_index(self.corpus)
        self.assertEqual(summary.sources, 2)
        self.assertTrue(os.path.isfile(self.db))
        if os.name == "posix":
            self.assertEqual(stat.S_IMODE(os.stat(self.db).st_mode), 0o600)


if __name__ == "__main__":
    unittest.main(verbosity=2)
