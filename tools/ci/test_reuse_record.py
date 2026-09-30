#!/usr/bin/env python3
"""Tests for tools/ci/reuse_record.py (per-job replay record of a macos job).

What must hold:
- every ctest JUnit status maps to the record's outcome (pass, fail, timeout,
  skipped, notrun), checked against a report ctest itself wrote
  (testdata/reuse_record/ctest.junit.xml, from a run with --repeat
  until-pass:2 covering a flake, a failure, a timeout, a skip code, a skip
  regex, a disabled test and a name that needs XML escaping);
- attempts come from ctest's LastTest.log from the same run (a flake and a
  failure ran twice); without the log a --repeat suite says null, never 1;
- every record carries exactly the neutral schema's fields, with the run
  context, the executable, and the per-test output key when one exists;
- the pull-request and merge-group contexts bind the right head, base and
  tree (a merge group's head is its commit's second parent);
- the Mach-O reader finds dylibs and rpaths in thin and universal files, the
  closure resolves @rpath/@loader_path and a script's interpreter, and the
  closure digest moves when a loaded library's bytes move while the
  executable's own digest does not;
- the executable digest equals protected_merge_receipt's for the same file;
- reports older than the job are left out, and `write` produces all three
  files and an annotation;
- build.yml records on every macos job it runs: continue-on-error steps, a
  90-day artifact, and a `reuse-record NOT written` warning on every way the
  record can go missing.

Run:
    python3 tools/ci/test_reuse_record.py
"""
from __future__ import annotations

import json
import os
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "tools" / "scripts"))
import protected_merge_receipt as pmr  # noqa: E402
import reuse_record as rr  # noqa: E402

DATA = ROOT / "tools/ci/testdata/reuse_record"
QUOTED = 'quoted "name" & <x>'
GIT_ENV = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "GIT_AUTHOR_NAME": "t",
           "GIT_AUTHOR_EMAIL": "t@t", "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t",
           "HOME": tempfile.gettempdir()}


def git(repo: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True,
                          check=True, env=GIT_ENV).stdout.strip()


def full_suite() -> dict:
    return rr.parse_suite(f"full={DATA / 'ctest.junit.xml'},attempts={DATA / 'LastTest.full.log'},repeat")


CTX = {"run_id": "7", "run_attempt": 1, "run_kind": "merge_group", "pr": 42, "head_sha": "h" * 40,
       "base_sha": "b" * 40, "merge_tree": "t" * 40}


class ResultTests(unittest.TestCase):
    def test_every_ctest_status_maps_to_its_outcome(self) -> None:
        got = {c["test_id"]: c["outcome"] for c in rr.junit_cases(DATA / "ctest.junit.xml")}
        self.assertEqual(got, {"ok": "pass", "flaky": "pass", "bad": "fail", "skip": "skipped",
                               "slow": "timeout", "off": "notrun", "rxskip": "skipped", QUOTED: "pass"})

    def test_duration_is_the_reported_seconds(self) -> None:
        got = {c["test_id"]: c["duration_s"] for c in rr.junit_cases(DATA / "ctest.junit.xml")}
        self.assertEqual(got["slow"], 1.043)
        self.assertEqual(got["off"], 0.0)

    def test_attempts_count_every_execution_in_last_test_log(self) -> None:
        counts = rr.attempts_from_log(DATA / "LastTest.full.log")
        self.assertEqual((counts["ok"], counts["flaky"], counts["bad"], counts["slow"], counts[QUOTED]),
                         (1, 2, 2, 2, 1))

    def test_attempts_without_a_log(self) -> None:
        self.assertIsNone(rr.attempts_for("pass", "x", None, repeat=True))
        self.assertEqual(rr.attempts_for("pass", "x", None, repeat=False), 1)
        self.assertEqual(rr.attempts_for("notrun", "x", {"x": 1}, repeat=True), 0)


class RecordTests(unittest.TestCase):
    def records(self) -> tuple[list[dict], dict]:
        return rr.build_records(CTX, "img0", [full_suite()], {"bad": "<build>/test/t-bad"},
                                {"bad": "k" * 64})

    def test_each_record_has_exactly_the_schema_fields(self) -> None:
        records, _ = self.records()
        self.assertEqual(len(records), 8)
        for r in records:
            self.assertEqual(tuple(r), rr.RECORD_FIELDS, r["test_id"])
            self.assertIn(r["outcome"], rr.OUTCOMES)

    def test_record_values(self) -> None:
        records, summary = self.records()
        bad = next(r for r in records if r["test_id"] == "bad")
        self.assertEqual(bad, {**CTX, "suite": "full", "test_id": "bad", "executable": "<build>/test/t-bad",
                               "outcome": "fail", "attempts": 2, "duration_s": bad["duration_s"],
                               "runner_image": "img0", "output_key": "k" * 64})
        flaky = next(r for r in records if r["test_id"] == "flaky")
        self.assertEqual((flaky["outcome"], flaky["attempts"], flaky["executable"], flaky["output_key"]),
                         ("pass", 2, None, None))
        off = next(r for r in records if r["test_id"] == "off")
        self.assertEqual((off["outcome"], off["attempts"]), ("notrun", 0))
        self.assertEqual(summary["full"]["outcomes"],
                         {"pass": 3, "fail": 1, "timeout": 1, "skipped": 2, "notrun": 1})
        self.assertEqual(summary["full"]["attempts_source"], "last-test-log")

    def test_a_report_older_than_the_job_is_left_out(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp) / "affected"
            d.mkdir()
            old, new = d / "batch-0.xml", d / "batch-1.xml"
            old.write_text('<testsuite><testcase name="stale" status="run"/></testsuite>')
            new.write_text('<testsuite><testcase name="fresh" status="run"/></testsuite>')
            os.utime(old, (1_000_000, 1_000_000))
            marker = Path(tmp) / "event.json"
            marker.write_text("{}")
            os.utime(marker, (2_000_000, 2_000_000))
            records, summary = rr.build_records(CTX, "i", [rr.parse_suite(f"pr-affected={d},repeat")], {}, {},
                                                not_before=marker.stat().st_mtime)
        self.assertEqual([r["test_id"] for r in records], ["fresh"])
        self.assertEqual(summary["pr-affected"]["stale_reports"], 1)
        self.assertIsNone(records[0]["attempts"])


class ContextTests(unittest.TestCase):
    def repo(self, tmp: str) -> tuple[Path, str, str, str]:
        repo = Path(tmp)
        git(repo, "init", "-q", "-b", "main")
        (repo / "a").write_text("1")
        git(repo, "add", "a")
        git(repo, "commit", "-q", "-m", "base")
        base = git(repo, "rev-parse", "HEAD")
        git(repo, "checkout", "-q", "-b", "pr")
        (repo / "b").write_text("2")
        git(repo, "add", "b")
        git(repo, "commit", "-q", "-m", "head")
        head = git(repo, "rev-parse", "HEAD")
        git(repo, "checkout", "-q", "main")
        git(repo, "merge", "-q", "--no-ff", "-m", "Merge pull request #42", "pr")
        return repo, base, head, git(repo, "rev-parse", "HEAD")

    def test_merge_group_head_is_the_second_parent(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo, base, head, merge = self.repo(tmp)
            ctx = rr.run_context({"GITHUB_EVENT_NAME": "merge_group", "GITHUB_SHA": merge,
                                  "GITHUB_RUN_ID": "9", "GITHUB_RUN_ATTEMPT": "2",
                                  "MERGE_GROUP_HEAD_REF": "refs/heads/gh-readonly-queue/main/pr-42-" + head,
                                  "MERGE_GROUP_BASE_SHA": base}, repo)
            self.assertEqual((ctx["run_kind"], ctx["pr"], ctx["head_sha"], ctx["base_sha"], ctx["run_attempt"]),
                             ("merge_group", 42, head, base, 2))
            self.assertEqual(ctx["merge_tree"], git(repo, "rev-parse", f"{merge}^{{tree}}"))

    def test_pull_request_binds_the_event_head_and_base(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo, base, head, merge = self.repo(tmp)
            ctx = rr.run_context({"GITHUB_EVENT_NAME": "pull_request", "GITHUB_SHA": merge, "PR_NUMBER": "42",
                                  "PR_HEAD_SHA": head, "PR_BASE_SHA": base}, repo)
            self.assertEqual((ctx["run_kind"], ctx["pr"], ctx["head_sha"], ctx["base_sha"]),
                             ("pr_head", 42, head, base))


def macho(dylibs: list[str], rpaths: list[str], big_endian: bool = False) -> bytes:
    e = ">" if big_endian else "<"
    cmds = b""
    for name, cmd in [(d, 0xC) for d in dylibs] + [(r, rr.LC_RPATH) for r in rpaths]:
        payload = name.encode() + b"\0"
        header = 24 if cmd == 0xC else 12
        size = (header + len(payload) + 7) // 8 * 8
        body = struct.pack(e + "III", cmd, size, header)
        if cmd == 0xC:
            body += struct.pack(e + "III", 0, 0, 0)
        cmds += (body + payload).ljust(size, b"\0")
    ncmds = len(dylibs) + len(rpaths)
    magic = struct.pack(e + "I", 0xFEEDFACF)
    return magic + struct.pack(e + "iiIIII", 0x0100000C, 0, 2, ncmds, len(cmds), 0) + struct.pack(e + "I", 0) + cmds


def fat(slices: list[bytes]) -> bytes:
    header = struct.pack(">II", 0xCAFEBABE, len(slices))
    offset = 4096
    table, body = b"", b""
    for s in slices:
        table += struct.pack(">iiIII", 0x0100000C, 0, offset + len(body), len(s), 12)
        body += s.ljust(4096, b"\0")
    return (header + table).ljust(offset, b"\0") + body


class ClosureTests(unittest.TestCase):
    def tree(self, tmp: str, blob: bytes = b"lib-v1") -> Path:
        root = Path(tmp)
        (root / "bin").mkdir()
        (root / "lib").mkdir()
        (root / "lib" / "libx.dylib").write_bytes(blob)
        exe = root / "bin" / "t"
        exe.write_bytes(macho(["@rpath/libx.dylib", "/usr/lib/libSystem.B.dylib", "@loader_path/../lib/liby.dylib"],
                              ["@loader_path/../lib"]))
        return exe

    def test_load_commands_of_thin_big_endian_and_universal_files(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "f"
            p.write_bytes(macho(["/a.dylib"], ["/r"]))
            self.assertEqual(rr.macho_load_commands(p), (["/a.dylib"], ["/r"]))
            p.write_bytes(macho(["/a.dylib"], [], big_endian=True))
            self.assertEqual(rr.macho_load_commands(p), (["/a.dylib"], []))
            p.write_bytes(fat([macho(["/a.dylib"], []), macho(["/a.dylib", "/b.dylib"], ["/r"])]))
            self.assertEqual(rr.macho_load_commands(p), (["/a.dylib", "/b.dylib"], ["/r"]))
            p.write_bytes(b"#!/bin/sh\necho\n")
            self.assertIsNone(rr.macho_load_commands(p))

    def test_closure_resolves_rpath_and_names_what_it_cannot(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            exe = self.tree(tmp)
            got = rr.runtime_closure(exe)
        lib = os.path.realpath(Path(tmp) / "lib" / "libx.dylib")
        self.assertEqual(got, sorted([lib, "system:/usr/lib/libSystem.B.dylib",
                                      "unresolved:@loader_path/../lib/liby.dylib"]))

    def test_a_script_closure_holds_its_interpreter(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            interp = Path(tmp) / "interp"
            interp.write_bytes(macho(["/usr/lib/libSystem.B.dylib"], []))
            script = Path(tmp) / "s.sh"
            script.write_text(f"#!{interp}\n")
            got = rr.runtime_closure(script)
        self.assertIn(str(interp.resolve()), got)
        self.assertIn("system:/usr/lib/libSystem.B.dylib", got)

    def identity(self, tmp: str, blob: bytes) -> dict:
        exe = self.tree(tmp, blob)
        exe.chmod(exe.stat().st_mode | stat.S_IXUSR)
        test = {"name": "T", "command": [str(exe)]}
        # The build directory is the checkout itself here, so the two roots
        # tie and the build one must win (in CI the build dir is the longer).
        doc, by_test = rr.executable_identity(Path(tmp), Path(tmp), [test], {})
        self.assertEqual(by_test, {"T": "<build>/bin/t"})
        self.assertEqual(doc["executables"]["<build>/bin/t"]["sha256"],
                         pmr.artifact_identity(Path(tmp), {"tests": [test]})["files"][0]["sha256"])
        return doc

    def test_closure_digest_moves_with_a_loaded_library_and_the_executable_digest_does_not(self) -> None:
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            one, two = self.identity(a, b"lib-v1"), self.identity(b, b"lib-v2")
        e1, e2 = one["executables"]["<build>/bin/t"], two["executables"]["<build>/bin/t"]
        self.assertEqual(e1["sha256"], e2["sha256"])
        self.assertNotEqual(e1["closure_digest"], e2["closure_digest"])
        self.assertEqual(e1["closure"], ["<build>/lib/libx.dylib", "unresolved:@loader_path/../lib/liby.dylib"])
        self.assertEqual(e1["system_images"], 1)

    def test_an_unavailable_executable_is_recorded_not_fatal(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            doc, by_test = rr.executable_identity(Path(tmp), Path(tmp),
                                                  [{"name": "T", "command": [f"{tmp}/missing"]}], {})
        self.assertEqual(by_test["T"], f"unresolved:{tmp}/missing")
        self.assertEqual(doc["unresolved_executables"], 1)


class CliTests(unittest.TestCase):
    def test_write_produces_records_identity_job_and_an_annotation(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp) / "build"
            build.mkdir()
            exe = build / "t"
            exe.write_bytes(macho([], []))
            names = [c["test_id"] for c in rr.junit_cases(DATA / "ctest.junit.xml")]
            sel = Path(tmp) / "selected.json"
            sel.write_text(json.dumps({"tests": [{"name": n, "command": [str(exe)]} for n in names]}))
            keys = Path(tmp) / "keys.json"
            keys.write_text(json.dumps({"bad": {"key": "k1", "kind": "binary"}, "ok": {"key": None, "kind": "label"}}))
            out = Path(tmp) / "out"
            env = {**os.environ, "GITHUB_EVENT_NAME": "push", "GITHUB_RUN_ID": "5", "GITHUB_RUN_ATTEMPT": "1"}
            env.pop("GITHUB_SHA", None)
            proc = subprocess.run([sys.executable, str(HERE / "reuse_record.py"), "write", "--out-dir", str(out),
                                   "--build-dir", str(build), "--source-root", str(tmp),
                                   "--suite", f"full={DATA / 'ctest.junit.xml'},attempts={DATA / 'LastTest.full.log'},repeat",
                                   "--suite", f"pr-fast={tmp}/absent.xml",
                                   "--selected-json", str(sel), "--test-keys", str(keys),
                                   "--build-outcome", "success", "--context", "ctest=failure"],
                                  capture_output=True, text=True, timeout=120, env=env)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            records = [json.loads(l) for l in (out / "tests.jsonl").read_text().splitlines()]
            job = json.loads((out / "job.json").read_text())
            identity = json.loads((out / "identity.json").read_text())
        self.assertEqual(len(records), 8)
        self.assertEqual({r["executable"] for r in records}, {"<build>/t"})
        self.assertEqual({r["test_id"]: r["output_key"] for r in records if r["output_key"]}, {"bad": "k1"})
        self.assertEqual((job["schema"], job["run_kind"], job["steps"], job["suites"]["pr-fast"]["tests"]),
                         (rr.SCHEMA, "push", {"ctest": "failure"}, 0))
        self.assertEqual(job["bytes"]["tests_jsonl"] > 0, True)
        self.assertEqual(list(identity["executables"]), ["<build>/t"])
        note = next(l for l in proc.stdout.splitlines() if l.startswith(f"::notice title={rr.TITLE}::"))
        self.assertEqual(json.loads(note.split("::", 2)[2])["tests"], 8)
        self.assertNotIn("::warning", proc.stdout)


class WorkflowContractTests(unittest.TestCase):
    """build.yml records on every macos job and never lets a record go missing silently."""

    def setUp(self) -> None:
        self.text = (ROOT / ".github" / "workflows" / "build.yml").read_text(encoding="utf-8")

    def blocks(self, name: str) -> list[str]:
        out, lines = [], self.text.splitlines()
        for i, line in enumerate(lines):
            if line.strip() == f"- name: {name}":
                indent = len(line) - len(line.lstrip())
                j = i + 1
                while j < len(lines) and not (lines[j].strip().startswith("- ") and
                                              len(lines[j]) - len(lines[j].lstrip()) <= indent) \
                        and not (lines[j] and not lines[j].startswith(" " * (indent + 1))):
                    j += 1
                out.append("\n".join(lines[i:j]))
        return out

    def test_every_macos_job_records_and_uploads_for_90_days(self) -> None:
        record = self.blocks("Record per-test results for reuse replay (macOS)")
        empty = self.blocks("Record that no suite ran, for reuse replay")
        self.assertEqual((len(record), len(empty)), (1, 2), "the matrix leg and both alias jobs")
        uploads = self.blocks("Upload reuse replay record (macOS)") + self.blocks("Upload reuse replay record")
        self.assertEqual(len(uploads), 3)
        for block in record + empty + uploads:
            self.assertIn("continue-on-error: true", block)
            self.assertIn("!cancelled()", block)
        for block in uploads:
            self.assertIn("retention-days: 90", block)
            self.assertIn("'reuse-record-macos'", block)
        self.assertIn("matrix.key == 'macos'", record[0])

    def test_every_missing_record_path_warns(self) -> None:
        for block in (self.blocks("Record per-test results for reuse replay (macOS)")
                      + self.blocks("Record that no suite ran, for reuse replay")):
            self.assertIn("::warning title=reuse-record NOT written::", block)
            self.assertIn("exit 1", block)
        announce = (self.blocks("Announce a missing reuse replay record (macOS)")
                    + self.blocks("Announce a missing reuse replay record"))
        self.assertEqual(len(announce), 3)
        for block in announce:
            self.assertIn("steps.reuse_record_upload.outcome != 'success'", block)
            self.assertIn("::warning title=reuse-record NOT written::", block)

    def test_the_full_run_keeps_its_last_test_log_before_the_listing_rewrites_it(self) -> None:
        keep = self.text.index('"$evidence_dir/LastTest.full.log"')
        run = self.text.index('--output-junit "$GITHUB_WORKSPACE/$PULP_BUILD_DIR/ctest.junit.xml"')
        listing = self.text.index('ctest --show-only=json-v1 --test-dir "$PULP_BUILD_DIR"')
        self.assertLess(run, keep)
        self.assertLess(keep, listing)
        self.assertIn("attempts=$ev/LastTest.full.log,repeat", self.text)


if __name__ == "__main__":
    unittest.main()
