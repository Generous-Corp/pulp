#!/usr/bin/env python3
"""Tests for relay_contract_check.py.

The failure being prevented: a download added to the protected macOS workflow
(or its test corpus) from a host the egress relay does not admit fails every
gate job at once. Each case below produces one way of getting there and checks
the gate names the host and the tartci file; the real-tree case and the
unrelated-edit case prove the instrument stays quiet when nothing is wrong.
"""

from __future__ import annotations

import io
import pathlib
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import relay_contract_check as rc  # noqa: E402

WORKFLOW = rc.DEFAULT_WORKFLOW.read_text(encoding="utf-8")
CONTRACT = rc.DEFAULT_CONTRACT.read_text(encoding="utf-8")

MACOS_JOB_STEP_ANCHOR = "      - name: Install pinned Chrome for browser-source fidelity (macOS ARM64)\n"


def run_gate(workflow: str = WORKFLOW, contract: str = CONTRACT) -> tuple[int, str]:
    with tempfile.TemporaryDirectory() as tmp:
        wf = pathlib.Path(tmp) / "build.yml"
        ct = pathlib.Path(tmp) / "hosts.toml"
        wf.write_text(workflow, encoding="utf-8")
        ct.write_text(contract, encoding="utf-8")
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = rc.gate(wf, ct)
    return code, out.getvalue() + err.getvalue()


def add_macos_step(body: str) -> str:
    assert MACOS_JOB_STEP_ANCHOR in WORKFLOW
    return WORKFLOW.replace(MACOS_JOB_STEP_ANCHOR, body + MACOS_JOB_STEP_ANCHOR, 1)


def drop_host(contract: str, host: str) -> str:
    kept = [line for line in contract.splitlines() if f'"{host}"' not in line]
    assert len(kept) < len(contract.splitlines()), host
    return "\n".join(kept) + "\n"


class RealTree(unittest.TestCase):
    def test_checked_in_workflow_and_contract_agree(self):
        code, out = run_gate()
        self.assertEqual(code, 0, out)

    def test_every_known_gate_download_is_derived(self):
        need, errors = rc.required_hosts(WORKFLOW)
        self.assertEqual(errors, [])
        for host in ("storage.googleapis.com", "pypi.org", "files.pythonhosted.org",
                     "registry.npmjs.org", "ghcr.io", "formulae.brew.sh",
                     "api.vcvrack.com"):
            self.assertIn(host, need)


class MissingHost(unittest.TestCase):
    def test_removing_pypi_from_the_contract_fails(self):
        code, out = run_gate(contract=drop_host(CONTRACT, "pypi.org"))
        self.assertEqual(code, 1)
        self.assertIn("pypi.org is downloaded by the protected macOS gate", out)
        self.assertIn(rc.TARTCI_FILE, out)
        self.assertIn("pip", out)

    def test_removing_a_corpus_host_fails(self):
        code, out = run_gate(contract=drop_host(CONTRACT, "api.vcvrack.com"))
        self.assertEqual(code, 1)
        self.assertIn("api.vcvrack.com", out)
        self.assertIn("tools/rack/library_catalog.py", out)

    def test_a_new_literal_url_on_macos_fails_naming_the_host(self):
        step = ("      - name: Fetch a thing\n"
                "        run: curl -fsSL https://downloads.example.org/thing.tgz -o t.tgz\n\n")
        code, out = run_gate(add_macos_step(step))
        self.assertEqual(code, 1)
        self.assertIn("downloads.example.org", out)
        self.assertIn(rc.TARTCI_FILE, out)

    def test_a_new_package_manager_brings_its_hosts(self):
        contract = drop_host(CONTRACT, "registry.npmjs.org")
        step = ("      - name: Install a tool\n"
                "        run: npm install --global some-tool\n\n")
        code, out = run_gate(add_macos_step(step), contract)
        self.assertEqual(code, 1)
        self.assertIn("registry.npmjs.org", out)
        self.assertIn("(npm)", out)


class Quiet(unittest.TestCase):
    def test_unrelated_workflow_edit_passes(self):
        edited = add_macos_step("      - name: Print a banner\n"
                                "        run: echo hello\n\n")
        edited = edited.replace("name: Build and Test", "name: Build and Test", 1)
        code, out = run_gate(edited)
        self.assertEqual(code, 0, out)

    def test_url_in_a_shell_comment_is_ignored(self):
        step = ("      - name: Commented\n"
                "        run: |\n"
                "          # docs: https://nowhere.example.net/readme\n"
                "          echo ok\n\n")
        code, out = run_gate(add_macos_step(step))
        self.assertEqual(code, 0, out)

    def test_linux_only_step_is_not_counted(self):
        step = ("      - name: Linux only\n"
                "        if: runner.os == 'Linux'\n"
                "        run: curl -fsSL https://linux-only.example.org/x -o x\n\n")
        code, out = run_gate(add_macos_step(step))
        self.assertEqual(code, 0, out)

    def test_hosted_ubuntu_job_is_not_counted(self):
        job = ("  hosted-helper:\n"
               "    runs-on: ubuntu-latest\n"
               "    steps:\n"
               "      - run: curl -fsSL https://hosted-only.example.org/x -o x\n")
        code, out = run_gate(WORKFLOW.rstrip("\n") + "\n" + job)
        self.assertEqual(code, 0, out)


class CorpusDeclarations(unittest.TestCase):
    def test_a_rotted_needle_fails(self):
        saved = rc.CORPUS_HOSTS
        try:
            rc.CORPUS_HOSTS = (rc.CorpusHost("api.vcvrack.com", "tools/rack/library_catalog.py",
                                             "text that is not there", "x"),)
            need, errors = rc.required_hosts(WORKFLOW)
        finally:
            rc.CORPUS_HOSTS = saved
        self.assertTrue(any("no longer contains" in e for e in errors), errors)
        self.assertNotIn("api.vcvrack.com", need)


class SourceCheck(unittest.TestCase):
    def _tartci(self, tmp: pathlib.Path, text: str) -> pathlib.Path:
        path = tmp / rc.TARTCI_FILE
        path.parent.mkdir(parents=True)
        path.write_text(text, encoding="utf-8")
        return tmp

    def test_matching_copy_passes_and_drift_names_hosts(self):
        with tempfile.TemporaryDirectory() as tmp:
            tartci = self._tartci(pathlib.Path(tmp), CONTRACT)
            copy = pathlib.Path(tmp) / "copy.toml"
            copy.write_text(drop_host(CONTRACT, "pypi.org"), encoding="utf-8")
            out = io.StringIO()
            with redirect_stdout(out), redirect_stderr(io.StringIO()):
                self.assertEqual(rc.main(["--check", "--tartci", str(tartci),
                                          "--contract", str(copy)]), 1)
            self.assertIn("tartci relays pypi.org", out.getvalue())
            with redirect_stdout(io.StringIO()):
                self.assertEqual(rc.main(["--write", "--tartci", str(tartci),
                                          "--contract", str(copy)]), 0)
                self.assertEqual(rc.main(["--check", "--tartci", str(tartci),
                                          "--contract", str(copy)]), 0)

    def test_unreadable_source_is_exit_2(self):
        with tempfile.TemporaryDirectory() as tmp, redirect_stderr(io.StringIO()):
            self.assertEqual(rc.main(["--check", "--tartci", tmp]), 2)


if __name__ == "__main__":
    unittest.main()
