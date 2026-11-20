#!/usr/bin/env python3
"""Negative controls for the pulp.lock readers."""

from __future__ import annotations

import copy
import hashlib
import io
import json
import tarfile
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import pulp_lock

# Every repository file these tests read, named literally so the script-test
# input scan (tools/scripts/script_test_inputs.py) selects this test when one
# of them changes. pulp.lock sits at the repository root, which the scan does
# not match as a literal, so ctest passes it on the command line instead.
REPO = Path(__file__).resolve().parents[2]
LOCK_FILE = Path(sys.argv[1]) if len(sys.argv) > 1 else pulp_lock.LOCK
LOCK_READER = REPO / "tools/cmake/PulpVellumLock.cmake"
GPU_RUNTIME_MODULE = REPO / "tools/cmake/PulpGpuRuntime.cmake"
GPU_RUNTIME_RECORD = REPO / "tools/cmake/PulpGpuRuntimeRecord.cmake"


def stage_modules(root: Path) -> Path:
    """Copy the three modules to <root>/tools/cmake and return that directory.

    The lock reader reads only the pulp.lock two levels above itself, so a
    test lock is evaluated by placing it at <root>/pulp.lock beside a copy.
    """
    cmake_dir = root / "tools" / "cmake"
    cmake_dir.mkdir(parents=True, exist_ok=True)
    for module in (LOCK_READER, GPU_RUNTIME_MODULE, GPU_RUNTIME_RECORD):
        (cmake_dir / module.name).write_bytes(module.read_bytes())
    return cmake_dir


class PulpLockTests(unittest.TestCase):
    def setUp(self) -> None:
        self.lock = pulp_lock.load(LOCK_FILE)

    def test_missing_field_fails_closed(self) -> None:
        lock = copy.deepcopy(self.lock)
        del lock["vellum"]["sdk_digest"]
        self.assertTrue(any("exactly tag, sha" in e for e in pulp_lock.validate(lock)))

    def test_tag_sha_disagreement_fails_closed(self) -> None:
        sha = self.lock["vellum"]["sha"]
        self.assertTrue(any("tag resolves" in e for e in pulp_lock.validate(self.lock, tag_sha="0" * 40)))
        self.assertFalse(pulp_lock.validate(self.lock, tag_sha=sha))

    def test_digest_mismatch_fails_cmake_reader(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            lock_path = root / "pulp.lock"
            lock_path.write_text(json.dumps(self.lock), encoding="utf-8")
            archive = root / "sdk.tar.gz"
            archive.write_bytes(b"deliberately wrong bytes")
            reader = stage_modules(root) / LOCK_READER.name
            result = subprocess.run(
                ["cmake", f"-DPULP_VELLUM_SDK_ARCHIVE={archive}", "-P", str(reader)],
                capture_output=True, text=True, encoding="utf-8", check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("digest mismatch", result.stderr)

    def test_matching_digest_passes_cmake_reader(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "sdk.tar.gz"
            archive.write_bytes(b"sdk bytes")
            lock = copy.deepcopy(self.lock)
            lock["vellum"]["sdk_digest"] = hashlib.sha256(b"sdk bytes").hexdigest()
            lock_path = root / "pulp.lock"
            lock_path.write_text(json.dumps(lock), encoding="utf-8")
            reader = stage_modules(root) / LOCK_READER.name
            result = subprocess.run(
                ["cmake", f"-DPULP_VELLUM_SDK_ARCHIVE={archive}", "-P", str(reader)],
                capture_output=True, text=True, encoding="utf-8", check=False)
            self.assertEqual(result.returncode, 0, result.stderr)


COMMIT = "d361ea5b855707391798b7315183d99b6cbe69b6"
CMAKE_DIR = GPU_RUNTIME_MODULE.parent
DRIVER = """cmake_minimum_required(VERSION 3.24)
set(APPLE 1)
set(PULP_SKIA_PROVIDES_DAWN ON)
set(SKIA_DIR "${WORK}/no-skia")
set(PULP_GPU_RUNTIME_PREFIX "${PREFIX}" CACHE PATH "" FORCE)
set(PULP_GPU_RUNTIME_ARCHIVE "${ARCHIVE}" CACHE FILEPATH "" FORCE)
include("${MODULE}")
"""


class GpuRuntimePrefixTests(unittest.TestCase):
    """PULP_GPU_RUNTIME_PREFIX must match the archive pulp.lock hashes."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)
        payload = b"locked library bytes"
        record = {
            "source_commit": COMMIT,
            "files": [{"path": "lib/libx.a", "sha256": hashlib.sha256(payload).hexdigest(),
                       "size": len(payload), "executable": False}],
        }
        record_bytes = json.dumps(record, sort_keys=True).encode()
        self.archive = self.root / "gpu-runtime.tar.gz"
        with tarfile.open(self.archive, "w:gz") as tar:
            for name, data in (("gpu-runtime-artifact.json", record_bytes), ("lib/libx.a", payload)):
                info = tarfile.TarInfo(name)
                info.size = len(data)
                tar.addfile(info, io.BytesIO(data))
        self.prefix = self.root / "prefix"
        (self.prefix / "lib").mkdir(parents=True)
        (self.prefix / "gpu-runtime-artifact.json").write_bytes(record_bytes)
        (self.prefix / "lib" / "libx.a").write_bytes(payload)
        lock = copy.deepcopy(pulp_lock.load())
        lock["vellum"]["gpu_runtime_digest"] = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        lock["vellum"]["sha"] = COMMIT
        self.lock_path = self.root / "pulp.lock"
        self.lock_path.write_text(json.dumps(lock), encoding="utf-8")
        (self.root / "driver.cmake").write_text(DRIVER, encoding="utf-8")
        self.module = stage_modules(self.root) / GPU_RUNTIME_MODULE.name

    def relock(self, listed: list[str], members: dict[str, bytes]) -> Path:
        """Rebuild the archive, record and lock so the record lists `listed`; return the new archive."""
        files = [{"path": name, "sha256": hashlib.sha256(members[name]).hexdigest(),
                  "size": len(members[name]), "executable": False} for name in listed]
        record_bytes = json.dumps({"source_commit": COMMIT, "files": files}, sort_keys=True).encode()
        archive = self.root / "relocked.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            info = tarfile.TarInfo("gpu-runtime-artifact.json")
            info.size = len(record_bytes)
            tar.addfile(info, io.BytesIO(record_bytes))
        (self.prefix / "gpu-runtime-artifact.json").write_bytes(record_bytes)
        lock = json.loads(self.lock_path.read_text(encoding="utf-8"))
        lock["vellum"]["gpu_runtime_digest"] = hashlib.sha256(archive.read_bytes()).hexdigest()
        self.lock_path.write_text(json.dumps(lock), encoding="utf-8")
        return archive

    def run_module(self, archive: Path | str | None) -> str:
        """Configure-time output of PulpGpuRuntime.cmake; 'REACHED-FIND' when it got past the digest checks."""
        result = subprocess.run(
            ["cmake", f"-DWORK={self.root}", f"-DPREFIX={self.prefix}",
             f"-DARCHIVE={archive if archive is not None else ''}",
             f"-DMODULE={self.module}",
             "-DCMAKE_BINARY_DIR=" + str(self.root / "bin"),
             "-P", str(self.root / "driver.cmake")],
            capture_output=True, text=True, encoding="utf-8", check=False)
        text = " ".join((result.stdout + result.stderr).split())
        if "no VellumGpuRuntime" in text:
            return "REACHED-FIND"
        self.assertNotEqual(result.returncode, 0, text)
        return text

    def test_matching_archive_and_prefix_pass_the_digest_check(self) -> None:
        self.assertEqual(self.run_module(self.archive), "REACHED-FIND")

    def test_missing_archive_fails_closed_naming_the_lock_field(self) -> None:
        out = self.run_module(None)
        self.assertIn("vellum.gpu_runtime_digest", out)
        self.assertIn("PULP_GPU_RUNTIME_ARCHIVE", out)

    def test_changed_archive_fails(self) -> None:
        other = self.root / "other.tar.gz"
        other.write_bytes(self.archive.read_bytes() + b"x")
        self.assertIn("archive digest mismatch", self.run_module(other))

    def test_tampered_prefix_file_fails(self) -> None:
        (self.prefix / "lib" / "libx.a").write_bytes(b"tampered")
        self.assertIn("changed after extraction", self.run_module(self.archive))

    def test_tampered_record_with_same_commit_fails(self) -> None:
        record = self.prefix / "gpu-runtime-artifact.json"
        record.write_bytes(record.read_bytes() + b" ")
        self.assertIn("differs from the archive's", self.run_module(self.archive))

    def test_extra_prefix_file_fails(self) -> None:
        (self.prefix / "lib" / "extra.cmake").write_text("message(WARNING injected)", encoding="utf-8")
        self.assertIn("holds lib/extra.cmake", self.run_module(self.archive))

    def test_nested_plain_layout_passes(self) -> None:
        data = {"lib/cmake/VellumGpuRuntime/a.cmake": b"a", "lib/libx.a": b"locked library bytes",
                "include/x/y.h": b"h"}
        for name, body in data.items():
            (self.prefix / name).parent.mkdir(parents=True, exist_ok=True)
            (self.prefix / name).write_bytes(body)
        archive = self.relock(list(data), data)
        self.assertEqual(self.run_module(archive), "REACHED-FIND")

    def test_symlinked_lib_directory_fails(self) -> None:
        outside = self.root / "outside"
        outside.mkdir()
        (outside / "libx.a").write_bytes(b"locked library bytes")
        (self.prefix / "lib" / "libx.a").unlink()
        (self.prefix / "lib").rmdir()
        (self.prefix / "lib").symlink_to(outside, target_is_directory=True)
        out = self.run_module(self.archive)
        self.assertIn("is a symlink", out)
        self.assertIn("lib", out)

    def test_symlinked_intermediate_directory_fails(self) -> None:
        outside = self.root / "outside"
        outside.mkdir()
        (outside / "y.h").write_bytes(b"h")
        (self.prefix / "include").mkdir()
        (self.prefix / "include" / "x").symlink_to(outside, target_is_directory=True)
        archive = self.relock(["lib/libx.a", "include/x/y.h"],
                              {"lib/libx.a": b"locked library bytes", "include/x/y.h": b"h"})
        out = self.run_module(archive)
        self.assertIn("is a symlink", out)
        self.assertIn("include/x", out)

    def test_dotdot_listed_path_fails(self) -> None:
        (self.root / "escape.bin").write_bytes(b"e")
        archive = self.relock(["lib/libx.a", "../escape.bin"],
                              {"lib/libx.a": b"locked library bytes", "../escape.bin": b"e"})
        self.assertIn("can leave the prefix", self.run_module(archive))

    def test_symlinked_directory_in_prefix_scan_fails(self) -> None:
        # A symlinked directory the record does not list is refused by the scan, not followed.
        outside = self.root / "outside"
        outside.mkdir()
        (outside / "extra.cmake").write_text("message(WARNING injected)", encoding="utf-8")
        (self.prefix / "lib" / "ext").symlink_to(outside, target_is_directory=True)
        self.assertIn("holds the symlink lib/ext", self.run_module(self.archive))


class GpuRuntimeCheckoutTests(unittest.TestCase):
    """Source mode builds only a clean checkout of the pinned commit, however it was populated."""

    def git(self, *args: str, cwd: Path | None = None) -> str:
        return subprocess.run(
            ["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid",
             "-c", "commit.gpgsign=false", "-c", "init.defaultBranch=main", *args],
            cwd=cwd or self.repo, check=True, capture_output=True, text=True,
            encoding="utf-8").stdout.strip()

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name).resolve()
        self.repo = self.root / "vellum"
        self.repo.mkdir()
        self.git("init", "-q")
        (self.repo / "a.txt").write_text("pinned\n", encoding="utf-8")
        self.git("add", "a.txt")
        self.git("commit", "-q", "-m", "pinned")
        self.pin = self.git("rev-parse", "HEAD")
        (self.repo / "a.txt").write_text("later\n", encoding="utf-8")
        self.git("commit", "-q", "-am", "later")
        self.later = self.git("rev-parse", "HEAD")
        self.git("checkout", "-q", "--detach", self.pin)

    def check(self, source_dir: str, commit: str) -> str:
        script = self.root / "check.cmake"
        script.write_text(f'include("{GPU_RUNTIME_RECORD}")\n'
                          'pulp_check_gpu_runtime_checkout("${DIR}" "${COMMIT}")\n'
                          "message(STATUS REACHED-END)\n", encoding="utf-8")
        result = subprocess.run(["cmake", f"-DDIR={source_dir}", f"-DCOMMIT={commit}",
                                 "-P", str(script)], capture_output=True, text=True,
                                encoding="utf-8", check=False)
        text = " ".join((result.stdout + result.stderr).split())
        if "REACHED-END" in text:
            self.assertEqual(result.returncode, 0, text)
            return "REACHED-END"
        self.assertNotEqual(result.returncode, 0, text)
        return text

    def test_a_clean_checkout_of_the_pin_passes(self) -> None:
        self.assertEqual(self.check(str(self.repo), self.pin), "REACHED-END")

    def test_another_commit_fails(self) -> None:
        self.assertIn("not the pinned", self.check(str(self.repo), self.later))
        self.git("checkout", "-q", "--detach", self.later)
        self.assertIn("not the pinned", self.check(str(self.repo), self.pin))

    def test_local_changes_fail(self) -> None:
        (self.repo / "a.txt").write_text("edited\n", encoding="utf-8")
        self.assertIn("has local changes", self.check(str(self.repo), self.pin))
        self.git("checkout", "-q", "--", "a.txt")
        (self.repo / "extra.cmake").write_text("message(WARNING injected)\n", encoding="utf-8")
        self.assertIn("has local changes", self.check(str(self.repo), self.pin))

    def test_no_checkout_fails(self) -> None:
        plain = self.root / "plain"
        plain.mkdir()
        self.assertIn("was not populated", self.check("", self.pin))
        self.assertIn("was not populated", self.check(str(self.root / "missing"), self.pin))
        self.assertIn("not the pinned", self.check(str(plain), self.pin))

    def configure(self, override: str) -> str:
        """A configure that redirects FetchContent through a name built at configure time."""
        source = self.root / "project"
        source.mkdir(exist_ok=True)
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24)\nproject(t NONE)\ninclude(FetchContent)\n"
            f'include("{GPU_RUNTIME_RECORD}")\n'
            "set(_name VELLUM)\nstring(APPEND _name _GPU_RUNTIME)\n"
            'set(FETCHCONTENT_SOURCE_DIR_${_name} "${OVERRIDE}")\n'
            'FetchContent_Declare(vellum_gpu_runtime GIT_REPOSITORY "file://${REPO}" '
            "GIT_TAG ${PIN} SOURCE_SUBDIR none)\n"
            "FetchContent_MakeAvailable(vellum_gpu_runtime)\n"
            'pulp_check_gpu_runtime_checkout("${vellum_gpu_runtime_SOURCE_DIR}" "${PIN}")\n'
            "message(STATUS REACHED-END)\n", encoding="utf-8")
        build = self.root / ("build-" + str(len(list(self.root.glob("build-*")))))
        result = subprocess.run(
            ["cmake", "-S", str(source), "-B", str(build), f"-DREPO={self.repo}",
             f"-DPIN={self.pin}", f"-DOVERRIDE={override}"],
            capture_output=True, text=True, encoding="utf-8", check=False, timeout=120)
        return " ".join((result.stdout + result.stderr).split())

    def test_an_override_set_under_a_built_name_cannot_change_the_source(self) -> None:
        # The control: with no override FetchContent fetches the pin and passes.
        self.assertIn("REACHED-END", self.configure(""))
        stale = self.root / "stale"
        self.git("clone", "-q", str(self.repo), str(stale), cwd=self.root)
        self.git("checkout", "-q", "--detach", self.later, cwd=stale)
        text = self.configure(str(stale))
        self.assertNotIn("REACHED-END", text)
        self.assertIn("not the pinned", text)


class RepositoryLockTests(unittest.TestCase):
    def test_repository_lock_validates(self) -> None:
        # The lock the build actually reads, not a temporary copy.
        self.assertEqual(LOCK_FILE.resolve(), (REPO / "pulp.lock").resolve())
        self.assertEqual(pulp_lock.validate(pulp_lock.load(LOCK_FILE)), [])

    def test_named_inputs_exist(self) -> None:
        for path in (LOCK_READER, GPU_RUNTIME_MODULE, GPU_RUNTIME_RECORD):
            self.assertTrue(path.is_file(), path)


if __name__ == "__main__":
    unittest.main(argv=sys.argv[:1])
