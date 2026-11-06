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

    def test_a_lock_without_the_platform_fails_closed(self) -> None:
        status, problems = audit.vellum_renderer_drift(
            lock_manifest(lock_platform="windows-x64"), reader(renderer_lock()))
        self.assertEqual(status, "unavailable")


class GpuRuntimePinTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / "tools/cmake").mkdir(parents=True)

    def pin(self, commit: str = COMMIT, version: str = "0.1.10") -> None:
        (self.root / "tools/cmake/PulpGpuRuntime.cmake").write_text(
            f'set(PULP_GPU_RUNTIME_COMMIT "{commit}")\nset(PULP_GPU_RUNTIME_VERSION "{version}")\n',
            encoding="utf-8")

    def entry(self, commit: str = COMMIT, version: str = "0.1.10 (cccccccc)") -> list[dict]:
        return [{"name": audit.GPU_RUNTIME_ENTRY, "version": version,
                 "upstream": {"kind": "git-commit", "ref": commit, "commit": commit},
                 "source_files": ["tools/cmake/PulpGpuRuntime.cmake"]}]

    def test_an_agreeing_pin_passes(self) -> None:
        self.pin()
        self.assertEqual(audit.gpu_runtime_pin_agreement(self.entry(), self.root), [])

    def test_a_commit_moved_only_in_cmake_fails(self) -> None:
        self.pin(commit="d" * 40)
        problems = audit.gpu_runtime_pin_agreement(self.entry(), self.root)
        self.assertTrue(problems and "fetches gpu-runtime at" in problems[0])

    def test_a_version_moved_only_in_the_manifest_fails(self) -> None:
        self.pin()
        problems = audit.gpu_runtime_pin_agreement(self.entry(version="0.1.11 (cccccccc)"), self.root)
        self.assertTrue(problems and "expects gpu-runtime 0.1.10" in problems[0])

    def test_a_pin_the_cmake_does_not_declare_fails(self) -> None:
        (self.root / "tools/cmake/PulpGpuRuntime.cmake").write_text("# no pin\n", encoding="utf-8")
        self.assertTrue(audit.gpu_runtime_pin_agreement(self.entry(), self.root))

    def test_the_real_tree_agrees(self) -> None:
        import json
        manifest = json.loads((ROOT / "tools/deps/manifest.json").read_text())["dependencies"]
        self.assertEqual(audit.gpu_runtime_pin_agreement(manifest), [])


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
                    mock.patch.object(sys, "argv", ["audit.py", "--strict"]), \
                    contextlib.redirect_stdout(io.StringIO()) as out:
                self.assertEqual(audit.main(), expected, out.getvalue())


if __name__ == "__main__":
    unittest.main()
