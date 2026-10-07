#!/usr/bin/env python3
"""Tests for tools/ci/executable_keys.py: per-executable source keys over a
base-recorded input set, and every always_run reason."""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    tomllib = None

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import executable_keys as ek  # noqa: E402

V2 = "pulp-codemodel-digest/v2"
# A record's toolchain identity (reuse_record.toolchain_identity's fields)
# and the key the gate's job carries for it.
IDENTITY = {"compiler_id": "AppleClang", "compiler_version": "21.0.0.21000111",
            "compiler": "Apple clang version 21.0.0 (clang-2100.1.1.101)", "target": "arm64-apple-darwin26.4.0",
            "sdk_version": "26.4", "sdk_build": "25E236", "deployment_target": "13.4", "env": {"CC": "unset"}}
TOOLCHAIN = {"os": "Darwin", "arch": "arm64", **{k: v for k, v in IDENTITY.items() if k != "target"}}
EXE, OTHER, MOD = "test/pulp-test-a", "test/pulp-test-b", "test/plug.so"
# The read audit observed both test executables (a clean audit's covered set).
ALL_AUDITED = frozenset({"pulp-test-a", "pulp-test-b"})
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
        self.record_fields = {"os": "Darwin", "arch": "arm64", "os_version": "26.4", "os_build": "25E246"}
        self.job_extra: dict = {"platform": "darwin-arm64",
                                "toolchain": {"complete": True, "missing": [], "fields": dict(IDENTITY)}}

    def targets(self) -> dict:
        def t(kind, art, deps=()):
            return {"type": kind, "digest": f"d-{art}", "artifacts": [f"<build>/{art}"], "dependencies": list(deps)}
        return {"pulp-test-a": t("EXECUTABLE", EXE, ["liba"]), "pulp-test-b": t("EXECUTABLE", OTHER, ["plug"]),
                "plug": t("MODULE_LIBRARY", MOD), "liba": t("STATIC_LIBRARY", "core/liba.a")}

    def commit(self) -> str:
        for path, body in self.files.items():
            p = self.root / path
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(body, encoding="utf-8")
        (self.root / ek.SCRIPT_INPUTS_PATH).parent.mkdir(parents=True, exist_ok=True)
        (self.root / ek.SCRIPT_INPUTS_PATH).write_text(json.dumps(self.scan), encoding="utf-8")
        git = ["git", "-C", str(self.root), "-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false"]
        subprocess.run(git + ["add", "-A"], check=True)
        subprocess.run(git + ["commit", "-q", "--allow-empty", "-m", "c"], check=True)
        return subprocess.run(git + ["rev-parse", "HEAD"], check=True, capture_output=True, text=True, encoding="utf-8").stdout.strip()

    def write_record(self) -> None:
        self.record.mkdir(exist_ok=True)
        headers = sorted({h for hs in self.deps.values() for h in hs})
        deps_doc = {"schema": "pulp-object-deps/v1", "headers": headers, "stale": self.stale,
                    # A stale object is distrusted even if a writer also lists its deps.
                    "objects": {o: [headers.index(h) for h in hs] for o, hs in self.deps.items()},
                    "members": {"<build>/core/liba.a": {"liba.cpp.o": [obj("liba", "core/liba.cpp")],
                                                        "unpulled.cpp.o": [obj("liba", "core/unpulled.cpp")]}}}
        (self.record / "codemodel-x.json").write_text(json.dumps(
            {"schema": V2, "generated_headers": "ninja-deps", "targets": self.base_targets}), encoding="utf-8")
        (self.record / "link-members-x.json").write_text(json.dumps(self.links), encoding="utf-8")
        (self.record / "object-deps-x.json").write_text(json.dumps(deps_doc), encoding="utf-8")
        (self.record / "job.json").write_text(json.dumps({"runner_image": {"digest": "img", "fields": self.record_fields},
                                                          **self.job_extra}), encoding="utf-8")

    def keys(self, head: str, record: bool = True, toolchain: dict | None = TOOLCHAIN,
             key_blind: Path | None = None,
             audited: frozenset | None = ALL_AUDITED) -> dict:
        self.write_record()
        rec = ek.load_record(self.record)[0] if record else None
        cm = {"schema": V2, "generated_headers": "ninja-deps", "targets": self.head_targets}
        return ek.compute(self.root, self.base, head, rec, cm, self.ctest, self.build, toolchain,
                          key_blind, audited=audited)["executables"]


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
        other = {**TOOLCHAIN, "compiler": "Apple clang version 21.0.0 (clang-2100.3.34.2)"}
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
        for field, value in (("compiler", "Apple clang version 21.0.0 (clang-2100.3.34.2)"),
                             ("compiler_version", "21.0.0.21000334"), ("sdk_build", "26A425"), ("sdk_version", "27.0"),
                             ("deployment_target", "14.0"), ("env", {"CC": "/opt/homebrew/bin/clang"}),
                             ("arch", "x86_64"), ("os", "Linux")):
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
        complete = dict(self.fx.job_extra)
        for extra in ({**complete, "platform": "linux-x86_64"},
                      {**complete, "toolchain": {**complete["toolchain"], "complete": False, "missing": ["sdk_build"]}},
                      {"platform": "darwin-arm64"}):                       # a record from before the identity block
            self.fx.job_extra = extra
            self.assertEqual({e["always_run"] for e in self.fx.keys(head).values()}, {"toolchain_unknown"}, extra)
        # Control: the same record, darwin and complete, keys.
        self.fx.job_extra = complete
        self.assertEqual({e["always_run"] for e in self.fx.keys(head).values()}, {None})

    def test_an_absent_fingerprint_field_reads_unknown(self):
        head = self.head(**{"docs/readme.md": "new\n"})
        self.fx.record_fields = {k: v for k, v in self.fx.record_fields.items() if k != "arch"}
        self.assertEqual({e["always_run"] for e in self.fx.keys(head).values()}, {"toolchain_unknown"})

    def test_the_os_version_and_the_printed_triple_do_not_change_the_key(self):
        job = {"platform": "darwin-arm64", "runner_image": {"fields": {"os": "Darwin", "arch": "arm64", "os_version": "27.0"}},
               "toolchain": {"complete": True, "fields": {**IDENTITY, "target": "arm64-apple-darwin27.0.0"}}}
        self.assertEqual(ek.toolchain_key(job), TOOLCHAIN)

    def test_an_unprobed_toolchain_keys_nothing(self):
        head = self.head(**{"docs/readme.md": "new\n"})
        keys = self.fx.keys(head, toolchain=None)
        self.assertEqual({e["always_run"] for e in keys.values()}, {"toolchain_unknown"})

    def test_the_lane_computes_its_toolchain_with_the_record_s_own_functions(self):
        import reuse_record
        calls = []

        def identity(build_dir, env, fields):
            calls.append(build_dir)
            return {"complete": True, "missing": [], "fields": dict(IDENTITY)}
        with mock.patch.object(reuse_record, "runner_image",
                               return_value={"fields": {"os": "Darwin", "arch": "arm64"}}), \
                mock.patch.object(reuse_record, "toolchain_identity", identity), \
                mock.patch.object(reuse_record, "platform_id", return_value="darwin-arm64"):
            self.assertEqual(ek.probe_toolchain(Path("/b")), TOOLCHAIN)
        self.assertEqual(calls, [Path("/b")])          # the compiler comes from the configured build

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

    def test_a_pin_change_no_dependency_can_be_named_for_reruns_everything(self):
        for manifest in ("{\"x\": 1}\n", "{\"dependencies\": [\n"):
            with self.subTest(manifest=manifest):
                keys = self.fx.keys(self.head(**{"tools/deps/manifest.json": manifest}))
                self.assertEqual({e["always_run"] for e in keys.values()}, {"dependency_pin"})

    def pins_base(self, **files):
        """Re-commit the base with these pin files, then name the head."""
        self.fx.files.update(files)
        self.fx.base = self.fx.commit()

    def test_another_platforms_skia_change_reruns_nothing_here(self):
        fixtures = HERE / "fixtures" / "dependency_pins"
        read = lambda name: (fixtures / name).read_text(encoding="utf-8")  # noqa: E731
        self.pins_base(**{"tools/deps/manifest.json": read("manifest.base.json"),
                          "tools/cmake/PulpDependencies.cmake": read("PulpDependencies.base.cmake.txt")})
        head = self.head(**{"tools/deps/manifest.json": read("manifest.head.json"),
                            "tools/cmake/PulpDependencies.cmake": read("PulpDependencies.head.cmake.txt")})
        keys = self.fx.keys(head)
        self.assertNotIn("dependency_pin", {e["always_run"] for e in keys.values()})
        self.assertTrue(self.equal(keys, EXE))

    def test_a_moved_dependency_reruns_only_what_builds_against_it(self):
        skia = "/Users/x/.cache/pulp/skia/darwin-arm64-1/build/lib/Release/libskia.a"
        self.fx.links["executables"][f"<build>/{EXE}"]["archives"][skia] = {"members": [], "whole": False}
        self.fx.links["members"][skia] = []
        assets = {"mac-arm64": {"url": "u", "sha256": "1" * 64}, "win-x64": {"url": "u", "sha256": "2" * 64}}
        manifest = lambda a: json.dumps({"dependencies": [  # noqa: E731
            {"name": "Skia", "version": "m1", "determinism": {"release_assets": a}}]})
        self.pins_base(**{"tools/deps/manifest.json": manifest(assets)})
        windows = self.head(**{"tools/deps/manifest.json": manifest({**assets, "win-x64": {"url": "u", "sha256": "3" * 64}})})
        self.assertNotIn("dependency_pin", {e["always_run"] for e in self.fx.keys(windows).values()})
        mac = self.head(**{"tools/deps/manifest.json": manifest({**assets, "mac-arm64": {"url": "u", "sha256": "4" * 64}})})
        keys = self.fx.keys(mac)
        self.assertEqual(keys[EXE]["always_run"], "dependency_pin")
        self.assertIsNone(keys[OTHER]["always_run"])
        self.assertTrue(self.equal(keys, OTHER))

    def test_a_moved_dependency_the_build_cannot_show_reruns_everything(self):
        # Highway is mapped, but nothing in this build is a Highway target:
        # a mapping that finds nothing cannot vouch for anything.
        manifest = lambda v: json.dumps({"dependencies": [{"name": "Highway", "version": v}]})  # noqa: E731
        self.pins_base(**{"tools/deps/manifest.json": manifest("1")})
        keys = self.fx.keys(self.head(**{"tools/deps/manifest.json": manifest("2")}))
        self.assertEqual({e["always_run"] for e in keys.values()}, {"dependency_pin"})

    def test_a_fetchcontent_dependency_reaches_through_its_target(self):
        yoga = {"type": "STATIC_LIBRARY", "digest": "d-yoga", "artifacts": ["<build>/_deps/yoga-build/libyogacore.a"],
                "dependencies": []}
        for side in (self.fx.head_targets, self.fx.base_targets):
            side["yogacore"] = dict(yoga)
            side["liba"]["dependencies"] = ["yogacore"]
        manifest = lambda v: json.dumps({"dependencies": [{"name": "Yoga", "version": v}]})  # noqa: E731
        self.pins_base(**{"tools/deps/manifest.json": manifest("v1")})
        keys = self.fx.keys(self.head(**{"tools/deps/manifest.json": manifest("v2")}))
        self.assertEqual((keys[EXE]["always_run"], keys[OTHER]["always_run"]), ("dependency_pin", None))

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
        self.assertIsNone(keys[MOD]["always_run"])          # scanned, and the scan found no reads
        self.fx.scan["executables_scanned"].append("pulp-test-b")
        for entry in KNOWN_READERS.values():
            entry["detected_sources"] = []                  # a blind scan
        try:
            self.assertEqual(self.fx.keys(self.head())[EXE]["always_run"], "data_unknown")
        finally:
            for entry in KNOWN_READERS.values():
                entry["detected_sources"] = ["test/k.cpp"]

    def test_a_module_the_scans_cannot_vouch_for_always_runs(self):
        self.fx.scan["executables_scanned"].remove("plug.so")
        self.assertEqual(self.fx.keys(self.head())[MOD]["always_run"], "data_unknown")
        self.fx.scan["executables_scanned"].append("plug.so")
        self.fx.scan["executables"]["plug.so"] = {"data": "undeclared", "inputs": [], "detected_sources": ["plug/plug.cpp"]}
        self.assertEqual(self.fx.keys(self.head())[MOD]["always_run"], "data_undeclared")

    def test_the_lane_keys_the_compiler_cmake_chose_not_the_one_on_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)

            def compiler(path: Path, line: str) -> Path:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("#!/bin/sh\n"
                                f'case "$1" in -print-target-triple) echo arm64-apple-darwin99.0.0 ;; '
                                f'*) echo "{line}" ;; esac\n', encoding="utf-8")
                path.chmod(0o755)
                return path
            chosen = compiler(tmp / "toolchain" / "c++", "CMake-chosen clang 1.0")
            compiler(tmp / "path" / "clang", "PATH clang 9.9")
            build = tmp / "build"
            (build / "CMakeFiles" / "3.30.0").mkdir(parents=True)
            (build / "CMakeFiles" / "3.30.0" / "CMakeCXXCompiler.cmake").write_text(
                f'set(CMAKE_CXX_COMPILER "{chosen}")\nset(CMAKE_CXX_COMPILER_ID "AppleClang")\n'
                'set(CMAKE_CXX_COMPILER_VERSION "1.0.0")\n', encoding="utf-8")
            (build / "CMakeCache.txt").write_text("CMAKE_OSX_DEPLOYMENT_TARGET:STRING=13.4\n", encoding="utf-8")
            with mock.patch.dict(os.environ, {"PATH": f"{tmp / 'path'}:{os.environ.get('PATH', '')}"}):
                key = ek.probe_toolchain(build)
            if key is None:
                self.skipTest("this host's SDK could not be read, so the identity is incomplete")
            self.assertEqual(key["compiler"], "CMake-chosen clang 1.0")
            self.assertNotIn("target", key)

    def test_a_key_blind_executable_always_runs(self):
        head = self.head(**{"docs/readme.md": "new\n"})
        listed = Path(self.tmp.name) / "key_blind.json"
        listed.write_text(json.dumps({"schema": ek.KEY_BLIND_SCHEMA, "executables": {
            EXE: {"example": {"pr": 1, "group_run_id": "g"}, "explained": "linker stub order"}}}), encoding="utf-8")
        keys = self.fx.keys(head, key_blind=listed)
        self.assertEqual((keys[EXE]["always_run"], keys[OTHER]["always_run"]), ("key_blind", None))
        listed.write_text(json.dumps({"schema": "something-else", "executables": {}}), encoding="utf-8")
        with self.assertRaises(ValueError):                 # an unreadable list is an error, not empty
            self.fx.keys(head, key_blind=listed)

    def test_a_loader_of_a_build_produced_shared_library_always_runs(self):
        head = self.head(**{"docs/readme.md": "new\n"})
        links = self.fx.links
        links["schema"] = "pulp-link-members/v4"
        links["executables"]["<build>/core/libs.dylib"] = {"kind": "shared", "objects": [], "archives": {}}
        links["executables"][f"<build>/{EXE}"]["shared"] = ["<build>/core/libs.dylib"]
        links["executables"][f"<build>/{MOD}"]["shared"] = []  # system libraries only: not in scope
        keys = self.fx.keys(head)
        # EXE's keys still compute equal, yet it runs: the library is not modelled.
        self.assertTrue(self.equal(keys, EXE))
        self.assertEqual((keys[EXE]["always_run"], keys[OTHER]["always_run"], keys[MOD]["always_run"]),
                         ("shared_link", None, None))
        # The same map before v4 cannot say who loads the library: nothing keys.
        links["schema"] = "pulp-link-members/v3"
        keys = self.fx.keys(head)
        self.assertEqual({e["always_run"] for e in keys.values()}, {"base_unrecorded"})

    def test_an_executable_the_read_audit_did_not_observe_always_runs(self):
        head = self.head(**{"docs/readme.md": "new\n"})
        keys = self.fx.keys(head, audited=frozenset({"pulp-test-b"}))  # a macOS-only test, say
        self.assertTrue(self.equal(keys, EXE))
        self.assertEqual((keys[EXE]["always_run"], keys[OTHER]["always_run"], keys[MOD]["always_run"]),
                         ("audit_uncovered", None, None))
        # No clean audit handed in: no executable may be keyed on its data.
        keys = self.fx.keys(head, audited=None)
        self.assertEqual((keys[EXE]["always_run"], keys[OTHER]["always_run"]), ("audit_uncovered",) * 2)

    def test_only_a_clean_audit_report_vouches_for_its_covered_set(self):
        report = {"schema": ek.READ_AUDIT_SCHEMA, "stage0": {"verdict": "clean", "covered": ["pulp-test-a"]}}
        self.assertEqual(ek.audit_covered(report), {"pulp-test-a"})
        for bad in (None, {**report, "schema": "pulp-read-audit/v9"},
                    {**report, "stage0": {"verdict": "findings", "covered": ["pulp-test-a"]}},
                    {**report, "stage0": {"verdict": "incomplete", "covered": ["pulp-test-a"]}},
                    {**report, "stage0": {"verdict": "clean"}}):
            self.assertIsNone(ek.audit_covered(bad), bad)

    def test_the_checked_in_key_blind_list_is_readable(self):
        self.assertIsInstance(ek.load_key_blind(ek.KEY_BLIND_LIST), frozenset)

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
            cm.write_text(json.dumps({"schema": V2, "generated_headers": "ninja-deps", "targets": fx.head_targets}), encoding="utf-8")
            out = Path(tmp) / "keys.json"
            tc = Path(tmp) / "tc.json"
            tc.write_text(json.dumps(TOOLCHAIN), encoding="utf-8")
            audit = Path(tmp) / "read-audit.json"
            audit.write_text(json.dumps({"schema": ek.READ_AUDIT_SCHEMA, "commit": "c0ffee",
                                         "stage0": {"verdict": "clean", "covered": ["pulp-test-a", "pulp-test-b"]}}), encoding="utf-8")
            argv = ["x", "--source-root", str(fx.root), "--base-sha", fx.base, "--head-sha", head,
                    "--base-record", str(fx.record), "--base-record-run-id", "42", "--head-codemodel", str(cm),
                    "--build-dir", str(fx.build), "--toolchain-json", str(tc), "--audit-report", str(audit),
                    "--out", str(out)]
            # Without the audit report every executable is unvouched for.
            self.assertEqual(ek.main([a for a in argv if a not in ("--audit-report", str(audit))]), 0)
            absent = json.loads(out.read_text(encoding="utf-8"))
            self.assertEqual((absent["reasons"], absent["producer"]["audit_status"]),
                             ({"audit_uncovered": 2, "keyed": 1}, "absent"))
            unclean = Path(tmp) / "unclean.json"
            unclean.write_text(json.dumps({"schema": ek.READ_AUDIT_SCHEMA, "stage0": {"verdict": "findings"}}), encoding="utf-8")
            self.assertEqual(ek.main([str(unclean) if a == str(audit) else a for a in argv]), 0)
            self.assertEqual(json.loads(out.read_text(encoding="utf-8"))["producer"]["audit_status"], "not_clean")
            self.assertEqual(ek.main(argv), 0)
            doc = json.loads(out.read_text(encoding="utf-8"))
            self.assertEqual((doc["producer"]["audit_commit"], len(doc["producer"]["audit_report_sha256"]),
                              doc["producer"]["audit_status"]), ("c0ffee", 64, "clean"))
            self.assertEqual(doc["producer"]["dependency_pins"], {"scope": "names", "names": [], "why": None})
            self.assertNotIn("dependency_pins", doc)
            self.assertEqual(doc["schema"], ek.SCHEMA)
            producer = doc["producer"]
            self.assertEqual((producer["base_sha"], producer["head_sha"], producer["base_record_run_id"]),
                             (fx.base, head, "42"))
            self.assertEqual(producer["code_paths"], list(ek.KEY_CODE_PATHS))
            first = producer["base_record_sha256"]
            self.assertEqual(producer["toolchain"], TOOLCHAIN)
            self.assertEqual(doc["reasons"], {"keyed": 3})
            (fx.record / "job.json").write_text(json.dumps({"runner_image": {"digest": "other"}}), encoding="utf-8")
            ek.main(argv)
            doc = json.loads(out.read_text(encoding="utf-8"))
            self.assertNotEqual(doc["producer"]["base_record_sha256"], first)
            self.assertEqual(doc["reasons"], {"toolchain_unknown": 3})

    @unittest.skipIf(tomllib is None, "tomllib unavailable; cannot read .shipyard/config.toml")
    def test_the_configured_rederive_command_parses_with_this_key_code(self):
        # Shipyard re-derives with the base's command against the base's key
        # code, so every flag the config passes must be one this copy accepts.
        with (HERE.parents[1] / ".shipyard" / "config.toml").open("rb") as handle:
            config = tomllib.load(handle)
        command = config["targets"]["mac"]["changed_surface_selection"]["executable_reuse"]["rederive"][0]
        self.assertEqual(command[:3], ["python3", "-I", "tools/ci/executable_keys.py"])
        argv = ["x"] + [re.sub(r"\{[a-z_]+\}", "v", arg) for arg in command[3:]]
        self.assertIn("--audit-report", argv)

        class Parsed(Exception):
            pass
        with mock.patch.object(ek, "load_record", side_effect=Parsed), self.assertRaises(Parsed):
            ek.main(argv)

    def test_registrations_match_the_build_dir_as_a_string(self):
        # The host re-deriving a manifest holds copies, not the build tree:
        # the configure's path need not exist there.
        ctest = {"tests": [{"name": "t", "command": ["/nowhere/build/test/x"], "properties": []},
                           {"name": "u", "command": ["/elsewhere/test/y"], "properties": []}]}
        self.assertEqual(list(ek.registrations(ctest, Path("/nowhere/build/"))), ["test/x"])

    def test_a_build_dir_spelled_unlike_the_inventory_keys_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(Path(tmp))
            fx.files["docs/readme.md"] = "new\n"
            head = fx.commit()
            fx.write_record()
            cm = {"schema": V2, "generated_headers": "ninja-deps", "targets": fx.head_targets}
            rec = ek.load_record(fx.record)[0]
            keyed = ek.compute(fx.root, fx.base, head, rec, cm, fx.ctest, fx.build, TOOLCHAIN,
                               audited=ALL_AUDITED)["executables"]
            self.assertIsNone(keyed[EXE]["always_run"])                        # control: same spelling keys
            other = ek.compute(fx.root, fx.base, head, rec, cm, fx.ctest, Path(tmp) / "elsewhere",
                               TOOLCHAIN, audited=ALL_AUDITED)["executables"]
            self.assertEqual({e["always_run"] for e in other.values()}, {"inventory_unmatched"})
            self.assertEqual({e["base_key"] for e in other.values()}, {None})

    def test_a_relative_build_dir_is_made_absolute_without_resolving_links(self):
        ctest = {"tests": [{"name": "t", "command": [os.path.join(os.getcwd(), "build", "test", "x")]}]}
        self.assertEqual(list(ek.registrations(ctest, Path("build"))), ["test/x"])

    def test_the_record_digest_follows_its_published_formula(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for rel, body in (("a/b", "slash\n"), ("a.b", "dot\n"), ("suites/x.xml", "<x/>\n")):
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_text(body, encoding="utf-8")
            # The planner reimplements this; byte order puts a.b before a/b.
            self.assertTrue(ek.record_digest_bytes(root).startswith(b"a.b\0"))
            self.assertEqual(ek.load_record(root)[1],
                             "e35428c1f78bd1c222fcb5efa8b0ecfd07d575c9712ae59254e3ec60922288fb")

    def test_the_printed_identity_is_the_record_s_own(self):
        import contextlib
        import io
        import reuse_record
        identity = {"digest": "abc", "complete": True, "missing": [], "fields": dict(IDENTITY)}
        out = io.StringIO()
        with mock.patch.object(reuse_record, "runner_image", return_value={"digest": "i", "fields": {"os": "Darwin"}}), \
                mock.patch.object(reuse_record, "toolchain_identity", return_value=identity), \
                mock.patch.object(reuse_record, "platform_id", return_value="darwin-arm64"), \
                contextlib.redirect_stdout(out):
            self.assertEqual(ek.main(["x", "--print-toolchain", "--build-dir", "/b"]), 0)
        printed = json.loads(out.getvalue())
        self.assertEqual(printed["toolchain"], identity)            # the record's digest, not a recomputed one
        self.assertEqual(printed["platform"], "darwin-arm64")
        with self.assertRaises(SystemExit):
            ek.main(["x", "--build-dir", "/b"])                     # keys still need their inputs

    def test_a_cold_build_dir_still_states_the_platform(self):
        # The planner binds a candidate set by platform before the lane has
        # configured anything; a cold lane must still answer.
        import contextlib
        import io
        import reuse_record
        for build in (Path(self.id()) / "never-configured", None):
            out = io.StringIO()
            argv = ["x", "--print-toolchain"] + (["--build-dir", str(build)] if build else [])
            with contextlib.redirect_stdout(out):
                self.assertEqual(ek.main(argv), 0, build)
            printed = json.loads(out.getvalue())
            self.assertEqual(printed["platform"], reuse_record.platform_id())
            self.assertFalse((printed["toolchain"] or {}).get("complete", False))

    def test_every_key_code_path_exists(self):
        repo = HERE.parents[1]
        self.assertEqual([p for p in ek.KEY_CODE_PATHS if not (repo / p).is_file()], [])


class KeyCodeClosureTests(unittest.TestCase):
    """The digests that decide when a plan must select everything cover what
    the key code reads, and nothing else: a file the key code does not import
    makes every base that moved it look like a policy change."""

    REPO = HERE.parents[1]
    ENTRY_POINTS = ("tools/ci/executable_keys.py", "tools/ci/executable_selection.py")
    # Named by the config but not imported by the key code: the adapter that
    # runs the selection.
    ADAPTER = ("tools/scripts/run_changed_surface_tests.py",)

    def closure(self) -> set[str]:
        """Repo-relative Python files the entry points import, transitively,
        resolved as the scripts resolve them (tools/ci, then tools/scripts)."""
        import ast
        seen, todo = set(), list(self.ENTRY_POINTS)
        while todo:
            rel = todo.pop()
            if rel in seen:
                continue
            seen.add(rel)
            for node in ast.walk(ast.parse((self.REPO / rel).read_text(encoding="utf-8"))):
                names = ([a.name for a in node.names] if isinstance(node, ast.Import) else
                         [node.module] if isinstance(node, ast.ImportFrom) and node.module and not node.level else [])
                for name in names:
                    for root in (HERE, HERE.parent / "scripts"):
                        candidate = root / f"{name.split('.')[0]}.py"
                        if candidate.is_file():
                            todo.append(candidate.relative_to(self.REPO).as_posix())
                            break
        return seen

    def derivation_paths(self) -> list[str]:
        import tomllib
        config = tomllib.loads((self.REPO / ".shipyard" / "config.toml").read_text(encoding="utf-8"))
        return config["targets"]["mac"]["changed_surface_selection"]["executable_reuse"]["derivation_paths"]

    def test_the_closure_reaches_the_shared_name_pattern(self):
        # Control: an empty or partial closure would pass every check below.
        closure = self.closure()
        self.assertIn("tools/ci/always_run_names.py", closure)
        self.assertIn("tools/scripts/gate_common.py", closure)
        self.assertGreaterEqual(len(closure), 10)

    def test_the_derivation_paths_are_the_closure_plus_the_adapter(self):
        python = sorted(p for p in self.derivation_paths() if p.endswith(".py"))
        self.assertEqual(python, sorted(self.closure() | set(self.ADAPTER)))

    def test_the_key_code_digest_names_only_files_the_key_code_reads(self):
        closure = self.closure()
        self.assertEqual([p for p in ek.KEY_CODE_PATHS if p.endswith(".py") and p not in closure], [])

    def test_the_receipts_shadow_is_in_neither(self):
        # Its edits moved the policy digest on bases that changed nothing the
        # keys read; the keys use only the name pattern it shares.
        self.assertNotIn("tools/ci/test_receipts_shadow.py", ek.KEY_CODE_PATHS)
        self.assertNotIn("tools/ci/test_receipts_shadow.py", self.derivation_paths())


if __name__ == "__main__":
    unittest.main()
