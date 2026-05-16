#!/usr/bin/env python3
"""Unit tests for coverage_tier_check.py (#566 Phase 2).

Pure-Python, no subprocess, no CI dependency. Exercises every branch
of classify_file / aggregate / render so a regression fails fast.
"""

from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest
from unittest import mock

import coverage_tier_check as ctc

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
TARGETS = REPO_ROOT / "ci" / "coverage-targets.yaml"

# First-party source roots audited by `ci/coverage-targets.yaml`.
# Mirrors the audit recipe in issue #1056. Every C/C++/Obj-C/Kotlin/
# Swift/TS/JS file under these prefixes must classify into exactly one
# tier OR appear in `_TIER_COMPLETENESS_ALLOWLIST` with a documented
# reason. `packages/` was added in pulp #1886 Phase 1 alongside the
# vitest v8 lane in `packages/pulp-react/**`.
_FIRST_PARTY_PREFIXES = (
    "core/", "tools/", "apple/", "android/", "inspect/",
    # Only the JS/TS package wired to vitest coverage is audited in
    # Phase 1. Other entries under `packages/` (e.g. pulp-import-ir)
    # join the audit when their coverage lane lands — adding them to
    # the prefix list without a coverage source would silently fail
    # the completeness check for files that genuinely have no Cobertura
    # data yet.
    "packages/pulp-react/",
)
# Extension list MUST stay in lock-step with the
# `coverage_tier_check.py::_INSTRUMENTED_EXTS` set + every
# Kotlin/Swift extension we audit. A drift between the two means the
# completeness gate silently skips files that the coverage pipeline
# DOES instrument. See `test_audit_suffixes_cover_every_instrumented_extension`
# below for the structural assertion. Codex P2 on PR #1154.
_FIRST_PARTY_SUFFIXES = (
    # C/C++/Obj-C/Obj-C++ — every C-family extension in
    # coverage_tier_check._INSTRUMENTED_EXTS:
    ".c", ".cc", ".cpp", ".cxx", ".c++",
    ".h", ".hh", ".hpp", ".hxx", ".h++",
    ".m", ".mm",
    # JS / TS — measured by the vitest v8 lane (pulp #1886 Phase 1):
    ".ts", ".tsx", ".js", ".jsx",
    # Non-C-family first-party sources (Kotlin / Swift) audited here
    # but classified by the Kotlin / Swift coverage lanes, not by
    # `is_instrumented_source`:
    ".kt", ".swift",
)

# Files that intentionally fall outside every tier. Empty by design —
# the gate catches new gaps before they go silently uncovered. Add an
# entry here ONLY with an explanatory comment AND a linked issue.
_TIER_COMPLETENESS_ALLOWLIST: frozenset[str] = frozenset()


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

    def test_live_targets_classify_inspect_as_user_facing(self) -> None:
        tiers = ctc.load_targets(TARGETS)
        tier = ctc.classify_file("inspect/src/inspector_server.cpp", tiers)
        self.assertIsNotNone(tier)
        self.assertEqual(tier.name, "user-facing")


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

    def test_c_family_sources_are_instrumented(self) -> None:
        # Codex P2 on PR #1154 — `is_instrumented_source` accepts every
        # C-family extension (.c / .cc / .cxx / .m), but the
        # completeness audit was only walking .cpp/.hpp/.mm/.h/.kt/.swift,
        # which silently skipped real instrumented sources (e.g. a
        # `core/audio/src/codecs.c`).
        for p in (
            "core/audio/src/codecs.c",
            "core/audio/src/foo.cc",
            "core/audio/src/bar.cxx",
            "platform/mac/baz.m",
            "core/dsp/src/quux.c++",
            "core/audio/include/mixer.hh",
            "core/midi/include/parse.hxx",
            "core/audio/include/api.h++",
        ):
            self.assertTrue(ctc.is_instrumented_source(p), msg=p)

    def test_non_cpp_is_not_instrumented(self) -> None:
        for p in ("tools/cmake/PulpUtils.cmake", "tools/build-skia.sh",
                  "tools/scripts/coverage_tier_check.py",
                  "ship/templates/appcast.xml.in", "README.md"):
            self.assertFalse(ctc.is_instrumented_source(p), msg=p)

    def test_ts_js_sources_are_instrumented(self) -> None:
        # pulp #1886 Phase 1 — the vitest v8 lane in
        # `packages/pulp-react/**` emits Cobertura rows for these
        # extensions, so the tier gate must treat them as instrumented.
        # Without this, a TS-only PR like the one that motivated #1886
        # has its diff lines filtered out of the per-tier aggregate
        # entirely and the gate silently reports "no touched lines".
        for p in (
            "packages/pulp-react/src/host-config.ts",
            "packages/pulp-react/src/components/Knob.tsx",
            "packages/pulp-react/src/index.js",
            "packages/pulp-react/src/legacy/foo.jsx",
        ):
            self.assertTrue(ctc.is_instrumented_source(p), msg=p)

    def test_ts_outside_reported_surface_not_instrumented(self) -> None:
        # Codex P2 on pulp #1886 — JS/TS coverage is scoped to
        # `packages/pulp-react/**`. A `.ts` / `.tsx` / `.js` / `.jsx`
        # file outside that surface (`tools/`, `inspect/`, anywhere
        # else) has no Cobertura row by design; treating it as
        # instrumented would mark every diff line as uncovered and
        # falsely fail the tier. Mirror non-source handling instead.
        for p in (
            "tools/scripts/foo.ts",
            "tools/cli/dev.tsx",
            "inspect/web/legacy/init.js",
            "core/view/src/widgets/dev_only.jsx",
            "packages/pulp-import-ir/src/main.ts",  # different package
        ):
            self.assertFalse(ctc.is_instrumented_source(p), msg=p)

    def test_ts_outside_surface_does_not_tank_tier(self) -> None:
        # End-to-end: a `tools/scripts/foo.ts` file under the
        # infrastructure tier with no Cobertura row used to count as
        # 100% uncovered. After the fix it's treated as
        # non-instrumented (same as a `.sh` / `.cmake` / `.py` file)
        # and the tier reports "no touched lines".
        results = ctc.aggregate(
            TIERS,
            ["tools/scripts/foo.ts"],
            {},
            lines_getter=lambda _p: {1, 2, 3, 4, 5},
        )
        infra = next(r for r in results if r.tier.name == "infrastructure")
        self.assertEqual(infra.touched_lines, 0)
        self.assertTrue(infra.passed)

    def test_ts_aggregate_counts_diff_lines(self) -> None:
        # End-to-end shape: a TS file under `packages/pulp-react/**`
        # classified into `user-facing` (70%), with Cobertura hits
        # supplied, contributes touched + covered lines to the tier.
        # This guards against regressions where a future refactor of
        # `is_instrumented_source` or `_INSTRUMENTED_EXTS` accidentally
        # drops the JS/TS branch.
        tiers = [
            ctc.Tier(
                name="user-facing",
                line_target=70,
                paths=("packages/pulp-react/**",),
            ),
        ]
        cov = {
            "packages/pulp-react/src/host-config.ts": ctc.FileCoverage(
                path="packages/pulp-react/src/host-config.ts",
                hits={10: 5, 11: 0, 12: 3, 13: 1, 14: 0},
            ),
        }
        results = ctc.aggregate(
            tiers,
            ["packages/pulp-react/src/host-config.ts"],
            cov,
            lines_getter=lambda _p: {10, 11, 12, 13, 14},
        )
        uf = next(r for r in results if r.tier.name == "user-facing")
        self.assertEqual(uf.touched_lines, 5)
        self.assertEqual(uf.covered_lines, 3)
        self.assertAlmostEqual(uf.percent, 60.0, places=1)
        # 60% < 70% floor — used to fail; under Phase 1's measure-only
        # gate (`continue-on-error` in the workflow) the failure is
        # advisory, not blocking. The assertion here verifies the
        # *aggregation* math, independent of enforcement policy.
        self.assertFalse(uf.passed)


class FirstPartySuffixDriftTests(unittest.TestCase):
    """Test-side suffix list must include every C-family ext that the
    coverage pipeline instruments. Codex P2 on PR #1154.

    Without this guard, the completeness gate silently skips real
    instrumented sources whose extension was added to
    `coverage_tier_check._INSTRUMENTED_EXTS` but not to the audit walk
    here — so a `core/audio/src/codecs.c` would never be classified
    into any tier and the gate would report green.
    """

    def test_audit_suffixes_cover_every_instrumented_extension(self) -> None:
        # The audit walks {C-family ∪ Kotlin ∪ Swift}. The C-family
        # subset MUST be a superset of the C-family extensions that
        # `is_instrumented_source` accepts.
        c_family_in_audit = {s for s in _FIRST_PARTY_SUFFIXES if s not in (".kt", ".swift")}
        missing = ctc._INSTRUMENTED_EXTS - c_family_in_audit
        self.assertEqual(
            missing, set(),
            f"_FIRST_PARTY_SUFFIXES is missing C-family extension(s) "
            f"{sorted(missing)} that the coverage pipeline instruments "
            "(see coverage_tier_check._INSTRUMENTED_EXTS). The completeness "
            "audit will silently skip files with these extensions, hiding "
            "real coverage gaps. Either add them to _FIRST_PARTY_SUFFIXES "
            "or remove them from _INSTRUMENTED_EXTS — keep the two in sync.",
        )

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


class MainEntrypointTests(unittest.TestCase):

    def test_main_skips_cleanly_when_cobertura_xml_is_missing(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            report = root / "tier-report.md"
            rc = ctc.main([
                "--cobertura", str(root / "missing.xml"),
                "--targets", str(root / "targets.yaml"),
                "--compare-branch", "origin/main",
                "--markdown-report", str(report),
            ])

            self.assertEqual(rc, 0)
            self.assertIn("Cobertura XML missing", report.read_text(encoding="utf-8"))

    def test_main_writes_report_and_returns_zero_when_all_tiers_pass(self) -> None:
        tiers = [ctc.Tier("audio-critical", 80, ("core/audio/**",))]
        coverage = {
            "core/audio/src/foo.cpp": ctc.FileCoverage(
                path="core/audio/src/foo.cpp",
                hits={10: 1, 11: 1},
            ),
        }

        with tempfile.TemporaryDirectory() as td, \
             mock.patch.object(ctc, "load_targets", return_value=tiers), \
             mock.patch.object(ctc, "parse_cobertura", return_value=coverage), \
             mock.patch.object(ctc, "diff_files", return_value=["core/audio/src/foo.cpp"]), \
             mock.patch.object(ctc, "diff_lines", return_value={10, 11}):
            root = pathlib.Path(td)
            cobertura = root / "coverage.xml"
            cobertura.write_text("<coverage />", encoding="utf-8")
            report = root / "tier-report.md"

            rc = ctc.main([
                "--cobertura", str(cobertura),
                "--targets", str(root / "targets.yaml"),
                "--compare-branch", "origin/main",
                "--markdown-report", str(report),
            ])

            self.assertEqual(rc, 0)
            self.assertIn("All touched tiers meet", report.read_text(encoding="utf-8"))

    def test_main_returns_one_when_a_tier_fails(self) -> None:
        tiers = [ctc.Tier("audio-critical", 80, ("core/audio/**",))]
        coverage = {
            "core/audio/src/foo.cpp": ctc.FileCoverage(
                path="core/audio/src/foo.cpp",
                hits={10: 1, 11: 0},
            ),
        }

        with tempfile.TemporaryDirectory() as td, \
             mock.patch.object(ctc, "load_targets", return_value=tiers), \
             mock.patch.object(ctc, "parse_cobertura", return_value=coverage), \
             mock.patch.object(ctc, "diff_files", return_value=["core/audio/src/foo.cpp"]), \
             mock.patch.object(ctc, "diff_lines", return_value={10, 11}):
            root = pathlib.Path(td)
            cobertura = root / "coverage.xml"
            cobertura.write_text("<coverage />", encoding="utf-8")
            report = root / "tier-report.md"

            rc = ctc.main([
                "--cobertura", str(cobertura),
                "--targets", str(root / "targets.yaml"),
                "--compare-branch", "origin/main",
                "--markdown-report", str(report),
            ])

            self.assertEqual(rc, 1)
            self.assertIn("Per-tier gate failed", report.read_text(encoding="utf-8"))


def _enumerate_first_party_sources() -> list[str]:
    """Return repo-relative paths of first-party source files in scope.

    Mirrors the audit recipe pinned in issue #1056 — same prefixes and
    suffixes — so the test guards exactly what the recipe walks.
    """
    out = subprocess.check_output(
        ["git", "ls-files"],
        text=True,
        cwd=str(REPO_ROOT),
    )
    files = [line.strip() for line in out.splitlines() if line.strip()]
    return [
        f for f in files
        if f.startswith(_FIRST_PARTY_PREFIXES)
        and f.endswith(_FIRST_PARTY_SUFFIXES)
    ]


class TierCoverageCompleteness(unittest.TestCase):
    """Audit gate (#1056): every tier matches ≥1 file, every file → 1 tier.

    Mirrors the structural lock-in pattern from #1005 (commits efefe144 +
    b258730c) and the inspect classification fix from #842. Two failure
    modes the runtime gate cannot catch on its own:

    1. A tier whose globs match nothing (silent no-op — the floor never
       binds). Hits the same bug class as #1049.
    2. A first-party source file outside every tier (silently exempt
       from per-tier enforcement; falls through to the looser global
       diff-cover floor).

    Both are caught here so the gate stays meaningful as the tree grows.
    """

    def test_every_tier_matches_at_least_one_file(self) -> None:
        tiers = ctc.load_targets(TARGETS)
        sources = _enumerate_first_party_sources()
        self.assertGreater(len(sources), 0, "git ls-files returned no first-party sources")
        empties: list[str] = []
        for tier in tiers:
            hits = [f for f in sources if ctc.classify_file(f, tiers) is tier]
            if not hits:
                empties.append(tier.name)
        self.assertEqual(
            empties, [],
            "Tiers with zero matching first-party files (silent no-op): "
            f"{empties}. Either tighten the patterns, remove the tier, or "
            "document the deliberate emptiness in coverage-targets.yaml.",
        )

    def test_every_first_party_source_file_in_exactly_one_tier(self) -> None:
        tiers = ctc.load_targets(TARGETS)
        sources = _enumerate_first_party_sources()
        unmatched = [
            f for f in sources
            if ctc.classify_file(f, tiers) is None
            and f not in _TIER_COMPLETENESS_ALLOWLIST
        ]
        self.assertEqual(
            unmatched, [],
            f"{len(unmatched)} first-party source file(s) match no tier in "
            "ci/coverage-targets.yaml — they would silently fall back to the "
            "global diff-cover floor. Either classify them under an existing "
            "tier or add a documented entry to _TIER_COMPLETENESS_ALLOWLIST. "
            f"Unmatched: {unmatched[:10]}{'...' if len(unmatched) > 10 else ''}",
        )

    def test_allowlist_entries_actually_exist(self) -> None:
        # Stale allowlist entries are themselves a silent gap: the file
        # may have been deleted, leaving a phantom exemption that masks a
        # future re-add of the same path.
        sources = set(_enumerate_first_party_sources())
        stale = sorted(p for p in _TIER_COMPLETENESS_ALLOWLIST if p not in sources)
        self.assertEqual(
            stale, [],
            f"Allowlist references files that no longer exist: {stale}. "
            "Remove them from _TIER_COMPLETENESS_ALLOWLIST.",
        )


if __name__ == "__main__":
    unittest.main()
