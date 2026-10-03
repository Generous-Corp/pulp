#!/usr/bin/env python3
"""Tests for tools/ci/executable_keys.py: per-executable source keys over a
base-recorded input set, and every always_run reason."""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import executable_keys as ek  # noqa: E402

V2 = "pulp-codemodel-digest/v2"
TOOLCHAIN = {"os": "Darwin", "arch": "arm64", "sdk_version": "26.4", "sdk_build": "25E236",
             "clang": "Apple clang version 21.0.0 (clang-2100.1.1.101)"}
EXE, OTHER, MOD = "test/pulp-test-a", "test/pulp-test-b", "test/plug.so"
# Enough declared readers that the data scan proves it saw something.
KNOWN_READERS = {f"pulp-test-k{i}": {"data": "declared", "inputs": [f"test/fixtures/k{i}"],
                                     "detected_sources": ["test/k.cpp"]}
                 for i in range(ek.DATA_SCAN_MIN_DETECTED_READERS)}


def obj(target: str, source: str) -> str:
    d, f = os.path.split(source)
    return f"<build>/{d}/CMakeFiles/{target}.dir/{f}.o"


class Fixture:
    """A git repo with a base and a head commit, the base's reuse record and
    the head's codemodel and ctest inventory."""

    def __init__(self, tmp: Path) -> None:
        self.root = tmp / "repo"
        self.record = tmp / "record"
        self.build = tmp / "build"
        self.build.mkdir(parents=True)
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        self.files = {
            "core/a.hpp": "a\n", "core/b.hpp": "b\n", "core/liba.cpp": "liba\n", "core/unpulled.cpp": "u\n",
            "test/test_a.cpp": "ta\n", "test/test_b.cpp": "tb\n", "test/fixtures/a/x.json": "{}\n",
            "docs/readme.md": "doc\n", "tools/deps/manifest.json": "{}\n", "plug/plug.cpp": "p\n",
        }
        self.scan = {"executables_scanned_for": ["data", "spawns"],
                     "executables_scanned": ["pulp-test-a", "pulp-test-b", "plug.so", *KNOWN_READERS],
                     "executables": dict(KNOWN_READERS)}
        self.base = self.commit()
        self.head_targets = self.targets()
        self.base_targets = self.targets()
        self.ctest = {"tests": [{"name": "a test", "command": [str(self.build / EXE)], "properties": []},
                                {"name": "b test", "command": [str(self.build / OTHER)], "properties": []}]}
        self.links = {
            "schema": "pulp-link-members/v3", "unreadable": 0,
            "members": {"<build>/core/liba.a": ["liba.cpp.o", "unpulled.cpp.o"]},
            "executables": {
                f"<build>/{EXE}": {"kind": "executable", "objects": [obj("pulp-test-a", "test/test_a.cpp")],
                                   "archives": {"<build>/core/liba.a": {"members": [0], "whole": False}}},
                f"<build>/{OTHER}": {"kind": "executable", "objects": [obj("pulp-test-b", "test/test_b.cpp")],
                                     "archives": {}},
                f"<build>/{MOD}": {"kind": "module", "objects": [obj("plug", "plug/plug.cpp")], "archives": {}}}}
        self.deps = {obj("pulp-test-a", "test/test_a.cpp"): ["<src>/core/a.hpp", "<build>/gen/version.h"],
                     obj("pulp-test-b", "test/test_b.cpp"): ["<src>/core/b.hpp"],
                     obj("liba", "core/liba.cpp"): ["<src>/core/b.hpp"],
                     obj("liba", "core/unpulled.cpp"): [],
                     obj("plug", "plug/plug.cpp"): []}
        self.stale: list[str] = []
        self.record_fields = {**TOOLCHAIN, "os_version": "26.4", "os_build": "25E246"}
        self.job_extra: dict = {}

    def targets(self) -> dict:
        def t(kind, art, deps=()):
            return {"type": kind, "digest": f"d-{art}", "artifacts": [f"<build>/{art}"], "dependencies": list(deps)}
        return {"pulp-test-a": t("EXECUTABLE", EXE, ["liba"]), "pulp-test-b": t("EXECUTABLE", OTHER, ["plug"]),
                "plug": t("MODULE_LIBRARY", MOD), "liba": t("STATIC_LIBRARY", "core/liba.a")}

    def commit(self) -> str:
        for path, body in self.files.items():
            p = self.root / path
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(body)
        (self.root / ek.SCRIPT_INPUTS_PATH).parent.mkdir(parents=True, exist_ok=True)
        (self.root / ek.SCRIPT_INPUTS_PATH).write_text(json.dumps(self.scan))
        git = ["git", "-C", str(self.root), "-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false"]
        subprocess.run(git + ["add", "-A"], check=True)
        subprocess.run(git + ["commit", "-q", "--allow-empty", "-m", "c"], check=True)
        return subprocess.run(git + ["rev-parse", "HEAD"], check=True, capture_output=True, text=True).stdout.strip()

    def write_record(self) -> None:
        self.record.mkdir(exist_ok=True)
        headers = sorted({h for hs in self.deps.values() for h in hs})
        deps_doc = {"schema": "pulp-object-deps/v1", "headers": headers, "stale": self.stale,
                    # A stale object is distrusted even if a writer also lists its deps.
                    "objects": {o: [headers.index(h) for h in hs] for o, hs in self.deps.items()},
                    "members": {"<build>/core/liba.a": {"liba.cpp.o": [obj("liba", "core/liba.cpp")],
                                                        "unpulled.cpp.o": [obj("liba", "core/unpulled.cpp")]}}}
        (self.record / "codemodel-x.json").write_text(json.dumps(
            {"schema": V2, "generated_headers": "ninja-deps", "targets": self.base_targets}))
        (self.record / "link-members-x.json").write_text(json.dumps(self.links))
        (self.record / "object-deps-x.json").write_text(json.dumps(deps_doc))
        (self.record / "job.json").write_text(json.dumps({"runner_image": {"digest": "img", "fields": self.record_fields},
                                                          **self.job_extra}))

    def keys(self, head: str, record: bool = True, toolchain: dict | None = TOOLCHAIN) -> dict:
        self.write_record()
        rec = ek.load_record(self.record)[0] if record else None
        cm = {"schema": V2, "generated_headers": "ninja-deps", "targets": self.head_targets}
        return ek.compute(self.root, self.base, head, rec, cm, self.ctest, self.build, toolchain)["executables"]


class KeyTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.fx = Fixture(Path(self.tmp.name))

    def tearDown(self):
        self.tmp.cleanup()

    def head(self, **files):
        self.fx.files.update(files)
        return self.fx.commit()

    def equal(self, keys, artifact):
        e = keys[artifact]
        return e["base_key"] is not None and e["base_key"] == e["head_key"]

    def test_an_unrelated_change_keeps_every_key(self):
        keys = self.fx.keys(self.head(**{"docs/readme.md": "new\n"}))
        self.assertTrue(self.equal(keys, EXE) and self.equal(keys, OTHER) and self.equal(keys, MOD))
        self.assertEqual({e["always_run"] for e in keys.values()}, {None})
        self.assertEqual(keys[EXE]["registrations"], ["a test"])
        self.assertEqual(keys[OTHER]["spawns"], [MOD])

    def test_a_header_an_object_included_changes_only_that_key(self):
        keys = self.fx.keys(self.head(**{"core/a.hpp": "a2\n"}))
        self.assertFalse(self.equal(keys, EXE))
        self.assertTrue(self.equal(keys, OTHER))

    def test_a_pulled_member_changes_the_key_and_an_unpulled_one_does_not(self):
        self.assertFalse(self.equal(self.fx.keys(self.head(**{"core/liba.cpp": "liba2\n"})), EXE))
        self.assertTrue(self.equal(self.fx.keys(self.head(**{"core/liba.cpp": "liba\n", "core/unpulled.cpp": "u2\n"})),
                                   EXE))

    def test_a_header_reached_through_a_member_changes_the_key(self):
        keys = self.fx.keys(self.head(**{"core/b.hpp": "b2\n"}))
        self.assertFalse(self.equal(keys, EXE))   # liba.cpp.o includes it
        self.assertFalse(self.equal(keys, OTHER))

    def test_the_codemodel_digest_and_the_toolchain_are_part_of_the_key(self):
        head = self.head(**{"docs/readme.md": "new\n"})
        self.fx.head_targets["pulp-test-a"]["digest"] = "moved"
        keys = self.fx.keys(head)
        self.assertFalse(self.equal(keys, EXE))
        self.assertTrue(self.equal(keys, OTHER))
        self.fx.head_targets["pulp-test-a"]["digest"] = self.fx.base_targets["pulp-test-a"]["digest"]
        other = {**TOOLCHAIN, "clang": "Apple clang version 21.0.0 (clang-2100.3.34.2)"}
        self.assertNotEqual(ek.key_of("d", TOOLCHAIN, ["p"], {"p": "1"}), ek.key_of("d", other, ["p"], {"p": "1"}))

    def test_a_linked_library_whose_flags_moved_changes_the_key(self):
        # Flags, definitions and generated inputs of a library's objects are
        # in the library target's digest, not the executable's.
        head = self.head(**{"docs/readme.md": "new\n"})
        self.fx.head_targets["liba"]["digest"] = "flags-moved"
        keys = self.fx.keys(head)
        self.assertFalse(self.equal(keys, EXE))
        self.assertTrue(self.equal(keys, OTHER))   # links nothing from liba

    def test_an_archive_no_target_owns_is_unrecorded(self):
        head = self.head(**{"docs/readme.md": "new\n"})
        self.fx.base_targets["liba"]["artifacts"] = ["<build>/core/elsewhere.a"]
        keys = self.fx.keys(head)
        self.assertEqual(keys[EXE]["always_run"], "unrecorded")
        self.assertIsNone(keys[OTHER]["always_run"])

    def test_a_record_from_another_toolchain_keys_nothing(self):
        head = self.head(**{"docs/readme.md": "new\n"})
        for field, value in (("clang", "Apple clang version 21.0.0 (clang-2100.3.34.2)"), ("sdk_build", "26A425"),
                             ("sdk_version", "27.0"), ("arch", "x86_64"), ("os", "Linux")):
            keys = self.fx.keys(head, toolchain={**TOOLCHAIN, field: value})
            self.assertEqual({e["always_run"] for e in keys.values()}, {"base_other_toolchain"}, field)
            self.assertEqual({e["base_key"] for e in keys.values()}, {None})

    def test_a_linux_record_keys_nothing_for_a_darwin_lane(self):
        head = self.head(**{"docs/readme.md": "new\n"})
        self.fx.record_fields = {**self.fx.record_fields, "os": "Linux"}
        keys = self.fx.keys(head)
        self.assertEqual({e["always_run"] for e in keys.values()}, {"base_other_toolchain"})
        # Control: the same record with only the OS family flipped back keys,
        # so the refusal above is the `os` field and nothing else.
        self.fx.record_fields = {**self.fx.record_fields, "os": "Darwin"}
        keys = self.fx.keys(head)
        self.assertEqual({e["always_run"] for e in keys.values()}, {None})
        self.assertTrue(self.equal(keys, EXE))

    def test_a_record_from_another_platform_or_an_incomplete_probe_keys_nothing(self):
        head = self.head(**{"docs/readme.md": "new\n"})
        for extra in ({"platform": "linux-x86_64"}, {"toolchain": {"complete": False, "missing": ["sdk_build"]}}):
            self.fx.job_extra = extra
            self.assertEqual({e["always_run"] for e in self.fx.keys(head).values()}, {"toolchain_unknown"}, extra)
        # Control: the same record saying darwin and complete keys.
        self.fx.job_extra = {"platform": "darwin-arm64", "toolchain": {"complete": True, "missing": []}}
        self.assertEqual({e["always_run"] for e in self.fx.keys(head).values()}, {None})

    def test_an_absent_probe_field_reads_unknown(self):
        head = self.head(**{"docs/readme.md": "new\n"})
        self.fx.record_fields = {k: v for k, v in self.fx.record_fields.items() if k != "sdk_version"}
        self.assertEqual({e["always_run"] for e in self.fx.keys(head).values()}, {"toolchain_unknown"})

    def test_the_os_version_alone_does_not_change_the_toolchain(self):
        self.assertEqual(ek.toolchain_of({**TOOLCHAIN, "os_version": "27.0", "os_build": "26A428"}), TOOLCHAIN)

    def test_an_unprobed_toolchain_keys_nothing(self):
        head = self.head(**{"docs/readme.md": "new\n"})
        self.assertIsNone(ek.toolchain_of({**TOOLCHAIN, "sdk_version": "unknown"}))
        self.assertIsNone(ek.toolchain_of({k: v for k, v in TOOLCHAIN.items() if k != "sdk_build"}))
        keys = self.fx.keys(head, toolchain=None)
        self.assertEqual({e["always_run"] for e in keys.values()}, {"toolchain_unknown"})

    def test_a_base_that_is_not_an_ancestor_of_head_keys_nothing(self):
        head = self.head(**{"docs/readme.md": "new\n"})
        later = self.head(**{"docs/readme.md": "newer\n"})
        self.fx.base = later                               # the record's tree is ahead of head
        self.assertEqual({e["always_run"] for e in self.fx.keys(head).values()}, {"base_unrecorded"})

    def test_a_stale_or_missing_object_is_unrecorded(self):
        head = self.head(**{"docs/readme.md": "new\n"})
        self.fx.stale = [obj("liba", "core/liba.cpp")]
        keys = self.fx.keys(head)
        self.assertEqual(keys[EXE]["always_run"], "unrecorded")
        self.assertIsNone(keys[EXE]["base_key"])
        self.assertIsNone(keys[OTHER]["always_run"])
        self.fx.stale = []
        del self.fx.deps[obj("pulp-test-b", "test/test_b.cpp")]
        self.assertEqual(self.fx.keys(head)[OTHER]["always_run"], "unrecorded")

    def test_an_added_header_named_like_an_input_may_shadow_it(self):
        keys = self.fx.keys(self.head(**{"test/a.hpp": "shadow\n"}))
        self.assertEqual(keys[EXE]["always_run"], "include_shadow")
        self.assertIsNone(keys[OTHER]["always_run"])

    def test_without_a_usable_record_nothing_is_keyed(self):
        head = self.head(**{"docs/readme.md": "new\n"})
        self.assertEqual({e["always_run"] for e in self.fx.keys(head, record=False).values()}, {"base_unrecorded"})
        self.fx.links["schema"] = "pulp-link-members/v99"
        self.assertEqual({e["always_run"] for e in self.fx.keys(head).values()}, {"base_unrecorded"})

    def test_a_dependency_pin_reruns_everything(self):
        keys = self.fx.keys(self.head(**{"tools/deps/manifest.json": "{\"x\": 1}\n"}))
        self.assertEqual({e["always_run"] for e in keys.values()}, {"dependency_pin"})

    def test_a_digest_that_is_not_content_keyed_is_unknown(self):
        head = self.head(**{"docs/readme.md": "new\n"})
        del self.fx.base_targets["pulp-test-a"]["digest"]
        self.assertEqual(self.fx.keys(head)[EXE]["always_run"], "codemodel_unknown")

    def test_commit_bound_and_environment_bound_executables_always_run(self):
        head = self.head(**{"docs/readme.md": "new\n"})
        self.fx.head_targets["pulp-test-a"]["commit_bound"] = True
        self.fx.ctest["tests"][1]["properties"] = [{"name": "LABELS", "value": ["gpu"]}]
        keys = self.fx.keys(head)
        self.assertEqual((keys[EXE]["always_run"], keys[OTHER]["always_run"]), ("commit_bound", "environment"))
        self.fx.head_targets["pulp-test-a"]["commit_bound"] = False
        self.fx.ctest["tests"][1]["properties"] = []
        self.fx.ctest["tests"][0]["name"] = "registry lint"
        self.assertEqual(self.fx.keys(head)[EXE]["always_run"], "environment")

    def test_declared_data_inputs_are_keyed_and_other_states_always_run(self):
        self.fx.scan["executables"]["pulp-test-a"] = {"data": "declared", "inputs": ["test/fixtures/a"],
                                                      "detected_sources": ["test/test_a.cpp"]}
        self.fx.base = self.fx.commit()
        self.assertTrue(self.equal(self.fx.keys(self.head(**{"docs/readme.md": "n\n"})), EXE))
        keys = self.fx.keys(self.head(**{"test/fixtures/a/x.json": "{\"y\": 1}\n"}))
        self.assertFalse(self.equal(keys, EXE))
        self.assertTrue(self.equal(keys, OTHER))
        for state, reason in (("undeclared", "data_undeclared"), ("whole_checkout", "data_whole_checkout"),
                              ("partly", "data_unknown")):
            self.fx.scan["executables"]["pulp-test-b"] = {"data": state, "inputs": [],
                                                          "detected_sources": ["test/test_b.cpp"]}
            self.assertEqual(self.fx.keys(self.head())[OTHER]["always_run"], reason, state)

    def test_an_unscanned_executable_or_an_untrusted_scan_always_runs(self):
        self.fx.scan["executables_scanned"].remove("pulp-test-b")
        keys = self.fx.keys(self.head())
        self.assertEqual(keys[OTHER]["always_run"], "data_unknown")
        self.assertIsNone(keys[EXE]["always_run"])
        self.assertIsNone(keys[MOD]["always_run"])          # a module reads nothing on its own
        self.fx.scan["executables_scanned"].append("pulp-test-b")
        for entry in KNOWN_READERS.values():
            entry["detected_sources"] = []                  # a blind scan
        try:
            self.assertEqual(self.fx.keys(self.head())[EXE]["always_run"], "data_unknown")
        finally:
            for entry in KNOWN_READERS.values():
                entry["detected_sources"] = ["test/k.cpp"]

    def test_an_undeclared_spawn_always_runs(self):
        self.fx.scan["executables"]["pulp-test-a"] = {"spawns": "undeclared", "data": "none"}
        self.assertEqual(self.fx.keys(self.head())[EXE]["always_run"], "spawns_undeclared")


class ManifestTests(unittest.TestCase):
    def test_the_manifest_names_its_producer_and_what_it_was_handed(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(Path(tmp))
            head = fx.commit()
            fx.write_record()
            cm = Path(tmp) / "cm.json"
            cm.write_text(json.dumps({"schema": V2, "generated_headers": "ninja-deps", "targets": fx.head_targets}))
            out = Path(tmp) / "keys.json"
            tc = Path(tmp) / "tc.json"
            tc.write_text(json.dumps(TOOLCHAIN))
            argv = ["x", "--source-root", str(fx.root), "--base-sha", fx.base, "--head-sha", head,
                    "--base-record", str(fx.record), "--base-record-run-id", "42", "--head-codemodel", str(cm),
                    "--build-dir", str(fx.build), "--toolchain-json", str(tc), "--out", str(out)]
            self.assertEqual(ek.main(argv), 0)
            doc = json.loads(out.read_text())
            self.assertEqual(doc["schema"], ek.SCHEMA)
            producer = doc["producer"]
            self.assertEqual((producer["base_sha"], producer["head_sha"], producer["base_record_run_id"]),
                             (fx.base, head, "42"))
            self.assertEqual(producer["code_paths"], list(ek.KEY_CODE_PATHS))
            first = producer["base_record_sha256"]
            self.assertEqual(producer["toolchain"], TOOLCHAIN)
            self.assertEqual(doc["reasons"], {"keyed": 3})
            (fx.record / "job.json").write_text(json.dumps({"runner_image": {"digest": "other"}}))
            ek.main(argv)
            doc = json.loads(out.read_text())
            self.assertNotEqual(doc["producer"]["base_record_sha256"], first)
            self.assertEqual(doc["reasons"], {"toolchain_unknown": 3})

    def test_every_key_code_path_exists(self):
        repo = HERE.parents[1]
        self.assertEqual([p for p in ek.KEY_CODE_PATHS if not (repo / p).is_file()], [])


if __name__ == "__main__":
    unittest.main()
