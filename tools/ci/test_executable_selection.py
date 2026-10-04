#!/usr/bin/env python3
"""Tests for tools/ci/executable_selection.py: the would-skip set, the seeded
sample, the tests and build targets left to run, and the canonical bytes."""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import executable_selection as es  # noqa: E402


def entry(key: str | None, base: str | None = None, reason: str | None = None, regs=(), spawns=(),
          kind: str = "executable") -> dict:
    return {"kind": kind, "head_key": key, "base_key": key if base is None else base,
            "always_run": reason, "registrations": list(regs), "spawns": list(spawns)}


def fixture(count: int = 0) -> tuple[dict, dict, dict]:
    executables = {
        "test/same": entry("k1", regs=["same-a", "same-b"]),
        "test/changed": entry("k2", base="k0", regs=["changed"], spawns=["test/helper"]),
        "test/env": entry("k3", reason="environment", regs=["env"]),
        "test/unkeyed": entry(None, reason="unrecorded", regs=["unkeyed"]),
        "test/helper": entry("k4", regs=[]),
        "test/plug.so": entry("k5", kind="module"),
    }
    for i in range(count):
        executables[f"test/many{i}"] = entry(f"m{i}", regs=[f"many{i}"])
    manifest = {"schema": es.MANIFEST_SCHEMA, "executables": executables}
    codemodel = {"targets": {a.rsplit("/", 1)[1]: {"artifacts": [f"<build>/{a}"]} for a in executables}}
    names = [r for e in executables.values() for r in e["registrations"]] + ["script-test"]
    return manifest, codemodel, {"tests": [{"name": n} for n in names]}


class SelectionTest(unittest.TestCase):
    def test_only_equal_keys_without_a_reason_would_skip(self) -> None:
        manifest, _, _ = fixture()
        # A module never skips on its own, a null key never matches a null key,
        # and helper's key is equal but changed (which runs) spawns it.
        self.assertEqual(es.would_skip(manifest), ["test/same"])

    def test_a_tool_spawned_by_a_running_executable_never_skips_transitively(self) -> None:
        manifest, _, _ = fixture()
        executables = manifest["executables"]
        executables["test/helper"]["spawns"] = ["test/deep"]
        executables["test/deep"] = entry("k6")
        executables["test/same"]["spawns"] = ["test/quiet"]
        executables["test/quiet"] = entry("k7")
        # deep is reached only through helper, which is pulled in by changed;
        # quiet is spawned only by same, which itself skips, so it may skip too.
        self.assertEqual(es.would_skip(manifest), ["test/quiet", "test/same"])

    def test_the_skipped_executables_tests_leave_the_selection(self) -> None:
        manifest, codemodel, ctest = fixture(40)
        selection = es.select(manifest, codemodel, ctest, "seed", 1)
        self.assertEqual(len(selection["sampled_executables"]), 1)
        sampled = set(selection["sampled_executables"])
        skipped = {r for a in set(selection["would_skip"]) - sampled
                   for r in manifest["executables"][a]["registrations"]}
        self.assertEqual(len(skipped), 41)  # same's two, and 39 of the 40 others
        self.assertEqual(set(selection["tests"]), {t["name"] for t in ctest["tests"]} - skipped)
        for name in ("changed", "env", "script-test", "unkeyed"):
            self.assertIn(name, selection["tests"])
        self.assertTrue({"changed", "env", "helper", "unkeyed"} <= set(selection["build_targets"]))

    def test_the_sample_is_seeded_sized_and_reruns_its_tests(self) -> None:
        manifest, codemodel, ctest = fixture(40)
        one = es.select(manifest, codemodel, ctest, "seed-a", 5)
        again = es.select(manifest, codemodel, ctest, "seed-a", 5)
        other = es.select(manifest, codemodel, ctest, "seed-b", 5)
        self.assertEqual(len(one["would_skip"]), 41)
        self.assertEqual(len(one["sampled_executables"]), 3)  # ceil(41 * 5 / 100)
        # The draw is the smallest sha256(seed NUL name), as Shipyard documents it.
        expected = sorted(one["would_skip"],
                          key=lambda a: hashlib.sha256(f"seed-a\0{a}".encode()).hexdigest())[:3]
        self.assertEqual(one["sampled_executables"], sorted(expected))
        self.assertEqual(one, again)
        self.assertNotEqual(one["sampled_executables"], other["sampled_executables"])
        for artifact in one["sampled_executables"]:
            for name in manifest["executables"][artifact]["registrations"]:
                self.assertIn(name, one["tests"])
        self.assertEqual(es.select(manifest, codemodel, ctest, "s", 100)["sampled_executables"],
                         one["would_skip"])
        self.assertEqual(es.sample(["x"], "s", 1), ["x"])  # at least one whenever any would skip

    def test_bad_inputs_refuse(self) -> None:
        manifest, codemodel, ctest = fixture()
        for bad in (0, 101, 5.0, True):
            with self.subTest(percent=bad), self.assertRaisesRegex(es.SelectionError, "integer"):
                es.select(manifest, codemodel, ctest, "s", bad)
        with self.assertRaisesRegex(es.SelectionError, "seed"):
            es.select(manifest, codemodel, ctest, "", 5)
        with self.assertRaisesRegex(es.SelectionError, "schema"):
            es.select({**manifest, "schema": "other"}, codemodel, ctest, "s", 5)
        del codemodel["targets"]["helper"]
        with self.assertRaisesRegex(es.SelectionError, "test/helper"):
            es.select(manifest, codemodel, ctest, "s", 5)

    def test_the_cli_writes_canonical_bytes_without_site_paths(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            paths = []
            for name, doc in zip(("m", "c", "j"), fixture()):
                (root / name).write_text(json.dumps(doc, indent=2), encoding="utf-8")
                paths.append(str(root / name))
            out = root / "selection.json"
            argv = [sys.executable, "-I", str(HERE / "executable_selection.py"), "--manifest", paths[0],
                    "--head-codemodel", paths[1], "--ctest-json", paths[2], "--seed", "s", "--percent", "50",
                    "--out", str(out)]
            self.assertEqual(subprocess.run(argv, capture_output=True).returncode, 0)
            expected = es.canonical(es.select(*fixture(), "s", 50))
            self.assertEqual(out.read_bytes(), expected)
            self.assertNotIn(b" ", out.read_bytes().replace(b"script-test", b""))
            argv[argv.index("50")] = "0"
            self.assertEqual(subprocess.run(argv, capture_output=True).returncode, 1)


if __name__ == "__main__":
    unittest.main()
