#!/usr/bin/env python3
"""Tests for tools/ci/vellum_sdk_consumer.py: run selection, archive digest
checks, the compile-command rewrite, and the negative control's verdicts."""

from __future__ import annotations

import hashlib
import io
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import vellum_sdk_consumer as vsc  # noqa: E402


def _run(run_id, *, name=vsc.GPU_WORKFLOW_NAME, event="push", branch="main",
         conclusion="success", created="2026-11-11T10:00:00Z"):
    return {"id": run_id, "name": name, "event": event, "head_branch": branch,
            "conclusion": conclusion, "created_at": created, "head_sha": f"sha{run_id}"}


class PickRunTests(unittest.TestCase):
    def test_newest_successful_push_run_on_main(self):
        runs = [_run(1, created="2026-11-11T09:00:00Z"), _run(2, created="2026-11-11T11:00:00Z"),
                _run(3, created="2026-11-11T12:00:00Z", conclusion="failure"),
                _run(4, created="2026-11-11T13:00:00Z", branch="feature/x"),
                _run(5, created="2026-11-11T14:00:00Z", name="Other workflow"),
                _run(6, created="2026-11-11T15:00:00Z", event="pull_request")]
        self.assertEqual(vsc.pick_run(runs)["id"], 2)

    def test_no_candidate_refuses(self):
        with self.assertRaises(vsc.ResolveError):
            vsc.pick_run([_run(1, conclusion="failure")])


class PickArtifactTests(unittest.TestCase):
    def test_exactly_one_live_evidence_artifact(self):
        art = {"id": 9, "name": vsc.EVIDENCE_ARTIFACT, "expired": False}
        self.assertEqual(vsc.pick_artifact([art, {"id": 8, "name": "other"}])["id"], 9)

    def test_missing_duplicate_or_expired_refuses(self):
        art = {"id": 9, "name": vsc.EVIDENCE_ARTIFACT, "expired": False}
        for artifacts in ([], [art, dict(art, id=10)], [dict(art, expired=True)]):
            with self.assertRaises(vsc.ResolveError):
                vsc.pick_artifact(artifacts)


class VerifyArchiveTests(unittest.TestCase):
    def _dir(self, payload=b"sdk", listed=None):
        d = Path(tempfile.mkdtemp())
        name = "vellum-sdk-0.1.10-darwin-arm64.tar.gz"
        (d / name).write_bytes(payload)
        digest = listed or hashlib.sha256(payload).hexdigest()
        (d / "SHA256SUMS").write_text(f"{digest}  {name}\nabc  install.sh\n", encoding="utf-8")
        return d

    def test_matching_digest_passes(self):
        archive, digest = vsc.verify_archive(self._dir())
        self.assertEqual(digest, hashlib.sha256(b"sdk").hexdigest())
        self.assertTrue(archive.name.endswith(vsc.ARCHIVE_SUFFIX))

    def test_mismatched_digest_refuses(self):
        with self.assertRaises(vsc.ResolveError):
            vsc.verify_archive(self._dir(listed="0" * 64))

    def test_missing_sums_refuses(self):
        d = self._dir()
        (d / "SHA256SUMS").unlink()
        with self.assertRaises(vsc.ResolveError):
            vsc.verify_archive(d)


class SafeExtractTests(unittest.TestCase):
    def test_member_escaping_destination_refuses(self):
        d = Path(tempfile.mkdtemp())
        archive = d / "evil.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            info = tarfile.TarInfo("../escape.txt")
            info.size = 1
            tar.addfile(info, io.BytesIO(b"x"))
        (d / "out").mkdir()
        with self.assertRaises(vsc.ResolveError):
            vsc._safe_extract(archive, d / "out")
        self.assertFalse((d / "escape.txt").exists())


def _zip(files: dict) -> bytes:
    import zipfile
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as bundle:
        for name, data in files.items():
            bundle.writestr(name, data)
    return buffer.getvalue()


class SkipIfUnchangedTests(unittest.TestCase):
    """The nightly run stops early only when the last GREEN run saw this head."""

    def setUp(self):
        self.calls = []
        self.saved = vsc._api
        vsc._api = self.fake

    def tearDown(self):
        vsc._api = self.saved

    def fake(self, path, token, raw=False):
        self.calls.append(path)
        if path.startswith("repos/Generous-Corp/pulp/actions/workflows/"):
            return {"workflow_runs": [{"id": 77}]}
        if path == "repos/Generous-Corp/pulp/actions/runs/77/artifacts":
            return {"artifacts": [{"id": 5, "name": vsc.RECEIPT_ARTIFACT, "expired": False}]}
        if path == "repos/Generous-Corp/pulp/actions/artifacts/5/zip":
            return _zip({"receipt.json": '{"vellum_head_sha": "%s"}' % self.recorded})
        if path.startswith(f"repos/{vsc.VELLUM_REPO}/actions/runs?"):
            return {"workflow_runs": [_run(1)]}
        raise vsc.ResolveError(f"unexpected {path}")

    def _resolve(self):
        import argparse
        out = tempfile.mkdtemp()
        outputs = Path(out) / "outputs"
        args = argparse.Namespace(out_dir=out, run_id=None, previous_receipt=None,
                                  skip_if_unchanged="Generous-Corp/pulp:vellum-sdk-consumer.yml",
                                  github_output=str(outputs))
        import os
        os.environ["GH_TOKEN"] = "t"
        rc = vsc.resolve(args)
        return rc, outputs.read_text(encoding="utf-8") if outputs.exists() else ""

    def test_same_head_reports_unchanged_without_downloading(self):
        self.recorded = "sha1"
        rc, outputs = self._resolve()
        self.assertEqual(rc, 0)
        self.assertIn("unchanged=true", outputs)
        self.assertFalse(any("runs/1/artifacts" in c for c in self.calls))

    def test_new_head_goes_on_to_download(self):
        self.recorded = "sha0"
        rc, outputs = self._resolve()
        # The fake has no Vellum artifact listing, so the full path fails loudly.
        self.assertEqual(rc, vsc.INCONCLUSIVE)
        self.assertNotIn("unchanged=true", outputs)
        self.assertTrue(any("runs/1/artifacts" in c for c in self.calls))

    def test_previous_head_reads_the_green_runs_receipt(self):
        self.recorded = "abc"
        self.assertEqual(vsc.previous_head("Generous-Corp/pulp", "vellum-sdk-consumer.yml", "t"), "abc")


class RedirectTests(unittest.TestCase):
    """Artifact zips redirect to blob storage; the token must not follow."""

    def _redirect(self, new_url):
        import email.message
        import urllib.request
        req = urllib.request.Request("https://api.github.com/repos/x/actions/artifacts/1/zip",
                                     headers={"Authorization": "Bearer secret"})
        new = vsc._DropAuthOnRedirect().redirect_request(req, None, 302, "Found",
                                                         email.message.Message(), new_url)
        return new.get_header("Authorization")

    def test_token_dropped_on_a_cross_host_redirect(self):
        self.assertIsNone(self._redirect("https://blob.core.windows.net/artifact.zip?sig=x"))

    def test_token_kept_on_a_same_host_redirect(self):
        self.assertEqual(self._redirect("https://api.github.com/repos/x/other"), "Bearer secret")


class TokenTests(unittest.TestCase):
    def test_no_token_fails_loudly(self):
        with self.assertRaises(vsc.ResolveError):
            vsc._api("repos/x", "")


NINJA_LINE = ("/opt/homebrew/bin/cmake -E env CCACHE_SLOPPINESS=pch_defines,time_macros "
              "/opt/homebrew/bin/ccache /usr/bin/c++ -DX=1 -isystem /sdk/mirror/include "
              "-I/sdk/include -std=gnu++20 -MD -MT obj.o -MF obj.o.d -o obj.o "
              "-c /repo/test/editor_host_slice/vellum_ui/knob_panel.cpp")
SOURCE = "test/editor_host_slice/vellum_ui/knob_panel.cpp"


class SyntaxOnlyTests(unittest.TestCase):
    def test_rewrite_drops_launcher_env_and_outputs(self):
        argv = vsc.syntax_only(NINJA_LINE, SOURCE).argv
        self.assertEqual(argv[0], "/usr/bin/c++")
        self.assertIn("-fsyntax-only", argv)
        for gone in ("-c", "-o", "-MD", "-MF", "-MT", "obj.o", "obj.o.d"):
            self.assertNotIn(gone, argv)
        self.assertEqual(argv[-1], "/repo/" + SOURCE)

    def test_command_for_another_source_refuses(self):
        with self.assertRaises(ValueError):
            vsc.syntax_only(NINJA_LINE, "other.cpp")

    def test_substitute_replaces_the_mirror_include_only(self):
        compile_ = vsc.syntax_only(NINJA_LINE, SOURCE)
        broken = vsc.substitute(compile_, "/sdk/mirror/include", "/tmp/broken/include").argv
        self.assertIn("/tmp/broken/include", broken)
        self.assertNotIn("/sdk/mirror/include", broken)
        self.assertIn("-I/sdk/include", broken)

    def test_substitute_joined_flag(self):
        compile_ = vsc.Compile(["c++", "-I/sdk/mirror/include", "-fsyntax-only", SOURCE])
        self.assertIn("-I/new", vsc.substitute(compile_, "/sdk/mirror/include", "/new").argv)

    def test_substitute_without_the_include_refuses(self):
        with self.assertRaises(ValueError):
            vsc.substitute(vsc.Compile(["c++", "-fsyntax-only", SOURCE]), "/sdk/mirror/include", "/x")


class JudgeTests(unittest.TestCase):
    HEADER = "vellum/canvas/canvas.hpp"
    MISSING = "mirror_spelling.hpp:23:10: fatal error: 'pulp/canvas/canvas.hpp' file not found"

    def test_confirmed_when_break_fails_on_the_renamed_header(self):
        self.assertEqual(vsc.judge(0, 1, self.MISSING, self.HEADER)[0], vsc.CONFIRMED)

    def test_not_confirmed_when_the_break_still_compiles(self):
        self.assertEqual(vsc.judge(0, 0, "", self.HEADER)[0], vsc.NOT_CONFIRMED)

    def test_inconclusive_when_the_clean_compile_fails(self):
        self.assertEqual(vsc.judge(1, 1, self.MISSING, self.HEADER)[0], vsc.INCONCLUSIVE)

    def test_inconclusive_when_the_break_fails_for_another_reason(self):
        other = "knob_panel.cpp:5:3: error: use of undeclared identifier 'oops'"
        self.assertEqual(vsc.judge(0, 1, other, self.HEADER)[0], vsc.INCONCLUSIVE)

    def test_another_missing_header_is_not_this_break(self):
        other = "fatal error: 'vellum/view/view.hpp' file not found"
        self.assertEqual(vsc.judge(0, 1, other, self.HEADER)[0], vsc.INCONCLUSIVE)


class _StandInSdk:
    """The real compile path against a stand-in SDK, with the system compiler."""

    def setUp(self):
        import shutil
        self.cxx = shutil.which("c++") or shutil.which("clang++")
        if not self.cxx or not shutil.which("ninja"):
            self.skipTest("needs c++ and ninja")
        self.root = Path(tempfile.mkdtemp())
        inc = self.root / "sdk/mirror/include/vellum/canvas"
        inc.mkdir(parents=True)
        (inc / "canvas.hpp").write_text("#pragma once\nint canvas_value();\n", encoding="utf-8")
        (self.root / "src").mkdir()
        self.source = "src/consumer.cpp"
        (self.root / self.source).write_text(self.body, encoding="utf-8")
        build = self.root / "build"
        build.mkdir()
        (build / "build.ninja").write_text(
            "rule cxx\n  command = {cxx} -isystem {inc} -std=c++17 -MD -MF $out.d -o $out -c $in\n"
            "build consumer.o: cxx {src}\nbuild target: phony consumer.o\n".format(
                cxx=self.cxx, inc=self.root / "sdk/mirror/include", src=self.root / self.source),
            encoding="utf-8")
        self.args = ["negative-control", "--build-dir", str(build), "--prefix", str(self.root / "sdk"),
                     "--target", "target", "--source", self.source, "--header", "vellum/canvas/canvas.hpp"]



class NegativeControlConsumerTests(_StandInSdk, unittest.TestCase):
    body = "#include <vellum/canvas/canvas.hpp>\nint use() { return canvas_value(); }\n"

    def test_a_consumer_of_the_header_is_confirmed(self):
        self.assertEqual(vsc.main(self.args), vsc.CONFIRMED)
        self.assertTrue((self.root / "sdk/mirror/include/vellum/canvas/canvas.hpp").is_file(),
                        "the control must never modify the published SDK")


class NegativeControlUnusedHeaderTests(_StandInSdk, unittest.TestCase):
    body = "int use() { return 0; }\n"

    def test_a_tu_that_does_not_include_the_header_is_not_confirmed(self):
        self.assertEqual(vsc.main(self.args), vsc.NOT_CONFIRMED)


if __name__ == "__main__":
    unittest.main()
