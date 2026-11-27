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


class SelectRunTests(unittest.TestCase):
    @staticmethod
    def _fetch(having):
        def fetch(run):
            return [{"id": run["id"], "name": vsc.EVIDENCE_ARTIFACT, "expired": False}] if run["id"] in having else []
        return fetch

    def test_newest_run_without_the_artifact_is_passed_over(self):
        runs = [_run(1, created="2026-11-11T09:00:00Z"), _run(2, created="2026-11-11T10:00:00Z")]
        run, artifact = vsc.select_run(runs, self._fetch({1}))
        self.assertEqual((run["id"], artifact["id"]), (1, 1))

    def test_newest_run_with_the_artifact_wins(self):
        runs = [_run(1, created="2026-11-11T09:00:00Z"), _run(2, created="2026-11-11T10:00:00Z")]
        self.assertEqual(vsc.select_run(runs, self._fetch({1, 2}))[0]["id"], 2)

    def test_expired_artifact_does_not_count(self):
        runs = [_run(1)]
        with self.assertRaises(vsc.ResolveError):
            vsc.select_run(runs, lambda r: [{"id": 1, "name": vsc.EVIDENCE_ARTIFACT, "expired": True}])

    def test_none_in_range_fails_clearly(self):
        runs = [_run(i, created=f"2026-11-11T{i:02d}:00:00Z") for i in range(1, 6)]
        with self.assertRaisesRegex(vsc.ResolveError, "none of the newest 5"):
            vsc.select_run(runs, self._fetch(set()))

    def test_search_is_bounded(self):
        runs = [_run(i, created=f"2026-11-11T{i:02d}:00:00Z") for i in range(1, 6)]
        with self.assertRaisesRegex(vsc.ResolveError, "none of the newest 2"):
            vsc.select_run(runs, self._fetch({1}), limit=2)

    def test_artifact_after_first_ten_green_runs_is_selected(self):
        runs = [_run(i, created=f"2026-11-11T{i:02d}:00:00Z") for i in range(1, 13)]
        run, artifact = vsc.select_run(runs, self._fetch({1}), limit=vsc.RUN_SEARCH_LIMIT)
        self.assertEqual((run["id"], artifact["id"]), (1, 1))

    @staticmethod
    def _reuse(runs, artifacts, sources, source_runs=None):
        source_runs = source_runs or {}
        return vsc.select_run(
            runs,
            lambda run: artifacts.get(run["id"], []),
            fetch_reuse_source=lambda run: sources.get(run["id"]),
            fetch_run=lambda run_id: source_runs.get(run_id) or _run(run_id))

    LIVE = [{"id": 20, "name": vsc.EVIDENCE_ARTIFACT, "expired": False}]
    EXPIRED = [{"id": 20, "name": vsc.EVIDENCE_ARTIFACT, "expired": True}]

    def test_reuse_chain_ends_at_source_artifact(self):
        run, artifact = self._reuse([_run(30)], {20: self.LIVE}, {30: 20})
        self.assertEqual((run["id"], artifact["id"]), (20, 20))

    def test_reuse_chain_follows_several_hops(self):
        run, artifact = self._reuse([_run(40)], {10: self.LIVE}, {40: 30, 30: 10})
        self.assertEqual(run["id"], 10)

    def test_reuse_source_expiry_names_the_chain(self):
        with self.assertRaisesRegex(vsc.ResolveError, r"reuse chain 30 -> 20.*expired"):
            self._reuse([_run(30)], {20: self.EXPIRED}, {30: 20})

    def test_expired_source_does_not_hide_an_older_usable_run(self):
        runs = [_run(30, created="2026-11-11T12:00:00Z"), _run(5, created="2026-11-11T08:00:00Z")]
        arts = {20: self.EXPIRED, 5: [{"id": 5, "name": vsc.EVIDENCE_ARTIFACT, "expired": False}]}
        run, artifact = self._reuse(runs, arts, {30: 20})
        self.assertEqual((run["id"], artifact["id"]), (5, 5))

    def test_reuse_cycle_terminates(self):
        with self.assertRaisesRegex(vsc.ResolveError, "loops"):
            self._reuse([_run(30)], {}, {30: 20, 20: 30})

    def test_self_reference_terminates(self):
        with self.assertRaisesRegex(vsc.ResolveError, "loops"):
            self._reuse([_run(30)], {}, {30: 30})

    def test_endless_chain_hits_the_depth_cap(self):
        sources = {i: i + 1 for i in range(1, 100)}
        with self.assertRaisesRegex(vsc.ResolveError, "exceeds"):
            self._reuse([_run(1)], {}, sources)

    def test_source_that_is_not_a_green_gpu_run_is_refused(self):
        with self.assertRaisesRegex(vsc.ResolveError, "not a successful"):
            self._reuse([_run(30)], {20: self.LIVE}, {30: 20}, {20: _run(20, conclusion="failure")})

    def test_zero_artifact_without_reuse_pointer_still_fails(self):
        with self.assertRaisesRegex(vsc.ResolveError, "none of the newest 1"):
            self._reuse([_run(30)], {}, {})


class ReuseSourceTests(unittest.TestCase):
    URL = f"https://github.com/{vsc.VELLUM_REPO}/actions/runs/45766542596"
    NOTICE = f"Tree fce3f962210c689fa704061e734330dfd749faff was proven by {URL}"

    def test_notice_text_names_source_run(self):
        self.assertEqual(vsc.reuse_source_run(self.NOTICE), 45766542596)
        self.assertEqual(vsc.reuse_source_run(("##[notice]" + self.NOTICE + "\n").encode()), 45766542596)

    def test_unrelated_text_has_no_source(self):
        self.assertIsNone(vsc.reuse_source_run("Running the full proof"))

    def test_other_repository_is_not_followed(self):
        text = "Tree abc was proven by https://github.com/someone/else/actions/runs/7"
        self.assertIsNone(vsc.reuse_source_run(text))

    def _jobs(self):
        return [{"id": 1, "name": "decide"}, {"id": 2, "name": vsc.GPU_JOB_NAME}]

    def test_annotation_is_preferred_and_logs_are_not_read(self):
        def logs(_job):
            raise AssertionError("log fetched although an annotation named the source")
        found = vsc.run_reuse_source(self._jobs(), lambda job: [{"message": self.NOTICE}], logs)
        self.assertEqual(found, 45766542596)

    def test_log_fallback_when_job_has_no_annotations(self):
        found = vsc.run_reuse_source(self._jobs(), lambda job: [], lambda job: self.NOTICE.encode())
        self.assertEqual(found, 45766542596)

    def test_log_fallback_when_annotations_cannot_be_fetched(self):
        def broken(_job):
            raise vsc.ResolveError("HTTP 500")
        found = vsc.run_reuse_source(self._jobs(), broken, lambda job: self.NOTICE.encode())
        self.assertEqual(found, 45766542596)

    def test_annotations_without_a_pointer_do_not_read_logs(self):
        def logs(_job):
            raise AssertionError("log fetched for a job that has annotations")
        found = vsc.run_reuse_source(
            self._jobs(), lambda job: [{"message": "Running the full proof"}], logs)
        self.assertIsNone(found)


class MinimumCommitTests(unittest.TestCase):
    """A run older than the required Vellum commit is refused, not downloaded."""

    def setUp(self):
        self.saved = vsc._api
        self.paths = []

    def tearDown(self):
        vsc._api = self.saved

    def _resolve(self, compare_status):
        import argparse
        import os

        def fake(path, token, raw=False):
            self.paths.append(path)
            if path.startswith(f"repos/{vsc.VELLUM_REPO}/actions/runs?"):
                return {"workflow_runs": [_run(1)]}
            if path == f"repos/{vsc.VELLUM_REPO}/actions/runs/1/artifacts":
                return {"artifacts": [{"id": 9, "name": vsc.EVIDENCE_ARTIFACT, "expired": False}]}
            if path.startswith(f"repos/{vsc.VELLUM_REPO}/compare/"):
                return {"status": compare_status}
            raise vsc.ResolveError(f"unexpected {path}")

        vsc._api = fake
        os.environ["GH_TOKEN"] = "t"
        args = argparse.Namespace(out_dir=tempfile.mkdtemp(), run_id=None, previous_receipt=None,
                                  skip_if_unchanged=None, github_output=None, min_vellum_sha="abc")
        return vsc.resolve(args)

    def test_run_behind_the_minimum_is_refused_before_any_download(self):
        self.assertEqual(self._resolve("behind"), vsc.INCONCLUSIVE)
        self.assertFalse(any(p.endswith("/zip") for p in self.paths))
        self.assertTrue(any(p.endswith("compare/abc...sha1") for p in self.paths))

    def test_run_at_or_ahead_of_the_minimum_goes_on_to_download(self):
        for status in ("identical", "ahead"):
            self.paths.clear()
            # The fake serves no archive, so the download step fails loudly.
            self.assertEqual(self._resolve(status), vsc.INCONCLUSIVE)
            self.assertTrue(any(p.endswith("artifacts/9/zip") for p in self.paths), status)


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
        if path == f"repos/{vsc.VELLUM_REPO}/actions/runs/1/artifacts":
            return {"artifacts": [{"id": 9, "name": vsc.EVIDENCE_ARTIFACT, "expired": False}]}
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
        self.assertFalse(any(c.endswith("artifacts/9/zip") for c in self.calls))

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


class BridgeWiringTests(unittest.TestCase):
    """The lane sees every file that wires the bridge, and the bridge's test
    configure works when Pulp is embedded with add_subdirectory."""

    ROOT = Path(__file__).resolve().parents[2]

    def _pull_request_paths(self):
        text = (self.ROOT / ".github/workflows/vellum-sdk-consumer.yml").read_text(encoding="utf-8")
        lines = text.splitlines()
        start = lines.index("  pull_request:")
        paths = []
        for line in lines[start + 1:]:
            stripped = line.strip()
            if line and not line.startswith("    "):
                break
            if stripped.startswith("- "):
                paths.append(stripped[2:].strip("'\""))
        return paths

    def test_pull_request_trigger_watches_the_bridge_wiring(self):
        paths = self._pull_request_paths()
        for required in ("integrations/pulp-vellum/**", "test/test_pulp_vellum_param_bridge.cpp",
                         "integrations/CMakeLists.txt", "CMakeLists.txt"):
            self.assertIn(required, paths)

    def test_bridge_cmake_does_not_root_paths_at_the_consumer_project(self):
        files = sorted((self.ROOT / "integrations/pulp-vellum").rglob("CMakeLists.txt"))
        self.assertTrue(files, "no pulp-vellum CMake files found; the check is blind")
        for path in files:
            text = path.read_text(encoding="utf-8")
            code = "\n".join(line.split("#", 1)[0] for line in text.splitlines())
            for variable in ("PROJECT_SOURCE_DIR", "CMAKE_SOURCE_DIR"):
                self.assertNotIn("${%s}" % variable, code, f"{path.relative_to(self.ROOT)} uses ${{{variable}}}")
        tests = (self.ROOT / "integrations/pulp-vellum/tests/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("${PULP_ROOT_DIR}/test/test_pulp_vellum_param_bridge.cpp", tests)


class LabelledTargetTests(unittest.TestCase):
    EXECUTABLES = {
        "pulp-test-pulp-vellum": {"source": "integrations/pulp-vellum/tests", "artifact": "a/pulp-test-pulp-vellum"},
        "pulp-test-vellum-scripted-wiring": {"source": "integrations/pulp-vellum/tests",
                                             "artifact": "a/pulp-test-vellum-scripted-wiring"},
        "pulp-test-elsewhere": {"source": "test", "artifact": "t/pulp-test-elsewhere"},
        "pulp-test-unrelated": {"source": "test", "artifact": "t/pulp-test-unrelated"},
    }

    def test_integration_executables_are_selected_without_a_ctest_entry(self):
        # A Catch2 executable that was never built registers no ctest entry, so
        # the directory rule alone must select it.
        self.assertEqual(vsc.vellum_test_targets(self.EXECUTABLES, []),
                         ["pulp-test-pulp-vellum", "pulp-test-vellum-scripted-wiring"])

    def test_absolute_source_paths_are_matched_against_the_checkout(self):
        root = "/work/checkout"
        absolute = {
            "in-tree": {"source": f"{root}/integrations/pulp-vellum-audio/tests/foo.cpp", "artifact": "a/in-tree",
                        "source_root": root},
            "elsewhere": {"source": f"{root}/test", "artifact": "a/elsewhere", "source_root": root},
            "outside": {"source": "/other/integrations/pulp-vellum/tests", "artifact": "a/outside",
                        "source_root": root},
            "relative": {"source": "integrations/pulp-vellum/tests", "artifact": "a/relative", "source_root": root},
            "windows": {"source": "C:\\work\\checkout\\integrations\\pulp-vellum\\tests", "artifact": "a/win",
                        "source_root": "C:\\work\\checkout"},
        }
        self.assertEqual(vsc.vellum_test_targets(absolute, []), ["in-tree", "relative", "windows"])

    def test_symlinked_checkout_root_is_resolved(self):
        with tempfile.TemporaryDirectory() as tmp:
            real = Path(tmp).resolve() / "real"
            (real / "integrations/pulp-vellum/tests").mkdir(parents=True)
            link = Path(tmp).resolve() / "link"
            link.symlink_to(real)
            self.assertEqual(vsc.checkout_relative(str(real / "integrations/pulp-vellum/tests"), str(link)),
                             "integrations/pulp-vellum/tests")

    def test_a_labelled_test_program_outside_the_integration_dirs_is_selected(self):
        programs = [("some-test", Path("/b/t/pulp-test-elsewhere")), ("not-an-exe", Path("/b/t/script.cmake"))]
        self.assertEqual(vsc.vellum_test_targets(self.EXECUTABLES, programs),
                         ["pulp-test-elsewhere", "pulp-test-pulp-vellum", "pulp-test-vellum-scripted-wiring"])

    def test_cmake_env_wrapper_is_looked_through(self):
        self.assertEqual(vsc._test_program(["/usr/bin/cmake", "-E", "env", "CI=0", "DISPLAY=:0", "/b/exe"]), "/b/exe")
        self.assertEqual(vsc._test_program(["/b/exe", "[tag]"]), "/b/exe")
        self.assertIsNone(vsc._test_program([]))

    def test_unbuilt_target_and_program_fail_verification(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            (build / "a").mkdir()
            (build / "a/pulp-test-pulp-vellum").write_text("", encoding="utf-8")
            targets = ["pulp-test-pulp-vellum", "pulp-test-vellum-scripted-wiring"]
            programs = [("wiring", build / "a/pulp-test-vellum-scripted-wiring")]
            problems = vsc.missing_programs(build, targets, self.EXECUTABLES, programs)
            self.assertEqual(len(problems), 2)
            self.assertIn("target pulp-test-vellum-scripted-wiring", problems[0])
            self.assertIn("test wiring", problems[1])
            # Control: once both exist the same check is clean.
            (build / "a/pulp-test-vellum-scripted-wiring").write_text("", encoding="utf-8")
            self.assertEqual(vsc.missing_programs(build, targets, self.EXECUTABLES, programs), [])

    def test_missing_reply_is_an_error_not_an_empty_selection(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(vsc.LabelError):
                vsc.codemodel_executables(Path(tmp))

    def test_codemodel_reply_is_read_and_non_executables_skipped(self):
        import json
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            reply = build / ".cmake/api/v1/reply"
            reply.mkdir(parents=True)
            (reply / "index-1.json").write_text(json.dumps(
                {"objects": [{"kind": "codemodel", "jsonFile": "codemodel.json"}]}), encoding="utf-8")
            (reply / "codemodel.json").write_text(json.dumps({"configurations": [{"targets": [
                {"name": "exe", "jsonFile": "exe.json"}, {"name": "lib", "jsonFile": "lib.json"}]}]}), encoding="utf-8")
            (reply / "exe.json").write_text(json.dumps({"name": "exe", "type": "EXECUTABLE",
                "paths": {"source": "integrations/pulp-vellum/tests"}, "artifacts": [{"path": "x/exe"}]}), encoding="utf-8")
            (reply / "lib.json").write_text(json.dumps({"name": "lib", "type": "STATIC_LIBRARY",
                "paths": {"source": "integrations/pulp-vellum"}, "artifacts": [{"path": "x/lib.a"}]}), encoding="utf-8")
            self.assertEqual(vsc.codemodel_executables(build),
                             {"exe": {"source": "integrations/pulp-vellum/tests", "artifact": "x/exe", "source_root": ""}})

    def test_workflow_builds_the_selection_and_verifies_it(self):
        text = (Path(__file__).resolve().parents[2] / ".github/workflows/vellum-sdk-consumer.yml").read_text(
            encoding="utf-8")
        for needed in ("labelled-targets", "--prepare", "--verify", "--no-tests=error"):
            self.assertIn(needed, text)
        self.assertNotIn("--target pulp-test-pulp-vellum ", text)


if __name__ == "__main__":
    unittest.main()
