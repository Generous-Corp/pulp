#!/usr/bin/env python3
"""Tests for link_closure_check.py.

Each case builds a small link graph and asserts what the walk reports. The
negative controls matter most: a walk that only read direct edges, or ignored
$<LINK_ONLY:> entries, would pass every real graph and prove nothing, so the
cases below put the forbidden target behind an intermediate archive and behind a
PRIVATE static link and require it to be found.
"""

from __future__ import annotations

import io
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import link_closure_check as lcc  # noqa: E402


def graph_text(records: dict[str, tuple[str, ...]], padding: int = 25) -> str:
    """records: name -> (LINK_LIBRARIES, INTERFACE_LINK_LIBRARIES[, SOURCES])."""
    lines = [f"{name}\tSTATIC_LIBRARY\t" + "\t".join(fields) for name, fields in records.items()]
    lines += [f"pulp-pad-{index}\tSTATIC_LIBRARY\t\t" for index in range(padding)]
    return "\n".join(lines) + "\n"


class ClosureWalk(unittest.TestCase):
    def run_check(self, records: dict[str, tuple[str, str]], roots: list[str],
                  forbid: list[str], allow: list[str] | None = None,
                  padding: int = 25) -> tuple[int, str, str]:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "graph.tsv"
            path.write_text(graph_text(records, padding), encoding="utf-8")
            argv = ["--graph", str(path)]
            for root in roots:
                argv += ["--root", root]
            for pattern in forbid:
                argv += ["--forbid", pattern]
            for edge in allow or []:
                argv += ["--allow", edge]
            out, err = io.StringIO(), io.StringIO()
            with redirect_stdout(out), redirect_stderr(err):
                code = lcc.main(argv)
            return code, out.getvalue(), err.getvalue()

    def test_clean_closure_passes(self) -> None:
        records = {"pulp-a": ("pulp::b", "pulp::b"), "pulp-b": ("pulp::runtime", ""),
                   "pulp-runtime": ("", "")}
        code, out, _ = self.run_check(records, ["pulp-a"], ["pulp-view*"])
        self.assertEqual(code, 0)
        self.assertIn("PASS pulp-a", out)

    def test_direct_forbidden_link_is_reported(self) -> None:
        records = {"pulp-a": ("pulp::view-core", "pulp::view-core"), "pulp-view-core": ("", "")}
        code, _, err = self.run_check(records, ["pulp-a"], ["pulp-view*"])
        self.assertEqual(code, 1)
        self.assertIn("FAIL pulp-a reaches pulp-view-core: pulp-a -> pulp-view-core", err)

    def test_forbidden_target_behind_an_intermediate_archive_is_reported(self) -> None:
        records = {"pulp-a": ("pulp::mid", "pulp::mid"), "pulp-mid": ("pulp::canvas", ""),
                   "pulp-canvas": ("", "")}
        code, _, err = self.run_check(records, ["pulp-a"], ["pulp-canvas"])
        self.assertEqual(code, 1)
        self.assertIn("pulp-a -> pulp-mid -> pulp-canvas", err)

    def test_private_static_link_only_edge_is_followed(self) -> None:
        # A PRIVATE link of a static library shows in LINK_LIBRARIES and as a
        # $<LINK_ONLY:...> entry in INTERFACE_LINK_LIBRARIES. Either alone must do.
        records = {"pulp-a": ("", "pulp::mid"), "pulp-mid": ("", "$<LINK_ONLY:pulp::canvas>"),
                   "pulp-canvas": ("", "")}
        code, _, err = self.run_check(records, ["pulp-a"], ["pulp-canvas"])
        self.assertEqual(code, 1)
        self.assertIn("pulp-mid -> pulp-canvas", err)

    def test_spellings_are_one_target(self) -> None:
        records = {"pulp-a": ("Pulp::view", ""), "pulp-view": ("", "")}
        code, _, err = self.run_check(records, ["Pulp::a"], ["pulp-view"])
        self.assertEqual(code, 1)
        self.assertIn("pulp-a reaches pulp-view", err)

    def test_generator_expression_wrappers_are_stripped(self) -> None:
        records = {"pulp-a": ("$<$<CONFIG:Debug>:pulp::view>;$<BUILD_INTERFACE:pulp::runtime>", ""),
                   "pulp-view": ("", ""), "pulp-runtime": ("", "")}
        code, _, err = self.run_check(records, ["pulp-a"], ["pulp-view"])
        self.assertEqual(code, 1)
        self.assertIn("pulp-a -> pulp-view", err)

    def test_a_longer_name_is_not_the_forbidden_one(self) -> None:
        records = {"pulp-a": ("pulp::renderer-docs", ""), "pulp-renderer-docs": ("", "")}
        code, _, _ = self.run_check(records, ["pulp-a"], ["pulp-render"])
        self.assertEqual(code, 0)

    def test_glob_patterns_match_families(self) -> None:
        records = {"pulp-a": ("pulp::view-audio", ""), "pulp-view-audio": ("", "")}
        self.assertEqual(self.run_check(records, ["pulp-a"], ["pulp-view*"])[0], 1)
        self.assertEqual(self.run_check(records, ["pulp-a"], ["pulp-canvas*"])[0], 0)

    def test_allowed_edge_is_exempt_and_named(self) -> None:
        records = {"pulp-a": ("pulp::format", ""), "pulp-format": ("pulp::view-core", ""),
                   "pulp-view-core": ("", "")}
        code, out, _ = self.run_check(records, ["pulp-a"], ["pulp-view*"],
                                      allow=["pulp-format>pulp-view-core"])
        self.assertEqual(code, 0)
        self.assertIn("allowed edge pulp-format -> pulp-view-core", out)

    def test_cycles_terminate(self) -> None:
        records = {"pulp-a": ("pulp::b", ""), "pulp-b": ("pulp::a", "")}
        self.assertEqual(self.run_check(records, ["pulp-a"], ["pulp-view*"])[0], 0)

    # The routes below each reached pulp-canvas past an earlier version of the
    # check and its graph writer.

    def test_target_not_named_pulp_is_walked(self) -> None:
        records = {"pulp-a": ("gpu_helper", ""), "gpu_helper": ("pulp::canvas", ""),
                   "pulp-canvas": ("", "")}
        code, _, err = self.run_check(records, ["pulp-a"], ["pulp-canvas"])
        self.assertEqual(code, 1)
        self.assertIn("pulp-a -> gpu_helper -> pulp-canvas", err)

    def test_raw_archive_path_names_its_target(self) -> None:
        for path in ("/x/build/core/canvas/libpulp-canvas.a", "C:/b/pulp-canvas.lib",
                     "/x/libpulp-canvas.dylib", "/x/libpulp-canvas.so.1"):
            with self.subTest(path=path):
                records = {"pulp-a": (path, ""), "pulp-canvas": ("", "")}
                code, _, err = self.run_check(records, ["pulp-a"], ["pulp-canvas"])
                self.assertEqual(code, 1)
                self.assertIn("pulp-a -> pulp-canvas", err)

    def test_target_objects_source_is_an_edge(self) -> None:
        records = {"pulp-a": ("", "", "$<TARGET_OBJECTS:objs>;x.cpp"),
                   "objs": ("pulp::canvas", ""), "pulp-canvas": ("", "")}
        code, _, err = self.run_check(records, ["pulp-a"], ["pulp-canvas"])
        self.assertEqual(code, 1)
        self.assertIn("pulp-a -> objs -> pulp-canvas", err)

    def test_forbid_patterns_are_spelled_like_roots(self) -> None:
        records = {"pulp-a": ("pulp::canvas", ""), "pulp-canvas": ("", "")}
        for pattern in ("pulp::canvas", "Pulp::Canvas", "pulp::canv*"):
            with self.subTest(pattern=pattern):
                self.assertEqual(self.run_check(records, ["pulp-a"], [pattern])[0], 1)

    def test_imported_target_is_walked(self) -> None:
        records = {"pulp-a": ("", "VellumGpuRuntime::Device"),
                   "VellumGpuRuntime::Device": ("", "pulp::canvas;skia::skia"),
                   "pulp-canvas": ("", "")}
        code, _, err = self.run_check(records, ["pulp-a"], ["pulp-canvas"])
        self.assertEqual(code, 1)
        self.assertIn("pulp-a -> vellumgpuruntime::device -> pulp-canvas", err)

    def test_missing_root_is_inconclusive_not_a_pass(self) -> None:
        code, _, err = self.run_check({"pulp-a": ("", "")}, ["pulp-gone"], ["pulp-view*"])
        self.assertEqual(code, 2)
        self.assertIn("INCONCLUSIVE", err)

    def test_a_tiny_graph_is_inconclusive(self) -> None:
        code, _, err = self.run_check({"pulp-a": ("", "")}, ["pulp-a"], ["pulp-view*"], padding=0)
        self.assertEqual(code, 2)
        self.assertIn("not a real build graph", err)

    def run_raw(self, text: str, roots: list[str]) -> tuple[int, str]:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "graph.tsv"
            path.write_text(text, encoding="utf-8")
            argv = ["--graph", str(path), "--forbid", "pulp-view*"]
            for root in roots:
                argv += ["--root", root]
            err = io.StringIO()
            with redirect_stdout(io.StringIO()), redirect_stderr(err):
                code = lcc.main(argv)
            return code, err.getvalue()

    def test_missing_intermediate_record_is_inconclusive_not_a_pass(self) -> None:
        records = {"pulp-root": ("pulp::mid", "pulp::mid")}
        code, _, err = self.run_check(records, ["pulp-root"], ["pulp-view*"])
        self.assertEqual(code, 2)
        self.assertIn("pulp-mid", err)
        self.assertIn("no record", err)

    def test_missing_namespaced_target_record_is_inconclusive(self) -> None:
        records = {"pulp-root": ("vendor::lib", "")}
        code, _, _ = self.run_check(records, ["pulp-root"], ["pulp-view*"])
        self.assertEqual(code, 2)

    def test_missing_target_objects_record_is_inconclusive(self) -> None:
        for expr in ("$<TARGET_OBJECTS:canvas_objs>", "$<TARGET_FILE:canvas_objs>",
                     "$<TARGET_PROPERTY:canvas_objs,INTERFACE_SOURCES>"):
            code, _, err = self.run_check({"pulp-root": ("", "", expr)}, ["pulp-root"],
                                          ["pulp-view*"])
            self.assertEqual(code, 2, expr)
            self.assertIn("canvas_objs", err)

    def test_present_target_objects_record_is_walked(self) -> None:
        records = {"pulp-root": ("", "", "$<TARGET_OBJECTS:objs>"),
                   "objs": ("pulp::view-core", "")}
        code, _, err = self.run_check(records, ["pulp-root"], ["pulp-view*"])
        self.assertEqual(code, 1)
        self.assertIn("reaches pulp-view-core", err)

    def test_truncated_record_is_inconclusive_not_a_pass(self) -> None:
        text = graph_text({"pulp-root": ("pulp::mid", "")})
        text += "pulp-mid\tSTATIC_LIBRARY\n"
        code, err = self.run_raw(text, ["pulp-root"])
        self.assertEqual(code, 2)
        self.assertIn("malformed", err)

    def test_paths_flags_and_generator_tokens_are_not_unresolved_targets(self) -> None:
        records = {"pulp-root": ("-lm;/opt/x/libfoo.a;$<BUILD_INTERFACE:/src/include>;"
                                 "$<LINK_ONLY:pthread>;src/pulp-thing.cpp", "")}
        code, _, _ = self.run_check(records, ["pulp-root"], ["pulp-view*"])
        self.assertEqual(code, 0)

    def test_malformed_allow_is_inconclusive(self) -> None:
        code, _, _ = self.run_check({"pulp-a": ("", "")}, ["pulp-a"], ["pulp-view*"],
                                    allow=["nonsense"])
        self.assertEqual(code, 2)


if __name__ == "__main__":
    unittest.main()
