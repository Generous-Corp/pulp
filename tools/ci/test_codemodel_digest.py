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
- a build-tree file the target compiles or included (Ninja's dependency log)
  is keyed by CONTENT: a changed generated source or header moves only its
  own target's `generated` part, while pinned `_deps` headers, checkout
  headers and object files listed as sources do not count; without a Ninja
  log the document says `generated_headers: unavailable`;
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

    def generated(self, gen_text: str = "v1", header_text: str = "h1", deps: str | None = "default",
                  dep_header: str = "gen/version.h", obj_text: str = "o1") -> dict:
        """t compiles a generated source and includes a generated header."""
        with tempfile.TemporaryDirectory() as tmp:
            build, src, tests = materialise(tmp)
            (build / "sub" / "gen.c").write_text(gen_text)
            (build / "sub" / "pre.o").write_text(obj_text)
            (build / "gen").mkdir()
            (build / "gen" / "version.h").write_text(header_text)
            (build / "_deps" / "x").mkdir(parents=True)
            (build / "_deps" / "x" / "cfg.h").write_text(header_text)
            (src / "plain.h").write_text(header_text)
            edit_target(build, "t", lambda d: d["sources"].extend(
                [{"path": str(build / "sub" / "gen.c"), "compileGroupIndex": 0},
                 {"path": str(build / "sub" / "pre.o")}]))
            if deps == "default":
                deps = (f"sub/CMakeFiles/t.dir/main.c.o: #deps 4, deps mtime 1 (VALID)\n"
                        f"    {src}/sub/main.c\n    {build}/{dep_header}\n"
                        f"    {build}/_deps/x/cfg.h\n    {src}/plain.h\n\n"
                        f"CMakeFiles/a.dir/a1.c.o: #deps 1, deps mtime 1 (VALID)\n    {src}/a1.c\n")
            return cd.digest_targets(build, src, tests, deps_text=deps)

    def test_generated_source_content_moves_only_that_targets_generated_part(self) -> None:
        before, after = self.generated(gen_text="v1"), self.generated(gen_text="v2")
        moved = {k for k in before["targets"] if before["targets"][k]["digest"] != after["targets"][k]["digest"]}
        self.assertEqual(moved, {"t"})
        self.assertNotEqual(before["targets"]["t"]["generated"], after["targets"]["t"]["generated"])
        self.assertEqual(before["targets"]["t"]["sources"], after["targets"]["t"]["sources"])

    def test_an_included_generated_header_moves_only_its_includer(self) -> None:
        before, after = self.generated(header_text="VERSION 1"), self.generated(header_text="VERSION 2")
        moved = {k for k in before["targets"] if before["targets"][k]["digest"] != after["targets"][k]["digest"]}
        self.assertEqual(moved, {"t"})
        self.assertEqual(before["generated_headers"], "ninja-deps")
        self.assertIsNone(before["targets"]["a"]["generated"])

    def test_pinned_dependency_and_source_tree_headers_are_not_generated_inputs(self) -> None:
        # Only the _deps and checkout copies differ: the header list excludes them.
        base = self.generated(header_text="h1", dep_header="sub/unused.h")
        other = self.generated(header_text="h2", dep_header="sub/unused.h")
        self.assertEqual(base["targets"]["t"]["generated"], other["targets"]["t"]["generated"])

    def test_an_object_file_listed_as_a_source_is_not_a_generated_input(self) -> None:
        self.assertEqual(self.generated(obj_text="o1")["targets"]["t"]["digest"],
                         self.generated(obj_text="o2")["targets"]["t"]["digest"])

    def test_without_a_ninja_log_only_generated_sources_count_and_it_says_so(self) -> None:
        doc = self.generated(deps=None)
        self.assertEqual(doc["generated_headers"], "unavailable")
        self.assertIsNotNone(doc["targets"]["t"]["generated"])

    def test_a_build_without_a_reply_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(cd.CodemodelError):
                cd.digest_targets(Path(tmp), Path(tmp), [])


if __name__ == "__main__":
    unittest.main()
