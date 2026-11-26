"""Yoga layout fixtures on Vellum's layout: mapping, comparison and the runner mode.

The pure cases need no host. The live cases run vellum-app-host when
VELLUM_APP_HOST names one and are skipped (with that reason) otherwise.
"""

from __future__ import annotations

import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))

from tools.harness.visual import runner, vellum_layout  # noqa: E402

VISUAL = ROOT / "tools" / "harness" / "visual"
HOST = os.environ.get(vellum_layout.HOST_ENV, "")
HAS_HOST = bool(HOST) and Path(HOST).is_file()


def fixture(name: str) -> dict:
    return json.loads((VISUAL / "fixtures" / "yoga" / f"{name}.json").read_text(encoding="utf-8"))


class MappingTests(unittest.TestCase):
    def test_percent_sizes_resolve_against_the_viewport(self) -> None:
        tree = vellum_layout.map_fixture(fixture("percentage-sizing"))
        child = next(c for c in tree["children"] if c["id"] == "percent-child")
        self.assertEqual((child["style"]["width"], child["style"]["height"]), (150.0, 50.0))
        self.assertEqual(tree["style"]["direction"], "horizontal")

    def test_absolute_offsets_become_x_and_y(self) -> None:
        tree = vellum_layout.map_fixture(fixture("position-absolute"))
        child = next(c for c in tree["children"] if c["id"] == "absolute-child")
        self.assertEqual((child["style"]["x"], child["style"]["y"]), (30.0, 20.0))

    def test_properties_without_an_equivalent_are_refused_by_name(self) -> None:
        for name, needle in (("aspect-ratio", "aspect_ratio"), ("box-sizing", "border_width"),
                             ("flex-grow-shrink", "align_items")):
            with self.assertRaises(vellum_layout.Unsupported) as caught:
                vellum_layout.map_fixture(fixture(name))
            self.assertIn(needle, str(caught.exception))

    def test_decoration_is_not_part_of_the_snapshot(self) -> None:
        raw = fixture("percentage-sizing")
        tree = vellum_layout.map_fixture(raw)
        self.assertEqual(tree["children"][-1]["id"], vellum_layout.DECOR_ID)
        dump = {"nodes": [
            {"id": n["id"], "box": {"x": 0, "y": 0, "width": 1, "height": 1}}
            for n in [tree, *tree["children"]]]}
        ids = [n["id"] for n in vellum_layout.snapshot_from_dump(raw, dump)["nodes"]]
        self.assertNotIn(vellum_layout.DECOR_ID, ids)

    def test_compare_rects_reports_a_moved_box_and_a_missing_node(self) -> None:
        golden = {"nodes": [{"id": "a", "rect": {"x": 0, "y": 0, "w": 10, "h": 10}},
                            {"id": "b", "rect": {"x": 0, "y": 0, "w": 5, "h": 5}}]}
        same = json.loads(json.dumps(golden))
        self.assertEqual(vellum_layout.compare_rects(golden, same, 0.001), [])
        moved = json.loads(json.dumps(golden))
        moved["nodes"][0]["rect"]["w"] = 11
        del moved["nodes"][1]
        problems = vellum_layout.compare_rects(golden, moved, 0.001)
        self.assertTrue(any(p.startswith("a.w") for p in problems), problems)
        self.assertTrue(any(p.startswith("b:") for p in problems), problems)


def run_runner(*arguments: str) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        code = runner.main(["--verify", "--engine", "vellum", "--repo-root", str(ROOT), *arguments])
    return code, out.getvalue(), err.getvalue()


class RunnerModeTests(unittest.TestCase):
    def test_a_run_that_measures_nothing_is_not_a_pass(self) -> None:
        # The host is never invoked: every selected fixture is unsupported.
        code, out, _ = run_runner("--vellum-host", sys.executable, "--entry", "aspect-ratio",
                                  "--entry", "box-sizing", "--entry", "flex-grow-shrink")
        self.assertEqual(code, 3)
        self.assertIn("measured=0 skipped=3", out)

    def test_the_mode_refuses_to_generate_and_needs_a_host(self) -> None:
        err = io.StringIO()
        with redirect_stderr(err):
            self.assertEqual(runner.main(["--generate", "--all", "--engine", "vellum",
                                          "--repo-root", str(ROOT)]), 2)
            os.environ.pop(vellum_layout.HOST_ENV, None)
            self.assertEqual(runner.main(["--verify", "--engine", "vellum",
                                          "--repo-root", str(ROOT)]), 2)
        self.assertIn("needs --vellum-host", err.getvalue())

    @unittest.skipUnless(HAS_HOST, "VELLUM_APP_HOST does not name a vellum-app-host binary")
    def test_supported_fixtures_match_the_yoga_goldens(self) -> None:
        code, out, err = run_runner("--vellum-host", HOST)
        self.assertEqual(code, 0, err)
        self.assertIn("ok yoga/percentage-sizing (vellum)", out)
        self.assertIn("ok yoga/position-absolute (vellum)", out)
        self.assertIn("measured=2 skipped=3 failed=0", out)

    @unittest.skipUnless(HAS_HOST, "VELLUM_APP_HOST does not name a vellum-app-host binary")
    def test_a_changed_golden_rect_fails(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            repo = Path(scratch)
            shutil.copytree(VISUAL / "fixtures", repo / "tools/harness/visual/fixtures")
            shutil.copytree(VISUAL / "goldens", repo / "tools/harness/visual/goldens")
            golden = repo / "tools/harness/visual/goldens/yoga/percentage-sizing.json"
            data = json.loads(golden.read_text(encoding="utf-8"))
            for node in data["nodes"]:
                if node["id"] == "percent-child":
                    node["rect"]["w"] = 149
            golden.write_text(json.dumps(data), encoding="utf-8")
            err = io.StringIO()
            with redirect_stdout(io.StringIO()), redirect_stderr(err):
                code = runner.main(["--verify", "--engine", "vellum", "--vellum-host", HOST,
                                    "--repo-root", str(repo), "--entry", "percentage-sizing"])
            self.assertEqual(code, 1)
            self.assertIn("percent-child.w: golden 149 vs vellum 150", err.getvalue())


if __name__ == "__main__":
    unittest.main()
