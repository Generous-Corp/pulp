#!/usr/bin/env python3
"""Coverage tests for tools/scripts/promote_quirk_tiers.py."""

from __future__ import annotations

import importlib.util
import io
import pathlib
import sys
import tempfile
import textwrap
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parent / "promote_quirk_tiers.py"
spec = importlib.util.spec_from_file_location("promote_quirk_tiers", SCRIPT)
assert spec and spec.loader
pqt = importlib.util.module_from_spec(spec)
sys.modules["promote_quirk_tiers"] = pqt
spec.loader.exec_module(pqt)


SAMPLE_HEADER = textwrap.dedent(
    """\
    #pragma once

    namespace pulp::format {

    enum class QuirkStatus : unsigned char { Validated, Speculative, LessonOnly };

    struct HostQuirksMeta {
        QuirkStatus synthesize_bypass_parameter = QuirkStatus::Validated;
        QuirkStatus reaper_process_while_bypassed = QuirkStatus::Speculative;
        QuirkStatus reaper_midsession_setstate = QuirkStatus::Speculative;
        QuirkStatus fl_studio_state_reader_skip = QuirkStatus::Speculative;
        QuirkStatus cubase9_state_blob_size_validation = QuirkStatus::Speculative;
        QuirkStatus skip_bus_arrangement_call = QuirkStatus::LessonOnly;
    };

    }  // namespace pulp::format
    """
)


def _write(tmp: pathlib.Path, name: str, body: str) -> pathlib.Path:
    path = tmp / name
    path.write_text(body, encoding="utf-8")
    return path


class StatusModelTests(unittest.TestCase):
    def test_alias_normalization(self) -> None:
        self.assertEqual(pqt._normalize_status("Confirmed"), pqt.STATUS_CONFIRMED)
        self.assertEqual(pqt._normalize_status("c"), pqt.STATUS_CONFIRMED)
        self.assertEqual(pqt._normalize_status("REFUTED"), pqt.STATUS_REFUTED)
        self.assertEqual(pqt._normalize_status("nt"), pqt.STATUS_NOT_TRIGGERED)
        self.assertEqual(pqt._normalize_status("N/A"), pqt.STATUS_NOT_TRIGGERED)
        self.assertIsNone(pqt._normalize_status("<C/R/NT>"))
        self.assertIsNone(pqt._normalize_status(""))

    def test_merge_keeps_highest(self) -> None:
        self.assertEqual(
            pqt._merge(None, pqt.STATUS_REFUTED), pqt.STATUS_REFUTED
        )
        self.assertEqual(
            pqt._merge(pqt.STATUS_REFUTED, pqt.STATUS_CONFIRMED),
            pqt.STATUS_CONFIRMED,
        )
        self.assertEqual(
            pqt._merge(pqt.STATUS_CONFIRMED, pqt.STATUS_REFUTED),
            pqt.STATUS_CONFIRMED,
        )
        self.assertEqual(
            pqt._merge(pqt.STATUS_NOT_TRIGGERED, pqt.STATUS_REFUTED),
            pqt.STATUS_NOT_TRIGGERED,
        )


class MarkdownParseTests(unittest.TestCase):
    def test_extracts_confirmed_rows(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            md = _write(
                tmp,
                "06-reaper-vst3.md",
                textwrap.dedent(
                    """\
                    # 06 — Reaper

                    ## Result

                    | Quirk flag                      | Row | Observed | Notes |
                    |---------------------------------|-----|----------|-------|
                    | `reaper_process_while_bypassed` | R1  | Confirmed | step 5 |
                    | `reaper_midsession_setstate`    | R6  | Refuted   | not seen |
                    | `synthesize_bypass_parameter`   | #23 | <C/R/NT>  | placeholder |
                    | `cubase9_state_blob_size_validation` | #4 | nt | wrong host |
                    """
                ),
            )
            got = pqt.parse_markdown_results(md)
            self.assertEqual(
                got,
                {
                    "reaper_process_while_bypassed": pqt.STATUS_CONFIRMED,
                    "reaper_midsession_setstate": pqt.STATUS_REFUTED,
                    "cubase9_state_blob_size_validation": pqt.STATUS_NOT_TRIGGERED,
                },
            )

    def test_missing_file_returns_empty(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            self.assertEqual(pqt.parse_markdown_results(tmp / "no.md"), {})


class LogParseTests(unittest.TestCase):
    def _make_log(self, tmp: pathlib.Path, name: str, events: list[str]) -> pathlib.Path:
        ts = "2026-05-26T12:00:00.000Z"
        lines = [
            f"{ts}\t{event}\tn={i+1}\n" for i, event in enumerate(events)
        ]
        return _write(tmp, name, "".join(lines))

    def test_reaper_log_promotes_multiple_flags(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            log = self._make_log(
                tmp,
                "Reaper-VST3-20260526T120000Z-pid42.log",
                [
                    "session_start",
                    "prepare",
                    "bus_layout_proposal",
                    "deserialize_plugin_state",
                    "process_without_prepare",
                ],
            )
            got = pqt.parse_log_results(log)
            # All Reaper rules whose event appeared should be Confirmed.
            self.assertEqual(got.get("reaper_permissive_bus_arrangements"), pqt.STATUS_CONFIRMED)
            self.assertEqual(got.get("reaper_midsession_setstate"), pqt.STATUS_CONFIRMED)
            self.assertEqual(got.get("reaper_process_while_bypassed"), pqt.STATUS_CONFIRMED)
            self.assertEqual(got.get("clamp_latency_to_nonneg"), pqt.STATUS_CONFIRMED)

    def test_fl_studio_log_promotes_state_reader(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            log = self._make_log(
                tmp,
                "FLStudio-VST3-20260526T120000Z-pid7.log",
                ["session_start", "prepare", "process_without_prepare",
                 "deserialize_plugin_state"],
            )
            got = pqt.parse_log_results(log)
            self.assertEqual(got.get("fl_studio_setactive_process_mutex"),
                             pqt.STATUS_CONFIRMED)
            self.assertEqual(got.get("fl_studio_state_reader_skip"),
                             pqt.STATUS_CONFIRMED)

    def test_unrelated_filename_is_skipped(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            log = _write(tmp, "random-other-file.txt", "garbage\n")
            self.assertEqual(pqt.parse_log_results(log), {})


class PromoteMetaTests(unittest.TestCase):
    def test_promotes_speculative_only_for_confirmed_flags(self) -> None:
        confirmed = {
            "reaper_process_while_bypassed",
            "fl_studio_state_reader_skip",
            "skip_bus_arrangement_call",  # LessonOnly — should NOT promote
            "synthesize_bypass_parameter",  # already Validated — no-op
        }
        new_src, promoted = pqt.promote_meta(SAMPLE_HEADER, confirmed)
        self.assertCountEqual(
            promoted,
            ["reaper_process_while_bypassed", "fl_studio_state_reader_skip"],
        )
        self.assertIn(
            "QuirkStatus reaper_process_while_bypassed = QuirkStatus::Validated;",
            new_src,
        )
        self.assertIn(
            "QuirkStatus fl_studio_state_reader_skip = QuirkStatus::Validated;",
            new_src,
        )
        # LessonOnly preserved.
        self.assertIn(
            "QuirkStatus skip_bus_arrangement_call = QuirkStatus::LessonOnly;",
            new_src,
        )
        # Untouched Speculative rows preserved.
        self.assertIn(
            "QuirkStatus cubase9_state_blob_size_validation = QuirkStatus::Speculative;",
            new_src,
        )

    def test_empty_confirmed_is_noop(self) -> None:
        new_src, promoted = pqt.promote_meta(SAMPLE_HEADER, set())
        self.assertEqual(promoted, [])
        self.assertEqual(new_src, SAMPLE_HEADER)


class CliTests(unittest.TestCase):
    def _setup(self, tmp: pathlib.Path) -> pathlib.Path:
        header = _write(tmp, "host_quirks.hpp", SAMPLE_HEADER)
        return header

    def test_print_status_reports_aggregation(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            md = _write(
                tmp,
                "07-reaper-clap.md",
                textwrap.dedent(
                    """\
                    ## Result

                    | `reaper_process_while_bypassed` | R1 | Confirmed | x |
                    | `fl_studio_state_reader_skip`   | #14 | Refuted | not in scope |
                    """
                ),
            )
            buf = io.StringIO()
            sys.stdout, prev = buf, sys.stdout
            try:
                rc = pqt.main([str(md), "--print-status"])
            finally:
                sys.stdout = prev
            self.assertEqual(rc, 0)
            self.assertIn("reaper_process_while_bypassed", buf.getvalue())
            self.assertIn("Confirmed", buf.getvalue())
            self.assertIn("Refuted", buf.getvalue())

    def test_diff_output_contains_promotion(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            header = self._setup(tmp)
            md = _write(
                tmp,
                "06-reaper-vst3.md",
                textwrap.dedent(
                    """\
                    ## Result

                    | `reaper_process_while_bypassed` | R1 | Confirmed | x |
                    """
                ),
            )
            out_patch = tmp / "out.patch"
            rc = pqt.main([
                str(md),
                "--quirks-header", str(header),
                "--repo-root", str(tmp),
                "--output", str(out_patch),
            ])
            self.assertEqual(rc, 0)
            patch_body = out_patch.read_text(encoding="utf-8")
            self.assertIn(
                "-    QuirkStatus reaper_process_while_bypassed = QuirkStatus::Speculative;",
                patch_body,
            )
            self.assertIn(
                "+    QuirkStatus reaper_process_while_bypassed = QuirkStatus::Validated;",
                patch_body,
            )

    def test_in_place_rewrites_header(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            header = self._setup(tmp)
            md = _write(
                tmp,
                "11-fl-studio-vst3.md",
                textwrap.dedent(
                    """\
                    ## Result

                    | `fl_studio_state_reader_skip` | #14 | Confirmed | step 8 |
                    """
                ),
            )
            rc = pqt.main([
                str(md),
                "--quirks-header", str(header),
                "--in-place",
            ])
            self.assertEqual(rc, 0)
            after = header.read_text(encoding="utf-8")
            self.assertIn(
                "QuirkStatus fl_studio_state_reader_skip = QuirkStatus::Validated;",
                after,
            )
            # Untouched Speculative entries remain.
            self.assertIn(
                "QuirkStatus reaper_process_while_bypassed = QuirkStatus::Speculative;",
                after,
            )

    def test_no_inputs_errors_out(self) -> None:
        with self.assertRaises(SystemExit) as ctx:
            pqt.main([])
        self.assertNotEqual(ctx.exception.code, 0)


if __name__ == "__main__":
    unittest.main()
