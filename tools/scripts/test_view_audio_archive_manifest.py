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
import os
import pathlib
import re
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
    "libpulp-view-audio.a[waveform.cpp.o]: __ZTVN4pulp4view12WaveformViewE W 0 0\n"
    "libpulp-view-audio.a[waveform.cpp.o]: __ZN4pulp4view6InlineEv V 0 0\n"
    "libpulp-view-audio.a[waveform.cpp.o]: __ZN4pulp4view6IndirectEv I 0 0\n"
    "libpulp-view-audio.a[waveform.cpp.o]:\n"
)


def write_exe(path: pathlib.Path, body: str) -> pathlib.Path:
    path.write_text(f"#!{sys.executable}\n{body}", encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


class ManifestFixture(unittest.TestCase):
    """A fake nm, demangler and source checkout, plus describe/verify helpers."""

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
            "print('nm: ' + sys.argv[sys.argv.index('--nm') + 1])\n"
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
        for flag, value in (("--source-sha", self.sha), ("--floor", str(self.floor)),
                            ("--gate", str(self.gate))):
            if flag not in extra:
                extra = (*extra, flag, value)
        return self.call(["verify", "--archive", str(self.archive),
                          "--manifest", str(self.manifest), *extra])

    def build_dir(self, name: str) -> pathlib.Path:
        directory = self.root / name
        (directory / "core/view").mkdir(parents=True, exist_ok=True)
        (directory / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Release\n", encoding="utf-8")
        return directory

    def rebuild(self, first: str, second: str, data: bytes) -> tuple[int, str]:
        rebuilt = self.build_dir(second) / "core/view/libpulp-view-audio.a"
        rebuilt.write_bytes(data)
        return self.describe("--build-dir", str(self.build_dir(first)),
                             "--rebuild-dir", str(self.root / second), "--rebuild-archive", str(rebuilt))


class ManifestTests(ManifestFixture):
    def test_floor_keeps_weak_definitions_and_drops_references(self):
        self.assertEqual(tool.defined_symbols(LISTING), [
            "_ZN4pulp4view12WaveformView5paintEv",
            "__ZN4pulp4view5Meter9set_levelEff",
            "__ZN4pulp4view6InlineEv",
            "__ZTIN4pulp4view5MeterE",
            "__ZTVN4pulp4view12WaveformViewE",
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
        self.assertEqual(manifest["symbol_floor"]["symbols"], 5)
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

    def test_describe_derives_the_rebuild_kind_from_its_directories(self):
        self.assertEqual(self.rebuild("build", "build-rebuild", self.archive.read_bytes())[0], 0)
        rebuild = json.loads(self.manifest.read_text(encoding="utf-8"))["rebuild"]
        self.assertTrue(rebuild["identical"])
        self.assertEqual(rebuild["kind"], "separate-build-dir")
        self.assertEqual((rebuild["build_dir"], rebuild["rebuild_dir"]), ("build", "build-rebuild"))
        self.assertEqual(self.rebuild("build", "build", b"!<arch>\ndifferent\n")[0], 0)
        rebuild = json.loads(self.manifest.read_text(encoding="utf-8"))["rebuild"]
        self.assertFalse(rebuild["identical"])
        self.assertEqual(rebuild["kind"], "same-build-dir")

    def test_verify_can_require_an_identical_separate_rebuild(self):
        self.assertEqual(self.describe()[0], 0)
        code, err = self.verify("--require-identical-rebuild")
        self.assertEqual(code, 1)
        self.assertIn("no identical rebuild", err)
        self.assertEqual(self.rebuild("build", "build-rebuild", b"!<arch>\ndifferent\n")[0], 0)
        self.assertEqual(self.verify("--require-identical-rebuild")[0], 1)
        self.assertEqual(self.rebuild("build", "build", self.archive.read_bytes())[0], 0)
        self.assertEqual(self.verify("--require-identical-rebuild")[0], 1)
        self.assertEqual(self.rebuild("build", "build-rebuild", self.archive.read_bytes())[0], 0)
        self.assertEqual(self.verify("--require-identical-rebuild")[0], 0)

    def test_describe_refuses_a_rebuild_it_cannot_place(self):
        stray = self.root / "stray.a"
        stray.write_bytes(self.archive.read_bytes())
        code, err = self.describe("--rebuild-archive", str(stray))
        self.assertEqual(code, 2)
        self.assertIn("--build-dir and --rebuild-dir", err)
        code, err = self.describe("--build-dir", str(self.build_dir("build")),
                                  "--rebuild-dir", str(self.build_dir("build-rebuild")),
                                  "--rebuild-archive", str(stray))
        self.assertEqual(code, 2)
        self.assertIn("was not produced in", err)
        code, err = self.describe("--build-dir", str(self.build_dir("build")),
                                  "--rebuild-dir", str(self.root), "--rebuild-archive", str(stray))
        self.assertEqual(code, 2)
        self.assertIn("not a configured build directory", err)

    def test_verify_requires_the_expected_source_commit(self):
        self.assertEqual(self.describe()[0], 0)
        with self.assertRaises(SystemExit) as caught, contextlib.redirect_stderr(io.StringIO()):
            tool.main(["verify", "--archive", str(self.archive), "--manifest", str(self.manifest)])
        self.assertEqual(caught.exception.code, 2)

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

    def test_verify_rejects_a_size_mismatch(self):
        self.assertEqual(self.describe()[0], 0)
        manifest = json.loads(self.manifest.read_text(encoding="utf-8"))
        manifest["archive"]["size"] += 1
        self.manifest.write_text(json.dumps(manifest), encoding="utf-8")
        code, err = self.verify()
        self.assertEqual(code, 1)
        self.assertIn("archive size", err)

    def test_verify_rejects_an_edited_gate_output(self):
        self.assertEqual(self.describe()[0], 0)
        self.gate.write_text("archive-symbol-closure: view-core: clean\n", encoding="utf-8")
        code, err = self.verify("--gate", str(self.gate))
        self.assertEqual(code, 1)
        self.assertIn("gate sha256", err)

    def test_verify_can_require_a_passing_gate(self):
        self.gate_status.write_text("1", encoding="utf-8")
        self.assertEqual(self.describe()[0], 0)
        self.assertEqual(self.verify()[0], 0)
        code, err = self.verify("--require-gate-pass")
        self.assertEqual(code, 1)
        self.assertIn("exit code is 1", err)
        self.gate_status.write_text("0", encoding="utf-8")
        self.assertEqual(self.describe()[0], 0)
        self.assertEqual(self.verify("--gate", str(self.gate), "--require-gate-pass")[0], 0)

    def test_verify_rejects_an_emptied_floor(self):
        self.assertEqual(self.describe()[0], 0)
        self.floor.write_text("", encoding="utf-8")
        code, err = self.verify()
        self.assertEqual(code, 1)
        self.assertIn("has 0 symbols", err)
        # Even with the manifest rewritten to match, an empty floor is refused.
        manifest = json.loads(self.manifest.read_text(encoding="utf-8"))
        manifest["symbol_floor"].update(sha256=tool.sha256_file(self.floor), symbols=0)
        self.manifest.write_text(json.dumps(manifest), encoding="utf-8")
        code, err = self.verify()
        self.assertEqual(code, 1)
        self.assertIn("has 0 symbols", err)

    def test_verify_needs_the_floor_and_the_gate(self):
        self.assertEqual(self.describe()[0], 0)
        for missing in ("--floor", "--gate", "--source-sha"):
            argv = ["verify", "--archive", str(self.archive), "--manifest", str(self.manifest)]
            for flag, value in (("--floor", str(self.floor)), ("--gate", str(self.gate)),
                                ("--source-sha", self.sha)):
                if flag != missing:
                    argv += [flag, value]
            with self.subTest(missing=missing), self.assertRaises(SystemExit) as caught, \
                    contextlib.redirect_stderr(io.StringIO()):
                tool.main(argv)
            self.assertEqual(caught.exception.code, 2)

    def test_verify_rejects_a_malformed_recorded_sha(self):
        self.assertEqual(self.describe()[0], 0)
        manifest = json.loads(self.manifest.read_text(encoding="utf-8"))
        manifest["source"]["sha"] = self.sha[:12]
        self.manifest.write_text(json.dumps(manifest), encoding="utf-8")
        code, err = self.verify("--source-sha", self.sha[:12])
        self.assertEqual(code, 1)
        self.assertIn("is not a full commit", err)

    def test_describe_refuses_a_demangler_that_drops_lines(self):
        short = write_exe(self.root / "short-cxxfilt", (
            "import sys\n"
            "print('dm:only-one')\n"))
        code, err = self.call([
            "describe", "--archive", str(self.archive), "--source-sha", self.sha,
            "--source-root", str(self.source), "--floor-out", str(self.floor),
            "--gate-out", str(self.gate), "--output", str(self.manifest),
            "--nm", str(self.nm), "--cxxfilt", str(short),
            "--check", "view-core=/x/libpulp-view-core.a"])
        self.assertEqual(code, 2)
        self.assertIn("one-to-one", err)
        self.assertFalse(self.manifest.exists())

    def test_the_gate_gets_an_absolute_nm_path(self):
        # The gate runs from the source checkout, so a relative nm path that
        # works from the caller's directory must be resolved first.
        cwd = pathlib.Path.cwd()
        os.chdir(self.root)
        try:
            code, err = self.describe("--nm", "./nm")
        finally:
            os.chdir(cwd)
        self.assertEqual(code, 0, err)
        gate = self.gate.read_text(encoding="utf-8")
        self.assertIn(f"nm: {self.nm.resolve()}\n", gate)

    def test_verify_rejects_an_unknown_manifest_schema(self):
        self.assertEqual(self.describe()[0], 0)
        manifest = json.loads(self.manifest.read_text(encoding="utf-8"))
        manifest["schema"] = "something-else"
        self.manifest.write_text(json.dumps(manifest), encoding="utf-8")
        self.assertEqual(self.verify()[0], 2)


class VerifyReleaseTests(ManifestFixture):
    """verify-release over a release directory assembled from a real describe."""

    def setUp(self):
        super().setUp()
        self.release = self.root / "release"
        self.release.mkdir()
        self.gh_log = self.root / "gh.log"
        self.gh_status = self.root / "gh.status"
        self.gh_status.write_text("0", encoding="utf-8")
        self.gh = write_exe(self.root / "gh", (
            "import os, pathlib, sys\n"
            f"log = pathlib.Path({str(self.gh_log)!r})\n"
            "token = 'token' if os.environ.get('GH_TOKEN') or os.environ.get('GITHUB_TOKEN') else 'no-token'\n"
            "log.open('a').write(token + ' ' + ' '.join(sys.argv[1:]) + '\\n')\n"
            f"raise SystemExit(int(pathlib.Path({str(self.gh_status)!r}).read_text()))\n"))
        self.assertEqual(self.rebuild("build", "build-rebuild", self.archive.read_bytes())[0], 0)
        names = {self.archive: "libpulp-view-audio.a", self.manifest: "pulp-view-audio-archive.manifest.json",
                 self.floor: "pulp-view-audio-symbol-floor.txt", self.gate: "pulp-view-audio-symbol-floor-gate.txt"}
        lines = []
        for path, name in names.items():
            (self.release / name).write_bytes(path.read_bytes())
            lines.append(f"{tool.sha256_file(path)}  {name}")
        (self.release / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="utf-8")
        (self.release / tool.BUNDLE).write_text("{}\n", encoding="utf-8")

    def check(self, sha=None) -> tuple[int, str]:
        return self.call(["verify-release", "--dir", str(self.release), "--source-sha", sha or self.sha,
                          "--repository", "Generous-Corp/pulp", "--gh", str(self.gh)])

    def test_a_complete_release_verifies_every_attested_file_without_a_token(self):
        os.environ["GH_TOKEN"] = "must-not-reach-gh"
        try:
            code, err = self.check()
        finally:
            del os.environ["GH_TOKEN"]
        self.assertEqual(code, 0, err)
        calls = self.gh_log.read_text(encoding="utf-8").splitlines()
        self.assertEqual(len(calls), 5)
        self.assertEqual(sorted(call.split()[3].rsplit("/", 1)[-1] for call in calls), sorted(tool.ATTESTED))
        for call in calls:
            self.assertTrue(call.startswith("no-token attestation verify "), call)
            for flag in ("--bundle", "--source-ref refs/heads/main", f"--source-digest {self.sha}",
                         "--signer-workflow Generous-Corp/pulp/.github/workflows/view-audio-archive.yml",
                         "--deny-self-hosted-runners"):
                self.assertIn(flag, call)

    def test_a_missing_or_extra_asset_is_refused(self):
        (self.release / "extra.a").write_bytes(b"x")
        self.assertEqual(self.check()[0], 1)
        (self.release / "extra.a").unlink()
        (self.release / tool.BUNDLE).unlink()
        self.assertEqual(self.check()[0], 1)
        self.assertFalse(self.gh_log.exists())

    def test_a_checksum_mismatch_is_refused(self):
        (self.release / "pulp-view-audio-symbol-floor.txt").write_text("dm:other\n", encoding="utf-8")
        code, err = self.check()
        self.assertEqual(code, 1)
        self.assertIn("SHA256SUMS does not hold for pulp-view-audio-symbol-floor.txt", err)

    def test_a_rejected_attestation_is_refused(self):
        self.gh_status.write_text("1", encoding="utf-8")
        code, err = self.check()
        self.assertEqual(code, 1)
        self.assertIn("attestation rejected", err)

    def test_another_commit_is_refused(self):
        code, err = self.check("f" * 40)
        self.assertEqual(code, 1)
        self.assertIn("!= expected", err)


class NmSymbolsTests(unittest.TestCase):
    LISTING = (
        "lib.a[a.o]:\n"
        "lib.a[a.o]: strong T 0 0\n"
        "lib.a[a.o]: weak W 0 0\n"
        "lib.a[a.o]: weakobj V 0 0\n"
        "lib.a[a.o]: needed U 0 0\n"
        "lib.a[a.o]: weakref w 0 0\n"
        "lib.a[a.o]: indirect I 0 0\n"
        "plain D 0 0\n"
    )

    def test_weak_definitions_count_only_when_asked_for(self):
        sys.path.insert(0, str(MODULE_PATH.parent))
        import nm_symbols
        self.assertEqual(nm_symbols.defined(self.LISTING, include_weak=False), {"strong", "plain"})
        self.assertEqual(nm_symbols.defined(self.LISTING, include_weak=True),
                         {"strong", "plain", "weak", "weakobj"})
        self.assertEqual(nm_symbols.undefined(self.LISTING), {"needed", "weakref"})
        self.assertEqual([m for m, _, _ in nm_symbols.entries(self.LISTING)][:2], ["a.o", "a.o"])


SOURCE = "a" * 40
BUNDLE_ASSET = {"name": "pulp-view-audio-archive.sigstore.json", "digest": "sha256:bundle"}


class ReleaseCheckTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.out = pathlib.Path(self.tmp.name) / "out"
        self.out.mkdir()
        for name in ("libpulp-view-audio.a", "SHA256SUMS"):
            (self.out / name).write_bytes(name.encode())
        self.ours = tool.asset_digests(self.out)
        self.assets = [{"name": n, "digest": d} for n, d in self.ours.items()]

    def tearDown(self):
        self.tmp.cleanup()

    def draft(self, **overrides):
        row = {"id": 7, "draft": True, "target_commitish": SOURCE,
               "assets": self.assets + [BUNDLE_ASSET]}
        row.update(overrides)
        return tool.draft_problems([row], self.ours, SOURCE)

    def test_no_release_needs_a_fresh_draft(self):
        self.assertEqual(tool.draft_problems([], self.ours, SOURCE), ([], False))

    def test_a_draft_holding_exactly_these_bytes_is_reused(self):
        self.assertEqual(self.draft(), ([], True))

    def test_a_draft_at_another_commit_is_refused(self):
        problems, reusable = self.draft(target_commitish="main")
        self.assertFalse(reusable)
        self.assertIn("draft targets 'main'", problems[0])

    def test_a_draft_with_an_extra_asset_is_refused(self):
        problems, reusable = self.draft(assets=self.assets + [BUNDLE_ASSET, {"name": "evil.a", "digest": "x"}])
        self.assertFalse(reusable)
        self.assertTrue(any("draft assets" in p for p in problems), problems)

    def test_a_draft_missing_an_asset_is_refused(self):
        problems, _ = self.draft(assets=self.assets)
        self.assertTrue(any("draft assets" in p for p in problems), problems)

    def test_a_draft_with_other_bytes_is_refused(self):
        other = [{"name": "libpulp-view-audio.a", "digest": "sha256:other"},
                 {"name": "SHA256SUMS", "digest": self.ours["SHA256SUMS"]}, BUNDLE_ASSET]
        problems, _ = self.draft(assets=other)
        self.assertTrue(any("other bytes for ['libpulp-view-audio.a']" in p for p in problems), problems)

    def test_a_published_release_or_two_releases_are_refused(self):
        self.assertIn("already published", self.draft(draft=False)[0][0])
        row = {"id": 1, "draft": True, "target_commitish": SOURCE, "assets": []}
        self.assertIn("2 releases", tool.draft_problems([row, row], self.ours, SOURCE)[0][0])

    def release(self, tag=f"commit\t{SOURCE}\n", **overrides):
        row = {"immutable": True, "draft": False, "prerelease": True, "assets": self.assets}
        row.update(overrides)
        return tool.release_problems([row], tag, self.ours, SOURCE)

    def test_the_published_release_must_be_exactly_ours(self):
        self.assertEqual(self.release(), [])
        self.assertIn("release immutable is False", self.release(immutable=False))
        self.assertIn("release draft is True", self.release(draft=True))
        self.assertIn("release prerelease is False", self.release(prerelease=False))
        self.assertTrue(self.release(tag=f"commit\t{'b' * 40}\n")[0].startswith("tag resolves"))
        self.assertTrue(self.release(tag=f"tag\t{SOURCE}\n")[0].startswith("tag resolves"))
        extra = self.release(assets=self.assets + [{"name": "evil.a", "digest": "x"}])
        self.assertTrue(any("published assets" in p for p in extra), extra)
        changed = self.release(assets=[{"name": "libpulp-view-audio.a", "digest": "sha256:x"},
                                       {"name": "SHA256SUMS", "digest": self.ours["SHA256SUMS"]}])
        self.assertTrue(any("published assets" in p for p in changed), changed)

    def test_the_cli_writes_the_draft_output_and_exits_nonzero_on_refusal(self):
        releases = pathlib.Path(self.tmp.name) / "releases.json"
        releases.write_text("[]", encoding="utf-8")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = tool.main(["check-draft", "--releases", str(releases), "--out", str(self.out),
                              "--source-sha", SOURCE])
        self.assertEqual((code, out.getvalue()), (0, "draft=false\n"))
        releases.write_text(json.dumps([{"id": 1, "draft": False}]), encoding="utf-8")
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(tool.main(["check-draft", "--releases", str(releases), "--out", str(self.out),
                                        "--source-sha", SOURCE]), 1)


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

    def test_it_builds_only_its_own_commit_of_main(self):
        if self.doc is None:
            self.skipTest("pyyaml not installed")
        triggers = self.doc.get("on", self.doc.get(True))
        # Dispatch only: a push trigger would publish a prerelease per commit.
        self.assertEqual(triggers, {"workflow_dispatch": {}})
        # No input can name another commit, and no checkout overrides the ref.
        self.assertNotIn("inputs.", self.text)
        self.assertEqual(self.doc["env"]["SOURCE_SHA"], "${{ github.sha }}")
        self.assertEqual(self.doc["env"]["TAG"], "artifact/pulp-view-audio/${{ github.sha }}")
        for job in self.doc["jobs"].values():
            for step in job["steps"]:
                if str(step.get("uses", "")).startswith("actions/checkout@"):
                    self.assertNotIn("ref", step.get("with", {}), step)
                    self.assertNotIn("repository", step.get("with", {}), step)
                    self.assertFalse(step["with"]["persist-credentials"], step)
        self.assertIn('"${GITHUB_REF}" != "refs/heads/main"', self.text)
        self.assertIn('test "$(git rev-parse HEAD)" = "${SOURCE_SHA}"', self.text)

    def jobs(self):
        if self.doc is None:
            self.skipTest("pyyaml not installed")
        return self.doc["jobs"]

    @staticmethod
    def steps(job):
        return {step.get("name"): step for step in job["steps"]}

    def test_build_is_clean_hosted_governed_and_holds_no_credentials(self):
        job = self.jobs()["build"]
        self.assertEqual(job["runs-on"], "macos-15")
        self.assertEqual(job["permissions"], {"contents": "read"})
        self.assertEqual(job["env"]["ZERO_AR_DATE"], "1")
        self.assertEqual(job["env"]["CCACHE_DISABLE"], "1")
        body = job_text(job)
        for credential in ("GH_TOKEN", "github.token", "secrets.", "id-token", "actions/attest",
                           "gh release", "gh api"):
            self.assertNotIn(credential, body)
        self.assertNotIn("RUNS_ON_JSON", self.text)
        self.assertNotIn("actions/cache", self.text)
        self.assertIn("-DPULP_USE_CCACHE=OFF", body)
        self.assertIn("tools/ci/governed-build.sh cmake --build build ", body)
        self.assertNotIn("--parallel", self.text)

    def test_rebuild_uses_a_separate_build_directory(self):
        steps = self.steps(self.jobs()["build"])
        rebuild = steps["Rebuild in a separate build directory"]["run"]
        self.assertIn("cmake -S . -B build-rebuild", rebuild)
        self.assertIn("--build build-rebuild", rebuild)
        self.assertNotIn("rm ", rebuild)
        describe = steps["Describe the archive"]["run"]
        self.assertIn("--rebuild-dir source/build-rebuild", describe)
        self.assertIn("--rebuild-archive source/build-rebuild/", describe)

    def test_a_failing_floor_gate_stops_before_publish(self):
        describe = self.steps(self.jobs()["build"])["Describe the archive"]["run"]
        self.assertIn("--require-gate-pass", describe)
        self.assertLess(describe.index("--require-gate-pass"), describe.index("SHA256SUMS"))
        reverify = self.steps(self.jobs()["publish"])["Re-verify the build's bytes"]["run"]
        for flag in ("sha256sum -c SHA256SUMS", "--source-sha", "--gate", "--require-gate-pass"):
            self.assertIn(flag, reverify)

    def test_only_the_publish_job_can_sign_or_write(self):
        jobs = self.jobs()
        self.assertEqual(jobs["request"]["permissions"], {"contents": "read"})
        self.assertEqual(jobs["publish"]["permissions"],
                         {"contents": "write", "id-token": "write", "attestations": "write"})
        self.assertEqual(jobs["publish"]["needs"], ["request", "build"])
        self.assertNotIn("GH_TOKEN", jobs["publish"].get("env", {}))
        self.assertNotIn("GH_TOKEN", self.doc.get("env", {}))
        names = [step.get("name") for step in jobs["publish"]["steps"]]
        self.assertLess(names.index("Re-verify the build's bytes"),
                        names.index("Attest the archive and its manifest"))
        self.assertLess(names.index("Refuse a tag or draft that points elsewhere"),
                        names.index("Stage the release as a draft"))

    def test_the_draft_is_verified_before_the_one_publishing_step(self):
        steps = self.jobs()["publish"]["steps"]
        names = [step.get("name") for step in steps]
        # Publishing is the last mutation; only a read-back check follows it.
        self.assertEqual(names[-3:], ["Verify the draft before publishing", "Publish the release",
                                      "Check the published release"])
        flips = [step["name"] for step in steps if "--draft=false" in step.get("run", "")]
        self.assertEqual(flips, ["Publish the release"])
        self.assertEqual(self.steps(self.jobs()["publish"])["Publish the release"]["run"].count("gh "), 1)
        verify = self.steps(self.jobs()["publish"])["Verify the draft before publishing"]["run"]
        for needle in ("git/ref/tags/${TAG}", 'printf \'commit\\t%s\' "${SOURCE_SHA}"',
                       "check-draft", '"draft=true"', 'gh release download "${TAG}"',
                       'verify-release \\\n  --dir "${RUNNER_TEMP}/draft-assets" --source-sha "${SOURCE_SHA}"'):
            self.assertIn(needle, verify)

    def test_every_published_file_is_attested_and_released_as_a_draft_first(self):
        steps = self.steps(self.jobs()["publish"])
        attest = steps["Attest the archive and its manifest"]
        self.assertTrue(attest["uses"].startswith("actions/attest@"))
        subjects = attest["with"]["subject-path"].split()
        self.assertEqual(sorted(p.split("/", 1)[1] for p in subjects), sorted(tool.ATTESTED))
        self.assertNotIn("TAG", self.jobs()["publish"]["env"])
        stage = steps["Stage the release as a draft"]["run"]
        self.assertIn("--verify-tag --target \"${SOURCE_SHA}\"", stage)
        self.assertIn("--draft --prerelease --latest=false", stage)
        self.assertLess(stage.index('-f ref="refs/tags/${TAG}" -f sha="${SOURCE_SHA}"'),
                        stage.index("gh release create"))
        self.assertNotIn("--draft=false", stage)

    def test_an_existing_tag_or_draft_must_point_at_the_source(self):
        existing = self.steps(self.jobs()["publish"])["Refuse a tag or draft that points elsewhere"]["run"]
        self.assertIn("git/ref/tags/${TAG}", existing)
        self.assertIn('"${sha}" != "${SOURCE_SHA}"', existing)
        self.assertIn("view_audio_archive_manifest.py check-draft", existing)
        self.assertNotIn("--clobber", self.text)
        self.assertNotIn("gh release upload", self.text)
        stage = self.steps(self.jobs()["publish"])["Stage the release as a draft"]["run"]
        draft_path = stage.split('if [ "${DRAFT_EXISTS}" = "true" ]; then', 1)[1].split("else", 1)[0]
        self.assertIn('--title "${title}" --notes "${notes}"', draft_path)
        checked = self.steps(self.jobs()["publish"])["Check the published release"]["run"]
        self.assertIn("git/ref/tags/${TAG}", checked)
        self.assertIn("view_audio_archive_manifest.py check-release", checked)

    def test_an_existing_published_release_is_verified_not_trusted(self):
        request = self.steps(self.jobs()["request"])["Check the request"]["run"]
        published = request.split('[ "${state}" = "false" ]; then', 1)[1].split("else", 1)[0]
        for needle in ("gh release download", "verify-release", "check-release", "published=true"):
            self.assertIn(needle, published)
        self.assertLess(published.index("verify-release"), published.index("published=true"))

    def test_the_documented_check_is_token_free_and_covers_every_attested_file(self):
        header = self.text.split("\non:", 1)[0]
        for needle in ("verify-release", "--bundle pulp-view-audio-archive.sigstore.json",
                       "--source-ref refs/heads/main --source-digest <expected commit>",
                       "--deny-self-hosted-runners", "--floor --gate --source-sha --require-gate-pass",
                       "--require-identical-rebuild", *tool.ATTESTED):
            self.assertIn(needle, header)

    def test_the_build_job_restores_no_cache(self):
        # The build runs this commit's setup.sh and build with the runner's
        # cache token in reach of the actions it uses. That is harmless only
        # while nothing in the job reads or writes the Actions cache.
        for step in self.jobs()["build"]["steps"]:
            uses = str(step.get("uses", ""))
            self.assertFalse(uses.startswith(("actions/cache", "namespacelabs/")), uses)
            if "/setup-" in uses:
                self.fail(f"{uses}: setup actions can restore a cache; the build job uses none")
            for key in (step.get("with") or {}):
                self.assertNotIn(key, ("cache", "cache-dependency-path", "actions-cache-folder",
                                       "package-manager-cache", "bundler-cache"), uses)
            self.assertNotIn("ACTIONS_CACHE", step.get("run", ""))
            self.assertNotIn("ACTIONS_RUNTIME_TOKEN", step.get("run", ""))

    def test_every_action_is_pinned_to_a_commit(self):
        uses = re.findall(r"^\s*(?:-\s*)?uses:\s*(\S+)(.*)$", self.text, re.M)
        self.assertGreaterEqual(len(uses), 5)
        for ref, comment in uses:
            self.assertRegex(ref, r"@[0-9a-f]{40}$", ref)
            self.assertRegex(comment.strip(), r"^# v\d+(\.\d+)*$", ref)

    def test_both_verifications_require_an_identical_rebuild(self):
        build = self.steps(self.jobs()["build"])["Describe the archive"]["run"]
        publish = self.steps(self.jobs()["publish"])["Re-verify the build's bytes"]["run"]
        for body in (build, publish):
            self.assertIn("--require-identical-rebuild", body)

    def test_a_published_release_is_never_rebuilt(self):
        self.assertIn("already published", self.text)
        for job in ("build", "publish"):
            self.assertEqual(self.jobs()[job]["if"], "needs.request.outputs.published != 'true'")


def job_text(value) -> str:
    """The job's every string, unwrapped, newline-joined."""
    parts = []

    def walk(node):
        if isinstance(node, dict):
            for key, item in node.items():
                parts.append(str(key))
                walk(item)
        elif isinstance(node, list):
            for item in node:
                walk(item)
        else:
            parts.append(str(node))

    walk(value)
    return "\n".join(parts)

if __name__ == "__main__":
    unittest.main()
