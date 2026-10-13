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
        pin = self.pins["choc"]["pin"]
        patch = "b" * 64
        row = {"path": f"@external/choc/{pin}+patch-{patch[:16]}/choc/text/choc_JSON.h", "source": self.pins["choc"]["source"],
               "pin": pin, "patched": True, "patch": patch, "sha256": "c" * 64}
        self.assertEqual(self.check(row), [])
        self.assertTrue(self.check({k: v for k, v in row.items() if k != "sha256"}))
        self.assertTrue(any("path pin" in v for v in self.check({**row, "path": f"@external/choc/{pin}/choc/text/choc_JSON.h"})))

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

    def test_dirty_checkout_is_labelled_patched_never_clean(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td).resolve()
            checkout, build, head = self.make_checkout(root)
            (checkout / "include/a.h").write_text("int patched;\n", encoding="utf-8")
            row = self.resolver(root, build, head).identify(str(checkout / "include/a.h"))
            self.assertTrue(row.get("patched"), row)
            self.assertRegex(row["patch"], r"^[0-9a-f]{64}$")
            self.assertEqual(row["path"], f"@external/dep/{head}+patch-{row['patch'][:16]}/include/a.h")
            self.assertEqual(row["sha256"], __import__("hashlib").sha256(b"int patched;\n").hexdigest())

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

    def test_pulp_patched_build_copy_names_its_patch_set(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td).resolve()
            checkout, build, head = self.make_checkout(root)
            copy = build / "_deps/pulp-mutable-dep-src"
            (copy / "include").mkdir(parents=True)
            (copy / "include/a.h").write_text("int a_patched;\n", encoding="utf-8")
            (copy / ".pulp-source-key").write_text(f"{checkout}\n{'d' * 64}\n", encoding="utf-8")
            row = self.resolver(root, build, head).identify(str(copy / "include/a.h"))
            self.assertTrue(row.get("patched"), row)
            self.assertEqual(row["pin"], head)
            self.assertTrue(row["path"].startswith(f"@external/dep/{head}+patch-"))
            (copy / ".pulp-source-key").write_text(f"{checkout}\nnot-a-key\n", encoding="utf-8")
            unknown = self.resolver(root, build, head).identify(str(copy / "include/a.h"))
            self.assertEqual(unknown["pin"], "unresolved")

    def test_assume_unchanged_edit_is_not_recorded_clean(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td).resolve()
            checkout, build, head = self.make_checkout(root)
            (checkout / "include/a.h").write_text("int hidden;\n", encoding="utf-8")
            git(checkout, "update-index", "--assume-unchanged", "include/a.h")
            self.assertEqual(git(checkout, "diff", "HEAD"), "")
            row = self.resolver(root, build, head).identify(str(checkout / "include/a.h"))
            self.assertEqual(row["pin"], "unresolved")

    def test_edit_to_patched_build_copy_changes_patch_identity(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td).resolve()
            checkout, build, head = self.make_checkout(root)
            copy = build / "_deps/pulp-mutable-dep-src"
            (copy / "include").mkdir(parents=True)
            (copy / "include/a.h").write_text("int a_patched;\n", encoding="utf-8")
            (copy / "include/b.h").write_text("int b;\n", encoding="utf-8")
            (copy / ".pulp-source-key").write_text(f"{checkout}\n{'d' * 64}\n", encoding="utf-8")
            before = self.resolver(root, build, head).identify(str(copy / "include/a.h"))
            (copy / "include/b.h").write_text("int b_edited;\n", encoding="utf-8")
            after = self.resolver(root, build, head).identify(str(copy / "include/a.h"))
            self.assertNotEqual(before["patch"], after["patch"])
            self.assertEqual(before["sha256"], after["sha256"])

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


class ExportProvenanceTests(unittest.TestCase):
    def test_export_refuses_a_closure_that_fails_provenance(self):
        from vellum_closure import exportable_closure, fetchcontent_pins
        sdl3 = fetchcontent_pins(REPO_ROOT)["sdl3"]
        good = {"source_root": str(REPO_ROOT), "build_dir": None, "external_inputs": [
            {"path": f"@external/sdl3/{sdl3['pin']}/include/SDL3/SDL.h", "source": sdl3["source"], "pin": sdl3["pin"], "sha256": "a" * 64}]}
        closure = exportable_closure(good, "unit")
        self.assertIsNone(closure["source_root"])
        forged = {**good, "external_inputs": [{**good["external_inputs"][0], "path": "@external/sdl3/" + "0" * 40 + "/SDL.h", "pin": "0" * 40}]}
        with self.assertRaises(SystemExit):
            exportable_closure(forged, "unit")
        with self.assertRaises(SystemExit):
            exportable_closure({**good, "public_include_dirs": ["/usr/include/freetype2"]}, "unit")


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
