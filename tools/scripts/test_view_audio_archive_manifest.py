#!/usr/bin/env python3
"""Positive and negative controls for view_audio_archive_manifest.py.

A fake `nm` serves a canned `nm -A -P -g` listing, a fake demangler prefixes
each name, and a stub view_audio_symbol_floor.py in a throwaway git checkout
stands in for the source tree, so describe and verify run without a build.
The workflow that publishes the archive is checked for the properties a
downstream verifier relies on.
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import pathlib
import stat
import subprocess
import sys
import tempfile
import unittest

MODULE_PATH = pathlib.Path(__file__).with_name("view_audio_archive_manifest.py")
WORKFLOW = pathlib.Path(__file__).resolve().parents[2] / ".github/workflows/view-audio-archive.yml"
SPEC = importlib.util.spec_from_file_location("view_audio_archive_manifest", MODULE_PATH)
tool = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = tool
SPEC.loader.exec_module(tool)

LISTING = (
    "libpulp-view-audio.a[visualizers.cpp.o]: __ZTIN4pulp4view5MeterE S 10 0\n"
    "libpulp-view-audio.a[visualizers.cpp.o]: __ZN4pulp4view5Meter9set_levelEff T 20 0\n"
    "libpulp-view-audio.a[visualizers.cpp.o]: __ZN4pulp4view4View5paintEv U 0 0\n"
    "libpulp-view-audio.a[visualizers.cpp.o]: __ZN4pulp4view4weakEv w 0 0\n"
    "libpulp-view-audio.a[visualizers.cpp.o]: __ZN4pulp4view4weakvEv v 0 0\n"
    "libpulp-view-audio.a[waveform.cpp.o]: _ZN4pulp4view12WaveformView5paintEv T 0 0\n"
    "libpulp-view-audio.a[waveform.cpp.o]:\n"
)


def write_exe(path: pathlib.Path, body: str) -> pathlib.Path:
    path.write_text(f"#!{sys.executable}\n{body}", encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        self.listing = self.root / "listing.txt"
        self.listing.write_text(LISTING, encoding="utf-8")
        self.nm = write_exe(self.root / "nm", (
            "import pathlib, sys\n"
            f"sys.stdout.write(pathlib.Path({str(self.listing)!r}).read_text(encoding='utf-8'))\n"))
        self.cxxfilt = write_exe(self.root / "cxxfilt", (
            "import sys\n"
            "for line in sys.stdin.read().splitlines():\n"
            "    print('dm:' + line)\n"))
        self.source = self.root / "source"
        (self.source / "tools/scripts").mkdir(parents=True)
        self.gate_status = self.source / "gate-status"
        self.gate_status.write_text("0", encoding="utf-8")
        (self.source / "tools/scripts/view_audio_symbol_floor.py").write_text(
            "import pathlib, sys\n"
            "print('gate argv: ' + ' '.join(a.rsplit('/', 1)[-1] for a in sys.argv[1:]))\n"
            f"raise SystemExit(int(pathlib.Path({str(self.gate_status)!r}).read_text(encoding='utf-8')))\n",
            encoding="utf-8")
        git = ["git", "-C", str(self.source), "-c", "user.name=t", "-c", "user.email=t@t"]
        subprocess.run(git[:3] + ["init", "-q"], check=True)
        subprocess.run(git + ["add", "-A"], check=True)
        subprocess.run(git + ["commit", "-q", "-m", "fixture"], check=True)
        self.sha = subprocess.run(git[:3] + ["rev-parse", "HEAD"], check=True,
                                  capture_output=True, text=True, encoding="utf-8").stdout.strip()
        self.archive = self.root / "libpulp-view-audio.a"
        self.archive.write_bytes(b"!<arch>\nfixture archive bytes\n")
        self.floor = self.root / "floor.txt"
        self.gate = self.root / "gate.txt"
        self.manifest = self.root / "manifest.json"

    def tearDown(self):
        self.tmp.cleanup()

    def call(self, argv: list[str]) -> tuple[int, str]:
        err = io.StringIO()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
            code = tool.main(argv)
        return code, err.getvalue()

    def describe(self, *extra: str, sha: str | None = None) -> tuple[int, str]:
        return self.call([
            "describe", "--archive", str(self.archive), "--source-sha", sha or self.sha,
            "--source-root", str(self.source), "--floor-out", str(self.floor),
            "--gate-out", str(self.gate), "--output", str(self.manifest),
            "--nm", str(self.nm), "--cxxfilt", str(self.cxxfilt),
            "--check", "view-core=/x/libpulp-view-core.a",
            "--configure=-DCMAKE_BUILD_TYPE=Release", *extra])

    def verify(self, *extra: str) -> tuple[int, str]:
        return self.call(["verify", "--archive", str(self.archive),
                          "--manifest", str(self.manifest), *extra])

    def test_floor_excludes_undefined_and_weak_references(self):
        self.assertEqual(tool.defined_symbols(LISTING), [
            "_ZN4pulp4view12WaveformView5paintEv",
            "__ZN4pulp4view5Meter9set_levelEff",
            "__ZTIN4pulp4view5MeterE",
        ])

    def test_describe_records_archive_floor_and_gate(self):
        code, err = self.describe()
        self.assertEqual(code, 0, err)
        manifest = json.loads(self.manifest.read_text(encoding="utf-8"))
        self.assertEqual(manifest["schema"], tool.SCHEMA)
        self.assertEqual(manifest["source"]["sha"], self.sha)
        self.assertEqual(manifest["archive"]["sha256"], tool.sha256_file(self.archive))
        self.assertEqual(manifest["archive"]["size"], self.archive.stat().st_size)
        self.assertEqual(manifest["configure"], ["-DCMAKE_BUILD_TYPE=Release"])
        self.assertEqual(manifest["symbol_floor"]["symbols"], 3)
        self.assertEqual(manifest["symbol_floor"]["sha256"], tool.sha256_file(self.floor))
        self.assertTrue(all(line.startswith("dm:") for line in self.floor.read_text(encoding="utf-8").splitlines()))
        self.assertEqual(manifest["symbol_floor_gate"]["exit_code"], 0)
        self.assertEqual(manifest["symbol_floor_gate"]["checks"], ["view-core"])
        self.assertIn("--check libpulp-view-core.a", self.gate.read_text(encoding="utf-8"))
        self.assertIsNone(manifest["rebuild"])

    def test_describe_records_a_gate_violation_without_hiding_it(self):
        self.gate_status.write_text("1", encoding="utf-8")
        code, err = self.describe()
        self.assertEqual(code, 0, err)
        self.assertEqual(json.loads(self.manifest.read_text(encoding="utf-8"))["symbol_floor_gate"]["exit_code"], 1)

    def test_describe_fails_when_the_gate_cannot_run(self):
        self.gate_status.write_text("2", encoding="utf-8")
        code, err = self.describe()
        self.assertEqual(code, 2)
        self.assertIn("could not run", err)

    def test_describe_refuses_a_checkout_at_another_commit(self):
        code, err = self.describe(sha="0" * 40)
        self.assertEqual(code, 2)
        self.assertIn("not " + "0" * 40, err)
        self.assertFalse(self.manifest.exists())

    def test_describe_refuses_an_abbreviated_sha(self):
        code, err = self.describe(sha=self.sha[:12])
        self.assertEqual(code, 2)
        self.assertIn("full 40-hex", err)

    def test_describe_refuses_an_archive_that_defines_nothing(self):
        self.listing.write_text(
            "libpulp-view-audio.a[a.cpp.o]: __ZN4pulp4view4View5paintEv U 0 0\n", encoding="utf-8")
        code, err = self.describe()
        self.assertEqual(code, 2)
        self.assertIn("defines no symbol", err)

    def test_describe_compares_a_rebuild(self):
        same = self.root / "same.a"
        same.write_bytes(self.archive.read_bytes())
        self.assertEqual(self.describe("--rebuild-archive", str(same))[0], 0)
        self.assertTrue(json.loads(self.manifest.read_text(encoding="utf-8"))["rebuild"]["identical"])
        other = self.root / "other.a"
        other.write_bytes(b"!<arch>\ndifferent\n")
        self.assertEqual(self.describe("--rebuild-archive", str(other))[0], 0)
        self.assertFalse(json.loads(self.manifest.read_text(encoding="utf-8"))["rebuild"]["identical"])

    def test_verify_accepts_the_described_archive(self):
        self.assertEqual(self.describe()[0], 0)
        code, err = self.verify("--floor", str(self.floor), "--source-sha", self.sha)
        self.assertEqual(code, 0, err)

    def test_verify_rejects_a_substituted_archive(self):
        self.assertEqual(self.describe()[0], 0)
        self.archive.write_bytes(b"!<arch>\nlocally built substitute\n")
        code, err = self.verify()
        self.assertEqual(code, 1)
        self.assertIn("archive sha256", err)

    def test_verify_rejects_another_source_commit(self):
        self.assertEqual(self.describe()[0], 0)
        code, err = self.verify("--source-sha", "f" * 40)
        self.assertEqual(code, 1)
        self.assertIn("!= expected", err)

    def test_verify_rejects_an_edited_floor(self):
        self.assertEqual(self.describe()[0], 0)
        self.floor.write_text("dm:nothing\n", encoding="utf-8")
        code, err = self.verify("--floor", str(self.floor))
        self.assertEqual(code, 1)
        self.assertIn("symbol floor", err)

    def test_verify_rejects_an_unknown_manifest_schema(self):
        self.assertEqual(self.describe()[0], 0)
        manifest = json.loads(self.manifest.read_text(encoding="utf-8"))
        manifest["schema"] = "something-else"
        self.manifest.write_text(json.dumps(manifest), encoding="utf-8")
        self.assertEqual(self.verify()[0], 2)


class WorkflowContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = WORKFLOW.read_text(encoding="utf-8")
        try:
            import yaml
        except ImportError:
            cls.doc = None
        else:
            cls.doc = yaml.safe_load(cls.text)

    def test_only_a_dispatch_with_a_required_source_sha_runs_it(self):
        if self.doc is None:
            self.skipTest("pyyaml not installed")
        triggers = self.doc.get("on", self.doc.get(True))
        self.assertEqual(list(triggers), ["workflow_dispatch"])
        source = triggers["workflow_dispatch"]["inputs"]["source_sha"]
        self.assertTrue(source["required"])

    def test_request_is_pinned_to_a_full_commit_on_main(self):
        self.assertIn("^[0-9a-f]{40}$", self.text)
        self.assertIn('"${GITHUB_REF}" != "refs/heads/main"', self.text)
        self.assertIn("compare/${SOURCE_SHA}...main", self.text)
        self.assertIn("ahead|identical) ;;", self.text)
        self.assertIn("ref: ${{ inputs.source_sha }}", self.text)
        self.assertIn('test "$(git rev-parse HEAD)" = "${SOURCE_SHA}"', self.text)

    def test_build_is_clean_hosted_and_governed(self):
        if self.doc is None:
            self.skipTest("pyyaml not installed")
        job = self.doc["jobs"]["publish"]
        self.assertEqual(job["runs-on"], "macos-15")
        self.assertEqual(job["env"]["ZERO_AR_DATE"], "1")
        self.assertNotIn("RUNS_ON_JSON", self.text)
        self.assertNotIn("actions/cache", self.text)
        self.assertIn("-DPULP_USE_CCACHE=OFF", self.text)
        self.assertIn("tools/ci/governed-build.sh cmake --build build", self.text)
        self.assertNotIn("--parallel", self.text)

    def test_every_published_file_is_attested_and_released_immutably(self):
        if self.doc is None:
            self.skipTest("pyyaml not installed")
        steps = {step.get("name"): step for step in self.doc["jobs"]["publish"]["steps"]}
        attest = steps["Attest the archive and its manifest"]
        self.assertEqual(attest["uses"], "actions/attest@v4")
        subjects = attest["with"]["subject-path"].split()
        for name in ("libpulp-view-audio.a", "pulp-view-audio-archive.manifest.json",
                     "pulp-view-audio-symbol-floor.txt", "SHA256SUMS"):
            self.assertIn(f"out/{name}", subjects)
        self.assertEqual(self.doc["jobs"]["publish"]["env"]["TAG"],
                         "artifact/pulp-view-audio/${{ inputs.source_sha }}")
        publish = steps["Publish the immutable release"]["run"]
        self.assertIn("--draft --prerelease --latest=false", publish)
        self.assertIn("--draft=false --prerelease --latest=false", publish)
        self.assertLess(publish.index("gh release create"), publish.index("--draft=false"))

    def test_a_published_release_is_never_rebuilt(self):
        self.assertIn("already published", self.text)
        self.assertGreaterEqual(self.text.count("if: steps.request.outputs.published != 'true'"), 8)


if __name__ == "__main__":
    unittest.main()
