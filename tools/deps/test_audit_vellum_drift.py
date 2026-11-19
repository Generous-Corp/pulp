#!/usr/bin/env python3
"""Tests for the renderer-lock agreement check in tools/deps/audit.py."""

from __future__ import annotations

import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]

sys.path.insert(0, str(ROOT / "tools" / "deps"))
import audit  # noqa: E402


PULP_SHA = "a" * 64
OTHER_SHA = "b" * 64
COMMIT = "c" * 40


def manifest(tag: str = "chrome/m158", sha: str = PULP_SHA, **spec_overrides) -> list[dict]:
    spec = {
        "temporary": True,
        "repository": "https://github.com/Generous-Corp/vellum",
        "commit": COMMIT,
        "path": "cmake/VellumSkiaDawn.cmake",
        "pulp_release_asset": "mac-universal",
    }
    spec.update(spec_overrides)
    return [{
        "name": "Skia",
        "version": tag,
        audit.VELLUM_DRIFT_FIELD: spec,
        "determinism": {"release_assets": {"mac-universal": {"url": "u", "sha256": sha}}},
    }]


def vellum_module(sha: str = PULP_SHA, tag: str = "chrome/m158") -> str:
    # The shape Vellum's cmake/VellumSkiaDawn.cmake uses: the digest on the line
    # after set(, and the release named inside a wrapped FATAL_ERROR message.
    return (
        "set(_vellum_expected_archive_sha256\n"
        f'    "{sha}")\n'
        "set(_vellum_expected_skia_sha256\n"
        f'    "{"d" * 64}")\n'
        "    message(FATAL_ERROR\n"
        '        "Vellum: extracted Skia/Dawn libraries do not match the locked "\n'
        f'        "{tag} macOS universal tuple")\n'
    )


def reader(text: str):
    calls = []

    def read(repository: str, commit: str, path: str) -> str:
        calls.append((repository, commit, path))
        return text

    read.calls = calls
    return read


def renderer_lock(sha: str = PULP_SHA, tag: str = "chrome/m158") -> str:
    import json
    return json.dumps({"schema": "gpu-runtime.renderer-lock.v1", "milestone": tag,
                       "platforms": {"macos-arm64": {"sha256": sha, "archives": {}}}})


def lock_manifest(runtime_commit: str | None = COMMIT, **overrides) -> list[dict]:
    spec = {"path": "gpu-runtime/renderer-lock.json", "lock_platform": "macos-arm64"}
    spec.update(overrides)
    entries = manifest(**spec)
    if runtime_commit is not None:
        entries.append({"name": audit.GPU_RUNTIME_ENTRY,
                        "upstream": {"kind": "git-commit", "ref": runtime_commit,
                                     "commit": runtime_commit}})
    return entries


class RendererLockTests(unittest.TestCase):
    def test_the_shared_lock_matches(self) -> None:
        read = reader(renderer_lock())
        self.assertEqual(audit.vellum_renderer_drift(lock_manifest(), read), ("match", []))
        self.assertEqual(read.calls[0][2], "gpu-runtime/renderer-lock.json")

    def test_a_lock_digest_that_differs_fails(self) -> None:
        status, problems = audit.vellum_renderer_drift(lock_manifest(),
                                                       reader(renderer_lock(sha=OTHER_SHA)))
        self.assertEqual(status, "mismatch")
        self.assertIn(f"Vellum {OTHER_SHA}", problems[0])

    def test_a_lock_milestone_that_differs_fails(self) -> None:
        status, problems = audit.vellum_renderer_drift(lock_manifest(),
                                                       reader(renderer_lock(tag="chrome/m159")))
        self.assertEqual(status, "mismatch")
        self.assertIn("chrome/m159", problems[0])

    def test_the_lock_must_be_read_at_the_pinned_runtime_commit(self) -> None:
        status, problems = audit.vellum_renderer_drift(lock_manifest(runtime_commit="d" * 40),
                                                       reader(renderer_lock()))
        self.assertEqual(status, "mismatch")
        self.assertIn("must be the same commit", problems[0])

    def test_every_mac_selector_must_resolve_the_locked_asset(self) -> None:
        entries = lock_manifest()
        assets = entries[0]["determinism"]["release_assets"]
        assets["mac-arm64"] = {"url": "u", "sha256": PULP_SHA}
        assets["mac-x86_64"] = {"url": "u", "sha256": PULP_SHA}
        assets["linux-x64"] = {"url": "u", "sha256": OTHER_SHA}
        self.assertEqual(audit.vellum_renderer_drift(entries, reader(renderer_lock())),
                         ("match", []))
        assets["mac-x86_64"] = {"url": "u", "sha256": OTHER_SHA}
        status, problems = audit.vellum_renderer_drift(entries, reader(renderer_lock()))
        self.assertEqual(status, "mismatch")
        self.assertEqual(len(problems), 1)
        self.assertIn("release_assets.mac-x86_64", problems[0])
        self.assertIn(OTHER_SHA, problems[0])

    def test_a_lock_without_the_platform_fails_closed(self) -> None:
        status, problems = audit.vellum_renderer_drift(
            lock_manifest(lock_platform="windows-x64"), reader(renderer_lock()))
        self.assertEqual(status, "unavailable")


class GpuRuntimePinTests(unittest.TestCase):
    """The strict audit asserts the CMake fetches and version-checks from pulp.lock."""

    GPU = "tools/cmake/PulpGpuRuntime.cmake"
    LOCK = "tools/cmake/PulpVellumLock.cmake"

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / "tools/cmake").mkdir(parents=True)
        for rel in (self.GPU, self.LOCK):
            (self.root / rel).write_text((ROOT / rel).read_text(encoding="utf-8"), encoding="utf-8")

    def edit(self, rel: str, old: str, new: str) -> None:
        path = self.root / rel
        text = path.read_text(encoding="utf-8")
        self.assertIn(old, text)
        path.write_text(text.replace(old, new, 1), encoding="utf-8")

    def entry(self) -> list[dict]:
        return [{"name": audit.GPU_RUNTIME_ENTRY, "version": "0.1.10 (cccccccc)",
                 "upstream": {"kind": "git-commit", "ref": COMMIT, "commit": COMMIT},
                 "source_files": [self.GPU]}]

    def problems(self) -> list[str]:
        return audit.gpu_runtime_pin_agreement(self.entry(), self.root)

    def assertFlags(self, fragment: str) -> None:
        found = self.problems()
        self.assertTrue(any(fragment in problem for problem in found), found)

    def test_the_copied_tree_passes(self) -> None:
        self.assertEqual(self.problems(), [])

    def test_the_real_tree_passes(self) -> None:
        import json
        manifest = json.loads((ROOT / "tools/deps/manifest.json").read_text(encoding="utf-8"))["dependencies"]
        self.assertEqual(audit.gpu_runtime_pin_agreement(manifest), [])

    def test_a_literal_git_tag_fails(self) -> None:
        self.edit(self.GPU, "GIT_TAG ${PULP_GPU_RUNTIME_COMMIT}", f"GIT_TAG {COMMIT}")
        self.assertFlags("GIT_TAG must be exactly ${PULP_GPU_RUNTIME_COMMIT}")

    def test_a_missing_git_tag_fails(self) -> None:
        self.edit(self.GPU, "GIT_TAG ${PULP_GPU_RUNTIME_COMMIT}", "")
        self.assertFlags("no GIT_TAG")

    def test_git_tag_survives_reordered_arguments_and_whitespace(self) -> None:
        self.edit(self.GPU, "GIT_TAG ${PULP_GPU_RUNTIME_COMMIT}\n        SOURCE_SUBDIR gpu-runtime",
                  "SOURCE_SUBDIR   gpu-runtime\n\n   GIT_TAG    ${PULP_GPU_RUNTIME_COMMIT}")
        self.assertEqual(self.problems(), [])

    def test_a_commented_out_git_tag_does_not_count(self) -> None:
        self.edit(self.GPU, "GIT_TAG ${PULP_GPU_RUNTIME_COMMIT}",
                  f"GIT_TAG {COMMIT} # GIT_TAG ${{PULP_GPU_RUNTIME_COMMIT}}")
        self.assertFlags("GIT_TAG must be exactly")

    def test_bracket_comment_fake_pin_does_not_count(self) -> None:
        (self.root / self.GPU).write_text(
            "#[=[\n"
            "include(\"${CMAKE_CURRENT_LIST_DIR}/PulpVellumLock.cmake\")\n"
            "FetchContent_Declare(vellum_gpu_runtime GIT_TAG ${PULP_GPU_RUNTIME_COMMIT})\n"
            "if(NOT _pulp_gpu_runtime_version STREQUAL PULP_GPU_RUNTIME_VERSION)\n"
            "pulp_find_gpu_runtime_package(${PULP_GPU_RUNTIME_VERSION})\n"
            "]=]\n",
            encoding="utf-8")
        found = self.problems()
        self.assertTrue(found, "bracket-comment-only pin must fail closed")
        self.assertTrue(any("does not include PulpVellumLock.cmake" in item for item in found), found)

    def test_quoted_fake_pin_does_not_count(self) -> None:
        (self.root / self.GPU).write_text(
            'set(_documentation "FetchContent_Declare(vellum_gpu_runtime GIT_TAG '
            '${PULP_GPU_RUNTIME_COMMIT}) include(PulpVellumLock.cmake)")\n',
            encoding="utf-8")
        found = self.problems()
        self.assertTrue(found, "quoted fake pin must fail closed")
        self.assertTrue(any("does not include PulpVellumLock.cmake" in item for item in found), found)

    def test_a_hand_set_commit_fails(self) -> None:
        self.edit(self.GPU, 'include(FetchContent)',
                  f'include(FetchContent)\nset(PULP_GPU_RUNTIME_COMMIT "{COMMIT}")')
        self.assertFlags("sets PULP_GPU_RUNTIME_COMMIT")

    def test_a_hand_set_version_fails(self) -> None:
        self.edit(self.GPU, 'include(FetchContent)',
                  'include(FetchContent)\nset(PULP_GPU_RUNTIME_VERSION "0.1.10" CACHE STRING "")')
        self.assertFlags("sets PULP_GPU_RUNTIME_VERSION")

    def test_the_lock_reader_must_derive_the_commit_from_the_lock_sha(self) -> None:
        self.edit(self.LOCK, 'set(PULP_GPU_RUNTIME_COMMIT "${PULP_VELLUM_sha}")',
                  f'set(PULP_GPU_RUNTIME_COMMIT "{COMMIT}")')
        self.assertFlags("must set PULP_GPU_RUNTIME_COMMIT exactly once")

    def test_the_lock_reader_must_derive_the_version_from_the_lock_tag(self) -> None:
        self.edit(self.LOCK, 'set(PULP_GPU_RUNTIME_VERSION "${PULP_VELLUM_tag}")',
                  'set(PULP_GPU_RUNTIME_VERSION "0.1.10")')
        self.assertFlags("must set PULP_GPU_RUNTIME_VERSION exactly once")

    def test_a_source_version_check_that_drops_the_pinned_version_fails(self) -> None:
        self.edit(self.GPU, "_pulp_gpu_runtime_version STREQUAL PULP_GPU_RUNTIME_VERSION",
                  '_pulp_gpu_runtime_version STREQUAL "0.1.10"')
        self.assertFlags("check on the fetched source")

    def test_a_prefix_package_found_by_a_literal_version_fails(self) -> None:
        self.edit(self.GPU, "pulp_find_gpu_runtime_package(${PULP_GPU_RUNTIME_VERSION}",
                  "pulp_find_gpu_runtime_package(0.1.10")
        self.assertFlags("prefix package")

    def test_dropping_the_lock_include_fails(self) -> None:
        self.edit(self.GPU, 'include("${CMAKE_CURRENT_LIST_DIR}/PulpVellumLock.cmake")', "")
        self.assertFlags("does not include PulpVellumLock.cmake")

    def test_a_pin_set_in_another_cmake_file_fails(self) -> None:
        (self.root / "tools/cmake/Other.cmake").write_text(
            'set(PULP_GPU_RUNTIME_COMMIT "x" CACHE STRING "")\n', encoding="utf-8")
        self.assertFlags("Other.cmake sets PULP_GPU_RUNTIME_COMMIT")

    def test_a_cmake_without_the_fetch_declaration_fails(self) -> None:
        (self.root / self.GPU).write_text("# no pin\n", encoding="utf-8")
        self.assertTrue(self.problems())

    def test_a_cmake_fetch_without_a_manifest_entry_fails(self) -> None:
        problems = audit.gpu_runtime_pin_agreement([], self.root)
        self.assertTrue(problems and "has no vellum-gpu-runtime entry" in problems[0])

    def test_no_manifest_entry_and_no_fetch_passes(self) -> None:
        self.assertEqual(audit.gpu_runtime_pin_agreement([], self.root / "empty"), [])
        (self.root / self.GPU).write_text("# no pin\n", encoding="utf-8")
        self.assertEqual(audit.gpu_runtime_pin_agreement([], self.root), [])


    # Each pin assertion, with the real command removed and a fake one placed
    # where the real one would count. "bare" is the control: the fake as real
    # code must satisfy the assertion, so the hidden forms fail only because
    # they are hidden.
    REAL_DECLARE = ("    FetchContent_Declare(\n"
                    "        vellum_gpu_runtime\n"
                    "        GIT_REPOSITORY https://github.com/Generous-Corp/vellum.git\n"
                    "        GIT_TAG ${PULP_GPU_RUNTIME_COMMIT}\n"
                    "        SOURCE_SUBDIR gpu-runtime\n"
                    "    )\n")
    MAKE_AVAILABLE = "FetchContent_MakeAvailable(vellum_gpu_runtime)\n"
    PIN_CASES = {
        "include": (
            'include("${CMAKE_CURRENT_LIST_DIR}/PulpVellumLock.cmake")\n', "",
            "set(PULP_GPU_RUNTIME_PREFIX",
            'include("${CMAKE_CURRENT_LIST_DIR}/PulpVellumLock.cmake")',
            "does not include PulpVellumLock.cmake"),
        "declare": (
            REAL_DECLARE, "", "    set(GPU_RUNTIME_SKIA_DIR \"${SKIA_DIR}\" CACHE",
            "FetchContent_Declare(vellum_gpu_runtime GIT_REPOSITORY "
            "https://github.com/Generous-Corp/vellum.git GIT_TAG ${PULP_GPU_RUNTIME_COMMIT} "
            "SOURCE_SUBDIR gpu-runtime)",
            "declare FetchContent vellum_gpu_runtime exactly once"),
        "version check": (
            "if(NOT _pulp_gpu_runtime_version STREQUAL PULP_GPU_RUNTIME_VERSION)",
            'if(NOT _pulp_gpu_runtime_version STREQUAL "0.0.0")',
            "    get_directory_property(_pulp_gpu_runtime_version",
            "if(NOT _pulp_gpu_runtime_version STREQUAL PULP_GPU_RUNTIME_VERSION)\n"
            "message(FATAL_ERROR stop)\nendif()",
            "check on the fetched source"),
        "prefix package": (
            "pulp_find_gpu_runtime_package(${PULP_GPU_RUNTIME_VERSION}",
            "pulp_find_gpu_runtime_package(0.0.0",
            "    set(GPU_RUNTIME_SKIA_DIR \"${SKIA_DIR}\")",
            "pulp_find_gpu_runtime_package(${PULP_GPU_RUNTIME_VERSION} p e)",
            "prefix package"),
    }

    @staticmethod
    def hidden_forms(fake: str) -> dict[str, str | None]:
        commented = "\n".join("# " + line for line in fake.splitlines())
        escaped = fake.replace('"', '\\"')
        return {
            "bare": None,
            "line comment": commented,
            "bracket comment": f"#[[\n{fake}\n]]",
            "bracket comment with equals": f"#[==[\n{fake}\n]==]",
            "quoted argument": f'set(_pulp_doc "\n{escaped}\n")',
            "bracket argument": f"set(_pulp_doc [=[\n{fake}\n]=])",
            "constant-false branch": f"if(FALSE)\n{fake}\nendif()",
            "function body": f"function(_pulp_never_called)\n{fake}\nendfunction()",
        }

    def test_fake_pins_in_comments_strings_brackets_and_dead_code_do_not_count(self) -> None:
        original = (ROOT / self.GPU).read_text(encoding="utf-8")
        for assertion, (real, replacement, before, fake, fragment) in self.PIN_CASES.items():
            self.assertIn(real, original)
            self.assertIn(before, original)
            for form, hidden in self.hidden_forms(fake).items():
                with self.subTest(assertion=assertion, form=form):
                    text = original.replace(real, replacement, 1)
                    text = text.replace(before, f"{hidden or fake}\n{before}", 1)
                    (self.root / self.GPU).write_text(text, encoding="utf-8")
                    found = self.problems()
                    if hidden is None:
                        self.assertEqual(found, [])
                    else:
                        self.assertTrue(any(fragment in problem for problem in found), found)

    def test_a_top_level_return_before_the_include_fails(self) -> None:
        self.edit(self.GPU, 'include("${CMAKE_CURRENT_LIST_DIR}/PulpVellumLock.cmake")',
                  'return()\ninclude("${CMAKE_CURRENT_LIST_DIR}/PulpVellumLock.cmake")')
        self.assertFlags("does not include PulpVellumLock.cmake")

    def test_a_declaration_before_the_include_fails(self) -> None:
        self.edit(self.GPU, 'include("${CMAKE_CURRENT_LIST_DIR}/PulpVellumLock.cmake")\n', "")
        self.edit(self.GPU, "include(FetchContent)",
                  'include(FetchContent)\ninclude("${CMAKE_CURRENT_LIST_DIR}/PulpVellumLock.cmake")')
        self.assertFlags("does not include PulpVellumLock.cmake at top level")

    def test_a_git_tag_in_a_bracket_argument_does_not_expand(self) -> None:
        self.edit(self.GPU, "GIT_TAG ${PULP_GPU_RUNTIME_COMMIT}", "GIT_TAG [[${PULP_GPU_RUNTIME_COMMIT}]]")
        self.assertFlags("GIT_TAG must be exactly")

    def test_a_declaration_that_takes_its_source_elsewhere_fails(self) -> None:
        self.edit(self.GPU, "SOURCE_SUBDIR gpu-runtime", "SOURCE_SUBDIR gpu-runtime\n        SOURCE_DIR /x")
        self.assertFlags("takes its source from SOURCE_DIR")

    def test_a_version_check_that_does_not_stop_configure_fails(self) -> None:
        self.edit(self.GPU, "        message(FATAL_ERROR\n            \"Pulp: gpu-runtime at",
                  "        message(WARNING\n            \"Pulp: gpu-runtime at")
        self.assertFlags("check on the fetched source")

    def test_pin_writes_are_found_by_command_not_text(self) -> None:
        other = self.root / "tools/cmake/Other.cmake"
        other.write_text('set(_doc "set(PULP_GPU_RUNTIME_COMMIT x)")\n'
                         "set(_doc [[string(REPLACE a b PULP_GPU_RUNTIME_VERSION x)]])\n"
                         "# set(PULP_VELLUM_sha x)\n", encoding="utf-8")
        self.assertEqual(self.problems(), [])
        for write in ('string(REPLACE a b PULP_GPU_RUNTIME_COMMIT "${PULP_GPU_RUNTIME_COMMIT}")',
                      "unset(PULP_GPU_RUNTIME_VERSION)",
                      'set("PULP_GPU_RUNTIME_COMMIT" x)',
                      "set([[PULP_GPU_RUNTIME_VERSION]] x)",
                      "set(PULP_VELLUM_sha 0)",
                      "function(f)\nset(PULP_GPU_RUNTIME_COMMIT x PARENT_SCOPE)\nendfunction()"):
            with self.subTest(write=write):
                other.write_text(write + "\n", encoding="utf-8")
                self.assertTrue(any("Other.cmake sets" in p for p in self.problems()), write)

    def test_the_lock_reader_must_set_the_pin_after_reading_the_lock(self) -> None:
        line = 'set(PULP_GPU_RUNTIME_COMMIT "${PULP_VELLUM_sha}")\n'
        self.edit(self.LOCK, line, "")
        self.edit(self.LOCK, "foreach(_field tag sha", line + "foreach(_field tag sha")
        self.assertFlags("must set PULP_GPU_RUNTIME_COMMIT exactly once")

    def test_the_lock_reader_may_not_overwrite_a_lock_field(self) -> None:
        self.edit(self.LOCK, "unset(_pulp_vellum_lock)", "set(PULP_VELLUM_sha 0)\nunset(_pulp_vellum_lock)")
        self.assertFlags("writes PULP_VELLUM_sha")

    def test_redefining_a_relied_on_command_fails(self) -> None:
        (self.root / "tools/cmake/Other.cmake").write_text(
            "macro(FetchContent_Declare)\nendmacro()\n", encoding="utf-8")
        self.assertFlags("redefines FetchContent_Declare()")

    def test_unparseable_cmake_fails_closed(self) -> None:
        self.edit(self.GPU, "SOURCE_SUBDIR gpu-runtime", 'SOURCE_SUBDIR "gpu-runtime')
        self.assertFlags("cannot parse")


class VellumDriftTests(unittest.TestCase):
    def test_match_passes_and_reads_the_pinned_commit(self) -> None:
        read = reader(vellum_module())
        status, problems = audit.vellum_renderer_drift(manifest(), read)
        self.assertEqual((status, problems), ("match", []))
        self.assertEqual(read.calls, [("https://github.com/Generous-Corp/vellum", COMMIT,
                                       "cmake/VellumSkiaDawn.cmake")])

    def test_digest_mismatch_names_both_digests(self) -> None:
        status, problems = audit.vellum_renderer_drift(
            manifest(), reader(vellum_module(sha=OTHER_SHA)))
        self.assertEqual(status, "mismatch")
        self.assertEqual(len(problems), 1)
        self.assertIn(f"Vellum {OTHER_SHA}", problems[0])
        self.assertIn(f"Pulp {PULP_SHA}", problems[0])

    def test_release_tag_mismatch_names_both_releases(self) -> None:
        status, problems = audit.vellum_renderer_drift(
            manifest(), reader(vellum_module(tag="chrome/m157")))
        self.assertEqual(status, "mismatch")
        self.assertEqual(len(problems), 1)
        self.assertIn("Vellum chrome/m157", problems[0])
        self.assertIn("Pulp chrome/m158", problems[0])

    def test_fetch_failure_fails_closed(self) -> None:
        def unavailable(*_args):
            raise audit.VellumPinUnavailable("HTTP Error 404: Not Found")

        status, problems = audit.vellum_renderer_drift(manifest(), unavailable)
        self.assertEqual(status, "unavailable")
        self.assertIn("404", problems[0])
        self.assertIn(COMMIT, problems[0])

    def test_network_error_from_the_default_reader_fails_closed(self) -> None:
        with mock.patch.dict(os.environ, {audit.VELLUM_CHECKOUT_ENV: ""}), \
                mock.patch.object(audit.urllib.request, "urlopen",
                                  side_effect=audit.urllib.error.URLError("offline")) as urlopen:
            status, problems = audit.vellum_renderer_drift(manifest())
        self.assertEqual(status, "unavailable")
        self.assertIn("offline", problems[0])
        self.assertEqual(
            urlopen.call_args.args[0],
            f"https://raw.githubusercontent.com/Generous-Corp/vellum/{COMMIT}/cmake/VellumSkiaDawn.cmake")

    def test_unparseable_module_fails_closed(self) -> None:
        status, problems = audit.vellum_renderer_drift(manifest(), reader("project(Vellum)\n"))
        self.assertEqual(status, "unavailable")
        self.assertIn("exactly one", problems[0])

    def test_two_declared_releases_fail_closed(self) -> None:
        text = vellum_module() + '"chrome/m157 macOS universal tuple"\n'
        status, _ = audit.vellum_renderer_drift(manifest(), reader(text))
        self.assertEqual(status, "unavailable")

    def test_malformed_spec_is_refused_before_any_read(self) -> None:
        for overrides in ({"commit": "main"}, {"path": "../x.cmake"},
                          {"pulp_release_asset": "mac-arm64-missing"}):
            with self.subTest(overrides=overrides):
                read = reader(vellum_module())
                status, problems = audit.vellum_renderer_drift(manifest(**overrides), read)
                self.assertEqual(status, "unavailable")
                self.assertTrue(problems)
                self.assertEqual(read.calls, [])

    def test_absent_spec_is_not_configured(self) -> None:
        skia = manifest()
        del skia[0][audit.VELLUM_DRIFT_FIELD]
        self.assertEqual(audit.vellum_renderer_drift(skia, reader("")), ("not-configured", []))

    def test_local_checkout_reads_the_pinned_commit_not_the_worktree(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            repo = Path(td)
            git = ["git", "-C", td, "-c", "user.email=t@example.invalid", "-c", "user.name=t"]
            subprocess.run([*git, "init", "-q"], check=True)
            module = repo / "cmake" / "VellumSkiaDawn.cmake"
            module.parent.mkdir()
            module.write_text(vellum_module(), encoding="utf-8")
            subprocess.run([*git, "add", "cmake"], check=True)
            subprocess.run([*git, "commit", "-q", "-m", "pin"], check=True)
            commit = subprocess.run([*git, "rev-parse", "HEAD"], check=True,
                                    capture_output=True, text=True,
                                    encoding="utf-8").stdout.strip()
            # The worktree drifts after the pinned commit; only the commit counts.
            module.write_text(vellum_module(sha=OTHER_SHA, tag="chrome/m157"), encoding="utf-8")
            with mock.patch.dict(os.environ, {audit.VELLUM_CHECKOUT_ENV: td}):
                self.assertEqual(audit.vellum_renderer_drift(manifest(commit=commit)),
                                 ("match", []))
                status, problems = audit.vellum_renderer_drift(manifest(commit="e" * 40))
            self.assertEqual(status, "unavailable")
            self.assertIn("cannot show", problems[0])

    def test_strict_main_fails_on_drift_and_passes_on_match(self) -> None:
        entry = {**manifest()[0], "license": "BSD-3-Clause", "source_kind": "prebuilt-toolchain",
                 "documented_in_dependencies_md": True, "documented_in_notice_md": True}
        for text, expected in ((vellum_module(sha=OTHER_SHA), 1), (vellum_module(), 0)):
            with self.subTest(expected=expected), \
                    mock.patch.object(audit, "load_manifest", lambda: [entry]), \
                    mock.patch.object(audit, "collect_declared", lambda: []), \
                    mock.patch.object(audit, "split_known_unlisted", lambda _u: ([], [], [])), \
                    mock.patch.object(audit, "parse_dependencies_md", lambda: {"Skia"}), \
                    mock.patch.object(audit, "parse_notice_md", lambda: {"Skia"}), \
                    mock.patch.object(audit, "parse_licensing_md", lambda: {"Skia"}), \
                    mock.patch.object(audit, "read_vellum_pin_source", reader(text)), \
                    mock.patch.object(audit, "gpu_runtime_pin_agreement", lambda *_a, **_k: []), \
                    mock.patch.object(audit, "pulp_lock_agreement", lambda *_a, **_k: []), \
                    mock.patch.object(sys, "argv", ["audit.py", "--strict"]), \
                    contextlib.redirect_stdout(io.StringIO()) as out:
                self.assertEqual(audit.main(), expected, out.getvalue())

    def test_pulp_lock_agreement_reads_the_real_lock_and_catches_drift(self) -> None:
        import json
        real = json.loads((ROOT / "tools/deps/manifest.json").read_text(encoding="utf-8"))["dependencies"]

        def offline(*_args):
            raise audit.VellumPinUnavailable("offline")

        self.assertEqual([p for p in audit.pulp_lock_agreement(real, read_bytes=offline)
                          if "cannot be checked: offline" not in p], [])
        stale = [{**dep, "upstream": {**dep.get("upstream", {}), "commit": "0" * 40}}
                 if dep.get("name") == audit.GPU_RUNTIME_ENTRY else dep for dep in real]
        self.assertTrue(any("pulp.lock pins" in p for p in audit.pulp_lock_agreement(stale, read_bytes=offline)))
        self.assertTrue(any("no " + audit.GPU_RUNTIME_ENTRY in p
                            for p in audit.pulp_lock_agreement([], read_bytes=offline)))


LOCK_BYTES = b'{"schema": "gpu-runtime.renderer-lock.v1",\r\n "milestone": "chrome/m158"}\n'


class RendererLockDigestTests(unittest.TestCase):
    """pulp.lock's render_toolchain.lock_digest is sha256 of the renderer lock's bytes."""

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        import json
        self.lock = json.loads((ROOT / "pulp.lock").read_text(encoding="utf-8"))
        self.sha = self.lock["vellum"]["sha"]
        self.manifest = json.loads(
            (ROOT / "tools/deps/manifest.json").read_text(encoding="utf-8"))["dependencies"]

    def write_lock(self, digest: str) -> None:
        import json
        self.lock["render_toolchain"]["lock_digest"] = digest
        (self.root / "pulp.lock").write_text(json.dumps(self.lock), encoding="utf-8")

    def reader(self, data: bytes = LOCK_BYTES):
        calls = []

        def read(repository: str, commit: str, path: str) -> bytes:
            calls.append((repository, commit, path))
            return data

        read.calls = calls
        return read

    def test_the_matching_digest_passes_and_reads_at_the_lock_sha(self) -> None:
        import hashlib
        self.write_lock(hashlib.sha256(LOCK_BYTES).hexdigest())
        read = self.reader()
        self.assertEqual(audit.pulp_lock_agreement(self.manifest, self.root, read), [])
        self.assertEqual(read.calls, [("https://github.com/Generous-Corp/vellum", self.sha,
                                       "gpu-runtime/renderer-lock.json")])

    def test_a_stale_digest_fails(self) -> None:
        self.write_lock("0" * 64)
        problems = audit.pulp_lock_agreement(self.manifest, self.root, self.reader())
        self.assertTrue(any("render_toolchain.lock_digest is " + "0" * 64 in p
                            and "hashes to" in p for p in problems), problems)

    def test_an_unreadable_renderer_lock_fails_closed(self) -> None:
        self.write_lock("0" * 64)

        def unavailable(*_args):
            raise audit.VellumPinUnavailable("offline")

        problems = audit.pulp_lock_agreement(self.manifest, self.root, unavailable)
        self.assertTrue(any("cannot be checked: offline" in p for p in problems), problems)

    def test_the_drift_check_must_read_the_lock_commit(self) -> None:
        import hashlib
        self.write_lock(hashlib.sha256(LOCK_BYTES).hexdigest())
        moved = [{**dep, audit.VELLUM_DRIFT_FIELD: {**dep[audit.VELLUM_DRIFT_FIELD], "commit": "e" * 40}}
                 if dep.get("name") == "Skia" else dep for dep in self.manifest]
        problems = audit.pulp_lock_agreement(moved, self.root, self.reader())
        self.assertTrue(any("must be the same commit" in p for p in problems), problems)

    def test_a_checkout_is_hashed_byte_for_byte_at_the_pinned_commit(self) -> None:
        import hashlib
        with tempfile.TemporaryDirectory() as td:
            git = ["git", "-C", td, "-c", "user.email=t@example.invalid", "-c", "user.name=t",
                   "-c", "core.autocrlf=false"]
            subprocess.run([*git, "init", "-q"], check=True)
            path = Path(td) / "gpu-runtime" / "renderer-lock.json"
            path.parent.mkdir()
            path.write_bytes(LOCK_BYTES)
            subprocess.run([*git, "add", "gpu-runtime"], check=True)
            subprocess.run([*git, "commit", "-q", "-m", "lock"], check=True)
            commit = subprocess.run([*git, "rev-parse", "HEAD"], check=True, capture_output=True,
                                    text=True, encoding="utf-8").stdout.strip()
            path.write_bytes(b"{}\n")  # the worktree drifts; only the commit counts
            with mock.patch.dict(os.environ, {audit.VELLUM_CHECKOUT_ENV: td}):
                data = audit.read_vellum_pin_bytes("unused", commit, "gpu-runtime/renderer-lock.json")
        self.assertEqual(hashlib.sha256(data).hexdigest(), hashlib.sha256(LOCK_BYTES).hexdigest())

    def test_pulp_lock_renderer_digest_is_bound_to_exact_lock_bytes(self) -> None:
        import hashlib
        import json
        lock = json.loads((ROOT / "pulp.lock").read_text(encoding="utf-8"))
        real = json.loads((ROOT / "tools/deps/manifest.json").read_text(encoding="utf-8"))["dependencies"]
        payload = b"renderer lock bytes"
        lock["render_toolchain"]["lock_digest"] = hashlib.sha256(payload).hexdigest()
        self.assertEqual(audit.renderer_lock_digest_problems(
            lock, real, lambda *_args: payload), [])
        lock["render_toolchain"]["lock_digest"] = "0" * 64
        problems = audit.renderer_lock_digest_problems(lock, real, lambda *_args: payload)
        self.assertTrue(any("hashes to" in problem for problem in problems), problems)


if __name__ == "__main__":
    unittest.main()
