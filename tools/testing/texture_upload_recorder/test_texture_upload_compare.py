#!/usr/bin/env python3
"""Tests for texture_upload_compare.py."""

from __future__ import annotations

import contextlib
import io
import json
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import texture_upload_compare as tuc  # noqa: E402


def record(width: int, height: int, sha: str, fmt: str = "RGBA8Unorm") -> dict:
    return {"width": width, "height": height, "depth": 1, "pixel_format": fmt, "sha256": sha}


class CompareTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def run_dir(self, name: str, records: list[dict]) -> pathlib.Path:
        path = self.root / name
        path.mkdir()
        with (path / "manifest.jsonl").open("w", encoding="utf-8") as handle:
            for r in records:
                handle.write(json.dumps(r) + "\n")
        return path

    def main(self, *args: str) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            code = tuc.main(list(args))
        return code, out.getvalue()

    def test_same_uploads_in_another_order_match(self) -> None:
        a = self.run_dir("a", [record(4, 3, "aa"), record(5, 2, "bb", "BGRA8Unorm"), record(3, 2, "cc")])
        b = self.run_dir("b", [record(3, 2, "cc"), record(4, 3, "aa"), record(5, 2, "bb", "BGRA8Unorm")])
        code, text = self.main(str(a), str(b))
        self.assertEqual(code, 0)
        self.assertIn("matched: 3", text)
        self.assertIn("IDENTICAL", text)

    def test_one_changed_texture_is_named_on_both_sides(self) -> None:
        a = self.run_dir("a", [record(4, 3, "aa"), record(5, 2, "bb"), record(3, 2, "cc")])
        b = self.run_dir("b", [record(4, 3, "aa"), record(5, 2, "bx"), record(3, 2, "cc")])
        code, out = self.main(str(a), str(b), "--json")
        self.assertEqual(code, 1)
        result = json.loads(out)
        self.assertEqual(result["matched"], 2)
        self.assertEqual([i["sha256"] for i in result["only_in_a"]], ["bb"])
        self.assertEqual([i["sha256"] for i in result["only_in_b"]], ["bx"])

    def test_a_repeated_upload_counts_as_many_times_as_it_happens(self) -> None:
        a = self.run_dir("a", [record(4, 3, "aa"), record(4, 3, "aa")])
        b = self.run_dir("b", [record(4, 3, "aa")])
        code, out = self.main(str(a), str(b), "--json")
        self.assertEqual(code, 1)
        self.assertEqual(json.loads(out)["only_in_a"][0]["count"], 1)

    def test_same_hash_with_another_format_or_size_differs(self) -> None:
        a = self.run_dir("a", [record(4, 3, "aa", "RGBA8Unorm")])
        b = self.run_dir("b", [record(4, 3, "aa", "BGRA8Unorm")])
        self.assertEqual(self.main(str(a), str(b))[0], 1)
        c = self.run_dir("c", [record(6, 2, "aa")])
        self.assertEqual(self.main(str(a), str(c))[0], 1)

    def test_an_unreadable_upload_never_passes(self) -> None:
        a = self.run_dir("a", [record(4, 3, "")])
        b = self.run_dir("b", [record(4, 3, "")])
        code, text = self.main(str(a), str(b))
        self.assertEqual(code, 1)
        self.assertIn("not comparable", text)

    def test_the_report_holds_no_pixels(self) -> None:
        a = self.run_dir("a", [dict(record(4, 3, "aa"), raw_file="000001-aa.bin")])
        b = self.run_dir("b", [record(4, 3, "bb")])
        _, out = self.main(str(a), str(b), "--json")
        self.assertEqual(
            set(json.loads(out)["only_in_a"][0]), {"width", "height", "depth", "pixel_format", "sha256", "count"}
        )

    def test_a_missing_or_broken_manifest_is_an_error(self) -> None:
        a = self.run_dir("a", [record(4, 3, "aa")])
        self.assertEqual(self.main(str(a), str(self.root / "absent"))[0], 2)
        broken = self.root / "broken"
        broken.mkdir()
        (broken / "manifest.jsonl").write_text("{not json}\n", encoding="utf-8")
        self.assertEqual(self.main(str(a), str(broken))[0], 2)


if __name__ == "__main__":
    unittest.main()
