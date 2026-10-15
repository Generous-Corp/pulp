#!/usr/bin/env python3
"""Positive and negative controls for the host adapter tracing contract."""
from __future__ import annotations

import contextlib
import importlib.util
import io
import pathlib
import tempfile
import unittest

MODULE_PATH = pathlib.Path(__file__).with_name("check_host_adapter_tracing_contract.py")
SPEC = importlib.util.spec_from_file_location("host_adapter_tracing_contract", MODULE_PATH)
contract = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(contract)


class HostAdapterTracingContractTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        for relative in contract.ADAPTERS:
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("runtime::ScopedTracingAttachment tracing_;\n", encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def _run(self) -> int:
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return contract.check(self.root)

    def test_complete_adapter_set_passes(self):
        self.assertEqual(self._run(), 0)

    def test_missing_adapter_fails(self):
        (self.root / contract.ADAPTERS[0]).unlink()
        self.assertEqual(self._run(), 1)

    def test_comment_only_mention_fails(self):
        path = self.root / contract.ADAPTERS[0]
        path.write_text("// ScopedTracingAttachment tracing_;\n", encoding="utf-8")
        self.assertEqual(self._run(), 1)


if __name__ == "__main__":
    unittest.main()
