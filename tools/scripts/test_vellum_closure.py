#!/usr/bin/env python3
import contextlib
import io
import subprocess
import unittest
import unittest.mock
import json
import tempfile
from pathlib import Path
from argparse import Namespace
from vellum_closure import (
    ExternalResolver,
    cmd_gate,
    collect_headers,
    fetchcontent_pins,
    external_input_violations,
    edge_key,
    load_allowlist,
    policy_include_forbidden,
    policy_target_forbidden,
    receipt_path_violations,
    symbol_closure,
)


class ClosurePolicyTests(unittest.TestCase):
    def test_policy_matches_boundaries(self):
        self.assertTrue(policy_target_forbidden("pulp::audio"))
        self.assertTrue(policy_target_forbidden("pulp::format-view"))
        self.assertTrue(policy_target_forbidden("pulp::gpu_audio"))
        self.assertFalse(policy_target_forbidden("pulp::canvas"))
        self.assertFalse(policy_target_forbidden("pulp::audiofx"))

    def test_include_policy_negative_control(self):
        self.assertTrue(policy_include_forbidden("pulp/audio/buffer.hpp"))
        self.assertTrue(policy_include_forbidden("pulp/content/registry.hpp"))
        self.assertFalse(policy_include_forbidden("pulp/canvas/canvas.hpp"))
        self.assertFalse(policy_include_forbidden("pulp/audiofx/effect.hpp"))

    def test_symbol_policy_negative_control(self):
        self.assertEqual(symbol_closure(["_ZN5pulp14ContentRegistryE", "_ZN5pulp6CanvasE"]), ["_ZN5pulp14ContentRegistryE"])
        self.assertEqual(symbol_closure(["_ZN5pulp14ContentRegistryE"], ["_ZN5pulp14ContentRegistryE"]), [])

    def test_unknown_build_tree_input_is_rejected(self):
        manifest = {
            "source_root": ".",
            "external_inputs": [{
                "path": "@build/_deps/rogue-src/include/should-export.hpp",
                "source": "build-tree",
                "pin": "unresolved",
            }],
        }
        self.assertTrue(external_input_violations(manifest))

    def test_receipt_paths_have_no_host_absolute_values(self):
        self.assertEqual(receipt_path_violations({
            "files": [{"pulp_path": "core/canvas/canvas.cpp", "mirror_path": "mirror/core/canvas/canvas.cpp"}],
            "external_inputs": [{"path": "@build/_deps/choc-src/include/choc.hpp"}],
            "generated_inputs": [{"path": "@build/core/runtime/generated/build_info.hpp", "producer_paths": ["core/runtime/CMakeLists.txt"]}],
        }), [])
        self.assertTrue(receipt_path_violations({"files": [{"pulp_path": "/Users/agent/build/rogue.hpp"}]}))

    def test_edge_allowlist_keeps_audio_targets_distinct(self):
        self.assertEqual(edge_key("pulp-design-import", "pulp-audio"), "pulp-design-import->pulp-audio")
        self.assertEqual(edge_key("pulp-design-import", "pulp::view-audio"), "pulp-design-import->pulp-view-audio")
        entries = load_allowlist(__import__("pathlib").Path(__file__).with_name("vellum_closure_allowlist.json"))
        self.assertFalse(any(e["kind"] == "edge" and e["match"] == "pulp-design-import->pulp-view-audio" for e in entries))
        self.assertFalse(any(e["kind"] == "target" for e in entries))

    def test_include_allowlist_is_edge_specific(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "core/platform/src/android/jni_motion.cpp"
            source.parent.mkdir(parents=True)
            source.write_text("#include <pulp/audio/secret.hpp>\n", encoding="utf-8")
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps({
                "source_root": str(root),
                "files": ["core/platform/src/android/jni_motion.cpp"],
                "targets": [],
                "undefined_symbols": [],
            }), encoding="utf-8")
            args = Namespace(
                manifest=[str(manifest)], allow=[], allow_include_path=[],
                allow_include_edge=[("core/platform/src/android/jni_motion.cpp", "pulp/view/motion.hpp")],
                allow_include_prefix=[], allow_root_prefix=[], report_only=False,
                allowlist=None, generate=False, unit_manifest=None, target=None,
                source_root=None, build_dir=None,
            )
            self.assertEqual(__import__("vellum_closure").cmd_gate(args), 1)


def gate_args(manifest, allowlist=None):
    return Namespace(
        manifest=[str(manifest)], allow=[], allow_include_path=[], allow_include_edge=[],
        allow_include_prefix=[], allow_root_prefix=[], report_only=False,
        allowlist=str(allowlist) if allowlist else None, generate=False, unit_manifest=None,
        target=None, source_root=None, build_dir=None,
    )


def run_gate(manifest, allowlist=None):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = cmd_gate(gate_args(manifest, allowlist))
    return rc, out.getvalue()


class EdgeAllowlistTests(unittest.TestCase):
    """A view-audio debt entry must never admit a pulp-audio link."""

    def test_audio_link_is_not_covered_by_view_audio_entry(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps({
                "source_root": str(root), "files": [], "external_inputs": [], "undefined_symbols": [],
                "targets": [{"name": "pulp-view-native", "link_targets": ["pulp-audio"]}],
            }), encoding="utf-8")
            allowlist = root / "allowlist.json"
            allowlist.write_text(json.dumps({
                "schema": "pulp.vellum.closure.allowlist.v1",
                "entries": [{"id": "view-audio", "owner": "test", "debt_id": "test", "kind": "edge",
                             "match": "pulp-view-native->pulp-view-audio", "exit_criterion": "test"}],
            }), encoding="utf-8")
            rc, out = run_gate(manifest, allowlist)
            self.assertEqual(rc, 1)
            self.assertIn("forbidden link pulp-view-native -> pulp-audio", out)
            self.assertIn("allowlist entry unused: view-audio", out)


class ImportDirectiveTests(unittest.TestCase):
    """Objective-C ``#import`` is an include edge for the gate and the collector."""

    def planted_gate(self, directive):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "core/canvas/platform/mac/cg_canvas.mm"
            source.parent.mkdir(parents=True)
            source.write_text(f"{directive} <pulp/audio/buffer.hpp>\n", encoding="utf-8")
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps({
                "source_root": str(root), "files": ["core/canvas/platform/mac/cg_canvas.mm"],
                "targets": [], "undefined_symbols": [], "external_inputs": [],
            }), encoding="utf-8")
            return run_gate(manifest)

    def test_import_of_forbidden_header_fails(self):
        rc, out = self.planted_gate("#import")
        self.assertEqual(rc, 1)
        self.assertIn("forbidden include core/canvas/platform/mac/cg_canvas.mm:1: pulp/audio/buffer.hpp", out)

    def test_include_of_forbidden_header_fails(self):
        rc, _out = self.planted_gate("#include")
        self.assertEqual(rc, 1)

    def test_header_collection_follows_import(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td).resolve()
            (root / "a.mm").write_text('#import "b.hpp"\n', encoding="utf-8")
            (root / "b.hpp").write_text("", encoding="utf-8")
            found = collect_headers({root / "a.mm"}, [], root)
            self.assertIn(root / "b.hpp", found)


REPO_ROOT = Path(__file__).resolve().parents[2]
CHOC_PATH = "@build/_deps/choc-src/include/choc/value_tree/value_tree.h"


class ExternalInputBranchTests(unittest.TestCase):
    """Each provenance branch is exercised by changing one field of a valid row."""

    def setUp(self):
        self.expected = fetchcontent_pins(REPO_ROOT)["choc"]

    def violations(self, **changes):
        row = {"path": CHOC_PATH, "source": self.expected["source"], "pin": self.expected["pin"]}
        row.update(changes)
        return external_input_violations({"source_root": str(REPO_ROOT), "external_inputs": [row]})

    def test_valid_row_passes(self):
        self.assertEqual(self.violations(), [])

    def test_wrong_repository_is_rejected(self):
        found = self.violations(source="https://github.com/example/choc.git")
        self.assertEqual(len(found), 1)
        self.assertIn("repository mismatch", found[0])

    def test_short_pin_is_rejected(self):
        found = self.violations(pin=self.expected["pin"][:12])
        self.assertEqual(len(found), 1)
        self.assertIn("full 40-hex SHA", found[0])

    def test_non_hex_pin_is_rejected(self):
        found = self.violations(pin="z" * 40)
        self.assertEqual(len(found), 1)
        self.assertIn("full 40-hex SHA", found[0])

    def test_different_full_pin_is_rejected(self):
        other = "0" * 40 if self.expected["pin"] != "0" * 40 else "1" * 40
        found = self.violations(pin=other)
        self.assertEqual(len(found), 1)
        self.assertIn("pin mismatch", found[0])

    def test_unknown_dependency_is_rejected(self):
        found = self.violations(path="@build/_deps/rogue-src/include/rogue.hpp")
        self.assertEqual(len(found), 1)
        self.assertIn("unknown FetchContent", found[0])

    def test_build_tree_source_is_rejected(self):
        found = self.violations(source="build-tree")
        self.assertEqual(len(found), 1)
        self.assertIn("unresolved build-tree", found[0])

    def test_unresolved_pin_is_rejected(self):
        found = self.violations(pin="unresolved")
        self.assertEqual(len(found), 1)
        self.assertIn("unresolved build-tree", found[0])


class EveryExternalRowTests(unittest.TestCase):
    """Provenance holds for every external row, not only ``@build/_deps`` ones."""

    def setUp(self):
        from vellum_closure import fetchcontent_pins
        self.pins = fetchcontent_pins(REPO_ROOT)

    def check(self, row):
        return external_input_violations({"source_root": str(REPO_ROOT), "external_inputs": [row]})

    def test_shared_cache_host_path_is_rejected(self):
        found = self.check({
            "path": "/Users/agent/Library/Caches/Pulp/fetchcontent-src/sdl3-release-3.2.12/include/SDL3/SDL.h",
            "source": self.pins["sdl3"]["source"], "pin": self.pins["sdl3"]["pin"],
        })
        self.assertTrue(any("host path" in v for v in found), found)

    def test_unresolved_toolchain_input_outside_build_tree_is_rejected(self):
        found = self.check({"path": "/Users/agent/.cache/pulp/skia/x/include/core/SkCanvas.h",
                            "source": "build-tree", "pin": "unresolved"})
        self.assertTrue(found)

    def test_tag_pin_is_rejected(self):
        found = self.check({"path": "@external/sdl3/release-3.2.12/include/SDL3/SDL.h",
                            "source": self.pins["sdl3"]["source"], "pin": "release-3.2.12"})
        self.assertTrue(any("full 40-hex" in v for v in found), found)

    def test_forged_commit_for_former_tag_dependency_is_rejected(self):
        commit = self.pins["sdl3"]["pin"]
        self.assertRegex(commit, r"^[0-9a-f]{40}$")
        row = {"path": f"@external/sdl3/{commit}/include/SDL3/SDL.h", "source": self.pins["sdl3"]["source"], "pin": commit, "sha256": "a" * 64}
        self.assertEqual(self.check(row), [])
        forged = "0" * 40
        found = self.check({**row, "path": f"@external/sdl3/{forged}/include/SDL3/SDL.h", "pin": forged})
        self.assertTrue(any("pin mismatch" in v for v in found), found)

    def test_closure_dependencies_carry_checked_in_commits(self):
        for name in ("sdl3", "yaml-cpp", "yoga", "sheenbidi", "choc", "brotli", "highway", "woff2", "mbed tls"):
            self.assertRegex(self.pins[name]["pin"], r"^[0-9a-f]{40}$", name)
        self.assertEqual(self.pins["sheenbidi"]["pin"], "cfe430e7375a7845b679adae9d51dac6deaa8858")

    def test_dependency_known_only_by_tag_is_rejected(self):
        pins = {"tagonly": {"source": "https://example.invalid/t.git", "pin": "v1.0"}}
        with unittest.mock.patch("vellum_closure.fetchcontent_pins", return_value=pins):
            found = external_input_violations({"source_root": str(REPO_ROOT), "external_inputs": [
                {"path": "@external/tagonly/" + "a" * 40 + "/t.h", "source": pins["tagonly"]["source"], "pin": "a" * 40, "sha256": "a" * 64}]})
        self.assertTrue(any("no checked-in 40-hex commit" in v for v in found), found)

    def test_path_pin_must_match_pin_field(self):
        pin = self.pins["choc"]["pin"]
        row = {"path": "@external/choc/" + "0" * 40 + "/include/choc.h", "source": self.pins["choc"]["source"], "pin": pin, "sha256": "a" * 64}
        self.assertTrue(any("path pin" in v for v in self.check(row)))

    def test_patched_row_needs_patch_identity_and_digest(self):
        from vellum_closure import PATCH_RECIPE_ID, PATCH_RECIPE_FORMULA, patch_id, patch_set_source, patch_replay
        pin = self.pins["choc"]["pin"]
        key = __import__("hashlib").sha256((REPO_ROOT / "tools/cmake/PulpPatchChoc.cmake").read_bytes()).hexdigest()
        patch_set = patch_set_source(REPO_ROOT, key)
        patch = patch_id(key, "", "c" * 64)
        row = {"path": f"@external/choc/{pin}+patch-{patch[:16]}/choc/text/choc_JSON.h", "source": self.pins["choc"]["source"],
               "pin": pin, "patched": True, "patch": patch, "sha256": "d" * 64, "patch_set": patch_set,
               "base_modification": "", "copy_tree": "c" * 64, "patch_recipe": {"id": PATCH_RECIPE_ID, "formula": PATCH_RECIPE_FORMULA,
                                                              "replay": patch_replay(REPO_ROOT / patch_set["pulp_path"], patch_set["pulp_blob"])}}
        self.assertEqual(self.check(row), [])
        self.assertTrue(self.check({k: v for k, v in row.items() if k != "sha256"}))
        self.assertTrue(any("path pin" in v for v in self.check({**row, "path": f"@external/choc/{pin}/choc/text/choc_JSON.h"})))
        self.assertTrue(any("recompute" in v for v in self.check({**row, "copy_tree": "e" * 64})))
        self.assertTrue(any("provenance" in v for v in self.check({k: v for k, v in row.items() if k != "patch_set"})))
        forged_label = {**row, "patch": "d" * 64, "path": f"@external/choc/{pin}+patch-dddddddddddddddd/choc/text/choc_JSON.h"}
        self.assertTrue(any("recompute" in v for v in self.check(forged_label)))
        replay = row["patch_recipe"]["replay"]
        self.assertEqual(replay["functions"], ["pulp_patch_choc_v8", "pulp_patch_choc_json_number_exponent"])
        reordered = {**row, "patch_recipe": {**row["patch_recipe"], "replay": {**replay, "functions": list(reversed(replay["functions"]))}}}
        self.assertTrue(any("in order" in v for v in self.check(reordered)))
        other_blob = {**row, "patch_recipe": {**row["patch_recipe"], "replay": {**replay, "patch_set_blob": "0" * 40}}}
        self.assertTrue(any("complete replay" in v for v in self.check(other_blob)))
        no_replay = {**row, "patch_recipe": {k: v for k, v in row["patch_recipe"].items() if k != "replay"}}
        self.assertTrue(any("complete replay" in v for v in self.check(no_replay)))
        clean = {"path": f"@external/choc/{pin}/choc/text/choc_JSON.h", "source": self.pins["choc"]["source"], "pin": pin, "sha256": "d" * 64}
        self.assertTrue(any("carries patch fields" in v for v in self.check({**clean, "patch": patch})))

    def test_row_without_dependency_identity_is_rejected(self):
        found = self.check({"path": "vendor/rogue.h", "source": self.pins["choc"]["source"], "pin": self.pins["choc"]["pin"]})
        self.assertTrue(any("no dependency identity" in v for v in found), found)

    def test_skia_row_needs_a_manifest_release_asset(self):
        skia = self.pins["skia"]
        row = {"path": f"@external/skia/{skia['pin']}/build/include/core/SkCanvas.h", "source": skia["source"],
               "pin": skia["pin"], "asset_sha256": skia["assets"][0], "sha256": "a" * 64}
        self.assertEqual(self.check(row), [])
        self.assertTrue(any("release asset" in v for v in self.check(row | {"asset_sha256": "f" * 64})))

    def test_npm_row_must_carry_its_lockfile_digest(self):
        lock = REPO_ROOT / "tools/import-design/jsx-runtime/package-lock.json"
        pin = "sha256:" + __import__("hashlib").sha256(lock.read_bytes()).hexdigest()
        row = {"path": "tools/import-design/jsx-runtime/node_modules/x", "source": "npm", "pin": pin}
        self.assertEqual(self.check(row), [])
        self.assertTrue(self.check(row | {"pin": "sha256:" + "0" * 64}))
        self.assertTrue(self.check(row | {"path": "anything/x.h"}))

    def test_system_row_needs_ownership_and_content_digest(self):
        row = {"path": "@system/libfreetype-dev/2.13.2/freetype2/ft2build.h", "source": "system", "pin": "2.13.2",
               "package": "libfreetype-dev", "provider": "dpkg", "sha256": "a" * 64}
        self.assertEqual(self.check(row), [])
        self.assertTrue(self.check(row | {"pin": "27"}))
        self.assertTrue(self.check({k: v for k, v in row.items() if k != "sha256"}))

    def test_planted_unowned_system_row_is_rejected(self):
        planted = {"path": "@system/freetype2/anything/x.h", "source": "system", "pin": "anything",
                   "package": "freetype2", "provider": "os-release", "sha256": "a" * 64}
        self.assertTrue(any("ownership" in v for v in self.check(planted)))


class ClosureHostPathTests(unittest.TestCase):
    def test_include_dirs_anywhere_in_a_closure_are_checked(self):
        self.assertTrue(receipt_path_violations({"public_include_dirs": ["/Users/agent/.cache/pulp/skia/x/include"]}))
        self.assertTrue(receipt_path_violations({"targets": [{"include_dirs": ["C:/Users/agent/cache"]}]}))
        self.assertEqual(receipt_path_violations({
            "source_root": "/Users/agent/pulp", "build_dir": "/Users/agent/pulp/build",
            "public_include_dirs": ["core/view/include", "@external/skia/" + "a" * 40 + "/build/include", "@system/freetype2/26.1.20"],
            "external_inputs": [{"source": "https://github.com/libsdl-org/SDL.git"}],
        }), [])

    def test_embedded_host_paths_are_rejected(self):
        for value in ("@external/brotli/" + "a" * 40 + "//Users/x/c.c", "-I/Users/me", "PATH:/Users/me",
                      "$HOME/.cache/pulp", "${HOME}/x", "@external/choc/" + "a" * 40 + "/../../etc/passwd",
                      "%USERPROFILE%\\cache"):
            self.assertTrue(receipt_path_violations({"compile_definitions": [value]}), value)

    def test_gate_rejects_host_path_in_public_include_dirs(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps({
                "source_root": str(root), "files": [], "targets": [], "undefined_symbols": [],
                "external_inputs": [], "public_include_dirs": ["/Users/agent/Library/Caches/Pulp/fetchcontent-src/yoga-v3.2.1"],
            }), encoding="utf-8")
            rc, out = run_gate(manifest)
            self.assertEqual(rc, 1)
            self.assertIn("host path in public_include_dirs", out)


def git(checkout, *args):
    return subprocess.check_output(["git", "-C", str(checkout), "-c", "user.email=t@t", "-c", "user.name=t", *args],
                                   text=True, encoding="utf-8").strip()


class ResolverTests(unittest.TestCase):
    """A row's pin and digest describe the bytes that were compiled."""

    SOURCE = "https://example.invalid/dep.git"

    def make_checkout(self, root, name="dep"):
        checkout = root / f"cache/{name}-v1"
        (checkout / "include").mkdir(parents=True)
        (checkout / "include/a.h").write_text("int a;\n", encoding="utf-8")
        subprocess.run(["git", "init", "-q", str(checkout)], check=True)
        git(checkout, "add", ".")
        git(checkout, "commit", "-qm", "base")
        build = root / "build"
        build.mkdir()
        (build / "CMakeCache.txt").write_text(f"FETCHCONTENT_SOURCE_DIR_{name.upper()}:PATH={checkout}\n", encoding="utf-8")
        return checkout, build, git(checkout, "rev-parse", "HEAD")

    def resolver(self, root, build, pin):
        return ExternalResolver(root, build, {"dep": {"source": self.SOURCE, "pin": pin}})

    def test_clean_checkout_at_its_pin_is_recorded_with_content_digest(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td).resolve()
            checkout, build, head = self.make_checkout(root)
            row = self.resolver(root, build, head).identify(str(checkout / "include/a.h"))
            digest = __import__("hashlib").sha256(b"int a;\n").hexdigest()
            self.assertEqual(row, {"path": f"@external/dep/{head}/include/a.h", "source": self.SOURCE, "pin": head, "sha256": digest})

    def test_checkout_moved_past_its_pin_is_unresolved(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td).resolve()
            checkout, build, base = self.make_checkout(root)
            (checkout / "include/b.h").write_text("", encoding="utf-8")
            git(checkout, "add", ".")
            git(checkout, "commit", "-qm", "next")
            row = self.resolver(root, build, base).identify(str(checkout / "include/a.h"))
            self.assertEqual(row["pin"], "unresolved")

    def make_patched_copy(self, root, build, checkout, body="int a_patched;\n"):
        patch_file = root / "tools/cmake/PulpPatchDep.cmake"
        patch_file.parent.mkdir(parents=True, exist_ok=True)
        patch_file.write_text('function(pulp_patch_dep dir)\n    file(WRITE "${dir}/include/a.h" "int a_patched;\\n")\nendfunction()\n',
                              encoding="utf-8")
        key = __import__("hashlib").sha256(patch_file.read_bytes()).hexdigest()
        copy = build / "_deps/pulp-mutable-dep-src"
        (copy / "include").mkdir(parents=True, exist_ok=True)
        (copy / "include/a.h").write_text(body, encoding="utf-8")
        (copy / ".pulp-source-key").write_text(f"{checkout}\n{key}\n", encoding="utf-8")
        return copy, key

    def test_modified_checkout_is_never_recorded_clean(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td).resolve()
            checkout, build, head = self.make_checkout(root)
            (checkout / "include/a.h").write_text("int patched;\n", encoding="utf-8")
            row = self.resolver(root, build, head).identify(str(checkout / "include/a.h"))
            self.assertEqual(row["pin"], "unresolved")
            self.assertNotIn("patched", row)

    def test_untracked_file_in_clean_checkout_is_unresolved(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td).resolve()
            checkout, build, head = self.make_checkout(root)
            (checkout / "include/extra.h").write_text("", encoding="utf-8")
            row = self.resolver(root, build, head).identify(str(checkout / "include/extra.h"))
            self.assertEqual(row["pin"], "unresolved")

    def test_copy_without_git_is_unresolved(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td).resolve()
            checkout, build, head = self.make_checkout(root)
            __import__("shutil").rmtree(checkout / ".git")
            row = self.resolver(root, build, head).identify(str(checkout / "include/a.h"))
            self.assertEqual(row["pin"], "unresolved")

    def test_patched_row_records_and_recomputes_its_patch_inputs(self):
        from vellum_closure import PATCH_RECIPE_ID, patch_id, patch_violations
        with tempfile.TemporaryDirectory() as td:
            root = Path(td).resolve()
            checkout, build, head = self.make_checkout(root)
            copy, key = self.make_patched_copy(root, build, checkout)
            row = self.resolver(root, build, head).identify(str(copy / "include/a.h"))
            self.assertTrue(row.get("patched"), row)
            self.assertEqual(row["patch_set"]["pulp_path"], "tools/cmake/PulpPatchDep.cmake")
            self.assertEqual(row["patch_set"]["key"], key)
            self.assertEqual(row["patch_set"]["pulp_blob"], git(checkout, "hash-object", str(root / "tools/cmake/PulpPatchDep.cmake")))
            self.assertEqual(row["base_modification"], "")
            self.assertEqual(row["patch_recipe"]["id"], PATCH_RECIPE_ID)
            self.assertEqual(row["patch_recipe"]["replay"]["functions"], ["pulp_patch_dep"])
            self.assertEqual(row["patch_recipe"]["replay"]["patch_set_blob"], row["patch_set"]["pulp_blob"])
            self.assertEqual(row["patch"], patch_id(row["patch_set"]["key"], row["base_modification"], row["copy_tree"]))
            self.assertEqual(row["path"], f"@external/dep/{head}+patch-{row['patch'][:16]}/include/a.h")
            self.assertEqual(patch_violations(row, root), [])
            for field, value in (("copy_tree", "e" * 64), ("base_modification", "e" * 64), ("patch", "e" * 64)):
                self.assertTrue(any("recompute" in v for v in patch_violations({**row, field: value}, root)), field)
            tampered_key = {**row, "patch_set": {**row["patch_set"], "key": "e" * 64}}
            self.assertTrue(patch_violations(tampered_key, root))
            tampered_blob = {**row, "patch_set": {**row["patch_set"], "pulp_blob": "0" * 40}}
            self.assertTrue(any("key and blob" in v for v in patch_violations(tampered_blob, root)))
            for field in ("patch_set", "copy_tree", "base_modification", "patch_recipe"):
                self.assertTrue(any("provenance" in v for v in patch_violations({k: v for k, v in row.items() if k != field}, root)), field)
            (copy / ".pulp-source-key").write_text(f"{checkout}\n{'d' * 64}\n", encoding="utf-8")
            unknown = self.resolver(root, build, head).identify(str(copy / "include/a.h"))
            self.assertEqual(unknown["pin"], "unresolved")

    def test_base_working_tree_does_not_affect_the_patched_identity(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td).resolve()
            checkout, build, head = self.make_checkout(root)
            copy, _key = self.make_patched_copy(root, build, checkout)
            before = self.resolver(root, build, head).identify(str(copy / "include/a.h"))
            self.assertTrue(before.get("patched"), before)
            (checkout / ".DS_Store").write_bytes(b"\0finder")
            (checkout / ".git/info/exclude").write_text("*.o\n", encoding="utf-8")
            (checkout / "stale.o").write_text("", encoding="utf-8")
            (checkout / "include/a.h").write_text("int a_patched;\nint stacked_patch;\n", encoding="utf-8")
            after = self.resolver(root, build, head).identify(str(copy / "include/a.h"))
            self.assertTrue(after.get("patched"), after)
            self.assertEqual((after["copy_tree"], after["patch"]), (before["copy_tree"], before["patch"]))

    def test_line_ending_converting_checkout_is_not_identified(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td).resolve()
            checkout, build, head = self.make_checkout(root)
            self.assertEqual(self.resolver(root, build, head).identify(str(checkout / "include/a.h"))["pin"], head)
            git(checkout, "config", "core.autocrlf", "true")
            self.assertEqual(self.resolver(root, build, head).identify(str(checkout / "include/a.h"))["pin"], "unresolved")

    def test_copy_tree_digest_is_host_independent(self):
        rows = []
        for _ in range(2):
            with tempfile.TemporaryDirectory() as td:
                root = Path(td).resolve()
                checkout, build, head = self.make_checkout(root)
                copy, _key = self.make_patched_copy(root, build, checkout)
                (copy / "include/nested").mkdir()
                (copy / "include/nested/z.h").write_text("z\n", encoding="utf-8")
                rows.append((self.resolver(root, build, head).identify(str(copy / "include/a.h")), root))
        (first, first_root), (second, second_root) = rows
        self.assertNotEqual(first_root, second_root)
        self.assertEqual(first["copy_tree"], second["copy_tree"])
        self.assertEqual(first["patch"], second["patch"])

    def test_assume_unchanged_edit_is_not_recorded_clean(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td).resolve()
            checkout, build, head = self.make_checkout(root)
            (checkout / "include/a.h").write_text("int hidden;\n", encoding="utf-8")
            git(checkout, "update-index", "--assume-unchanged", "include/a.h")
            self.assertEqual(git(checkout, "diff", "HEAD"), "")
            row = self.resolver(root, build, head).identify(str(checkout / "include/a.h"))
            self.assertEqual(row["pin"], "unresolved")

    def test_edit_to_patched_build_copy_is_not_recorded_patched(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td).resolve()
            checkout, build, head = self.make_checkout(root)
            copy, _key = self.make_patched_copy(root, build, checkout)
            self.assertTrue(self.resolver(root, build, head).identify(str(copy / "include/a.h")).get("patched"))
            (copy / "include/a.h").write_text("int a_patched;\nint local_edit;\n", encoding="utf-8")
            row = self.resolver(root, build, head).identify(str(copy / "include/a.h"))
            self.assertEqual(row["pin"], "unresolved")
            self.assertIn("patch set replayed", row["reason"])

    def test_gate_rejects_clean_claim_for_patched_bytes(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td).resolve()
            checkout, build, head = self.make_checkout(root)
            pins = {"dep": {"source": self.SOURCE, "pin": head}}
            clean = __import__("hashlib").sha256(b"int a;\n").hexdigest()
            row = {"path": f"@external/dep/{head}/include/a.h", "source": self.SOURCE, "pin": head, "sha256": clean}
            data = {"source_root": str(root), "build_dir": str(build), "external_inputs": [row]}
            with unittest.mock.patch("vellum_closure.fetchcontent_pins", return_value=pins):
                self.assertEqual(external_input_violations(data), [])
                stripped = {**data, "external_inputs": [{**row, "sha256": __import__("hashlib").sha256(b"int patched;\n").hexdigest()}]}
                found = external_input_violations(stripped)
            self.assertTrue(any("differ from its pinned commit" in v for v in found), found)

    def test_manifest_reconfigures_when_the_file_api_reply_is_missing(self):
        from vellum_closure import ensure_codemodel_reply
        with tempfile.TemporaryDirectory() as td:
            with unittest.mock.patch("vellum_closure.subprocess.run") as cmake:
                ensure_codemodel_reply("src", td)
                self.assertEqual(cmake.call_args.args[0], ["cmake", "-S", "src", "-B", td])
                reply = Path(td) / ".cmake/api/v1/reply"
                reply.mkdir(parents=True)
                (reply / "index-1.json").write_text("{}", encoding="utf-8")
                cmake.reset_mock()
                ensure_codemodel_reply("src", td)
                cmake.assert_not_called()

    def test_skia_file_must_match_its_generation_receipt(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td).resolve()
            asset = "e" * 64
            gen = root / f"skia/linux-x64-{asset}-receipt-v3"
            (gen / "build/include").mkdir(parents=True)
            (gen / "build/include/SkCanvas.h").write_text("canvas\n", encoding="utf-8")
            digest = __import__("hashlib").sha256(b"canvas\n").hexdigest()
            (gen / ".skia-asset-sha256").write_text(asset + "\n", encoding="utf-8")
            (gen / ".skia-generation-manifest.json").write_text(json.dumps({
                "asset_sha256": asset, "files": [{"path": "build/include/SkCanvas.h", "sha256": digest, "size": 7}]}), encoding="utf-8")
            pins = {"skia": {"source": "https://skia.googlesource.com/skia.git", "pin": "a" * 40, "assets": [asset]}}
            resolver = ExternalResolver(root, None, pins)
            row = resolver.identify(str(gen / "build/include/SkCanvas.h"))
            self.assertEqual(row["path"], "@external/skia/" + "a" * 40 + "/build/include/SkCanvas.h")
            self.assertEqual(row["asset_sha256"], asset)
            (gen / "build/include/SkCanvas.h").write_text("tampered\n", encoding="utf-8")
            self.assertEqual(ExternalResolver(root, None, pins).identify(str(gen / "build/include/SkCanvas.h"))["pin"], "unresolved")
            (gen / ".skia-generation-manifest.json").unlink()
            self.assertEqual(ExternalResolver(root, None, pins).identify(str(gen / "build/include/SkCanvas.h"))["pin"], "unresolved")


class SystemIdentityTests(unittest.TestCase):
    """System and SDK paths are identified by their owner, never by name lookup."""

    DPKG = {"/usr/include/freetype2": ("libfreetype-dev", "2.13.2+dfsg-1ubuntu0.2"),
            "/usr/include/freetype2/ft2build.h": ("libfreetype-dev", "2.13.2+dfsg-1ubuntu0.2"),
            "/usr/include/libpng16": ("libpng-dev", "1.6.43-5build1")}
    PROBE = {"exists": lambda path: True, "dpkg": DPKG.get, "rpm": lambda path: None,
             "pkg-config": lambda path, module: None}

    def resolver(self, **changes):
        return ExternalResolver(REPO_ROOT, None, {}, system_probe=self.PROBE | changes)

    def test_linux_dpkg_owned_include_dirs(self):
        resolver = self.resolver()
        self.assertEqual(resolver.portable("/usr/include/freetype2"), "@system/libfreetype-dev/2.13.2+dfsg-1ubuntu0.2/freetype2")
        self.assertEqual(resolver.portable("/usr/include/libpng16"), "@system/libpng-dev/1.6.43-5build1/libpng16")

    def test_dpkg_and_pkg_config_agree_on_one_identity(self):
        pkg_config = {"pkg-config": lambda path, module: ("freetype2", "26.1.20")}
        with_both = self.resolver(**pkg_config).portable("/usr/include/freetype2")
        dpkg_only = self.resolver().portable("/usr/include/freetype2")
        self.assertEqual(with_both, dpkg_only)
        pkg_config_only = self.resolver(dpkg=lambda path: None, **pkg_config).identify("/usr/include/freetype2/ft2build.h")
        self.assertEqual(pkg_config_only["provider"], "pkg-config")

    def test_unowned_path_has_no_identity(self):
        resolver = self.resolver(dpkg=lambda path: None)
        self.assertTrue(receipt_path_violations({"public_include_dirs": [resolver.portable("/usr/include/freetype2")]}))

    def test_nonexistent_path_has_no_identity(self):
        owns_everything = {"dpkg": lambda path: ("libowner", "1.0"), "rpm": lambda path: None, "pkg-config": lambda path, module: None}
        resolver = ExternalResolver(REPO_ROOT, None, {}, system_probe=owns_everything)
        self.assertTrue(resolver.identify("/usr/include/g2b-no-such-dir/x.h", digest=False)["pin"] != "1.0")
        row = resolver.identify("/usr/include/g2b-no-such-dir/x.h")
        self.assertEqual(row["pin"], "unresolved")

    @unittest.skipUnless(__import__("shutil").which("pkg-config"), "pkg-config is not installed")
    def test_pkg_config_path_spoof_is_ignored(self):
        from vellum_closure import _pkg_config_owner
        with tempfile.TemporaryDirectory() as td:
            root = Path(td).resolve()
            (root / "include").mkdir()
            (root / "include/spoof.h").write_text("", encoding="utf-8")
            (root / "g2bspoof.pc").write_text(
                f"Name: g2bspoof\nDescription: spoof\nVersion: 99.0.0\nCflags: -I{root}/include\n", encoding="utf-8")
            env = {**__import__("os").environ, "PKG_CONFIG_PATH": str(root)}
            with unittest.mock.patch.dict("os.environ", env):
                control = subprocess.run(["pkg-config", "--modversion", "g2bspoof"], capture_output=True, text=True, encoding="utf-8")
                self.assertEqual(control.stdout.strip(), "99.0.0")
                self.assertIsNone(_pkg_config_owner(str(root / "include/spoof.h"), "g2bspoof"))

    def test_sdk_row_carries_a_content_digest(self):
        with tempfile.TemporaryDirectory() as td:
            header = Path(td).resolve() / "SDKs/MacOSX15.0.sdk/usr/include/x.h"
            header.parent.mkdir(parents=True)
            header.write_text("sdk\n", encoding="utf-8")
            row = ExternalResolver(REPO_ROOT, None, {}).identify(str(header))
            self.assertEqual(row["path"], "@system/macosx-sdk/15.0/usr/include/x.h")
            self.assertEqual(row["sha256"], __import__("hashlib").sha256(b"sdk\n").hexdigest())
            self.assertEqual(external_input_violations({"source_root": str(REPO_ROOT), "external_inputs": [row]}), [])

    def test_windows_sdk_and_msvc_include_dirs(self):
        resolver = self.resolver()
        self.assertEqual(resolver.portable("C:/Program Files (x86)/Windows Kits/10/Include/10.0.22621.0/ucrt"),
                         "@system/windows-sdk/10.0.22621.0/ucrt")
        self.assertEqual(resolver.portable(r"C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\MSVC\14.40.33807\include"),
                         "@system/msvc/14.40.33807/include")

    def test_linux_shaped_closure_passes_the_gate(self):
        resolver = self.resolver()
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            manifest = root / "manifest.json"
            dirs = [resolver.portable(p) for p in ("/usr/include/freetype2", "/usr/include/libpng16")]
            manifest.write_text(json.dumps({
                "source_root": str(root), "files": [], "undefined_symbols": [], "external_inputs": [],
                "public_include_dirs": dirs, "targets": [{"name": "pulp-canvas", "link_targets": [], "include_dirs": dirs}],
            }), encoding="utf-8")
            rc, out = run_gate(manifest)
            self.assertEqual(rc, 0, out)


def gpu_configure(gpu=True, skia=True, text_shaping=True, system="Darwin", processor="arm64"):
    from vellum_closure import configure_options_sha256
    options = {"CMAKE_BUILD_TYPE": "Release", "PULP_ENABLE_GPU": "ON" if gpu else "OFF",
               "PULP_HAS_SKIA": "TRUE" if skia else "FALSE", "PULP_TEXT_SHAPING": "ON" if text_shaping else "OFF",
               "CMAKE_SYSTEM_NAME": system, "CMAKE_SYSTEM_PROCESSOR": processor}
    return {"options": options, "options_sha256": configure_options_sha256(options),
            "identity": {"gpu": gpu, "skia": skia, "text_shaping": text_shaping, "build_type": "Release",
                         "system": system, "processor": processor}}


def canvas_manifest(configure):
    from vellum_closure import fetchcontent_pins
    defines = (["PULP_HAS_SKIA=1"] if configure["identity"]["skia"] else []) + \
              (["PULP_HAS_TEXT_SHAPING=1"] if configure["identity"]["text_shaping"] else [])
    skia = fetchcontent_pins(REPO_ROOT)["skia"]
    links = [{"dependency": "skia", "source": skia["source"], "pin": skia["pin"], "asset_sha256": skia["assets"][0]}] \
        if configure["identity"]["skia"] else []
    return {"source_root": str(REPO_ROOT), "build_dir": None, "files": [], "external_inputs": [],
            "targets": [{"name": "pulp-canvas", "link_targets": ["pulp-platform"], "compile_definitions": defines,
                         "external_link_inputs": links}],
            "transitive_link_targets": ["pulp-platform"], "generated_inputs": [], "configure": configure}


class ExportProvenanceTests(unittest.TestCase):
    def test_export_refuses_a_closure_that_fails_provenance(self):
        from vellum_closure import exportable_closure, fetchcontent_pins
        sdl3 = fetchcontent_pins(REPO_ROOT)["sdl3"]
        good = {"source_root": str(REPO_ROOT), "build_dir": None, "configure": gpu_configure(), "external_inputs": [
            {"path": f"@external/sdl3/{sdl3['pin']}/include/SDL3/SDL.h", "source": sdl3["source"], "pin": sdl3["pin"], "sha256": "a" * 64}]}
        closure = exportable_closure(good, "unit")
        self.assertIsNone(closure["source_root"])
        forged = {**good, "external_inputs": [{**good["external_inputs"][0], "path": "@external/sdl3/" + "0" * 40 + "/SDL.h", "pin": "0" * 40}]}
        with self.assertRaises(SystemExit):
            exportable_closure(forged, "unit")
        with self.assertRaises(SystemExit):
            exportable_closure({**good, "public_include_dirs": ["/usr/include/freetype2"]}, "unit")

    def test_export_refuses_a_closure_without_its_configure(self):
        from vellum_closure import exportable_closure
        manifest = canvas_manifest(gpu_configure())
        exportable_closure(manifest, "canvas")
        with self.assertRaises(SystemExit):
            exportable_closure({k: v for k, v in manifest.items() if k != "configure"}, "canvas")

    def test_export_refuses_a_patch_replayed_onto_a_modified_base(self):
        from vellum_closure import exportable_closure
        manifest = {**canvas_manifest(gpu_configure()), "external_inputs": [
            {"path": "@external/choc/x", "patched": True, "base_modification": "e" * 64}]}
        with self.assertRaises(SystemExit) as raised:
            exportable_closure(manifest, "canvas")
        self.assertIn("modified base checkout", str(raised.exception))

    def test_canvas_closure_from_cpu_only_configure_is_refused(self):
        from vellum_closure import exportable_closure
        cpu = canvas_manifest(gpu_configure(gpu=False, skia=False, text_shaping=False))
        with self.assertRaises(SystemExit):
            exportable_closure(cpu, "canvas")
        labelled = exportable_closure(cpu, "canvas", cpu_only_units=["canvas"])
        self.assertEqual(labelled["configure"]["label"], "cpu-only")
        self.assertNotIn("label", exportable_closure(canvas_manifest(gpu_configure()), "canvas")["configure"])


class ConfigureRecordTests(unittest.TestCase):
    def test_record_carries_options_and_identity_without_host_paths(self):
        from vellum_closure import configure_record
        with tempfile.TemporaryDirectory() as td:
            build = Path(td)
            (build / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=Release\nPULP_ENABLE_GPU:BOOL=ON\nPULP_TEXT_SHAPING:BOOL=ON\n"
                "PULP_HAS_SKIA:INTERNAL=TRUE\nPULP_SHARED_DIR:STRING=/Users/agent/cache\nPULP_SOURCE:PATH=/Users/agent/pulp\n"
                "OTHER_OPTION:BOOL=ON\n", encoding="utf-8")
            (build / "CMakeFiles/4.0").mkdir(parents=True)
            (build / "CMakeFiles/4.0/CMakeSystem.cmake").write_text(
                'set(CMAKE_SYSTEM_NAME "Linux")\nset(CMAKE_SYSTEM_PROCESSOR "x86_64")\n', encoding="utf-8")
            record = configure_record(build)
        self.assertEqual(record["identity"], {"gpu": True, "skia": True, "text_shaping": True, "build_type": "Release",
                                              "system": "Linux", "processor": "x86_64"})
        self.assertEqual(record["options"]["PULP_SHARED_DIR"], "@host-path")
        self.assertEqual((record["options"]["CMAKE_SYSTEM_NAME"], record["options"]["CMAKE_SYSTEM_PROCESSOR"]), ("Linux", "x86_64"))
        self.assertNotIn("PULP_SOURCE", record["options"])
        self.assertNotIn("OTHER_OPTION", record["options"])
        self.assertEqual(receipt_path_violations({"configure": record}), [])

    def test_build_type_must_agree_with_options_and_codemodel(self):
        from vellum_closure import configure_violations, configure_options_sha256
        manifest = {**canvas_manifest(gpu_configure()), "configuration": "Release"}
        self.assertEqual(configure_violations(manifest), [])
        self.assertTrue(configure_violations({**manifest, "configuration": "Debug"}))
        debug = gpu_configure()
        debug["options"]["CMAKE_BUILD_TYPE"] = "Debug"
        debug["options_sha256"] = configure_options_sha256(debug["options"])
        self.assertTrue(any("CMAKE_BUILD_TYPE" in v for v in configure_violations({**manifest, "configure": debug})))
        claimed = gpu_configure()
        claimed["identity"] = {**claimed["identity"], "system": "Linux", "processor": "x86_64"}
        found = configure_violations({**manifest, "configure": claimed})
        self.assertTrue(any("CMAKE_SYSTEM_NAME" in v for v in found) and any("CMAKE_SYSTEM_PROCESSOR" in v for v in found))

    def test_record_must_match_the_build_tree_it_names(self):
        from vellum_closure import configure_record, configure_violations
        with tempfile.TemporaryDirectory() as td:
            build = Path(td)
            (build / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=Release\nPULP_ENABLE_GPU:BOOL=ON\nPULP_TEXT_SHAPING:BOOL=ON\nPULP_HAS_SKIA:INTERNAL=TRUE\n", encoding="utf-8")
            (build / "CMakeFiles/4.0").mkdir(parents=True)
            (build / "CMakeFiles/4.0/CMakeSystem.cmake").write_text(
                'set(CMAKE_SYSTEM_NAME "Darwin")\nset(CMAKE_SYSTEM_PROCESSOR "arm64")\n', encoding="utf-8")
            record = configure_record(build)
            manifest = {**canvas_manifest(record), "build_dir": str(build), "configuration": "Release"}
            self.assertEqual(configure_violations(manifest), [])
            claimed_linux = {**record, "identity": {**record["identity"], "system": "Linux"}}
            self.assertTrue(any("build tree" in v for v in configure_violations({**manifest, "configure": claimed_linux})))

    def test_record_must_agree_with_its_options_and_targets(self):
        from vellum_closure import configure_violations
        manifest = canvas_manifest(gpu_configure())
        self.assertEqual(configure_violations(manifest), [])
        tampered = gpu_configure()
        tampered["options"]["PULP_ENABLE_GPU"] = "OFF"
        self.assertTrue(configure_violations({**manifest, "configure": tampered}))
        cpu_defines = {**manifest, "targets": [{**manifest["targets"][0], "compile_definitions": []}]}
        self.assertTrue(any("Skia identity" in v for v in configure_violations(cpu_defines)))


class ProducerAttributionTests(unittest.TestCase):
    SDL_REVISION = "@build/_deps/sdl3-build/include-revision/SDL3/SDL_revision.h"

    def gate(self, files, generated):
        with tempfile.TemporaryDirectory() as td:
            manifest = Path(td) / "manifest.json"
            manifest.write_text(json.dumps({"source_root": str(REPO_ROOT), "files": files, "targets": [],
                                            "undefined_symbols": [], "external_inputs": [], "generated_inputs": generated}),
                                encoding="utf-8")
            return run_gate(manifest)

    def test_planted_audio_edge_via_misattributed_producer_fails(self):
        rc, out = self.gate(["tools/cmake/PulpInstallRules.cmake"], [
            {"path": self.SDL_REVISION, "sha256": "a" * 64, "producer_paths": ["tools/cmake/PulpInstallRules.cmake"]}])
        self.assertEqual(rc, 1)
        self.assertIn("forbidden target reference tools/cmake/PulpInstallRules.cmake", out)
        self.assertIn(f"generated input attributed to a non-producer {self.SDL_REVISION}", out)

    def test_generated_input_must_name_its_real_generator(self):
        rc, out = self.gate(["core/view/cmake/PulpViewJsPreludes.cmake"], [
            {"path": "@build/core/runtime/generated/Release/pulp/runtime/build_info.hpp", "sha256": "a" * 64,
             "producer_paths": ["core/view/cmake/PulpViewJsPreludes.cmake"]}])
        self.assertEqual(rc, 1)
        self.assertIn("attributed to a non-producer", out)

    def test_producer_without_its_generated_output_fails(self):
        rc, out = self.gate(["core/view/cmake/PulpViewJsPreludes.cmake", "core/view/js/embed_js.py"], [])
        self.assertEqual(rc, 1)
        self.assertIn("producer in closure without its generated input core/view/cmake/PulpViewJsPreludes.cmake", out)

    def test_registry_names_real_generators(self):
        from vellum_closure import generated_producers, generated_dependency
        self.assertIsNone(generated_producers(self.SDL_REVISION, REPO_ROOT))
        self.assertEqual(generated_dependency(self.SDL_REVISION, ExternalResolver(REPO_ROOT, None))["name"], "sdl3")
        fonts = generated_producers("@build/core/canvas/pulp-bundled-fonts_data.hpp", REPO_ROOT)
        self.assertIn("tools/cmake/PulpEmbedData.cmake", fonts)
        self.assertIn("external/fonts/Inter-Regular.ttf", fonts)
        self.assertNotIn("core/canvas/src/bundled_fonts.cpp", fonts)
        per_font = generated_producers("@build/core/canvas/pulp-bundled-fonts_data_Inter_Regular_ttf.cpp", REPO_ROOT)
        self.assertEqual(per_font, fonts)
        emoji = generated_producers("@build/core/canvas/pulp-bundled-noto-color-emoji_data.hpp", REPO_ROOT)
        self.assertIn("external/fonts/NotoColorEmoji.ttf", emoji)
        self.assertIsNone(generated_producers("@build/core/canvas/pulp-bundled-unknown_data.hpp", REPO_ROOT))

    def test_dependency_generated_input_names_its_dependency(self):
        rc, out = self.gate([], [{"path": self.SDL_REVISION, "sha256": "a" * 64, "producer_paths": [],
                                  "producer_dependency": {"name": "sdl3", "source": "https://github.com/libsdl-org/SDL.git", "pin": "5ac37a8ffcf89da390404c1016833d56e2d67ae4"}}])
        self.assertEqual(rc, 0, out)
        rc, out = self.gate([], [{"path": self.SDL_REVISION, "sha256": "a" * 64, "producer_paths": []}])
        self.assertIn("no registered generator", out)


class MirroredLinkTests(unittest.TestCase):
    UNITS = REPO_ROOT / "tools/scripts/vellum_d3_units.json"

    def test_units_carry_their_framework_and_data_targets(self):
        from vellum_closure import unit_manifest_targets
        self.assertEqual(sorted(unit_manifest_targets(self.UNITS, "foundation")),
                         ["pulp-events", "pulp-platform", "pulp-runtime", "pulp-simd"])
        self.assertEqual(unit_manifest_targets(self.UNITS, "canvas"),
                         ["pulp-canvas", "pulp-bundled-fonts", "?pulp-bundled-noto-color-emoji"])
        self.assertEqual(unit_manifest_targets(self.UNITS, "gpu-device-api"),
                         ["pulp-gpu-device", "?pulp-skia-chromium-raw-ptr-compat"])
        self.assertNotIn("pulp-platform", unit_manifest_targets(self.UNITS, "view-platform"))

    def test_allowlist_has_no_target_wide_debt_kind(self):
        entries = load_allowlist(REPO_ROOT / "tools/scripts/vellum_closure_allowlist.json")
        self.assertTrue(all(e["kind"] != "unmirrored" for e in entries))
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "a.json"
            path.write_text(json.dumps({"schema": "pulp.vellum.closure.allowlist.v1", "entries": [
                {"id": "x", "owner": "o", "debt_id": "d", "kind": "unmirrored", "match": "pulp-x", "exit_criterion": "e"}]}),
                encoding="utf-8")
            with self.assertRaises(SystemExit):
                load_allowlist(path)

    def test_gate_rejects_link_to_unmirrored_pulp_target(self):
        from vellum_closure import unmirrored_link_violations
        data = {"targets": [{"name": "pulp-canvas", "link_targets": ["pulp-platform", "pulp-runtime", "SkiaLib"]}]}
        self.assertEqual(unmirrored_link_violations(data, {"pulp-canvas", "pulp-platform", "pulp-runtime"}, None, set()), [])
        found = unmirrored_link_violations(data, {"pulp-canvas", "pulp-runtime"}, None, set())
        self.assertEqual(found, ["unmirrored Pulp target pulp-canvas -> pulp-platform"])

    def test_only_the_exact_edge_admits_an_unmirrored_link(self):
        from vellum_closure import unmirrored_link_violations
        edges = [{"id": "e", "owner": "carve-p4", "debt_id": "d", "kind": "edge", "match": "pulp-view-native->pulp-view-audio", "exit_criterion": "x"},
                 {"id": "t", "owner": "carve-p4", "debt_id": "d", "kind": "edge", "match": "pulp-view-native->pulp-timebase", "exit_criterion": "x"}]
        mirrored = {"pulp-view-native", "pulp-canvas"}
        native = {"targets": [{"name": "pulp-view-native", "link_targets": ["pulp-view-audio", "pulp-timebase"]}]}
        self.assertEqual(unmirrored_link_violations(native, mirrored, edges, set()), [])
        riding = {"targets": [{"name": "pulp-view-native", "link_targets": ["pulp-view-audio", "pulp-newthing"]}]}
        self.assertEqual(unmirrored_link_violations(riding, mirrored, edges, set()), ["unmirrored Pulp target pulp-view-native -> pulp-newthing"])
        planted = {"targets": [{"name": "pulp-canvas", "link_targets": ["pulp-timebase"]}]}
        self.assertEqual(unmirrored_link_violations(planted, mirrored, edges, set()), ["unmirrored Pulp target pulp-canvas -> pulp-timebase"])

    def test_approved_view_audio_edges_are_exact_and_scoped(self):
        entries = {e["id"]: e for e in load_allowlist(REPO_ROOT / "tools/scripts/vellum_closure_allowlist.json")}
        for name in ("pulp-timebase", "pulp-sample-bank-manifest"):
            entry = next(e for e in entries.values() if e["match"] == f"pulp-view-native->{name}")
            self.assertEqual((entry["kind"], entry["owner"], entry["approver"]), ("edge", "carve-p4", "team-lead 2026-10-13"))
            self.assertTrue(entry["platforms"])

    def test_raw_ptr_compat_is_a_linux_only_target_below_every_skia_consumer(self):
        from vellum_closure import unit_layer_violations, unit_manifest_layout, unmirrored_link_violations
        layout = unit_manifest_layout(self.UNITS)
        data = {"targets": [{"name": "pulp-gpu-device", "link_targets": ["pulp-skia-chromium-raw-ptr-compat"]},
                            {"name": "pulp-canvas", "link_targets": ["pulp-skia-chromium-raw-ptr-compat"]},
                            {"name": "pulp-skia-chromium-raw-ptr-compat", "link_targets": []}]}
        linux = {**data, "configure": gpu_configure(system="Linux", processor="x86_64")}
        mirrored, found = unit_layer_violations(linux, layout)
        self.assertEqual(found, [])
        self.assertEqual(unmirrored_link_violations(linux, mirrored, None, set()), [])
        darwin = {**data, "configure": gpu_configure()}
        mirrored, found = unit_layer_violations(darwin, layout)
        self.assertTrue(any("only on Linux" in v for v in found))
        self.assertTrue(unmirrored_link_violations(darwin, mirrored, None, set()))
        forged = {"configure": linux["configure"], "targets": [{"name": "pulp-canvas", "link_targets": ["pulp-skia-evil"]}]}
        mirrored, _found = unit_layer_violations(forged, layout)
        self.assertTrue(unmirrored_link_violations(forged, mirrored, None, set()))

    def test_raw_ptr_compat_source_is_owned_by_exactly_one_unit(self):
        from vellum_closure import export_owned_file_violations, unit_manifest_layout
        source = "core/canvas/src/skia_chromium_raw_ptr_compat.cpp"
        owners = [u["name"] for u in unit_manifest_layout(self.UNITS) if source in u["owned_files"]]
        self.assertEqual(owners, ["gpu-device-api"])
        self.assertEqual(export_owned_file_violations([{"files": [source]}], ["gpu-device-api"]), [])
        self.assertEqual(export_owned_file_violations([{"files": [source]}], ["canvas"]),
                         [f"{source} belongs to D3 unit gpu-device-api, not canvas"])
        with tempfile.TemporaryDirectory() as td:
            twice = Path(td) / "units.json"
            twice.write_text(json.dumps({"schema": "pulp.vellum.d3-units.v1", "units": [
                {"name": "a", "targets": ["pulp-a"], "owned_files": [source]},
                {"name": "b", "targets": ["pulp-b"], "owned_files": [source]}]}), encoding="utf-8")
            with self.assertRaises(SystemExit):
                unit_manifest_layout(twice)

    def test_no_skia_consumer_sits_below_the_skia_unit(self):
        from vellum_closure import unit_layer_violations, unit_manifest_layout
        layout = unit_manifest_layout(self.UNITS)
        real = {"configure": gpu_configure(), "targets": [
            {"name": "pulp-gpu-device", "compile_definitions": ["PULP_HAS_SKIA=1", "SK_DAWN"], "link_targets": ["pulp-runtime"]},
            {"name": "pulp-runtime", "compile_definitions": [], "link_targets": []}]}
        self.assertEqual(unit_layer_violations(real, layout)[1], [])
        planted = {"configure": gpu_configure(), "targets": [
            {"name": "pulp-runtime", "compile_definitions": ["PULP_HAS_SKIA=1"], "link_targets": []}]}
        self.assertTrue(any("Skia consumer pulp-runtime" in v for v in unit_layer_violations(planted, layout)[1]))

    def test_links_follow_the_unit_order(self):
        from vellum_closure import unit_layer_violations, unit_manifest_layout
        layout = unit_manifest_layout(self.UNITS)
        names = [u["name"] for u in layout]
        self.assertLess(names.index("view-core"), names.index("design-ir"))
        self.assertLess(names.index("view-script"), names.index("view-platform"))
        real = {"configure": gpu_configure(), "targets": [
            {"name": "pulp-design-import", "link_targets": ["pulp-view-core", "pulp-canvas", "pulp-runtime"]},
            {"name": "pulp-view-native", "link_targets": ["pulp-view-script", "pulp-design-import"]},
            {"name": "pulp-view-core", "link_targets": ["pulp-canvas"]}, {"name": "pulp-canvas", "link_targets": ["pulp-runtime"]}]}
        self.assertEqual(unit_layer_violations(real, layout)[1], [])
        upward = {"configure": gpu_configure(), "targets": [{"name": "pulp-runtime", "link_targets": ["pulp-bundled-fonts"]}]}
        self.assertEqual(unit_layer_violations(upward, layout)[1], ["upward D3 link pulp-runtime (foundation) -> pulp-bundled-fonts (canvas)"])

    def test_a_link_cycle_fails_even_inside_one_unit(self):
        from vellum_closure import unit_layer_violations, unit_manifest_layout
        layout = unit_manifest_layout(self.UNITS)
        cycle = {"configure": gpu_configure(), "targets": [{"name": "pulp-canvas", "link_targets": ["pulp-bundled-fonts"]},
                                                           {"name": "pulp-bundled-fonts", "link_targets": ["pulp-canvas"]}]}
        found = unit_layer_violations(cycle, layout)[1]
        self.assertTrue(any(v.startswith("D3 link cycle") for v in found), found)
        self.assertFalse(any(v.startswith("upward") for v in found))

    def test_export_rejects_closures_that_link_an_unexported_pulp_target(self):
        from vellum_closure import export_unmirrored_links
        canvas = canvas_manifest(gpu_configure())
        foundation = {"targets": [{"name": "pulp-runtime"}, {"name": "pulp-simd"}], "transitive_link_targets": ["pulp-platform"]}
        self.assertEqual(export_unmirrored_links([foundation, canvas]), ["pulp-platform"])
        foundation["targets"].append({"name": "pulp-platform"})
        self.assertEqual(export_unmirrored_links([foundation, canvas]), [])


class PlatformScopedAllowlistTests(unittest.TestCase):
    def gate(self, system, link="pulp-midi"):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps({
                "source_root": str(root), "files": [], "undefined_symbols": [], "external_inputs": [],
                "targets": [{"name": "pulp-view-native", "link_targets": [link] if link else []}],
                "configure": gpu_configure(system=system),
            }), encoding="utf-8")
            allowlist = root / "allowlist.json"
            allowlist.write_text(json.dumps({"schema": "pulp.vellum.closure.allowlist.v1", "entries": [
                {"id": "linux-only", "owner": "test", "debt_id": "t", "kind": "edge", "match": "pulp-view-native->pulp-midi",
                 "platforms": ["Linux"], "exit_criterion": "test"}]}), encoding="utf-8")
            return run_gate(manifest, allowlist)

    def test_platform_scoped_entry_matches_only_on_its_platform(self):
        rc, out = self.gate("Linux")
        self.assertEqual(rc, 0, out)
        rc, out = self.gate("Darwin")
        self.assertEqual(rc, 1)
        self.assertIn("forbidden link pulp-view-native -> pulp-midi", out)

    def test_platform_scoped_entry_is_stale_on_its_platform_when_unused(self):
        rc, out = self.gate("Linux", link=None)
        self.assertEqual(rc, 1)
        self.assertIn("allowlist entry unused: linux-only", out)
        rc, out = self.gate("Darwin", link=None)
        self.assertEqual(rc, 0, out)


class GateScopeTests(unittest.TestCase):
    def test_cmake_closure_requires_the_unit_manifest(self):
        with tempfile.TemporaryDirectory() as td:
            manifest = Path(td) / "manifest.json"
            manifest.write_text(json.dumps({"schema": "pulp.vellum.closure.v1", "source_root": td, "files": [], "external_inputs": [],
                                            "targets": [{"name": "pulp-runtime", "link_targets": []}]}), encoding="utf-8")
            rc, out = run_gate(manifest)
            self.assertEqual(rc, 1)
            self.assertIn("pass --unit-manifest", out)


class ClosureMentionTests(unittest.TestCase):
    """A closure file naming a Pulp-owned subsystem fails before export, comments included."""

    def scan(self, text, name="tools/cmake/PulpX.cmake"):
        from vellum_closure import closure_mention_violations
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / name
            path.parent.mkdir(parents=True)
            path.write_text(text, encoding="utf-8")
            return closure_mention_violations({"source_root": td, "files": [name]})

    def test_comment_mention_of_a_product_target_fails(self):
        found = self.scan("# PulpUtils fails fast unless `pulp::format` already exists.\n")
        self.assertEqual(found, ["mirror-product-symbol mention tools/cmake/PulpX.cmake:1: pulp::format"])
        self.assertTrue(self.scan("// validated against pulp::signal::Fft\n", "core/render/x.hpp"))
        self.assertTrue(self.scan('#include <pulp/audio/buffer.hpp>\n', "core/canvas/x.cpp"))
        self.assertTrue(self.scan("target_link_libraries(x PRIVATE pulp-audio)\n"))
        self.assertTrue(self.scan('LINK_FLAGS "-framework CoreAudio"\n'))
        self.assertEqual(self.scan("# the plug-in adapter targets\n"), [])

    def test_files_without_a_code_suffix_are_scanned(self):
        for name in ("tools/run", "docs/notes.md", "tools/x.sh", "tools/x.ps1", "ui/x.ts", "ui/x.tsx",
                     "ci/x.yaml", "ci/x.yml", "core/x/CMakeLists.txt"):
            self.assertTrue(self.scan("uses pulp::audio here\n", name), name)

    def test_every_source_suffix_main_scanned_is_still_scanned(self):
        for name in ("core/x/a.hh", "core/x/a.hxx", "core/x/a.hpp.in", "tools/x.py", "ui/x.mjs"):
            self.assertEqual(self.scan("uses pulp::audio here\n", name),
                             [f"mirror-product-symbol mention {name}:1: pulp::audio"], name)

    def test_an_unclassified_suffix_is_refused(self):
        found = self.scan("int x;\n", "core/x/a.weird")
        self.assertEqual(found, ["mirror-unclassified-suffix core/x/a.weird: .weird is neither scanned nor declared binary"])
        self.assertEqual(self.scan("binary", "external/fonts/Inter.ttf"), [])

    def test_a_scanned_file_that_is_not_utf8_is_still_scanned(self):
        from vellum_closure import closure_mention_violations
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "core/x/a.cpp"
            path.parent.mkdir(parents=True)
            path.write_bytes(b"\xff\xfe // pulp::audio\n")
            self.assertEqual(closure_mention_violations({"source_root": td, "files": ["core/x/a.cpp"]}),
                             ["mirror-product-symbol mention core/x/a.cpp:1: pulp::audio"])

    def test_pulp_kept_includes_and_files_are_refused(self):
        self.assertTrue(any(v.startswith("mirror-pulp-kept-include") for v in
                            self.scan('#include "pulp/view/widgets/eq_curve_view.hpp"\n', "core/view/src/x.cpp")))
        self.assertTrue(any(v.startswith("mirror-pulp-kept-file") for v in
                            self.scan("int x;\n", "core/view/audio/src/meter.cpp")))
        self.assertEqual(self.scan("int x;\n", "core/view/src/label.cpp"), [])

    def test_gate_and_vellum_share_one_pattern_table(self):
        import vellum_closure
        table = json.loads((REPO_ROOT / "tools/scripts/vellum_mirror_edge_patterns.json").read_text(encoding="utf-8"))
        self.assertEqual(table["schema"], "pulp.vellum.mirror-edge-patterns.v1")
        self.assertEqual(set(table["content_patterns"]), {"mirror-product-include", "mirror-product-symbol",
                                                          "mirror-audio-framework-link", "mirror-pulp-kept-include"})
        self.assertEqual(set(table["path_patterns"]), {"mirror-pulp-kept-file"})
        self.assertTrue({"", ".md", ".ps1", ".sh", ".ts", ".tsx", ".yaml", ".yml", ".txt",
                         ".hh", ".hxx", ".in", ".py", ".mjs"} <= set(table["suffixes"]))
        self.assertEqual(set(table["suffixes"]) & set(table["binary_suffixes"]), set())
        loaded = vellum_closure.mirror_edge_table()
        self.assertEqual({k: v.pattern for k, v in loaded["content"].items()}, table["content_patterns"])
        self.assertFalse(hasattr(vellum_closure, "CLOSURE_MENTION_PATTERNS"))

    def test_export_closure_files_are_free_of_mentions(self):
        from vellum_closure import closure_mention_violations
        for rel in ("tools/cmake/PulpEmbedData.cmake", "core/render/include/pulp/render/gpu_compute.hpp"):
            self.assertEqual(closure_mention_violations({"source_root": str(REPO_ROOT), "files": [rel]}), [], rel)

    def test_export_refuses_a_closure_with_a_mention(self):
        from vellum_closure import exportable_closure
        with tempfile.TemporaryDirectory() as td:
            (Path(td) / "x.cmake").write_text("# needs pulp::format\n", encoding="utf-8")
            manifest = {**canvas_manifest(gpu_configure()), "source_root": td, "files": ["x.cmake"]}
            with self.assertRaises(SystemExit) as raised:
                exportable_closure(manifest, "canvas")
            self.assertIn("pulp::format", str(raised.exception))


class GeneratedDigestTests(unittest.TestCase):
    BUILD_INFO = "@build/core/runtime/generated/Release/pulp/runtime/build_info.hpp"

    def build_info(self, td, stamp, dirty="false"):
        path = Path(td) / f"build_info_{stamp}_{dirty}.hpp"
        path.write_text(f'inline constexpr std::string_view kBuildIso8601 = "{stamp}";\n'
                        f'inline constexpr bool kGitDirty = {dirty};\n'
                        f'inline constexpr std::string_view kStampLabel = "0.1 (Release, abc, {stamp})";\n', encoding="utf-8")
        return path

    def test_build_info_digest_ignores_the_configure_timestamp(self):
        from vellum_closure import generated_digest
        with tempfile.TemporaryDirectory() as td:
            first = generated_digest(self.BUILD_INFO, self.build_info(td, "2026-10-13T19:05:17Z"), REPO_ROOT)
            second = generated_digest(self.BUILD_INFO, self.build_info(td, "2026-10-14T08:00:00Z"), REPO_ROOT)
            dirty = generated_digest(self.BUILD_INFO, self.build_info(td, "2026-10-14T08:00:00Z", "true"), REPO_ROOT)
        self.assertEqual(first, second)
        self.assertEqual(first["normalization"], {"build_timestamp": "<build-timestamp>"})
        self.assertNotEqual(first["sha256"], dirty["sha256"])

    def test_binary_data_digest_does_not_depend_on_the_build(self):
        from vellum_closure import generated_digest
        rel = "@build/core/canvas/pulp-bundled-fonts_data_Inter_Regular_ttf.cpp"
        with tempfile.TemporaryDirectory() as td:
            missing = generated_digest(rel, Path(td) / "never-built.cpp", REPO_ROOT)
            out = Path(td) / "built.cpp"
            subprocess.run([__import__("sys").executable, str(REPO_ROOT / "tools/cmake/scripts/encode_binary_data.py"),
                            "--src", str(REPO_ROOT / "external/fonts/Inter-Regular.ttf"), "--out", str(out),
                            "--namespace", "pulp_bundled_fonts", "--var", "Inter_Regular_ttf"], check=True)
            built = generated_digest(rel, out, REPO_ROOT)
            self.assertEqual(missing, built)
            self.assertEqual(missing["sha256"], __import__("hashlib").sha256(out.read_bytes()).hexdigest())

    def test_export_refuses_a_generated_input_without_a_reproducible_digest(self):
        from vellum_closure import exportable_closure
        manifest = {**canvas_manifest(gpu_configure()), "generated_inputs": [
            {"path": "@build/core/view/web_compat_preludes_gen.cpp", "sha256": None,
             "producer_paths": ["core/view/cmake/PulpViewJsPreludes.cmake", "core/view/js/embed_js.py"]}]}
        with self.assertRaises(SystemExit) as raised:
            exportable_closure(manifest, "canvas")
        self.assertIn("reproducible digest", str(raised.exception))


class SkiaLinkInputTests(unittest.TestCase):
    def test_skia_consumer_records_skia_as_a_link_input(self):
        from vellum_closure import external_link_violations, fetchcontent_pins
        skia = fetchcontent_pins(REPO_ROOT)["skia"]
        link = {"dependency": "skia", "source": skia["source"], "pin": skia["pin"], "asset_sha256": skia["assets"][0]}
        canvas = {"name": "pulp-canvas", "compile_definitions": ["PULP_HAS_SKIA=1"], "external_link_inputs": [link]}
        self.assertEqual(external_link_violations({"targets": [canvas]}), [])
        self.assertTrue(external_link_violations({"targets": [{**canvas, "external_link_inputs": []}]}))
        self.assertTrue(external_link_violations({"targets": [{**canvas, "external_link_inputs": [{**link, "pin": "0" * 40}]}]}))
        self.assertTrue(external_link_violations({"targets": [{"name": "pulp-runtime", "compile_definitions": [], "external_link_inputs": [link]}]}))

    def test_a_missing_link_key_does_not_skip_the_check(self):
        from vellum_closure import external_link_violations
        canvas = {"name": "pulp-canvas", "compile_definitions": ["PULP_HAS_SKIA=1"]}
        self.assertEqual(external_link_violations({"targets": [canvas]}),
                         ["pulp-canvas compiles against Skia but records no Skia link input"])
        self.assertEqual(external_link_violations({"targets": [{"name": "pulp-runtime", "compile_definitions": []}]}), [])

    def test_non_skia_and_repeated_link_inputs_are_refused(self):
        from vellum_closure import external_link_violations, fetchcontent_pins
        skia = fetchcontent_pins(REPO_ROOT)["skia"]
        link = {"dependency": "skia", "source": skia["source"], "pin": skia["pin"], "asset_sha256": skia["assets"][0]}
        canvas = {"name": "pulp-canvas", "compile_definitions": ["PULP_HAS_SKIA=1"]}
        other = {**link, "dependency": "dawn"}
        self.assertIn("pulp-canvas records a link input other than Skia",
                      external_link_violations({"targets": [{**canvas, "external_link_inputs": [link, other]}]}))
        self.assertIn("pulp-canvas records more than one Skia link input",
                      external_link_violations({"targets": [{**canvas, "external_link_inputs": [link, link]}]}))
        self.assertTrue(external_link_violations({"targets": [{**canvas, "external_link_inputs": {"skia": link}}]}))


class AllowlistGateTests(unittest.TestCase):
    def test_unused_allowlist_entry_fails_the_gate(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps({
                "source_root": str(root), "files": [], "targets": [],
                "undefined_symbols": [], "external_inputs": [],
            }), encoding="utf-8")
            allowlist = root / "allowlist.json"
            allowlist.write_text(json.dumps({
                "schema": "pulp.vellum.closure.allowlist.v1",
                "entries": [{"id": "stale", "owner": "test", "debt_id": "stale",
                             "kind": "root", "match": "core/nothing/"}],
            }), encoding="utf-8")
            args = Namespace(
                manifest=[str(manifest)], allow=[], allow_include_path=[], allow_include_edge=[],
                allow_include_prefix=[], allow_root_prefix=[], report_only=False,
                allowlist=str(allowlist), generate=False, unit_manifest=None, target=None,
                source_root=None, build_dir=None,
            )
            self.assertEqual(cmd_gate(args), 1)


if __name__ == "__main__":
    unittest.main()
