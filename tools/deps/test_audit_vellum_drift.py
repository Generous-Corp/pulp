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
        self.assertEqual(audit.pulp_lock_agreement(real), [])
        stale = [{**dep, "upstream": {**dep.get("upstream", {}), "commit": "0" * 40}}
                 if dep.get("name") == audit.GPU_RUNTIME_ENTRY else dep for dep in real]
        self.assertTrue(any("pulp.lock pins" in p for p in audit.pulp_lock_agreement(stale)))
        self.assertTrue(any("no " + audit.GPU_RUNTIME_ENTRY in p
                            for p in audit.pulp_lock_agreement([])))


if __name__ == "__main__":
    unittest.main()
