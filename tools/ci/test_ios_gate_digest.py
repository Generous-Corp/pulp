#!/usr/bin/env python3
"""Tests for tools/ci/ios_gate_digest.py (shadow-mode iOS gate identity).

What must hold:
- the digest is a function of the gate's inputs and the toolchain only: a
  docs, workflow, skill or tools/scripts change leaves it unchanged; a change
  to a core source, a CMake file, a dependency pin, the gate's own script, a
  path in IOS_COMPILE_REQUIRED_PATTERNS, or the toolchain id changes it;
- the same tree digests identically from two clones (no path or time input);
- lookup reports a hit only for an unexpired artifact with the exact name;
- the annotation is a `notice` carrying the schema, mode, verdict and digest;
- enforcement skips ONLY with a trusted receipt (this repository's build.yml,
  a merge_group or same-repository pull_request run, a marker naming the same
  digest and run), only in enforce mode, and never on a control run (one run
  id in CONTROL_EVERY, every schedule/push run); every failure path runs.

Run:
    python3 tools/ci/test_ios_gate_digest.py
"""
from __future__ import annotations

import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ios_gate_digest as igd  # noqa: E402

TOOLCHAIN = "Xcode 27.0 | 27.0 | 27.0"


def git(repo: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True,
                          check=True, env={"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
                                           "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t",
                                           "PATH": "/usr/bin:/bin:/opt/homebrew/bin"}).stdout


def write(repo: Path, rel: str, text: str) -> None:
    p = repo / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8")


class Fixture:
    def __init__(self, tmp: Path) -> None:
        self.repo = tmp / "repo"
        self.repo.mkdir()
        git(self.repo, "init", "-q")
        for rel, text in {
            "CMakeLists.txt": "project(pulp)\n",
            "core/midi/src/midi.cpp": "int a;\n",
            "core/midi/include/pulp/midi/midi.hpp": "#pragma once\n",
            "core/README.md": "notes\n",
            "examples/ios-gpu/CMakeLists.txt": "add_executable(x x.mm)\n",
            "tools/deps/manifest.json": "{}\n",
            "test/cmake/test_ios_compile_gate.sh": "#!/bin/bash\n",
            "test/ios/harness.mm": "// harness\n",
            "test/test_biquad.cpp": "// not an iOS input\n",
            "docs/guide.md": "docs\n",
            ".github/workflows/build.yml": "on: push\n",
            "tools/scripts/classify_changes.py": "# unrelated tool\n",
            ".agents/skills/ci/SKILL.md": "skill\n",
            "tools/scripts/package-lock.json": "{}\n",
            "tools/scripts/bundle_threejs_for_jsc.mjs": "// bundler\n",
            "tools/ci/governed-build.sh": "#!/bin/bash\n",
            "tools/audio/src/service.cpp": "int s;\n",
            "tools/import-design/importer.py": "# script, never configured\n",
            "ship/CMakeLists.txt": "# ship\n",
        }.items():
            write(self.repo, rel, text)
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "base")

    def digest(self) -> str:
        return igd.compute(self.repo, "HEAD", TOOLCHAIN)["digest"]

    def commit(self, rel: str, text: str) -> None:
        write(self.repo, rel, text)
        git(self.repo, "add", rel)
        git(self.repo, "commit", "-q", "-m", f"touch {rel}")


class DigestTests(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.fx = Fixture(Path(tmp.name))
        self.base = self.fx.digest()

    def test_non_inputs_leave_the_digest_unchanged(self) -> None:
        for rel in ("docs/guide.md", ".github/workflows/build.yml", "core/README.md",
                    "tools/scripts/classify_changes.py", ".agents/skills/ci/SKILL.md",
                    "test/test_biquad.cpp", "tools/import-design/importer.py"):
            self.fx.commit(rel, f"changed {rel}\n")
            self.assertEqual(self.fx.digest(), self.base, rel)

    def test_each_input_class_changes_the_digest(self) -> None:
        seen = {self.base}
        for rel in ("core/midi/src/midi.cpp", "core/midi/include/pulp/midi/midi.hpp",
                    "CMakeLists.txt", "examples/ios-gpu/CMakeLists.txt",
                    "tools/deps/manifest.json", "test/cmake/test_ios_compile_gate.sh",
                    "test/ios/harness.mm", "tools/scripts/package-lock.json",
                    "tools/scripts/bundle_threejs_for_jsc.mjs", "tools/ci/governed-build.sh",
                    "tools/audio/src/service.cpp", "ship/CMakeLists.txt"):
            self.fx.commit(rel, f"changed {rel}\n")
            d = self.fx.digest()
            self.assertNotIn(d, seen, rel)
            seen.add(d)

    def test_toolchain_is_part_of_the_identity(self) -> None:
        other = igd.compute(self.fx.repo, "HEAD", "Xcode 26.4 | 26.4 | 26.4")["digest"]
        self.assertNotEqual(other, self.base)

    def test_same_tree_digests_identically_from_another_clone(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            clone = Path(tmp) / "elsewhere"
            git(self.fx.repo.parent, "clone", "-q", str(self.fx.repo), str(clone))
            self.assertEqual(igd.compute(clone, "HEAD", TOOLCHAIN)["digest"], self.base)

    def test_input_count_and_classification(self) -> None:
        r = igd.compute(self.fx.repo, "HEAD", TOOLCHAIN)
        # 12 inputs: CMakeLists, 2 core files, examples CMakeLists, deps manifest,
        # gate script, test/ios harness, the bundler + its lockfile, the build
        # wrapper, a tools/audio source, ship's CMakeLists. core/README.md is
        # excluded as *.md, tools/import-design/importer.py as a script.
        self.assertEqual(r["inputs"], 12)
        self.assertTrue(igd.is_input("tools/cli/gpu_health/src/model.cpp"))
        self.assertFalse(igd.is_input("tools/import-design/browser_capture/capture.mjs"))
        self.assertFalse(igd.is_input("tools/cli/cmd_build.cpp"))
        self.assertTrue(igd.is_input("core/view/src/widgets.cpp"))
        self.assertTrue(igd.is_input("test/cmake/quality_tests.cmake"))
        self.assertFalse(igd.is_input("core/view/README.md"))
        self.assertFalse(igd.is_input("tools/scripts/gates.sh"))


class LookupAndNoteTests(unittest.TestCase):
    def test_lookup_hits_only_an_unexpired_exact_name(self) -> None:
        d = "a" * 64
        page = {"artifacts": [
            {"name": igd.artifact_name(d), "expired": True, "created_at": "2026-09-01T00:00:00Z",
             "workflow_run": {"id": 1}},
            {"name": igd.artifact_name(d), "expired": False, "created_at": "2026-09-02T00:00:00Z",
             "workflow_run": {"id": 2}},
            {"name": igd.artifact_name(d), "expired": False, "created_at": "2026-09-03T00:00:00Z",
             "workflow_run": {"id": 3}},
            {"name": "ios-gate-ok-other", "expired": False, "created_at": "2026-09-04T00:00:00Z",
             "workflow_run": {"id": 4}}]}
        seen = []

        def fetch(url, token):
            seen.append((url, token))
            return page
        r = igd.lookup("O/R", "tok", d, fetch=fetch)
        self.assertTrue(r["found"])
        self.assertEqual(r["source_run_id"], 3)
        self.assertEqual(r["candidates"], 2)
        self.assertIn(f"name={igd.artifact_name(d)}", seen[0][0])
        miss = igd.lookup("O/R", "tok", "b" * 64, fetch=lambda u, t: {"artifacts": []})
        self.assertFalse(miss["found"])
        expired_only = igd.lookup("O/R", "tok", d, fetch=lambda u, t: {"artifacts": page["artifacts"][:1]})
        self.assertFalse(expired_only["found"])

    def test_note_is_a_notice_with_schema_and_verdict(self) -> None:
        line = igd.note("would_skip", "c" * 64, "merge_group", 42)
        self.assertTrue(line.startswith("::notice title=ios-gate-shadow::"))
        rec = json.loads(line.split("::", 2)[2])
        self.assertEqual(rec["schema"], igd.SCHEMA)
        self.assertEqual(rec["mode"], "shadow")
        self.assertEqual((rec["verdict"], rec["event"], rec["source_run_id"]),
                         ("would_skip", "merge_group", 42))

    def test_cli_compute_prints_json(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(Path(tmp))
            proc = subprocess.run([sys.executable, str(HERE / "ios_gate_digest.py"), "compute",
                                   "--repo", str(fx.repo), "--toolchain-id", TOOLCHAIN],
                                  capture_output=True, text=True, timeout=60)
            expected = fx.digest()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        out = json.loads(proc.stdout)
        self.assertEqual(out["digest"], expected)
        self.assertEqual(len(out["digest"]), 64)


def marker_zip(digest: str, run_id: int, schema: str = igd.SCHEMA, name: str = "marker.json") -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr(name, json.dumps({"schema": schema, "digest": digest, "run_id": str(run_id),
                                     "sha": "f" * 40, "event": "merge_group"}))
    return buf.getvalue()


class TrustedLookupTests(unittest.TestCase):
    D = "d" * 64

    def world(self, runs: dict, archives: dict, created: dict | None = None):
        created = created or {}
        arts = [{"name": igd.artifact_name(self.D), "expired": False,
                 "created_at": created.get(rid, f"2026-09-2{rid % 10}T00:00:00Z"),
                 "workflow_run": {"id": rid}, "archive_download_url": f"https://blob/{rid}"}
                for rid in runs]

        def fetch(url, token):
            if "/actions/artifacts?" in url:
                return {"artifacts": arts}
            rid = int(url.rsplit("/", 1)[1])
            return runs[rid]

        def download(url, token):
            return archives[int(url.rsplit("/", 1)[1])]
        return fetch, download

    @staticmethod
    def run_rec(rid: int, event: str = "merge_group", path: str = igd.WORKFLOW_PATH,
                repo: str = "O/R", head_repo: str = "O/R") -> dict:
        return {"id": rid, "event": event, "path": path, "repository": {"full_name": repo},
                "head_repository": {"full_name": head_repo}}

    def test_trusted_merge_group_and_same_repo_pr_receipts_qualify(self) -> None:
        for event in ("merge_group", "pull_request"):
            fetch, download = self.world({7: self.run_rec(7, event)}, {7: marker_zip(self.D, 7)})
            r = igd.trusted_lookup("O/R", "t", self.D, fetch=fetch, download=download)
            self.assertTrue(r["trusted"], (event, r))
            self.assertEqual(r["source_run_id"], 7)

    def test_untrusted_receipts_are_refused(self) -> None:
        cases = {
            "fork": (self.run_rec(7, "pull_request", head_repo="fork/R"), marker_zip(self.D, 7)),
            "other workflow": (self.run_rec(7, path=".github/workflows/evil.yml"), marker_zip(self.D, 7)),
            "push event": (self.run_rec(7, "push"), marker_zip(self.D, 7)),
            "wrong digest": (self.run_rec(7), marker_zip("e" * 64, 7)),
            "wrong run": (self.run_rec(7), marker_zip(self.D, 8)),
            "wrong schema": (self.run_rec(7), marker_zip(self.D, 7, schema="x/v0")),
            "extra layout": (self.run_rec(7), marker_zip(self.D, 7, name="other.json")),
            "not a zip": (self.run_rec(7), b"garbage"),
        }
        for label, (run, archive) in cases.items():
            fetch, download = self.world({7: run}, {7: archive})
            r = igd.trusted_lookup("O/R", "t", self.D, fetch=fetch, download=download)
            self.assertFalse(r["trusted"], label)
            self.assertIsNone(r["source_run_id"], label)
            self.assertEqual(len(r["refusals"]), 1, label)

    def test_a_trusted_older_receipt_behind_an_untrusted_newer_one_still_qualifies(self) -> None:
        fetch, download = self.world(
            {8: self.run_rec(8, head_repo="fork/R"), 5: self.run_rec(5)},
            {8: marker_zip(self.D, 8), 5: marker_zip(self.D, 5)},
            created={8: "2026-09-28T00:00:00Z", 5: "2026-09-25T00:00:00Z"})
        r = igd.trusted_lookup("O/R", "t", self.D, fetch=fetch, download=download)
        self.assertTrue(r["trusted"])
        self.assertEqual(r["source_run_id"], 5)

    def test_a_candidate_whose_api_call_fails_is_refused_not_trusted(self) -> None:
        def fetch(url, token):
            if "/actions/artifacts?" in url:
                return {"artifacts": [{"name": igd.artifact_name(self.D), "expired": False,
                                       "workflow_run": {"id": 3}, "archive_download_url": "u"}]}
            raise OSError("503")
        r = igd.trusted_lookup("O/R", "t", self.D, fetch=fetch, download=lambda u, t: b"")
        self.assertFalse(r["trusted"])


class DecideTests(unittest.TestCase):
    HIT = {"trusted": True, "source_run_id": 41}

    def test_enforce_skips_only_with_a_trusted_hit_on_a_non_control_run(self) -> None:
        d = igd.decide("enforce", "merge_group", 123, self.HIT)
        self.assertEqual((d["action"], d["verdict"], d["source_run_id"]), ("skip", "skipped", 41))
        for hit in (None, {"trusted": False, "source_run_id": 41}, {}):
            d = igd.decide("enforce", "merge_group", 123, hit)
            self.assertEqual((d["action"], d["verdict"]), ("run", "run"), hit)

    def test_control_cadence_runs_the_gate_despite_a_hit(self) -> None:
        self.assertEqual(igd.decide("enforce", "merge_group", 120, self.HIT)["verdict"], "control_run")
        self.assertEqual(igd.decide("enforce", "merge_group", 120, self.HIT)["action"], "run")
        for event in ("schedule", "push"):
            self.assertEqual(igd.decide("enforce", event, 123, self.HIT)["action"], "run", event)
        controls = sum(igd.is_control(r, "merge_group") for r in range(1000, 2000))
        self.assertEqual(controls, 1000 // igd.CONTROL_EVERY)
        self.assertFalse(igd.is_control(None, "pull_request"))

    def test_shadow_and_off_never_skip(self) -> None:
        d = igd.decide("shadow", "merge_group", 123, self.HIT)
        self.assertEqual((d["action"], d["verdict"]), ("run", "would_skip"))
        d = igd.decide("off", "merge_group", 123, self.HIT)
        self.assertEqual((d["action"], d["verdict"]), ("run", None))
        self.assertEqual(igd.decide("bogus", "merge_group", 123, self.HIT)["action"], "run")

    def test_summary_line_names_the_skip(self) -> None:
        line = igd.summary_line(igd.decide("enforce", "merge_group", 123, self.HIT), "a" * 64)
        self.assertIn("SKIPPED", line)
        self.assertIn("41", line)
        self.assertNotIn("SKIPPED", igd.summary_line(igd.decide("enforce", "merge_group", 120, self.HIT), "a" * 64))

    def test_cli_decide_fails_closed_to_run(self) -> None:
        """No token (or any lookup error) must decide `run`, still write the env
        file, and exit 0 so the Build step goes on to run the gate."""
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(Path(tmp))
            env_out = Path(tmp) / "decision.env"
            summary = Path(tmp) / "summary.md"
            env = {k: v for k, v in os.environ.items() if k != "GITHUB_TOKEN"}
            env["GITHUB_STEP_SUMMARY"] = str(summary)
            proc = subprocess.run([sys.executable, str(HERE / "ios_gate_digest.py"), "decide",
                                   "--repo", str(fx.repo), "--repository", "O/R", "--run-id", "123",
                                   "--event", "merge_group", "--toolchain-id", TOOLCHAIN,
                                   "--env-out", str(env_out)],
                                  capture_output=True, text=True, timeout=60, env=env)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            vals = dict(line.split("=", 1) for line in env_out.read_text().splitlines())
            self.assertEqual(vals["ios_action"], "run")
            self.assertEqual(vals["ios_digest"], fx.digest())
            self.assertIn("iOS compile gate: ran", summary.read_text())


if __name__ == "__main__":
    unittest.main()
