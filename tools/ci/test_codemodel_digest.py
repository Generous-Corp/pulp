#!/usr/bin/env python3
"""Tests for tools/ci/codemodel_digest.py (per-target CMake codemodel digests).

What must hold, against a real file-API reply (testdata/codemodel_digest,
written by CMake for a two-archive project with an executable `t` that
force-loads one archive and runs as ctest `t-runs`):
- the same configuration under two different roots digests identically;
- each part moves only with what it covers: a define moves `compile`, a link
  fragment moves `link`, an added source moves `sources`, a test property
  moves `tests`, and every one of them moves the target's `digest`;
- another target's digests do not move, and backtrace indices are ignored;
- a target's tests are the ctest registrations that run its artifact;
- a build without a reply is refused, never digested as empty.

Run:
    python3 tools/ci/test_codemodel_digest.py
"""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
import codemodel_digest as cd  # noqa: E402

FIXTURE = ROOT / "tools/ci/testdata/codemodel_digest"


def materialise(tmp: str) -> tuple[Path, Path, list[dict]]:
    """The fixture reply placed under `tmp`, its roots rewritten to it."""
    src, build = Path(tmp) / "src", Path(tmp) / "build"
    reply = build / cd.REPLY
    reply.mkdir(parents=True)
    src.mkdir()
    for f in (FIXTURE / "reply").iterdir():
        text = f.read_text().replace("@BUILD@", str(build)).replace("@SRC@", str(src))
        (reply / f.name).write_text(text)
    tests = json.loads((FIXTURE / "ctest.json").read_text().replace("@BUILD@", str(build)))["tests"]
    (build / "sub").mkdir()
    (build / "sub" / "t").write_text("")  # the artifact a test command resolves to
    return build, src, tests


def edit_target(build: Path, name: str, change) -> None:
    reply = build / cd.REPLY
    path = next(p for p in reply.iterdir() if p.name.startswith(f"target-{name}-"))
    doc = json.loads(path.read_text())
    change(doc)
    path.write_text(json.dumps(doc))


class DigestTests(unittest.TestCase):
    def digest(self, change=None, test_change=None) -> dict:
        with tempfile.TemporaryDirectory() as tmp:
            build, src, tests = materialise(tmp)
            if change:
                edit_target(build, "t", change)
            if test_change:
                test_change(tests)
            return cd.digest_targets(build, src, tests)

    def test_the_same_configuration_elsewhere_digests_identically(self) -> None:
        self.assertEqual(self.digest(), self.digest())
        doc = self.digest()
        self.assertEqual(doc["targets"]["t"]["artifacts"], ["<build>/sub/t"])
        self.assertEqual(sorted(doc["targets"]), ["a", "app", "d", "f", "t"])

    def test_tests_and_dependencies_belong_to_their_target(self) -> None:
        doc = self.digest()
        self.assertIsNotNone(doc["targets"]["t"]["tests"])
        self.assertIsNone(doc["targets"]["a"]["tests"])
        self.assertEqual(doc["tests_unmatched"], 0)
        self.assertEqual(doc["targets"]["t"]["dependencies"], ["a", "f"])

    def moved(self, after: dict) -> set[str]:
        before = self.digest()["targets"]
        return {f"{name}.{part}" for name in before for part in ("sources", "compile", "link", "tests", "digest")
                if before[name][part] != after["targets"][name][part]}

    def test_a_define_moves_only_compile(self) -> None:
        after = self.digest(lambda d: d["compileGroups"][0]["defines"].append({"define": "EXTRA=1"}))
        self.assertEqual(self.moved(after), {"t.compile", "t.digest"})

    def test_a_link_fragment_moves_only_link(self) -> None:
        after = self.digest(lambda d: d["link"]["commandFragments"].append({"fragment": "-lz", "role": "libraries"}))
        self.assertEqual(self.moved(after), {"t.link", "t.digest"})

    def test_an_added_source_moves_sources(self) -> None:
        after = self.digest(lambda d: d["sources"].append({"path": "sub/extra.c", "compileGroupIndex": 0}))
        self.assertEqual(self.moved(after), {"t.sources", "t.digest"})

    def test_a_test_property_moves_only_tests(self) -> None:
        def longer(tests):
            for p in tests[0]["properties"]:
                if p["name"] == "TIMEOUT":
                    p["value"] = 99
        self.assertEqual(self.moved(self.digest(test_change=longer)), {"t.tests", "t.digest"})

    def test_backtraces_are_ignored(self) -> None:
        def shift(d):
            for s in d["sources"]:
                s["backtrace"] = 999
            for f in d["link"]["commandFragments"]:
                f["backtrace"] = 999
        self.assertEqual(self.moved(self.digest(shift)), set())

    def test_a_build_without_a_reply_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(cd.CodemodelError):
                cd.digest_targets(Path(tmp), Path(tmp), [])


if __name__ == "__main__":
    unittest.main()
