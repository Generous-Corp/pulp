#!/usr/bin/env python3
"""Unit tests for coverage_tier_check.py (#566 Phase 2).

Pure-Python, no subprocess, no CI dependency. Exercises every branch
of classify_file / aggregate / render so a regression fails fast.
"""

from __future__ import annotations

import unittest

import coverage_tier_check as ctc


TIERS = [
    ctc.Tier(
        name="audio-critical",
        line_target=80,
        paths=("core/audio/**", "core/midi/**"),
    ),
    ctc.Tier(
        name="infrastructure",
        line_target=50,
        paths=("core/events/**", "tools/**"),
    ),
]


class ClassifyTests(unittest.TestCase):

    def test_matches_audio_prefix(self) -> None:
        t = ctc.classify_file("core/audio/src/foo.cpp", TIERS)
        self.assertIsNotNone(t)
        self.assertEqual(t.name, "audio-critical")

    def test_matches_midi_prefix(self) -> None:
        t = ctc.classify_file("core/midi/include/pulp/midi/mpe_buffer.hpp", TIERS)
        self.assertIsNotNone(t)
        self.assertEqual(t.name, "audio-critical")

    def test_matches_infrastructure(self) -> None:
        t = ctc.classify_file("tools/cli/cmd_pr.cpp", TIERS)
        self.assertIsNotNone(t)
        self.assertEqual(t.name, "infrastructure")

    def test_unmatched_file_is_none(self) -> None:
        # Files outside any configured tier path must return None so
        # the global 75% gate applies instead of a per-tier rule.
        self.assertIsNone(ctc.classify_file("docs/guides/coverage.md", TIERS))
        self.assertIsNone(ctc.classify_file("README.md", TIERS))


class AggregateTests(unittest.TestCase):

    def _coverage(self, data: dict[str, dict[int, int]]) -> dict[str, ctc.FileCoverage]:
        return {
            path: ctc.FileCoverage(path=path, hits=hits)
            for path, hits in data.items()
        }

    def test_all_covered_passes(self) -> None:
        cov = self._coverage({
            "core/audio/src/foo.cpp": {10: 1, 11: 1, 12: 1, 13: 1, 14: 1},
        })
        # 5/5 lines covered = 100%, easily beats 80% audio-critical floor
        results = ctc.aggregate(
            TIERS,
            ["core/audio/src/foo.cpp"],
            cov,
            lines_getter=lambda _p: {10, 11, 12, 13, 14},
        )
        audio = next(r for r in results if r.tier.name == "audio-critical")
        self.assertEqual(audio.touched_lines, 5)
        self.assertEqual(audio.covered_lines, 5)
        self.assertAlmostEqual(audio.percent, 100.0)
        self.assertTrue(audio.passed)

    def test_sub_threshold_fails(self) -> None:
        cov = self._coverage({
            "core/audio/src/foo.cpp": {10: 1, 11: 0, 12: 0, 13: 0, 14: 0},
        })
        # 1/5 = 20%, way under 80% → fail
        results = ctc.aggregate(
            TIERS,
            ["core/audio/src/foo.cpp"],
            cov,
            lines_getter=lambda _p: {10, 11, 12, 13, 14},
        )
        audio = next(r for r in results if r.tier.name == "audio-critical")
        self.assertEqual(audio.covered_lines, 1)
        self.assertFalse(audio.passed)

    def test_file_without_coverage_counts_as_untested(self) -> None:
        # New source file not present in the Cobertura XML — all
        # changed lines count as uncovered.
        results = ctc.aggregate(
            TIERS,
            ["core/audio/src/newfile.cpp"],
            {},
            lines_getter=lambda _p: {1, 2, 3, 4, 5},
        )
        audio = next(r for r in results if r.tier.name == "audio-critical")
        self.assertEqual(audio.touched_lines, 5)
        self.assertEqual(audio.covered_lines, 0)
        self.assertFalse(audio.passed)

    def test_untouched_tier_is_pass_not_fail(self) -> None:
        # A PR that touches nothing under a tier's paths must NOT
        # fail that tier — empty is a pass, not a 0%.
        results = ctc.aggregate(
            TIERS,
            ["docs/README.md"],  # doesn't match any tier
            {},
            lines_getter=lambda _p: {1, 2},
        )
        for r in results:
            self.assertEqual(r.touched_lines, 0)
            self.assertTrue(r.passed)

    def test_multiple_files_aggregate(self) -> None:
        cov = self._coverage({
            "core/audio/src/a.cpp": {1: 1, 2: 0},  # 1/2
            "core/midi/src/b.cpp": {5: 1, 6: 1},   # 2/2
        })
        results = ctc.aggregate(
            TIERS,
            ["core/audio/src/a.cpp", "core/midi/src/b.cpp"],
            cov,
            lines_getter=lambda p: {1, 2} if "a.cpp" in p else {5, 6},
        )
        audio = next(r for r in results if r.tier.name == "audio-critical")
        self.assertEqual(audio.touched_lines, 4)
        self.assertEqual(audio.covered_lines, 3)
        self.assertAlmostEqual(audio.percent, 75.0, places=1)
        self.assertFalse(audio.passed)  # 75% < 80% floor


class InstrumentedSourceTests(unittest.TestCase):

    def test_cpp_sources_are_instrumented(self) -> None:
        for p in ("core/audio/src/a.cpp", "core/midi/include/x.hpp",
                  "platform/mac/foo.mm", "tools/cli/cmd_pr.cpp"):
            self.assertTrue(ctc.is_instrumented_source(p), msg=p)

    def test_non_cpp_is_not_instrumented(self) -> None:
        for p in ("tools/cmake/PulpUtils.cmake", "tools/build-skia.sh",
                  "tools/scripts/coverage_tier_check.py",
                  "ship/templates/appcast.xml.in", "README.md"):
            self.assertFalse(ctc.is_instrumented_source(p), msg=p)

    def test_aggregate_skips_non_instrumented_files(self) -> None:
        # Codex #612 P1: a PR that only touches CMake/Python under
        # `tools/**` must NOT fail the infrastructure tier because
        # those files never produce Cobertura entries. Without the
        # skip, all their changed lines would be counted as uncovered.
        results = ctc.aggregate(
            TIERS,
            ["tools/cmake/PulpUtils.cmake", "tools/build-skia.sh"],
            {},
            lines_getter=lambda _p: {1, 2, 3, 4, 5, 6, 7, 8, 9, 10},
        )
        infra = next(r for r in results if r.tier.name == "infrastructure")
        self.assertEqual(infra.touched_lines, 0)
        self.assertTrue(infra.passed)


class RenderTests(unittest.TestCase):

    def test_all_pass_banner(self) -> None:
        results = [
            ctc.TierResult(tier=TIERS[0], touched_lines=10, covered_lines=10),
            ctc.TierResult(tier=TIERS[1], touched_lines=0, covered_lines=0),
        ]
        body = ctc.render(results)
        self.assertIn("All touched tiers meet their per-tier floors.", body)
        self.assertIn("100.0%", body)
        self.assertIn("— (no touched lines)", body)

    def test_failure_names_tiers(self) -> None:
        results = [
            ctc.TierResult(tier=TIERS[0], touched_lines=10, covered_lines=5),
            ctc.TierResult(tier=TIERS[1], touched_lines=4, covered_lines=1),
        ]
        body = ctc.render(results)
        self.assertIn("Per-tier gate failed", body)
        self.assertIn("audio-critical", body)
        self.assertIn("infrastructure", body)


if __name__ == "__main__":
    unittest.main()
