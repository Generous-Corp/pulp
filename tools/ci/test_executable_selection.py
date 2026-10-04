#!/usr/bin/env python3
"""Tests for tools/ci/executable_selection.py: the would-skip set, the seeded
sample, the tests and build targets left to run, and the canonical bytes."""
from __future__ import annotations

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
        # A module never skips on its own, and a null key never matches a null key.
        self.assertEqual(es.would_skip(manifest), ["test/helper", "test/same"])

    def test_the_skipped_executables_tests_leave_the_selection(self) -> None:
        selection = es.select(*fixture(), "seed", 0.0)
        self.assertEqual(selection["sampled_executables"], [])
        self.assertEqual(selection["tests"], ["changed", "env", "script-test", "unkeyed"])
        # helper would skip on its own key but is built because changed spawns it.
        self.assertEqual(selection["build_targets"], ["changed", "env", "helper", "unkeyed"])

    def test_the_sample_is_seeded_sized_and_reruns_its_tests(self) -> None:
        manifest, codemodel, ctest = fixture(40)
        one = es.select(manifest, codemodel, ctest, "seed-a", 0.05)
        again = es.select(manifest, codemodel, ctest, "seed-a", 0.05)
        other = es.select(manifest, codemodel, ctest, "seed-b", 0.05)
        self.assertEqual(len(one["would_skip"]), 42)
        self.assertEqual(len(one["sampled_executables"]), 3)  # ceil(0.05 * 42)
        self.assertEqual(one, again)
        self.assertNotEqual(one["sampled_executables"], other["sampled_executables"])
        for artifact in one["sampled_executables"]:
            for name in manifest["executables"][artifact]["registrations"]:
                self.assertIn(name, one["tests"])
        self.assertEqual(es.select(manifest, codemodel, ctest, "s", 1.0)["sampled_executables"],
                         one["would_skip"])

    def test_bad_inputs_refuse(self) -> None:
        manifest, codemodel, ctest = fixture()
        with self.assertRaisesRegex(es.SelectionError, "outside"):
            es.select(manifest, codemodel, ctest, "s", 1.5)
        with self.assertRaisesRegex(es.SelectionError, "seed"):
            es.select(manifest, codemodel, ctest, "", 0.1)
        with self.assertRaisesRegex(es.SelectionError, "schema"):
            es.select({**manifest, "schema": "other"}, codemodel, ctest, "s", 0.1)
        del codemodel["targets"]["helper"]
        with self.assertRaisesRegex(es.SelectionError, "test/helper"):
            es.select(manifest, codemodel, ctest, "s", 0.0)

    def test_the_cli_writes_canonical_bytes_without_site_paths(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            paths = []
            for name, doc in zip(("m", "c", "j"), fixture()):
                (root / name).write_text(json.dumps(doc, indent=2), encoding="utf-8")
                paths.append(str(root / name))
            out = root / "selection.json"
            argv = [sys.executable, "-I", str(HERE / "executable_selection.py"), "--manifest", paths[0],
                    "--head-codemodel", paths[1], "--ctest-json", paths[2], "--seed", "s", "--rate", "0.5",
                    "--out", str(out)]
            self.assertEqual(subprocess.run(argv, capture_output=True).returncode, 0)
            expected = es.canonical(es.select(*fixture(), "s", 0.5))
            self.assertEqual(out.read_bytes(), expected)
            self.assertNotIn(b" ", out.read_bytes().replace(b"script-test", b""))
            argv[argv.index("0.5")] = "2"
            self.assertEqual(subprocess.run(argv, capture_output=True).returncode, 1)


if __name__ == "__main__":
    unittest.main()
