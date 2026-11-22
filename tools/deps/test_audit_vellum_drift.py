#!/usr/bin/env python3
"""Tests for the renderer-lock agreement check in tools/deps/audit.py."""

from __future__ import annotations

import contextlib
import datetime
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
# The repository lock; ctest passes it so the input record declares it.
LOCK_FILE = (Path(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].endswith(".lock")
             else ROOT / "pulp.lock")

sys.path.insert(0, str(ROOT / "tools" / "deps"))
import audit  # noqa: E402


PULP_SHA = "a" * 64
OTHER_SHA = "b" * 64
COMMIT = "c" * 40
# Pinned so transition windows never depend on the wall clock.
PINNED_NOW = datetime.datetime(2026, 11, 21, 12, 0, tzinfo=datetime.timezone.utc)


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

    @contextlib.contextmanager
    def transition_repo(self, transition: dict | None, *, commit_record: bool):
        temporary = tempfile.TemporaryDirectory()
        repo = Path(temporary.name)
        git = ["git", "-C", str(repo), "-c", "user.email=t@example.invalid",
               "-c", "user.name=transition-test"]
        subprocess.run([*git, "init", "-q", "-b", "main"], check=True)
        if transition is not None:
            record = repo / audit.PIN_TRANSITION_FILE
            record.parent.mkdir(parents=True, exist_ok=True)
            record.write_text(json.dumps(transition), encoding="utf-8")
            if commit_record:
                subprocess.run([*git, "add", audit.PIN_TRANSITION_FILE], check=True)
        subprocess.run([*git, "commit", "-q", "--allow-empty", "-m", "base"], check=True)
        base = subprocess.run([*git, "rev-parse", "HEAD"], check=True,
                              capture_output=True, text=True, encoding="utf-8").stdout.strip()
        subprocess.run([*git, "update-ref", "refs/remotes/origin/main", base], check=True)
        subprocess.run([*git, "checkout", "-q", "-b", "work"], check=True)
        with mock.patch.object(audit, "ROOT", repo), mock.patch.dict(
                os.environ, {"GITHUB_BASE_REF": "main"}):
            yield repo
        temporary.cleanup()

    def transition(self, now: datetime.datetime, **overrides) -> dict:
        value = {
            "schema": "pulp.pin-transition.v1",
            "issued_at": (now - datetime.timedelta(days=1)).isoformat().replace("+00:00", "Z"),
            "expires_at": (now + datetime.timedelta(days=1)).isoformat().replace("+00:00", "Z"),
            "owner": "danielraffel",
            "reason": "cycle",
            "entries": [{"artifact_id": "renderer/mac-universal", "old_sha256": OTHER_SHA,
                          "new_sha256": PULP_SHA, "owner": "danielraffel", "reason": "cycle"}],
        }
        value.update(overrides)
        return value

    def drift(self, now: datetime.datetime) -> tuple[str, list[str]]:
        return audit.vellum_renderer_drift(lock_manifest(),
                                            reader(renderer_lock(sha=OTHER_SHA)), now=now)

    def test_working_tree_record_is_ignored(self) -> None:
        now = PINNED_NOW
        with self.transition_repo(self.transition(now), commit_record=False) as repo:
            (repo / audit.PIN_TRANSITION_FILE).parent.mkdir(parents=True, exist_ok=True)
            (repo / audit.PIN_TRANSITION_FILE).write_text(
                json.dumps(self.transition(now)), encoding="utf-8")
            status, problems = self.drift(now)
        self.assertEqual(status, "match", problems)

    def test_record_added_by_this_pr_is_applied(self) -> None:
        now = PINNED_NOW
        with self.transition_repo(None, commit_record=False) as repo:
            record = repo / audit.PIN_TRANSITION_FILE
            record.parent.mkdir(parents=True, exist_ok=True)
            record.write_text(json.dumps(self.transition(now)), encoding="utf-8")
            git = ["git", "-C", str(repo), "-c", "user.email=t@example.invalid",
                   "-c", "user.name=transition-test"]
            subprocess.run([*git, "add", audit.PIN_TRANSITION_FILE], check=True)
            subprocess.run([*git, "commit", "-q", "-m", "add record"], check=True)
            status, problems = self.drift(now)
        self.assertEqual(status, "match", problems)

    def test_valid_base_transition_covers_drift(self) -> None:
        now = PINNED_NOW
        with self.transition_repo(self.transition(now), commit_record=True):
            status, problems = self.drift(now)
        self.assertEqual(status, "match")
        self.assertEqual(problems, [])

    def test_expired_transition_fails_closed(self) -> None:
        now = PINNED_NOW
        transition = self.transition(
            now, expires_at=(now - datetime.timedelta(seconds=1)).isoformat().replace("+00:00", "Z"))
        with self.transition_repo(transition, commit_record=True):
            status, problems = self.drift(now)
        self.assertEqual(status, "mismatch")
        self.assertIn("expired", problems[0])

    def test_overlong_transition_fails_closed(self) -> None:
        now = PINNED_NOW
        transition = self.transition(
            now, expires_at=(now + datetime.timedelta(days=15)).isoformat().replace("+00:00", "Z"))
        with self.transition_repo(transition, commit_record=True):
            status, problems = self.drift(now)
        self.assertEqual(status, "mismatch")
        self.assertIn("at most 14 days", problems[0])

    def test_future_issued_at_fails_closed(self) -> None:
        now = PINNED_NOW
        transition = self.transition(
            now, issued_at=(now + datetime.timedelta(seconds=1)).isoformat().replace("+00:00", "Z"))
        with self.transition_repo(transition, commit_record=True):
            status, problems = self.drift(now)
        self.assertEqual(status, "mismatch")
        self.assertIn("future", problems[0])

    def test_a_reversed_transition_fails_closed(self) -> None:
        # The digests name the same pair of values in reverse: the record would
        # authorize the opposite renderer change, so direction must be checked.
        now = PINNED_NOW
        reversed_entry = {"artifact_id": "renderer/mac-universal", "old_sha256": PULP_SHA,
                          "new_sha256": OTHER_SHA, "owner": "danielraffel", "reason": "cycle"}
        with self.transition_repo(self.transition(now, entries=[reversed_entry]),
                                  commit_record=True):
            status, problems = self.drift(now)
        self.assertEqual(status, "mismatch")
        self.assertIn("does not name the observed old/new digests", problems[0])

    def test_malformed_transition_fails_closed(self) -> None:
        now = PINNED_NOW
        with self.transition_repo({"schema": "wrong"}, commit_record=True):
            status, problems = self.drift(now)
        self.assertEqual(status, "mismatch")
        self.assertIn("invalid schema", problems[0])

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
    """The strict audit evaluates the pin file with real CMake and asserts on what it does."""

    GPU = "tools/cmake/PulpGpuRuntime.cmake"
    LOCK = "tools/cmake/PulpVellumLock.cmake"
    RECORD = "tools/cmake/PulpGpuRuntimeRecord.cmake"
    URL = "https://github.com/Generous-Corp/vellum.git"

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / "tools/cmake").mkdir(parents=True)
        for rel in (self.GPU, self.LOCK, self.RECORD):
            (self.root / rel).write_text((ROOT / rel).read_text(encoding="utf-8"), encoding="utf-8")
        (self.root / "pulp.lock").write_text(LOCK_FILE.read_text(encoding="utf-8"), encoding="utf-8")
        self.original = (ROOT / self.GPU).read_text(encoding="utf-8")

    def edit(self, rel: str, old: str, new: str) -> None:
        path = self.root / rel
        text = path.read_text(encoding="utf-8")
        self.assertIn(old, text)
        path.write_text(text.replace(old, new, 1), encoding="utf-8")

    def entry(self) -> list[dict]:
        return [{"name": audit.GPU_RUNTIME_ENTRY, "version": "0.2.0 (d361ea5b)",
                 "upstream": {"kind": "git-commit", "ref": COMMIT, "commit": COMMIT},
                 "source_files": [self.GPU]}]

    def problems(self) -> list[str]:
        return audit.gpu_runtime_pin_agreement(self.entry(), self.root)

    def assertFlags(self, fragment: str) -> None:
        found = self.problems()
        self.assertTrue(any(fragment in problem for problem in found), found)

    def assertFails(self) -> None:
        self.assertTrue(self.problems(), "the audit accepted an edit it must refuse")

    def test_the_copied_tree_passes(self) -> None:
        self.assertEqual(self.problems(), [])

    def test_the_real_tree_passes(self) -> None:
        import json
        manifest = json.loads((ROOT / "tools/deps/manifest.json").read_text(encoding="utf-8"))["dependencies"]
        self.assertEqual(audit.gpu_runtime_pin_agreement(manifest), [])

    def test_the_driver_records_what_cmake_evaluates(self) -> None:
        run = audit.run_gpu_runtime_driver(self.root, self.GPU, "0.2.0")
        (_, declare), = run.named("FetchContent_Declare")
        self.assertEqual(declare, ["vellum_gpu_runtime", "GIT_REPOSITORY", self.URL, "GIT_TAG",
                                   "d361ea5b855707391798b7315183d99b6cbe69b6", "SOURCE_SUBDIR",
                                   "gpu-runtime"])
        self.assertTrue(run.stopped)

    def test_cmake_missing_fails_closed(self) -> None:
        with mock.patch.object(audit, "find_cmake", lambda: None):
            self.assertFlags("cmake was not found")

    # Each pin assertion, with the real code removed and a fake placed where
    # the real code would count. "bare" is the control: the fake as real code
    # must satisfy the audit, so the hidden forms fail only because CMake does
    # not run them.
    REAL_DECLARE = ("    FetchContent_Declare(\n"
                    "        vellum_gpu_runtime\n"
                    "        GIT_REPOSITORY https://github.com/Generous-Corp/vellum.git\n"
                    "        GIT_TAG ${PULP_GPU_RUNTIME_COMMIT}\n"
                    "        SOURCE_SUBDIR gpu-runtime\n"
                    "    )\n")
    REAL_CHECK = ("    if(NOT _pulp_gpu_runtime_version STREQUAL PULP_GPU_RUNTIME_VERSION)\n"
                  "        message(FATAL_ERROR\n"
                  "            \"Pulp: gpu-runtime at ${PULP_GPU_RUNTIME_COMMIT} is version \"\n"
                  "            \"${_pulp_gpu_runtime_version}, not the pinned ${PULP_GPU_RUNTIME_VERSION}\")\n"
                  "    endif()\n")
    PIN_CASES = {
        "lock include": (
            'include("${CMAKE_CURRENT_LIST_DIR}/PulpVellumLock.cmake")\n', "",
            "set(PULP_GPU_RUNTIME_PREFIX",
            'include("${CMAKE_CURRENT_LIST_DIR}/PulpVellumLock.cmake")'),
        "declaration": (
            REAL_DECLARE, "", "    set(GPU_RUNTIME_SKIA_DIR \"${SKIA_DIR}\" CACHE",
            "FetchContent_Declare(vellum_gpu_runtime GIT_REPOSITORY "
            "https://github.com/Generous-Corp/vellum.git GIT_TAG ${PULP_GPU_RUNTIME_COMMIT} "
            "SOURCE_SUBDIR gpu-runtime)"),
        "version check": (
            REAL_CHECK, "", "endif()\n\nforeach(_pulp_gpu_runtime_target",
            "if(NOT _pulp_gpu_runtime_version STREQUAL PULP_GPU_RUNTIME_VERSION)\n"
            "message(FATAL_ERROR stop)\nendif()"),
        "prefix package": (
            "    pulp_find_gpu_runtime_package(${PULP_GPU_RUNTIME_VERSION} \"${PULP_GPU_RUNTIME_PREFIX}\"\n"
            "        _pulp_gpu_runtime_error)\n", "",
            "    if(_pulp_gpu_runtime_error)\n",
            'pulp_find_gpu_runtime_package(${PULP_GPU_RUNTIME_VERSION} "${PULP_GPU_RUNTIME_PREFIX}" '
            "_pulp_gpu_runtime_error)"),
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
            "escaped references": fake.replace("${", "\\${"),
            "constant-false branch": f"if(FALSE)\n{fake}\nendif()",
            "else of a constant-true branch": f"if(TRUE)\nelse()\n{fake}\nendif()",
            "function body": f"function(_pulp_never_called)\n{fake}\nendfunction()",
            "after a constant-true return": f"if(TRUE)\nreturn()\nendif()\n{fake}",
        }

    def test_fake_pins_cmake_does_not_run_do_not_count(self) -> None:
        for assertion, (real, replacement, before, fake) in self.PIN_CASES.items():
            self.assertIn(real, self.original)
            removed = self.original.replace(real, replacement, 1)
            self.assertIn(before, removed)
            for form, hidden in self.hidden_forms(fake).items():
                if form == "escaped references" and "${" not in fake:
                    continue
                with self.subTest(assertion=assertion, form=form):
                    text = removed.replace(before, f"{hidden or fake}\n{before}", 1)
                    (self.root / self.GPU).write_text(text, encoding="utf-8")
                    found = self.problems()
                    if hidden is None:
                        self.assertEqual(found, [])
                    else:
                        self.assertTrue(found, "a hidden fake satisfied the audit")

    def test_a_literal_pin_equal_to_the_lock_fails(self) -> None:
        self.edit(self.GPU, "GIT_TAG ${PULP_GPU_RUNTIME_COMMIT}",
                  "GIT_TAG d361ea5b855707391798b7315183d99b6cbe69b6")
        self.assertFlags("GIT_TAG must be the lock's vellum.sha 0123456789abcdef")

    def test_a_literal_prefix_version_equal_to_the_lock_fails(self) -> None:
        self.edit(self.GPU, "pulp_find_gpu_runtime_package(${PULP_GPU_RUNTIME_VERSION}",
                  "pulp_find_gpu_runtime_package(0.2.0")
        self.assertFlags("find_package(VellumGpuRuntime 9.8.7 EXACT")

    def test_the_declaration_must_name_the_canonical_repository(self) -> None:
        for replacement in ("https://github.com/someone/vellum.git", self.URL + ".evil",
                            '"${PULP_ELSEWHERE}"', ""):
            with self.subTest(replacement=replacement):
                self.setUp()
                self.edit(self.GPU, f"GIT_REPOSITORY {self.URL}",
                          f"GIT_REPOSITORY {replacement}" if replacement else "")
                self.assertFlags("GIT_REPOSITORY must be exactly")

    def test_every_alternate_source_option_is_rejected(self) -> None:
        for option in ("URL https://example.invalid/v.tgz", "URL_HASH SHA256=00",
                       "SOURCE_DIR /x", "DOWNLOAD_COMMAND true", "SVN_REPOSITORY x",
                       "SVN_REVISION 1", "HG_REPOSITORY x", "HG_TAG x", "CVS_REPOSITORY x",
                       "CVS_MODULE x", "PATCH_COMMAND true", "UPDATE_COMMAND true"):
            with self.subTest(option=option):
                self.setUp()
                self.edit(self.GPU, "SOURCE_SUBDIR gpu-runtime",
                          f"SOURCE_SUBDIR gpu-runtime\n        {option}")
                self.assertFlags(f"carries {option.split()[0]}")

    def test_a_source_override_in_the_pin_file_fails(self) -> None:
        for override in ('set(FETCHCONTENT_SOURCE_DIR_VELLUM_GPU_RUNTIME "/x")',
                         'set("FETCHCONTENT_FULLY_DISCONNECTED" ON)'):
            with self.subTest(override=override):
                self.setUp()
                self.edit(self.GPU, "    include(FetchContent)",
                          f"    include(FetchContent)\n    {override}")
                self.assertFlags("FetchContent takes vellum_gpu_runtime from")

    def test_an_include_the_driver_cannot_evaluate_fails(self) -> None:
        (self.root / "tools/cmake/Other.cmake").write_text("set(_x 1)\n", encoding="utf-8")
        self.edit(self.GPU, "    include(FetchContent)",
                  '    include(FetchContent)\n    include("${CMAKE_CURRENT_LIST_DIR}/Other.cmake")')
        self.assertFlags("which the audit cannot evaluate")

    def test_the_lock_include_must_be_the_real_reader(self) -> None:
        (self.root / "tools/other").mkdir(parents=True)
        (self.root / "tools/other/PulpVellumLock.cmake").write_text(
            (ROOT / self.LOCK).read_text(encoding="utf-8"), encoding="utf-8")
        self.edit(self.GPU, 'include("${CMAKE_CURRENT_LIST_DIR}/PulpVellumLock.cmake")',
                  'include("${CMAKE_CURRENT_LIST_DIR}/../other/PulpVellumLock.cmake")')
        self.assertFails()

    def test_the_version_check_must_compare_the_fetched_version_with_the_pin(self) -> None:
        check = "if(NOT _pulp_gpu_runtime_version STREQUAL PULP_GPU_RUNTIME_VERSION)"
        for new in ("if(NOT PULP_GPU_RUNTIME_VERSION STREQUAL PULP_GPU_RUNTIME_VERSION)",
                    "if(NOT _pulp_gpu_runtime_version STREQUAL _pulp_gpu_runtime_version)",
                    'if(NOT _pulp_gpu_runtime_version STREQUAL "PULP_GPU_RUNTIME_VERSION")',
                    "if(NOT _pulp_gpu_runtime_version VERSION_LESS PULP_GPU_RUNTIME_VERSION)",
                    "if(_pulp_gpu_runtime_version STREQUAL PULP_GPU_RUNTIME_VERSION)",
                    "if(FALSE)",
                    'set(_pulp_gpu_runtime_version "${PULP_GPU_RUNTIME_VERSION}")\n    ' + check):
            with self.subTest(new=new):
                self.setUp()
                self.edit(self.GPU, check, new)
                self.assertFlags("does not stop configure with FATAL_ERROR when, and only when")

    def test_the_version_check_must_stop_configure_unconditionally(self) -> None:
        self.edit(self.GPU, "        message(FATAL_ERROR\n            \"Pulp: gpu-runtime at",
                  "        if(PULP_NEVER_SET)\n        message(FATAL_ERROR\n            "
                  "\"Pulp: gpu-runtime at")
        self.edit(self.GPU, "not the pinned ${PULP_GPU_RUNTIME_VERSION}\")",
                  "not the pinned ${PULP_GPU_RUNTIME_VERSION}\")\n        endif()")
        self.assertFlags("does not stop configure with FATAL_ERROR when, and only when")

    def test_an_indirect_write_of_the_pin_fails(self) -> None:
        indirect = ('set(_suffix "")\nset(PULP_GPU_RUNTIME_COMMIT${_suffix} '
                    '"d361ea5b855707391798b7315183d99b6cbe69b6")\n')
        for rel, after in ((self.GPU, 'include("${CMAKE_CURRENT_LIST_DIR}/PulpVellumLock.cmake")\n'),
                           (self.LOCK, "unset(_pulp_vellum_lock)\n")):
            with self.subTest(rel=rel):
                self.setUp()
                self.edit(rel, after, after + indirect)
                self.assertFlags("GIT_TAG must be the lock's vellum.sha 0123456789abcdef")

    def test_a_return_before_the_version_fatal_fails(self) -> None:
        self.edit(self.GPU, "        message(FATAL_ERROR\n            \"Pulp: gpu-runtime at",
                  "        return()\n        message(FATAL_ERROR\n            \"Pulp: gpu-runtime at")
        self.assertFlags("does not stop configure with FATAL_ERROR when, and only when")

    def test_a_redefined_recorded_command_fails(self) -> None:
        fake_version = ("macro(get_directory_property output)\n"
                        "    set(${output} 0.2.0)\nendmacro()\n")
        for label, code in (
                ("macro", fake_version),
                ("cmake_language(EVAL)",
                 'cmake_language(EVAL CODE "macro(get_directory_property output)\n'
                 '    set(\\${output} 0.2.0)\nendmacro()")\n'),
                ("FetchContent_Declare",
                 'cmake_language(EVAL CODE "function(FetchContent_Declare)\nendfunction()")\n'),
                ("find_package",
                 'cmake_language(EVAL CODE "function(find_package)\n'
                 '    set(VellumGpuRuntime_FOUND TRUE PARENT_SCOPE)\nendfunction()")\n')):
            with self.subTest(label=label):
                self.setUp()
                self.edit(self.GPU, "    include(FetchContent)\n", "    include(FetchContent)\n" + code)
                self.edit(self.GPU, "if(PULP_GPU_RUNTIME_PREFIX)\n", code + "if(PULP_GPU_RUNTIME_PREFIX)\n")
                self.assertFails()

    def test_the_lock_reader_must_derive_the_pin_from_the_lock(self) -> None:
        for old, new in (
                ('set(PULP_GPU_RUNTIME_COMMIT "${PULP_VELLUM_sha}")',
                 'set(PULP_GPU_RUNTIME_COMMIT "d361ea5b855707391798b7315183d99b6cbe69b6")'),
                ('set(PULP_GPU_RUNTIME_VERSION "${PULP_VELLUM_tag}")',
                 'set(PULP_GPU_RUNTIME_VERSION "v0.2.0")'),
                ('file(READ "${PULP_VELLUM_LOCK_FILE}" _pulp_vellum_lock)',
                 'file(READ "${CMAKE_CURRENT_LIST_DIR}/../../pulp.lock" _pulp_vellum_lock)'),
                ('set(PULP_VELLUM_${_field} "${_value}")',
                 'set(PULP_VELLUM_${_field} "${_value}")\n    if(_field STREQUAL "sha")\n'
                 '        set(PULP_VELLUM_sha d361ea5b855707391798b7315183d99b6cbe69b6)\n    endif()')):
            with self.subTest(new=new):
                self.setUp()
                self.edit(self.LOCK, old, new)
                self.assertFails()

    def test_reading_another_lock_fails(self) -> None:
        import json
        other = json.loads(LOCK_FILE.read_text(encoding="utf-8"))
        other["vellum"]["sha"] = "f" * 40
        (self.root / "other.lock").write_text(json.dumps(other), encoding="utf-8")
        self.edit(self.LOCK, '"${CMAKE_CURRENT_LIST_DIR}/../../pulp.lock"',
                  '"${CMAKE_CURRENT_LIST_DIR}/../../other.lock"')
        self.assertFlags("read the pin from")

    def test_the_lock_reader_refuses_a_tag_pulp_lock_py_refuses(self) -> None:
        import json
        for tag in ("0.2.0", "v0.2", "v0.2.0-rc1", "release-1"):
            with self.subTest(tag=tag):
                lock = json.loads(LOCK_FILE.read_text(encoding="utf-8"))
                lock["vellum"]["tag"] = tag
                path = self.root / "bad.lock"
                path.write_text(json.dumps(lock), encoding="utf-8")
                self.assertTrue(audit.validate_pulp_lock(lock))
                run = audit.run_gpu_runtime_driver(self.root, self.GPU, "0.2.0", lock_file=path)
                self.assertEqual(run.calls[-1][0], "message", run.output)
                self.assertIn("vellum.tag is not a release tag", run.calls[-1][1][1])
                self.assertEqual(run.named("FetchContent_Declare"), [])
        lock = json.loads(LOCK_FILE.read_text(encoding="utf-8"))
        path.write_text(json.dumps(lock), encoding="utf-8")
        run = audit.run_gpu_runtime_driver(self.root, self.GPU, "0.2.0", lock_file=path)
        self.assertTrue(run.named("FetchContent_Declare"), run.output)

    def test_the_prefix_checks_must_use_the_lock(self) -> None:
        for old, new, fragment in (
                ('pulp_check_gpu_runtime_record("${PULP_GPU_RUNTIME_PREFIX}" "${PULP_GPU_RUNTIME_COMMIT}")',
                 'pulp_check_gpu_runtime_record("${PULP_GPU_RUNTIME_PREFIX}" "x")', "prefix record"),
                ('"${PULP_GPU_RUNTIME_ARCHIVE}" "${PULP_VELLUM_gpu_runtime_digest}")',
                 '"${PULP_GPU_RUNTIME_ARCHIVE}" "' + "f0ed5c873c9513725044cbca30d3604fbcb6f6dfdca99ee588dca1050b3d857c" + '")',
                 "prefix archive")):
            with self.subTest(fragment=fragment):
                self.setUp()
                self.edit(self.GPU, old, new)
                self.assertFlags(fragment)

    def test_source_overrides_elsewhere_in_the_tree_fail(self) -> None:
        other = self.root / "tools/cmake/Other.cmake"
        for override, fragment in (
                ("set(FETCHCONTENT_SOURCE_DIR_VELLUM_GPU_RUNTIME /x)", "sets FETCHCONTENT_SOURCE_DIR"),
                ('set("FETCHCONTENT_SOURCE_DIR_VELLUM_GPU_RUNTIME" /x CACHE PATH "")',
                 "sets FETCHCONTENT_SOURCE_DIR"),
                ("set([[FETCHCONTENT_FULLY_DISCONNECTED]] ON)", "sets FETCHCONTENT_FULLY_DISCONNECTED"),
                ('set(PULP_VELLUM_LOCK_FILE "/tmp/other.lock" CACHE FILEPATH "" FORCE)',
                 "sets PULP_VELLUM_LOCK_FILE"),
                ('set([==[PULP_VELLUM_LOCK_FILE]==] "/tmp/other.lock")', "sets PULP_VELLUM_LOCK_FILE"),
                ("pulp_register_fetchcontent_source(vellum_gpu_runtime DIR x REF y)",
                 "registers a FetchContent source for vellum_gpu_runtime"),
                ("cmake_language(SET_DEPENDENCY_PROVIDER p SUPPORTED_METHODS "
                 "FETCHCONTENT_MAKEAVAILABLE_SERIAL)", "sets a dependency provider"),
                ("macro(FetchContent_Declare)\nendmacro()", "redefines FetchContent_Declare()")):
            with self.subTest(override=override):
                other.write_text(override + "\n", encoding="utf-8")
                self.assertFlags(fragment)
        other.write_text("pulp_register_fetchcontent_source(Catch2 DIR catch2 REF v3)\n"
                         'set(_doc "set(FETCHCONTENT_SOURCE_DIR_VELLUM_GPU_RUNTIME /x)")\n'
                         "# set(PULP_VELLUM_LOCK_FILE /x)\n", encoding="utf-8")
        self.assertEqual(self.problems(), [])

    def test_the_pin_source_must_be_the_canonical_in_tree_file(self) -> None:
        for source in ("tools/cmake/Other.cmake", "/abs/PulpGpuRuntime.cmake",
                       "../PulpGpuRuntime.cmake", "tools/cmake/../cmake/PulpGpuRuntime.cmake"):
            with self.subTest(source=source):
                entry = self.entry()
                entry[0]["source_files"] = [source]
                found = audit.gpu_runtime_pin_agreement(entry, self.root)
                self.assertTrue(any("must be exactly" in problem for problem in found), found)

    def test_expanded_and_eval_writes_elsewhere_in_the_tree_fail(self) -> None:
        other = self.root / "tools/cmake/Other.cmake"
        for text, fragment in (
                ("set(_name PULP_VELLUM_LOCK_FILE)\nset(${_name} /other.lock)", "sets PULP_VELLUM_LOCK_FILE"),
                ("set(_name PULP_VELLUM_LOCK_${_s})\nset(${_name} /other.lock)", "named by an expansion"),
                ("set(_a PULP_VELLUM)\nset(_b LOCK_FILE)\nset(${_a}_${_b} /x)", "named by an expansion"),
                ("string(CONCAT _n PULP_VELLUM _LOCK_FILE)\nset(${_n} /x)", "named by an expansion"),
                ("set(PULP_VELLUM_${_s} /other.lock CACHE FILEPATH \"\" FORCE)", "named by an expansion"),
                ("set(FETCHCONTENT_SOURCE_DIR_${_u} /x)", "named by an expansion"),
                ('cmake_language(EVAL CODE "set(PULP_VELLUM_LOCK_FILE /x)")', "sets PULP_VELLUM_LOCK_FILE"),
                ('cmake_language(EVAL CODE "set(${_n} /x")', "cannot parse"),
                ("cmake_language(CALL ${_c} PULP_VELLUM_LOCK_FILE /x)", "names its command by an expansion"),
                ("cmake_language(EVAL ${_code})", "not in the form"),
                ("cmake_language(CALL set PULP_VELLUM_LOCK_FILE /x)", "sets PULP_VELLUM_LOCK_FILE")):
            with self.subTest(text=text):
                other.write_text(text + "\n", encoding="utf-8")
                self.assertFlags(fragment)
        # Negative controls: PARENT_SCOPE out-parameters, deferred calls and
        # expansions that cannot be a protected name stay clean.
        other.write_text("function(f out)\n  set(${out} 1 PARENT_SCOPE)\nendfunction()\n"
                         "set(FETCHCONTENT_SOURCE_DIR_${_u} /x PARENT_SCOPE)\n"
                         "set(OTHER_${_u} 1)\ncmake_language(DEFER CALL message hi)\n",
                         encoding="utf-8")
        self.assertEqual(self.problems(), [])

    def test_unparseable_cmake_elsewhere_fails_closed(self) -> None:
        (self.root / "tools/cmake/Other.cmake").write_text('set(x "unterminated)\n', encoding="utf-8")
        self.assertFlags("cannot parse")

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
        self.lock = json.loads(LOCK_FILE.read_text(encoding="utf-8"))
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
        lock = json.loads(LOCK_FILE.read_text(encoding="utf-8"))
        real = json.loads((ROOT / "tools/deps/manifest.json").read_text(encoding="utf-8"))["dependencies"]
        payload = b"renderer lock bytes"
        lock["render_toolchain"]["lock_digest"] = hashlib.sha256(payload).hexdigest()
        self.assertEqual(audit.renderer_lock_digest_problems(
            lock, real, lambda *_args: payload), [])
        lock["render_toolchain"]["lock_digest"] = "0" * 64
        problems = audit.renderer_lock_digest_problems(lock, real, lambda *_args: payload)
        self.assertTrue(any("hashes to" in problem for problem in problems), problems)


class LicenseAuditCheckoutTests(unittest.TestCase):
    def test_license_audit_checkout_keeps_history_for_the_base_merge_base(self) -> None:
        # renderer_transition needs origin/<base> and a merge-base with HEAD;
        # the default shallow checkout has neither, so the audit would fail closed.
        workflow = (ROOT / ".github" / "workflows" / "license-audit.yml").read_text(
            encoding="utf-8")
        checkout = workflow.split("actions/checkout@", 1)[1].split("\n      - ", 1)[0]
        self.assertIn("fetch-depth: 0", checkout)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *(arg for arg in sys.argv[1:] if not arg.endswith(".lock"))])
